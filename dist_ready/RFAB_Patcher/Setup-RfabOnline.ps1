param(
    [string]$RfabDir = "",
    [string]$ModSourceDir = "",
    [string]$ServerSourceDir = "",
    [switch]$SkipFriendArchive,
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Write-Step([string]$Msg) {
    Write-Host "`n==> $Msg" -ForegroundColor Cyan
}

function Write-Ok([string]$Msg) {
    Write-Host "  [OK] $Msg" -ForegroundColor Green
}

function Write-Warn([string]$Msg) {
    Write-Host "  [!]  $Msg" -ForegroundColor Yellow
}

function Decode-Mo2IniValue([string]$Raw) {
    if ([string]::IsNullOrWhiteSpace($Raw)) { return "" }
    $val = $Raw.Trim()
    if ($val.StartsWith("@ByteArray(") -and $val.EndsWith(")")) {
        $inner = $val.Substring(11, $val.Length - 12)
        $bytes = New-Object System.Collections.Generic.List[byte]
        for ($i = 0; $i -lt $inner.Length; $i++) {
            if ($inner[$i] -eq '\' -and ($i + 1) -lt $inner.Length) {
                $n = $inner[$i + 1]
                if ($n -eq 'x' -and ($i + 3) -lt $inner.Length) {
                    $hex = $inner.Substring($i + 2, 2)
                    if ($hex -match '^[0-9a-fA-F]{2}$') {
                        $bytes.Add([Convert]::ToByte($hex, 16))
                        $i += 3
                        continue
                    }
                } elseif ($n -eq '\') {
                    $bytes.Add([byte][char]'\')
                    $i += 1
                    continue
                }
            }
            foreach ($b in [System.Text.Encoding]::UTF8.GetBytes([string]$inner[$i])) {
                $bytes.Add($b)
            }
        }
        $val = [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
    } else {
        $val = [regex]::Replace($val, '\\x([0-9a-fA-F]{4})', {
            param($m) [string][char][Convert]::ToInt32($m.Groups[1].Value, 16)
        })
        $val = $val.Replace('\\', '\')
    }
    if ($val.Length -ge 2 -and $val.StartsWith('"') -and $val.EndsWith('"')) {
        $val = $val.Substring(1, $val.Length - 2)
    }
    return $val
}

function Encode-QtIniPath([string]$PathStr) {
    $norm = $PathStr.Replace('\', '/')
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $norm.ToCharArray()) {
        $code = [int][char]$ch
        if ($code -ge 32 -and $code -le 126 -and $ch -ne '\' -and $ch -ne '"') {
            [void]$sb.Append($ch)
        } else {
            [void]$sb.AppendFormat('\x{0:x4}', $code)
        }
    }
    return $sb.ToString()
}

function Backup-Once([string]$FilePath) {
    $bak = "$FilePath.rfab_online.bak"
    if ((Test-Path -LiteralPath $FilePath) -and -not (Test-Path -LiteralPath $bak)) {
        Copy-Item -LiteralPath $FilePath -Destination $bak -Force
    }
}

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "   RFAB Online (Skyrim SE 1.5.97.0.8) - Автонастройка под MO2 " -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

# 1. Определение путей комплекта (репозиторий хоста ИЛИ архив для друзей)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CandidateRepoRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\.."))
$CandidateBundleRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))

if ([string]::IsNullOrWhiteSpace($ModSourceDir)) {
    if (Test-Path -LiteralPath (Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_MO2\SkyrimTogether.esp")) {
        $ModSourceDir = Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_MO2"
    } elseif (Test-Path -LiteralPath (Join-Path $CandidateBundleRoot "RFAB_Online_MO2\SkyrimTogether.esp")) {
        $ModSourceDir = Join-Path $CandidateBundleRoot "RFAB_Online_MO2"
    } elseif (Test-Path -LiteralPath (Join-Path $ScriptDir "RFAB_Online_MO2\SkyrimTogether.esp")) {
        $ModSourceDir = Join-Path $ScriptDir "RFAB_Online_MO2"
    }
}

if ([string]::IsNullOrWhiteSpace($ServerSourceDir)) {
    if (Test-Path -LiteralPath (Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_Server\SkyrimTogetherServer.exe")) {
        $ServerSourceDir = Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_Server"
    }
}

if ([string]::IsNullOrWhiteSpace($ModSourceDir) -or -not (Test-Path -LiteralPath $ModSourceDir)) {
    throw "Не найдена папка готового мода RFAB_Online_MO2 рядом со скриптом!"
}

# 2. Проверка и авто-дополнение текстур курсора UI (assets\images\cursor.{png,dds})
Write-Step "Шаг 1/7. Проверка целостности собранного мода RFAB_Online_MO2"
$CursorDestDir = Join-Path $ModSourceDir "SkyrimTogetherReborn\assets\images"
if (-not (Test-Path -LiteralPath $CursorDestDir)) {
    New-Item -ItemType Directory -Path $CursorDestDir -Force | Out-Null
}
foreach ($cursorFile in @("cursor.png", "cursor.dds")) {
    $destCursor = Join-Path $CursorDestDir $cursorFile
    if (-not (Test-Path -LiteralPath $destCursor)) {
        foreach ($cand in @(
            (Join-Path $CandidateRepoRoot "Code\skyrim_ui\src\assets\images\$cursorFile"),
            (Join-Path $CandidateRepoRoot "distrib\bin\assets\images\$cursorFile")
        )) {
            if (Test-Path -LiteralPath $cand) {
                Copy-Item -LiteralPath $cand -Destination $destCursor -Force
                Write-Ok "Добавлен отсутствовавший файл курсора меню: $cursorFile"
                break
            }
        }
    }
}

$RequiredModFiles = @(
    "SkyrimTogether.esp",
    "SKSE\Plugins\RFAB_Online_Bindings.json",
    "SkyrimTogetherReborn\SkyrimTogether.exe",
    "SkyrimTogetherReborn\TPProcess.exe",
    "SkyrimTogetherReborn\EarlyLoad.dll",
    "SkyrimTogetherReborn\libcef.dll",
    "SkyrimTogetherReborn\UI\index.html",
    "SkyrimTogetherReborn\assets\images\cursor.png",
    "SkyrimTogetherReborn\assets\images\cursor.dds"
)
foreach ($rel in $RequiredModFiles) {
    $full = Join-Path $ModSourceDir $rel
    if (-not (Test-Path -LiteralPath $full)) {
        throw "В папке мода отсутствует обязательный файл: $full (если вы в git-репозитории, убедитесь что выполнен git lfs pull)"
    }
    $len = (Get-Item -LiteralPath $full).Length
    if ($len -lt 200 -and ($rel.EndsWith(".exe") -or $rel.EndsWith(".dll") -or $rel.EndsWith(".esp"))) {
        throw "Файл $full является Git LFS указателем ($len байт), а не бинарником! Выполните 'git lfs pull' в папке репозитория."
    }
}
Write-Ok "Все бинарники клиента, UI, плагин и курсор на месте."

# 3. Проверка что Mod Organizer 2 закрыт (чтобы MO2 не перезаписал свои ini/txt при выходе)
if (-not $NonInteractive) {
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    while (Get-Process -Name "ModOrganizer" -ErrorAction SilentlyContinue) {
        $ans = [System.Windows.Forms.MessageBox]::Show(
            "Обнаружен запущенный Mod Organizer 2!`n`nПожалуйста, ЗАКРОЙТЕ окно Mod Organizer 2 прямо сейчас (иначе при закрытии он перезапишет ModOrganizer.ini и список плагинов), затем нажмите ОК.",
            "RFAB Online - Закройте Mod Organizer 2",
            [System.Windows.Forms.MessageBoxButtons]::OKCancel,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )
        if ($ans -eq [System.Windows.Forms.DialogResult]::Cancel) {
            throw "Установка отменена пользователем."
        }
    }
}

# 4. Выбор папки сборки RFAB
Write-Step "Шаг 2/7. Поиск папки сборки RFAB, Mod Organizer 2 и SkyrimSE.exe (1.5.97)"
if ([string]::IsNullOrWhiteSpace($RfabDir)) {
    if ($NonInteractive) {
        throw "Не указан параметр -RfabDir в неинтерактивном режиме."
    }
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Выберите КОРНЕВУЮ папку вашей установленной сборки RFAB (где находится ModOrganizer.exe или папка MO2)"
    $dlg.ShowNewFolderButton = $false
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw "Папка сборки RFAB не выбрана."
    }
    $RfabDir = $dlg.SelectedPath
}
$RfabDir = [System.IO.Path]::GetFullPath($RfabDir)
if (-not (Test-Path -LiteralPath $RfabDir)) {
    throw "Указанная папка не существует: $RfabDir"
}

$Mo2ExeCandidates = @()
if (Test-Path -LiteralPath (Join-Path $RfabDir "ModOrganizer.exe")) {
    $Mo2ExeCandidates += (Join-Path $RfabDir "ModOrganizer.exe")
}
if (Test-Path -LiteralPath (Join-Path $RfabDir "MO2\ModOrganizer.exe")) {
    $Mo2ExeCandidates += (Join-Path $RfabDir "MO2\ModOrganizer.exe")
}
if ($Mo2ExeCandidates.Count -eq 0) {
    $foundMo2 = Get-ChildItem -LiteralPath $RfabDir -Filter "ModOrganizer.exe" -Recurse -Depth 3 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($foundMo2) { $Mo2ExeCandidates += $foundMo2.FullName }
}
if ($Mo2ExeCandidates.Count -eq 0) {
    throw "В папке '$RfabDir' не найден ModOrganizer.exe! Выберите корневую папку сборки RFAB."
}

$Mo2Exe = $Mo2ExeCandidates[0]
$Mo2Dir = Split-Path -Parent $Mo2Exe
$Mo2Ini = Join-Path $Mo2Dir "ModOrganizer.ini"
if (-not (Test-Path -LiteralPath $Mo2Ini)) {
    throw "Не найден файл конфигурации MO2: $Mo2Ini (запустите ModOrganizer.exe хотя бы один раз перед установкой мода)."
}
Write-Ok "Найден Mod Organizer 2: $Mo2Exe"

$Mo2IniLines = [System.IO.File]::ReadAllLines($Mo2Ini, [System.Text.Encoding]::UTF8)
$Mo2BaseDir = $Mo2Dir
$Mo2GamePath = ""
$Mo2ModsDir = ""
$Mo2ProfilesDir = ""
$Mo2SelectedProfile = ""

foreach ($line in $Mo2IniLines) {
    if ($line -match '^\s*base_directory\s*=\s*(.+)$')          { $raw = $Matches[1]; $Mo2BaseDir = Decode-Mo2IniValue $raw }
    elseif ($line -match '^\s*gamePath\s*=\s*(.+)$')            { $raw = $Matches[1]; $Mo2GamePath = Decode-Mo2IniValue $raw }
    elseif ($line -match '^\s*mod_directory\s*=\s*(.+)$')       { $raw = $Matches[1]; $Mo2ModsDir = Decode-Mo2IniValue $raw }
    elseif ($line -match '^\s*profiles_directory\s*=\s*(.+)$')  { $raw = $Matches[1]; $Mo2ProfilesDir = Decode-Mo2IniValue $raw }
    elseif ($line -match '^\s*selected_profile\s*=\s*(.+)$')    { $raw = $Matches[1]; $Mo2SelectedProfile = Decode-Mo2IniValue $raw }
}

if ([string]::IsNullOrWhiteSpace($Mo2BaseDir) -or -not (Test-Path -LiteralPath $Mo2BaseDir)) {
    $Mo2BaseDir = $Mo2Dir
}
$ModsDir = if (-not [string]::IsNullOrWhiteSpace($Mo2ModsDir)) { $Mo2ModsDir.Replace('%BASE_DIR%', $Mo2BaseDir) } else { Join-Path $Mo2BaseDir "mods" }
$ProfilesDir = if (-not [string]::IsNullOrWhiteSpace($Mo2ProfilesDir)) { $Mo2ProfilesDir.Replace('%BASE_DIR%', $Mo2BaseDir) } else { Join-Path $Mo2BaseDir "profiles" }
$ModsDir = [System.IO.Path]::GetFullPath($ModsDir)
$ProfilesDir = [System.IO.Path]::GetFullPath($ProfilesDir)

if (-not (Test-Path -LiteralPath $ModsDir)) {
    throw "Не найдена папка модов MO2: $ModsDir"
}

# Поиск SkyrimSE.exe (строго SE 1.5.97!)
$SkyrimCandidates = New-Object System.Collections.Generic.List[string]
if (-not [string]::IsNullOrWhiteSpace($Mo2GamePath)) {
    $gpExe = Join-Path ($Mo2GamePath.Replace('%BASE_DIR%', $Mo2BaseDir)) "SkyrimSE.exe"
    if (Test-Path -LiteralPath $gpExe) { $SkyrimCandidates.Add([System.IO.Path]::GetFullPath($gpExe)) }
}
$SearchRoots = @($RfabDir)
$ParentOfMo2 = Split-Path -Parent $Mo2Dir
if ($ParentOfMo2 -and (Test-Path -LiteralPath $ParentOfMo2) -and ($SearchRoots -notcontains $ParentOfMo2)) {
    $SearchRoots += $ParentOfMo2
}
foreach ($sr in $SearchRoots) {
    if (Test-Path -LiteralPath (Join-Path $sr "SkyrimSE.exe")) {
        $SkyrimCandidates.Add([System.IO.Path]::GetFullPath((Join-Path $sr "SkyrimSE.exe")))
    }
    Get-ChildItem -LiteralPath $sr -Filter "SkyrimSE.exe" -Recurse -Depth 3 -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not $_.FullName.StartsWith($ModsDir, [System.StringComparison]::OrdinalIgnoreCase)) {
            $SkyrimCandidates.Add($_.FullName)
        }
    }
}

$SkyrimExe = $null
$BadVersionFound = ""
foreach ($cand in ($SkyrimCandidates | Select-Object -Unique)) {
    $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($cand)
    $verText = "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
    if ($vi.FileMajorPart -eq 1 -and $vi.FileMinorPart -eq 5 -and $vi.FileBuildPart -eq 97) {
        $SkyrimExe = $cand
        Write-Ok "Подтверждён SkyrimSE.exe версии $verText -> $SkyrimExe"
        break
    } elseif ($cand -eq $SkyrimCandidates[0]) {
        $BadVersionFound = "$verText ($cand)"
    }
}
if (-not $SkyrimExe) {
    throw "Не найден SkyrimSE.exe версии 1.5.97! Обнаружено: '$BadVersionFound'. RFAB Online работает строго на Skyrim SE 1.5.97 (не Steam AE 1.6.x)."
}
$GameRoot = Split-Path -Parent $SkyrimExe

$SkseDll = Join-Path $GameRoot "skse64_1_5_97.dll"
if (Test-Path -LiteralPath $SkseDll) {
    Write-Ok "Найден SKSE64 2.0.20: $SkseDll"
} else {
    Write-Warn "В '$GameRoot' не найден skse64_1_5_97.dll! Убедитесь, что SKSE64 2.0.20 установлен в папку игры."
}

$AddrLibFound = (Test-Path -LiteralPath (Join-Path $GameRoot "Data\SKSE\Plugins\version-1-5-97-0.bin")) -or
                ([bool](Get-ChildItem -LiteralPath $ModsDir -Filter "version-1-5-97-0.bin" -Recurse -Depth 4 -ErrorAction SilentlyContinue | Select-Object -First 1))
if ($AddrLibFound) {
    Write-Ok "Найден Address Library SE (version-1-5-97-0.bin)"
} else {
    Write-Warn "Не найден version-1-5-97-0.bin! Убедитесь, что мод Address Library for SKSE Plugins (SE 1.5.97) включён в MO2."
}

# 5. Установка мода в <MO2>\mods\RFAB_Online
Write-Step "Шаг 3/7. Установка мода в $ModsDir\RFAB_Online"
$TargetModDir = Join-Path $ModsDir "RFAB_Online"
if (-not (Test-Path -LiteralPath $TargetModDir)) {
    New-Item -ItemType Directory -Path $TargetModDir -Force | Out-Null
}
if ([System.IO.Path]::GetFullPath($ModSourceDir) -ne [System.IO.Path]::GetFullPath($TargetModDir)) {
    & robocopy ($ModSourceDir.TrimEnd('\')) ($TargetModDir.TrimEnd('\')) /E /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "Ошибка копирования файлов мода (robocopy код $LASTEXITCODE)."
    }
}
Write-Ok "Мод RFAB_Online успешно скопирован в $TargetModDir"

# 6. Активация мода и плагина во всех профилях MO2 + генерация точного loadorder.txt для сервера
Write-Step "Шаг 4/7. Включение +RFAB_Online и *SkyrimTogether.esp в профиле MO2"
$ProfileDirs = @(Get-ChildItem -LiteralPath $ProfilesDir -Directory -ErrorAction SilentlyContinue | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName "modlist.txt")) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName "plugins.txt"))
})
if ($ProfileDirs.Count -eq 0) {
    throw "В '$ProfilesDir' не найдено ни одного профиля MO2 с modlist.txt и plugins.txt."
}

