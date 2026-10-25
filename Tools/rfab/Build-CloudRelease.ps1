param(
    [string]$VersionTag = "v1.2.0",
    [string]$BuildMode = "releasedbg"
)

if ($VersionTag -ne "v1.2.0") {
    Write-Host "[RELEASE POLICY] Remapping -VersionTag '$VersionTag' -> 'v1.2.0' (v1.0.0 and v1.1.0 backups are preserved untouched; active release target is v1.2.0)." -ForegroundColor Yellow
    $VersionTag = "v1.2.0"
}
if ($VersionTag -eq "v1.0.0" -or $VersionTag -eq "v1.1.0") {
    throw "Hard guard: v1.0.0 and v1.1.0 must never be overwritten!"
}

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$script:PackagesReady = $false
$script:LastDiagMatches = ""
$script:LastDiagTail = ""
$script:ProgressLog = New-Object System.Collections.Generic.List[string]
$CacheMarker = Join-Path $env:LOCALAPPDATA ".xmake\packages\.rfab_cached_ok"

function Update-ReleaseTelemetry([string]$StateTitle, [string]$BodyText) {
    try {
        if ($StateTitle.Length -gt 200) { $StateTitle = $StateTitle.Substring(0, 200) }
        if ($BodyText.Length -gt 60000) {
            $BodyText = $BodyText.Substring($BodyText.Length - 60000)
        }
        $tmpNotes = Join-Path $env:TEMP ("rfab_telemetry_" + [Guid]::NewGuid().ToString("N") + ".md")
        [System.IO.File]::WriteAllText($tmpNotes, $BodyText, (New-Object System.Text.UTF8Encoding($false)))
        $sha = (& git rev-parse HEAD).Trim()
        & cmd.exe /d /s /c "gh release view $VersionTag >nul 2>&1"
        if ($LASTEXITCODE -ne 0) {
            & cmd.exe /d /s /c "gh release create $VersionTag --target $sha --title `"$StateTitle`" --notes-file `"$tmpNotes`" --prerelease >nul 2>&1"
        } else {
            & cmd.exe /d /s /c "gh release edit $VersionTag --title `"$StateTitle`" --notes-file `"$tmpNotes`" >nul 2>&1"
        }
        Remove-Item -LiteralPath $tmpNotes -Force -ErrorAction SilentlyContinue
    } catch {}
}

function Emit-Annotation([string]$Level, [string]$Title, [string]$Msg) {
    $clean = $Msg -replace "\r", "" -replace "\n", " | " -replace "%", "%25"
    if ($clean.Length -gt 3500) { $clean = $clean.Substring($clean.Length - 3500) }
    Write-Host "::${Level} title=${Title}::${clean}"
}

