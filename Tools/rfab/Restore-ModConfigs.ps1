# RFAB Online: undo the on-disk edits that installers and launchers BEFORE v4.1.0 made to other
# mods' settings (EngineFixes.toml, SkyrimSoulsRE.ini, [RFAB] Autosave.ini, po3_Tweaks.ini) and the
# BetterStealing.dll -> .mohidden rename.
#
# Since v4.1.0 RFAB Online never writes those files: the launcher prepares a co-op copy in its own
# folder and only the co-op process reads it (Code/common/Compat/ConfigOverlay.h). Solo RFAB keeps
# the modpack's own settings.
#
# A file is restored from its "<file>.rfab_online.bak" only when it differs from that backup in
# keys RFAB Online itself writes - so an RFAB update or a user's own edit made after the backup is
# never reverted (the file and backup are then kept and reported).
#
# The key lists MUST match kEngineFixesKeys / kSkyrimSoulsKeys / kRfabAutosaveKeys /
# kPo3TweaksKeys in Code/common/Compat/ConfigOverlay.h (Tools/rfab/check_release_invariants.py
# fails the build otherwise).

$RfabOnlineManagedKeys = [ordered]@{
    "EngineFixes.toml"    = @("memorymanager", "scaleformallocator", "formcaching", "treelodreferencecaching", "regularquicksaves", "maxstdio", "savegamemaxsize", "cleansksecosaves", "overridevanilla", "overridescaleform", "overridescrapheap", "bmemorymanager", "bscaleformallocator", "boverridescrapheap", "isavegamemaxsize", "bregularquicksaves", "imaxstdio", "bformcaching", "btreelodreferencecaching", "s", "t", "f")
    "SkyrimSoulsRE.ini"   = @("fslowmotionmultiplier", "bstatsmenu", "benableslowmotion*")
    "[RFAB] Autosave.ini" = @("enabled", "enable autosave", "save in combat", "save after combat", "save on location change", "save on sleep/wait")
    "po3_Tweaks.ini"      = @("load editorids")
}

# "section|key" (lower case) -> value, comments stripped, last assignment wins.
function Get-RfabIniKeyMap([string]$Text) {
    $map = @{}
    $section = ""
    if ($Text.Length -gt 0 -and $Text[0] -eq [char]0xFEFF) { $Text = $Text.Substring(1) }
    foreach ($rawLine in ($Text -split "`n")) {
        $line = $rawLine.TrimEnd("`r").Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(";") -or $line.StartsWith("#")) { continue }
        if ($line.StartsWith("[")) {
            $close = $line.IndexOf("]")
            if ($close -gt 0) { $section = $line.Substring(1, $close - 1).Trim().ToLowerInvariant() }
            continue
        }
        $eq = $line.IndexOf("=")
        if ($eq -le 0) { continue }
        $key = $line.Substring(0, $eq).Trim().ToLowerInvariant()
        $value = $line.Substring($eq + 1)
        $comment = [regex]::Match($value, '[ \t][;#]')
        if ($comment.Success) { $value = $value.Substring(0, $comment.Index) }
        $map["$section|$key"] = $value.Trim()
    }
    return $map
}

function Test-RfabManagedKey([string]$Key, [string[]]$Managed) {
    foreach ($m in $Managed) {
        if ($m.EndsWith("*")) {
            if ($Key.StartsWith($m.Substring(0, $m.Length - 1), [System.StringComparison]::OrdinalIgnoreCase)) { return $true }
        } elseif ($Key -ieq $m) {
            return $true
        }
    }
    return $false
}

function Test-RfabOnlyManagedKeysDiffer([string]$Before, [string]$After, [string[]]$Managed) {
    $a = Get-RfabIniKeyMap $Before
    $b = Get-RfabIniKeyMap $After
    foreach ($k in (@($a.Keys) + @($b.Keys))) {
        $va = $a[$k]
        $vb = $b[$k]
        if (-not [string]::Equals($va, $vb)) {
            $keyName = $k.Substring($k.IndexOf("|") + 1)
            if (-not (Test-RfabManagedKey $keyName $Managed)) { return $false }
        }
    }
    return $true
}

function Invoke-RfabOnlineConfigRestore([string[]]$Roots) {
    $result = [pscustomobject]@{ Restored = 0; Kept = 0; Unhidden = 0 }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $bakSuffix = ".rfab_online.bak"

    foreach ($root in $Roots) {
        if ([string]::IsNullOrWhiteSpace($root) -or -not (Test-Path -LiteralPath $root)) { continue }

        foreach ($bak in @(Get-ChildItem -LiteralPath $root -Filter "*$bakSuffix" -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            if (-not $seen.Add($bak.FullName)) { continue }
            $target = $bak.FullName.Substring(0, $bak.FullName.Length - $bakSuffix.Length)
            $name = [System.IO.Path]::GetFileName($target)
            $managed = $null
            foreach ($fileName in $RfabOnlineManagedKeys.Keys) {
                if ($name -ieq $fileName) { $managed = [string[]]$RfabOnlineManagedKeys[$fileName] }
            }
            if ($null -eq $managed) { continue } # a backup of something else (e.g. MO2 profile files)

            if (-not (Test-Path -LiteralPath $target)) {
                Write-Host "  [!] Нет файла $target - резервная копия оставлена: $($bak.FullName)" -ForegroundColor Yellow
                $result.Kept++
                continue
            }
            $current = [System.IO.File]::ReadAllText($target, [System.Text.Encoding]::UTF8)
            $original = [System.IO.File]::ReadAllText($bak.FullName, [System.Text.Encoding]::UTF8)
            if (Test-RfabOnlyManagedKeysDiffer $original $current $managed) {
                Copy-Item -LiteralPath $bak.FullName -Destination $target -Force
                Remove-Item -LiteralPath $bak.FullName -Force
                Write-Host "  [OK] Восстановлены исходные настройки мода: $target" -ForegroundColor Green
                $result.Restored++
            } else {
                Write-Host "  [!] $target изменён после установки RFAB Online (обновление RFAB или ваша правка) - оставлен как есть; старая копия: $($bak.FullName)" -ForegroundColor Yellow
                $result.Kept++
            }
        }

        foreach ($hidden in @(Get-ChildItem -LiteralPath $root -Filter "BetterStealing.dll.mohidden" -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            if (-not $seen.Add($hidden.FullName)) { continue }
            $dll = $hidden.FullName.Substring(0, $hidden.FullName.Length - ".mohidden".Length)
            if (Test-Path -LiteralPath $dll) { continue }
            Rename-Item -LiteralPath $hidden.FullName -NewName "BetterStealing.dll" -Force
            Write-Host "  [OK] BetterStealing.dll снова включён для одиночной игры (в совместной RFAB Online отключает его сам): $dll" -ForegroundColor Green
            $result.Unhidden++
        }
    }
    return $result
}