$ActiveProfileDir = $ProfileDirs[0].FullName
foreach ($pd in $ProfileDirs) {
    if ($pd.Name -eq $Mo2SelectedProfile) {
        $ActiveProfileDir = $pd.FullName
    }
    $modlistPath = Join-Path $pd.FullName "modlist.txt"
    $pluginsPath = Join-Path $pd.FullName "plugins.txt"
    $loadOrderPath = Join-Path $pd.FullName "loadorder.txt"

    Backup-Once $modlistPath
    Backup-Once $pluginsPath
    if (Test-Path -LiteralPath $loadOrderPath) { Backup-Once $loadOrderPath }

    $mlLines = @([System.IO.File]::ReadAllLines($modlistPath, [System.Text.Encoding]::UTF8) | Where-Object {
        $_.Trim() -notin @("+RFAB_Online", "-RFAB_Online", "*RFAB_Online", "+RFAB_Online_MO2", "-RFAB_Online_MO2")
    })
    $insertIdx = 0
    for ($i = 0; $i -lt [Math]::Min($mlLines.Count, 5); $i++) {
        if ($mlLines[$i].StartsWith("#") -or $mlLines[$i].EndsWith("_separator")) {
            $insertIdx = $i + 1
        } else {
            break
        }
    }
    $newMl = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $mlLines.Count; $i++) {
        if ($i -eq $insertIdx) { $newMl.Add("+RFAB_Online") }
        $newMl.Add($mlLines[$i])
    }
    if ($insertIdx -ge $mlLines.Count) { $newMl.Add("+RFAB_Online") }
    [System.IO.File]::WriteAllLines($modlistPath, $newMl, (New-Object System.Text.UTF8Encoding($false)))

    $plLines = @([System.IO.File]::ReadAllLines($pluginsPath, [System.Text.Encoding]::UTF8) | Where-Object {
        $_.Trim() -notin @("*SkyrimTogether.esp", "SkyrimTogether.esp")
    })
    $newPl = New-Object System.Collections.Generic.List[string]
    foreach ($l in $plLines) { $newPl.Add($l) }
    $newPl.Add("*SkyrimTogether.esp")
    [System.IO.File]::WriteAllLines($pluginsPath, $newPl, (New-Object System.Text.UTF8Encoding($false)))

    if (Test-Path -LiteralPath $loadOrderPath) {
        $loLines = @([System.IO.File]::ReadAllLines($loadOrderPath, [System.Text.Encoding]::UTF8) | Where-Object {
            $_.Trim() -ne "SkyrimTogether.esp"
        })
        $newLo = New-Object System.Collections.Generic.List[string]
        foreach ($l in $loLines) { $newLo.Add($l) }
        $newLo.Add("SkyrimTogether.esp")
        [System.IO.File]::WriteAllLines($loadOrderPath, $newLo, (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Ok "Профиль '$($pd.Name)': включён +RFAB_Online и *SkyrimTogether.esp"
}

function Build-ActiveLoadOrder([string]$ProfDir) {
    $enabledSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($baseMaster in @("Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm")) {
        [void]$enabledSet.Add($baseMaster)
    }

    $pFile = Join-Path $ProfDir "plugins.txt"
    foreach ($line in [System.IO.File]::ReadAllLines($pFile, [System.Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t.StartsWith("*") -and $t.Length -gt 1) {
            [void]$enabledSet.Add($t.Substring(1).Trim())
        }
    }

    $orderedEnabled = New-Object System.Collections.Generic.List[string]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $loFile = Join-Path $ProfDir "loadorder.txt"
    if (Test-Path -LiteralPath $loFile) {
        foreach ($line in [System.IO.File]::ReadAllLines($loFile, [System.Text.Encoding]::UTF8)) {
            $t = $line.Trim()
            if ([string]::IsNullOrEmpty($t) -or $t.StartsWith("#")) { continue }
            if ($enabledSet.Contains($t) -and $seen.Add($t)) {
                $orderedEnabled.Add($t)
            }
        }
    }
    foreach ($baseMaster in @("Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm")) {
        if ($seen.Add($baseMaster)) {
            $orderedEnabled.Insert([Math]::Min($orderedEnabled.Count, 5), $baseMaster)
        }
    }
    if ($seen.Add("SkyrimTogether.esp")) {
        $orderedEnabled.Add("SkyrimTogether.esp")
    }
    return $orderedEnabled
}

$ActiveLoadOrder = Build-ActiveLoadOrder -ProfDir $ActiveProfileDir
$CleanLoadOrderText = [string]::Join("`n", $ActiveLoadOrder) + "`n"
[System.IO.File]::WriteAllText((Join-Path $TargetModDir "loadorder.txt"), $CleanLoadOrderText, (New-Object System.Text.UTF8Encoding($false)))
Write-Ok "Синхронизирован активный список плагинов ($($ActiveLoadOrder.Count) шт., без выключенных .esp) -> $TargetModDir\loadorder.txt"

# 7. Патч совместимости конфигов модпака RFAB (EngineFixes.toml, [RFAB] Autosave.ini, po3_Tweaks.ini)
Write-Step "Шаг 5/7. Настройка EngineFixes.toml, [RFAB] Autosave.ini и po3_Tweaks.ini"
$CompatRoots = @($ModsDir, (Join-Path $GameRoot "Data")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique

$EngineFixesFiles = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "EngineFixes.toml" -Recurse -ErrorAction SilentlyContinue
}
foreach ($ef in ($EngineFixesFiles | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($ef.FullName, [System.Text.Encoding]::UTF8)
    $updated = $raw
    foreach ($pair in @(
        @("MemoryManager", "false"),
        @("ScaleformAllocator", "false"),
        @("RegularQuicksaves", "true"),
        @("MaxStdio", "8192")
    )) {
        $k = $pair[0]; $v = $pair[1]
        $updated = [regex]::Replace($updated, "(?m)^(\s*$([regex]::Escape($k))\s*=\s*)([^\r\n#]+)(.*)$", "`${1}$v`${3}")
    }
    if ($updated -ne $raw) {
        Backup-Once $ef.FullName
        [System.IO.File]::WriteAllText($ef.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Патч EngineFixes.toml применён: $($ef.FullName)"
    } else {
        Write-Ok "EngineFixes.toml уже совместим: $($ef.FullName)"
    }
}

$AutosaveFiles = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "[RFAB] Autosave.ini" -Recurse -ErrorAction SilentlyContinue
}
foreach ($af in ($AutosaveFiles | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($af.FullName, [System.Text.Encoding]::UTF8)
    $updated = [regex]::Replace($raw, "(?m)^(\s*Enabled\s*=\s*)([^\r\n#]+)(.*)$", "`${1}false`${3}")
    if ($updated -ne $raw) {
        Backup-Once $af.FullName
        [System.IO.File]::WriteAllText($af.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Отключён локальный несинхронный автосейв [RFAB] Autosave.ini: $($af.FullName)"
    } else {
        Write-Ok "[RFAB] Autosave.ini уже отключён: $($af.FullName)"
    }
}

$Po3Files = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "po3_Tweaks.ini" -Recurse -ErrorAction SilentlyContinue
}
foreach ($pf in ($Po3Files | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($pf.FullName, [System.Text.Encoding]::UTF8)
    if ($raw -notmatch '(?i)Load\s+EditorIDs\s*=\s*true') {
        Backup-Once $pf.FullName
        $updated = [regex]::Replace($raw, "(?mi)^(\s*Load\s+EditorIDs\s*=\s*)([^\r\n#]+)(.*)$", "`${1}true`${3}")
        [System.IO.File]::WriteAllText($pf.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Включён Load EditorIDs = true в: $($pf.FullName)"
    } else {
        Write-Ok "po3_Tweaks.ini (Load EditorIDs = true) проверен: $($pf.FullName)"
    }
}

# 8. Запись пути к RFAB SkyrimSE.exe в реестр Windows + добавление ярлыка в ModOrganizer.ini
Write-Step "Шаг 6/7. Привязка пути SkyrimSE.exe (1.5.97) и добавление кнопки запуска в MO2"
$RegPath = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
if (-not (Test-Path -LiteralPath $RegPath)) {
    New-Item -Path $RegPath -Force | Out-Null
}
Set-ItemProperty -LiteralPath $RegPath -Name "TitlePath" -Value $GameRoot -Type String
Set-ItemProperty -LiteralPath $RegPath -Name "TitleExe" -Value $SkyrimExe -Type String
Write-Ok "Реестр TiltedEvolution настроен строго на: $SkyrimExe"

Backup-Once $Mo2Ini
$ClientExe = Join-Path $TargetModDir "SkyrimTogetherReborn\SkyrimTogether.exe"
$ClientWorkDir = Join-Path $TargetModDir "SkyrimTogetherReborn"
$QtClientBin = Encode-QtIniPath $ClientExe
$QtClientWorkDir = Encode-QtIniPath $ClientWorkDir

$iniRawLines = [System.IO.File]::ReadAllLines($Mo2Ini, [System.Text.Encoding]::UTF8)
$secStart = -1
$secEnd = $iniRawLines.Count
for ($i = 0; $i -lt $iniRawLines.Count; $i++) {
    if ($iniRawLines[$i].Trim() -eq "[customExecutables]") {
        $secStart = $i
        for ($j = $i + 1; $j -lt $iniRawLines.Count; $j++) {
            if ($iniRawLines[$j].Trim() -match '^\[.+\]$') {
                $secEnd = $j
                break
            }
        }
        break
    }
}

$rebuilt = New-Object System.Collections.Generic.List[string]
if ($secStart -ge 0) {
    $maxIdx = 0
    $existingIdx = 0
    for ($i = $secStart + 1; $i -lt $secEnd; $i++) {
        if ($iniRawLines[$i] -match '^\s*(\d+)\\binary\s*=\s*(.+)$') {
            $idx = [int]$Matches[1]
            $valBin = $Matches[2]
            if ($idx -gt $maxIdx) { $maxIdx = $idx }
            $decodedBin = Decode-Mo2IniValue $valBin
            if ($decodedBin.EndsWith("SkyrimTogether.exe", [System.StringComparison]::OrdinalIgnoreCase)) {
                $existingIdx = $idx
            }
        } elseif ($iniRawLines[$i] -match '^\s*size\s*=\s*(\d+)\s*$') {
            $sz = [int]$Matches[1]
            if ($sz -gt $maxIdx) { $maxIdx = $sz }
        }
    }

    $targetIdx = if ($existingIdx -gt 0) { $existingIdx } else { $maxIdx + 1 }
    $newSize = [Math]::Max($maxIdx, $targetIdx)

    for ($i = 0; $i -le $secStart; $i++) { $rebuilt.Add($iniRawLines[$i]) }
    $wroteSize = $false
    for ($i = $secStart + 1; $i -lt $secEnd; $i++) {
        $l = $iniRawLines[$i]
        if ($l -match "^\s*$targetIdx\\") { continue }
        if ($l -match '^\s*size\s*=') {
            $rebuilt.Add("size=$newSize")
            $wroteSize = $true
            continue
        }
        if (-not [string]::IsNullOrWhiteSpace($l)) {
            $rebuilt.Add($l)
        }
    }
    if (-not $wroteSize) {
        $rebuilt.Insert($secStart + 1, "size=$newSize")
    }
    $rebuilt.Add("$targetIdx\arguments=")
    $rebuilt.Add("$targetIdx\binary=$QtClientBin")
    $rebuilt.Add("$targetIdx\custom=true")
    $rebuilt.Add("$targetIdx\hide=false")
    $rebuilt.Add("$targetIdx\ownicon=true")
    $rebuilt.Add("$targetIdx\steamAppID=")
    $rebuilt.Add("$targetIdx\title=RFAB Online - Play")
    $rebuilt.Add("$targetIdx\toolbar=true")
    $rebuilt.Add("$targetIdx\workingDirectory=$QtClientWorkDir")
    $rebuilt.Add("")
    for ($i = $secEnd; $i -lt $iniRawLines.Count; $i++) { $rebuilt.Add($iniRawLines[$i]) }
} else {
    foreach ($l in $iniRawLines) { $rebuilt.Add($l) }
    $rebuilt.Add("")
    $rebuilt.Add("[customExecutables]")
    $rebuilt.Add("size=1")
    $rebuilt.Add("1\arguments=")
    $rebuilt.Add("1\binary=$QtClientBin")
    $rebuilt.Add("1\custom=true")
    $rebuilt.Add("1\hide=false")
    $rebuilt.Add("1\ownicon=true")
    $rebuilt.Add("1\steamAppID=")
    $rebuilt.Add("1\title=RFAB Online - Play")
    $rebuilt.Add("1\toolbar=true")
    $rebuilt.Add("1\workingDirectory=$QtClientWorkDir")
}
[System.IO.File]::WriteAllLines($Mo2Ini, $rebuilt, (New-Object System.Text.UTF8Encoding($false)))
Write-Ok "В выпадающий список Mod Organizer 2 добавлен ярлык: 'RFAB Online - Play'"

# 9. Если есть серверная папка (ПК хоста): настройка сервера + генерация START_RFAB_SERVER.bat + архив для друзей
Write-Step "Шаг 7/7. Финализация сервера и сборка готового архива для друзей"
if (-not [string]::IsNullOrWhiteSpace($ServerSourceDir) -and (Test-Path -LiteralPath $ServerSourceDir)) {
    $SrvDataDir = Join-Path $ServerSourceDir "Data"
    $SrvCfgDir = Join-Path $ServerSourceDir "config"
    New-Item -ItemType Directory -Path $SrvDataDir -Force | Out-Null
    New-Item -ItemType Directory -Path $SrvCfgDir -Force | Out-Null

    [System.IO.File]::WriteAllText((Join-Path $SrvDataDir "loadorder.txt"), $CleanLoadOrderText, (New-Object System.Text.UTF8Encoding($false)))
    $EulaText = ";Please indicate your agreement to the Tilted platform service agreement`r`n[EULA]`r`nbConfirmEULA=true`r`n"
    [System.IO.File]::WriteAllText((Join-Path $SrvCfgDir "EULA.txt"), $EulaText, (New-Object System.Text.UTF8Encoding($false)))

    $RunServerTemplate = @'
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$SrvDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location -LiteralPath $SrvDir

$ProfDir = "__ACTIVE_PROFILE_DIR__"
if ((Test-Path -LiteralPath (Join-Path $ProfDir "plugins.txt")) -and (Test-Path -LiteralPath (Join-Path $ProfDir "loadorder.txt"))) {
    $enabled = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    @("Skyrim.esm","Update.esm","Dawnguard.esm","HearthFires.esm","Dragonborn.esm") | ForEach-Object { [void]$enabled.Add($_) }
    Get-Content -LiteralPath (Join-Path $ProfDir "plugins.txt") -Encoding UTF8 | ForEach-Object {
        $t = $_.Trim()
        if ($t.StartsWith("*") -and $t.Length -gt 1) { [void]$enabled.Add($t.Substring(1).Trim()) }
    }
    $outList = New-Object System.Collections.Generic.List[string]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    Get-Content -LiteralPath (Join-Path $ProfDir "loadorder.txt") -Encoding UTF8 | ForEach-Object {
        $t = $_.Trim()
        if ($t -and -not $t.StartsWith("#") -and $enabled.Contains($t) -and $seen.Add($t)) { $outList.Add($t) }
    }
    if ($seen.Add("SkyrimTogether.esp")) { $outList.Add("SkyrimTogether.esp") }
    [System.IO.File]::WriteAllText((Join-Path $SrvDir "Data\loadorder.txt"), ([string]::Join("`n", $outList) + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[RFAB Server] Автоматически синхронизировано $($outList.Count) активных плагинов из профиля MO2." -ForegroundColor Green
}

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "  Сервер RFAB Online запущен! Порт по умолчанию: 10578 (UDP)  " -ForegroundColor Cyan
Write-Host "  Для подключения САМОМУ СЕБЕ (на этом ПК):  127.0.0.1:10578  " -ForegroundColor Yellow
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
    $_.IPAddress -ne "127.0.0.1" -and -not $_.IPAddress.StartsWith("169.254.")
} | ForEach-Object {
    Write-Host "  IP для друзей ($($_.InterfaceAlias)):  $($_.IPAddress):10578" -ForegroundColor Green
}
Write-Host "  Настройки HP/баланса/пароля: $SrvDir\config\STServer.ini" -ForegroundColor Gray
Write-Host "==============================================================" -ForegroundColor Cyan

$env:TILTED_ACCEPT_EULA = "true"
& (Join-Path $SrvDir "SkyrimTogetherServer.exe")
'@
    $RunServerPs1 = $RunServerTemplate.Replace("__ACTIVE_PROFILE_DIR__", $ActiveProfileDir)
    $Utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path $ServerSourceDir "Run-Server.ps1"), $RunServerPs1, $Utf8Bom)

    $StartServerBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online Dedicated Server (Port 10578)`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Run-Server.ps1`"`r`npause`r`n"
    [System.IO.File]::WriteAllText((Join-Path $ServerSourceDir "START_SERVER.bat"), $StartServerBat, [System.Text.Encoding]::ASCII)

    $ShortcutServerBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online Dedicated Server (Port 10578)`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $ServerSourceDir 'Run-Server.ps1')`"`r`npause`r`n"
    [System.IO.File]::WriteAllText((Join-Path $RfabDir "START_RFAB_SERVER.bat"), $ShortcutServerBat, [System.Text.Encoding]::ASCII)
    Write-Ok "Готов запуск сервера в 1 клик: $(Join-Path $RfabDir 'START_RFAB_SERVER.bat')"

    if (-not $SkipFriendArchive) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
        $DistReadyRoot = Split-Path -Parent $ModSourceDir

        $UpdatedMo2Zip = Join-Path $DistReadyRoot "RFAB_Online_MO2.zip"
        Write-Host "  Обновляем $UpdatedMo2Zip (с исправленным курсором UI)..." -ForegroundColor Gray
        if (Test-Path -LiteralPath $UpdatedMo2Zip) { Remove-Item -LiteralPath $UpdatedMo2Zip -Force }
        [System.IO.Compression.ZipFile]::CreateFromDirectory($ModSourceDir, $UpdatedMo2Zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
        Write-Ok "Обновлён MO2-архив: $UpdatedMo2Zip"

        $FriendStage = Join-Path $env:TEMP ("rfab_friend_pkg_" + [Guid]::NewGuid().ToString("N"))
        try {
            New-Item -ItemType Directory -Path (Join-Path $FriendStage "Tools") -Force | Out-Null
            & robocopy ($ModSourceDir.TrimEnd('\')) ((Join-Path $FriendStage "RFAB_Online_MO2").TrimEnd('\')) /E /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
            Copy-Item -LiteralPath $MyInvocation.MyCommand.Path -Destination (Join-Path $FriendStage "Tools\Setup-RfabOnline.ps1") -Force
            $FriendBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online - Avtoustanovka dlya Druzey`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Tools\Setup-RfabOnline.ps1`" -SkipFriendArchive %*`r`nif errorlevel 1 (`r`n  echo.`r`n  echo [ERROR] Oshibka ustanovki. Smotrite soobshchenie vyshe.`r`n  pause`r`n  exit /b 1`r`n)`r`necho.`r`npause`r`n"
            [System.IO.File]::WriteAllText((Join-Path $FriendStage "USTANOVIT_RFAB_ONLINE.bat"), $FriendBat, [System.Text.Encoding]::ASCII)

            $FriendZip = Join-Path $DistReadyRoot "RFAB_Online_For_Friends.zip"
            Write-Host "  Упаковываем единый архив для друзей $FriendZip (подождите 15-30 сек)..." -ForegroundColor Gray
            if (Test-Path -LiteralPath $FriendZip) { Remove-Item -LiteralPath $FriendZip -Force }
            [System.IO.Compression.ZipFile]::CreateFromDirectory($FriendStage, $FriendZip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
            Write-Ok "Готов единый архив для друзей: $FriendZip"
        } finally {
            if (Test-Path -LiteralPath $FriendStage) {
                Remove-Item -LiteralPath $FriendStage -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}

Write-Host "`n==============================================================" -ForegroundColor Green
Write-Host "  ГОТОВО! RFAB Online полностью установлен и настроен!        " -ForegroundColor Green
Write-Host "==============================================================" -ForegroundColor Green
Write-Host "  1. Хост запускает сервер: $RfabDir\START_RFAB_SERVER.bat" -ForegroundColor White
Write-Host "  2. Откройте Mod Organizer 2 -> выберите 'RFAB Online - Play' -> Запустить" -ForegroundColor White
Write-Host "  3. В игре нажмите ПРАВЫЙ CTRL (или F2) -> Подключиться (Connect)" -ForegroundColor White
Write-Host "     - Хост вводит:   127.0.0.1:10578" -ForegroundColor Yellow
Write-Host "     - Друзья вводят: <IP_хоста_из_окна_сервера>:10578" -ForegroundColor Yellow
if (-not [string]::IsNullOrWhiteSpace($ServerSourceDir) -and -not $SkipFriendArchive) {
    Write-Host "  4. Архив для отправки друзьям: $(Join-Path (Split-Path -Parent $ModSourceDir) 'RFAB_Online_For_Friends.zip')" -ForegroundColor Cyan
}
