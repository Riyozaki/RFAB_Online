param([switch]$SelfTest)
$ErrorActionPreference = "Stop"

# Runtime data is NOT a release asset. A clean binary install must not reset a campaign or
# remove a character receipt, and must refuse while another process holds a journal lease.
function Copy-RfabTreeChecked {
    param([string]$Source, [string]$Destination, [switch]$SkipLocks)
    $base = [IO.Path]::GetFullPath($Source).TrimEnd('\', '/')
    if ((Get-Item -LiteralPath $base -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point install source refused: $base" }
    $entries = @(Get-ChildItem -LiteralPath $base -Recurse -Force -ErrorAction Stop)
    if ($entries.Count -gt 100000) { throw "Install tree exceeds the file/directory limit" }
    foreach ($entry in $entries) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point install entry refused: $($entry.FullName)" }
    }
    [void][IO.Directory]::CreateDirectory($Destination)
    foreach ($entry in $entries) {
        $rel = $entry.FullName.Substring($base.Length).TrimStart('\', '/')
        $dest = Join-Path $Destination $rel
        if ($entry.PSIsContainer) {
            [void][IO.Directory]::CreateDirectory($dest)
        } elseif (-not ($SkipLocks -and $entry.Extension -ieq '.lock')) {
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))
            [IO.File]::Copy($entry.FullName, $dest, $false)
            if ((Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash) {
                throw "Copy verification failed; original retained: $($entry.FullName)"
            }
        }
    }
}

function Install-RfabCleanTree {
    param(
        [Parameter(Mandatory=$true)][string]$Source,
        [Parameter(Mandatory=$true)][string]$Destination,
        [Parameter(Mandatory=$true)][string]$BackupRoot,
        [string[]]$KeepPaths = @(),
        [scriptblock]$AfterBackup = $null # deterministic fault-injection seam; unused by installers
    )
    $sourcePath = [IO.Path]::GetFullPath($Source).TrimEnd('\', '/')
    $destPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
    $backupPath = [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\', '/')
    $separator = [IO.Path]::DirectorySeparatorChar
    if ($sourcePath -ieq $destPath) { return $null }
    if ($sourcePath.StartsWith($destPath + $separator, [StringComparison]::OrdinalIgnoreCase) -or
        $destPath.StartsWith($sourcePath + $separator, [StringComparison]::OrdinalIgnoreCase) -or
        $backupPath -ieq $destPath -or $backupPath.StartsWith($destPath + $separator, [StringComparison]::OrdinalIgnoreCase) -or
        $backupPath -ieq $sourcePath -or $backupPath.StartsWith($sourcePath + $separator, [StringComparison]::OrdinalIgnoreCase)) { throw "Nested install source/destination/backup refused" }
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) { throw "Install source missing: $sourcePath" }
    foreach ($rel in $KeepPaths) {
        if ([IO.Path]::IsPathRooted($rel) -or $rel -match '(^|[\\/])\.\.([\\/]|$)' -or [string]::IsNullOrWhiteSpace($rel)) { throw "Invalid runtime relative path" }
        if (Test-Path -LiteralPath (Join-Path $sourcePath $rel)) { throw "Release contains live/generated runtime data '$rel'; refusing to distribute somebody else's campaign/profile" }
    }
    $archive = $null
    $staging = Join-Path ([IO.Path]::GetDirectoryName($destPath)) ('.rfab-install-' + [Guid]::NewGuid().ToString('N'))
    $leases = New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
    $lockPaths = New-Object 'System.Collections.Generic.List[string]'
    try {
        if (Test-Path -LiteralPath $destPath) {
            if ((Get-Item -LiteralPath $destPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point install destination refused" }
            foreach ($rel in $KeepPaths) {
                $old = Join-Path $destPath $rel
                if (Test-Path -LiteralPath $old -PathType Container) {
                    foreach ($file in @(Get-ChildItem -LiteralPath $old -Filter '*.lock' -File -Recurse -Force -ErrorAction Stop)) {
                        # Delete sharing permits directory rename on Windows; read/write sharing
                        # stays disabled, so the native DurableLease (share=0) cannot be acquired.
                        $leases.Add([IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Delete))
                        $lockPaths.Add($file.FullName.Substring($destPath.Length).TrimStart('\', '/'))
                    }
                }
            }
        }
        # Stage and verify everything BEFORE moving the old installation. On a copy/hash error
        # the working installation and its receipts/journals have not been changed at all.
        Copy-RfabTreeChecked -Source $sourcePath -Destination $staging
        foreach ($rel in $KeepPaths) {
            $old = Join-Path $destPath $rel
            $dest = Join-Path $staging $rel
            if (Test-Path -LiteralPath $old -PathType Container) {
                Copy-RfabTreeChecked -Source $old -Destination $dest -SkipLocks
            } elseif (Test-Path -LiteralPath $old -PathType Leaf) {
                if ((Get-Item -LiteralPath $old -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse-point runtime file refused: $old" }
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest))
                [IO.File]::Copy($old, $dest, $false)
                if ((Get-FileHash -LiteralPath $old -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash) { throw "Runtime file hash mismatch: $rel" }
            }
        }
        # Fresh lease placeholders, not copies of owned lock files. Keep both generations held
        # across the rename so publishing new binaries cannot expose an unlocked copied journal.
        foreach ($rel in $lockPaths) {
            $file = Join-Path $staging $rel
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
            $leases.Add([IO.File]::Open($file, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Delete))
        }
        if (Test-Path -LiteralPath $destPath) {
            [void][IO.Directory]::CreateDirectory($backupPath)
            $archive = Join-Path $backupPath (([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')) + '-' + [Guid]::NewGuid().ToString('N'))
            [void][IO.Directory]::CreateDirectory($archive)
            Move-Item -LiteralPath $destPath -Destination (Join-Path $archive 'previous') -ErrorAction Stop
        }
        if ($AfterBackup) { & $AfterBackup | Out-Null }
        Move-Item -LiteralPath $staging -Destination $destPath -ErrorAction Stop
        if ($archive) { Write-Host "[RFAB] Runtime data preserved; previous installation retained at $archive" }
        return $archive
    } catch {
        throw "RFAB install refused/incomplete. No backup or user save was deleted. Previous data retained at '$archive' (or untouched '$destPath'); staging '$staging': $($_.Exception.Message)"
    } finally {
        foreach ($lease in $leases) { $lease.Dispose() }
    }
}

if ($SelfTest) {
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('rfab-runtime-install-' + [Guid]::NewGuid().ToString('N'))
    try {
        $src = Join-Path $temp 'package'
        $dst = Join-Path $temp 'installed'
        $bak = Join-Path $temp 'backups'
        [void][IO.Directory]::CreateDirectory($src)
        [void][IO.Directory]::CreateDirectory((Join-Path $dst 'Data/rfab_campaign'))
        [IO.File]::WriteAllText((Join-Path $src 'new.exe'), 'new-binary')
        [IO.File]::WriteAllText((Join-Path $dst 'obsolete.dll'), 'old-binary')
        [IO.File]::WriteAllText((Join-Path $dst 'Data/rfab_campaign/test.inventory'), 'durable inventory and uncertain receipts')
        [IO.File]::WriteAllText((Join-Path $dst 'Data/rfab_campaign/test.lock'), 'not data, must not be copied')
        $oldHash = (Get-FileHash -LiteralPath (Join-Path $dst 'Data/rfab_campaign/test.inventory') -Algorithm SHA256).Hash
        $archive = Install-RfabCleanTree -Source $src -Destination $dst -BackupRoot $bak -KeepPaths @('Data/rfab_campaign')
        if ((Test-Path -LiteralPath (Join-Path $dst 'obsolete.dll')) -or -not (Test-Path -LiteralPath (Join-Path $dst 'new.exe'))) { throw 'Clean binary replacement failed' }
        if ((Get-FileHash -LiteralPath (Join-Path $dst 'Data/rfab_campaign/test.inventory') -Algorithm SHA256).Hash -cne $oldHash) { throw 'Campaign journal changed' }
        if (-not (Test-Path -LiteralPath (Join-Path $archive 'previous/obsolete.dll')) -or
            -not (Test-Path -LiteralPath (Join-Path $archive 'previous/Data/rfab_campaign/test.inventory'))) { throw 'Previous installation was not retained' }
        if ((Get-Item -LiteralPath (Join-Path $dst 'Data/rfab_campaign/test.lock')).Length -ne 0) { throw 'Owned lease contents were copied' }
        $lock = [IO.File]::Open((Join-Path $dst 'Data/rfab_campaign/test.lock'), [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $refused = $false
        try {
            try { Install-RfabCleanTree -Source $src -Destination $dst -BackupRoot $bak -KeepPaths @('Data/rfab_campaign') | Out-Null }
            catch { $refused = $true }
        } finally { $lock.Dispose() }
        if (-not $refused -or -not (Test-Path -LiteralPath (Join-Path $dst 'new.exe'))) { throw 'Live journal owner was not refused before mutation' }
        $refused = $false
        try { Install-RfabCleanTree -Source $src -Destination $dst -BackupRoot $bak -KeepPaths @('Data/rfab_campaign') -AfterBackup { throw 'injected post-backup publication failure' } | Out-Null }
        catch { $refused = $true }
        if (-not $refused) { throw 'Publication fault was ignored' }
        $found = @(Get-ChildItem -LiteralPath $bak -Filter 'test.inventory' -File -Recurse | Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash -ceq $oldHash })
        if ($found.Count -lt 2) { throw 'Original runtime data not retained after injected publication failure' }
        [void][IO.Directory]::CreateDirectory((Join-Path $src 'Data/rfab_campaign'))
        $refused = $false
        try { Install-RfabCleanTree -Source $src -Destination $dst -BackupRoot $bak -KeepPaths @('Data/rfab_campaign') | Out-Null }
        catch { $refused = $true }
        if (-not $refused) { throw 'Package with live campaign data was accepted' }
        Write-Host '[RFAB_RESULT] runtime install self-test PASS: clean binaries, byte-exact campaign preservation, retained backups, live-lease refusal, publication fault, dirty-package refusal'
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    }
}