function Invoke-LoggedCmd([string]$StepName, [string]$CmdString) {
    Write-Host "`n==> $StepName" -ForegroundColor Cyan
    $script:ProgressLog.Add("- ⏳ Running: ``$StepName`` ($([DateTime]::UtcNow.ToString('HH:mm:ss')) UTC)")

    $uid = [Guid]::NewGuid().ToString("N")
    $stepBat = Join-Path $env:TEMP ("rfab_cmd_" + $uid + ".cmd")
    $stepLog = Join-Path $env:TEMP ("rfab_step_" + $uid + ".log")
    $batBody = "@echo off`r`ncall :__rfab_step > `"$stepLog`" 2>&1`r`nexit /b %ERRORLEVEL%`r`n:__rfab_step`r`n$CmdString`r`n"
    [System.IO.File]::WriteAllText($stepBat, $batBody, (New-Object System.Text.UTF8Encoding($false)))

    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & cmd.exe /d /c $stepBat
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    Remove-Item -LiteralPath $stepBat -Force -ErrorAction SilentlyContinue

    if (Test-Path -LiteralPath $stepLog) {
        $allLines = [System.IO.File]::ReadAllLines($stepLog)
        $tailShow = if ($allLines.Length -gt 80) { $allLines[($allLines.Length - 80)..($allLines.Length - 1)] } else { $allLines }
        foreach ($ln in $tailShow) { Write-Host $ln }
        if ($code -ne 0) {
            $errMatches = @($allLines | Where-Object { $_ -match '(?i)(error\s+[A-Z]*\d+|fatal error|unresolved external|error:|Traceback|Assertion|Exception)' } | ForEach-Object { $_ -replace '-external:[^\s]+', '' })
            if ($errMatches.Count -gt 0) {
                $script:LastDiagMatches = ($errMatches -join "`r`n")
                Emit-Annotation "warning" "DIAG_MATCHES" ($errMatches -join "`n")
            }
            $compactTail = @($tailShow | ForEach-Object { $_ -replace '-external:[^\s]+', '' })
            $script:LastDiagTail = ($compactTail -join "`r`n")
            Emit-Annotation "warning" "DIAG_TAIL" ($compactTail -join "`n")
            $failLogAsset = Join-Path $env:TEMP "build_step_log.txt"
            Copy-Item -LiteralPath $stepLog -Destination $failLogAsset -Force -ErrorAction SilentlyContinue
            & cmd.exe /d /s /c "gh release upload $VersionTag `"$failLogAsset`" --clobber >nul 2>&1"
            Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
            throw "$StepName exited with code $code"
        }
        Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
    } elseif ($code -ne 0) {
        throw "$StepName exited with code $code (no log file)"
    }
    $script:ProgressLog[$script:ProgressLog.Count - 1] = "- ✅ Completed: ``$StepName`` ($([DateTime]::UtcNow.ToString('HH:mm:ss')) UTC)"
    Emit-Annotation "notice" "RFAB_BUILD" "OK: $StepName"
}

try {
    $RepoRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."))
    Set-Location -LiteralPath $RepoRoot

    Invoke-LoggedCmd "[1/7] Submodules init + SE 1.5.97 Submodule Patches" "git submodule sync --recursive && git submodule update --init --force --recursive --depth=1 && python Tools\rfab\patch_submodules.py"
    Invoke-LoggedCmd "[2/7] SE 1.5.97 RVA & Struct Audit" "python Tools\rfab\verify_se1597_port.py"
    Invoke-LoggedCmd "[3/7] xmake config (Windows x64 /MT $BuildMode)" "xmake config --plat=windows --arch=x64 --mode=$BuildMode --yes -vD"
    $script:PackagesReady = $true

    Invoke-LoggedCmd "[4/7] xmake build (Client + Launcher + Server + Tests)" "xmake -y -v"

    $TpTests = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "build\windows\x64\$BuildMode") -Filter "TPTests.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $TpTests) {
        throw "TPTests.exe not found under build\windows\x64\$BuildMode!"
    }
    Invoke-LoggedCmd "[5/7] Native Windows x64 Unit Tests (TPTests.exe)" "`"$($TpTests.FullName)`""

    Invoke-LoggedCmd "[6/7] xmake install + Package-Release.ps1 + Smoke & Clean-Wipe Verification" `
        "xmake install -o distrib && powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools\rfab\Package-Release.ps1 -VersionTag $VersionTag -BuildMode $BuildMode"

    $NotesTemplateFile = Join-Path $RepoRoot "Tools\rfab\RELEASE_NOTES_TEMPLATE.md"
    if (-not (Test-Path -LiteralPath $NotesTemplateFile)) {
        throw "Release notes template missing: $NotesTemplateFile"
    }
    $NotesFile = Join-Path $RepoRoot "release_artifacts\RELEASE_NOTES.md"
    $ReleaseNotes = [System.IO.File]::ReadAllText($NotesTemplateFile, [System.Text.Encoding]::UTF8)
    $ReleaseNotes = $ReleaseNotes.Replace("{{VERSION_TAG}}", $VersionTag)
    [System.IO.File]::WriteAllText($NotesFile, $ReleaseNotes, (New-Object System.Text.UTF8Encoding($false)))

    $CommitSha    = (& git rev-parse HEAD).Trim()
    $InstallerZip = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Full_Installer.zip"
    $QuickUpdZip  = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Quick_Update.zip"
    $PdbZip       = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_PDB_Symbols.zip"
    $ShaFile      = Join-Path $RepoRoot "release_artifacts\SHA256SUMS.txt"
    $RelTitle     = "RFAB Online $VersionTag - Full Clean Standalone Installer (Skyrim SE 1.5.97.0.8)"

    & cmd.exe /d /s /c "gh release view $VersionTag >nul 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Invoke-LoggedCmd "[7/7] Create GitHub Release $VersionTag" `
            "gh release create $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" --latest"
    } else {
        Invoke-LoggedCmd "[7/7] Update GitHub Release Metadata $VersionTag (in-place)" `
            "gh release edit $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" --prerelease=false --latest"
    }

    foreach ($assetPath in @($QuickUpdZip, $InstallerZip, $PdbZip, $ShaFile)) {
        $assetName = Split-Path -Leaf $assetPath
        $uploadedOk = $false
        for ($attempt = 1; $attempt -le 4; $attempt++) {
            Write-Host "::group::Upload $assetName (attempt $attempt/4)"
            & cmd.exe /d /s /c "gh release upload $VersionTag `"$assetPath`" --clobber 2>&1"
            $upCode = $LASTEXITCODE
            Write-Host "::endgroup::"
            if ($upCode -eq 0) {
                Emit-Annotation "notice" "RFAB_UPLOAD" "OK: Uploaded $assetName (attempt $attempt)"
                $uploadedOk = $true
                break
            }
            Write-Warning "Transient upload failure for $assetName (attempt $attempt/4, exit $upCode); retrying in 4s..."
            Start-Sleep -Seconds 4
        }
        if (-not $uploadedOk) {
            throw "[7/7] Failed to upload $assetName after 4 attempts"
        }
    }

    & cmd.exe /d /s /c "gh release view v1.0.0-rfab >nul 2>&1"
    if ($LASTEXITCODE -eq 0) {
        & cmd.exe /d /s /c "gh release delete v1.0.0-rfab --yes --cleanup-tag"
    }

    if (Test-Path -LiteralPath (Split-Path -Parent $CacheMarker)) {
        [System.IO.File]::WriteAllText($CacheMarker, "ok")
    }
    & cmd.exe /d /s /c "gh release delete-asset $VersionTag build_step_log.txt --yes >nul 2>&1"
    & cmd.exe /d /s /c "gh release edit $VersionTag --target $CommitSha --title `"$RelTitle`" --notes-file `"$NotesFile`" --prerelease=false --latest"
    Emit-Annotation "notice" "RELEASE_READY" "Published https://github.com/Riyozaki/RFAB_Online/releases/tag/$VersionTag"
    $global:LASTEXITCODE = 0
    exit 0
} catch {
    $errMsg = $_.Exception.Message
    $failBody = "## ❌ Build Failure: $errMsg`r`n`r`n### Progress`r`n$($script:ProgressLog -join "`r`n")`r`n`r`n### Diagnostic Matches`r`n``````text`r`n$($script:LastDiagMatches)`r`n```````r`n`r`n### Log Tail`r`n``````text`r`n$($script:LastDiagTail)`r`n``````"
    Update-ReleaseTelemetry "RFAB Online $VersionTag (FAILED: $errMsg)" $failBody
    Emit-Annotation "error" "CI_EXCEPTION" $errMsg
    exit 1
}
