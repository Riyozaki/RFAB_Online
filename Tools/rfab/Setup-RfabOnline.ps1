param(
    [string]$RfabDir = "",
    [string]$ModSourceDir = "",
    [string]$ServerSourceDir = "",
    [switch]$BuildFriendArchive,
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
Write-Host "  RFAB Online (Skyrim SE 1.5.97.0.8) - Чистая автоустановка   " -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

# 1. Определение путей комплекта (автономный Release-архив ИЛИ репозиторий)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$CandidateBundleRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))
$CandidateRepoRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\.."))

if ([string]::IsNullOrWhiteSpace($ModSourceDir)) {
    foreach ($candMod in @(
        (Join-Path $CandidateBundleRoot "RFAB_Online_MO2"),
        (Join-Path $ScriptDir "RFAB_Online_MO2"),
        (Join-Path $CandidateRepoRoot "RFAB_Online_MO2"),
        (Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_MO2")
    )) {
        if (Test-Path -LiteralPath (Join-Path $candMod "SkyrimTogether.esp")) {
            $ModSourceDir = $candMod
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ServerSourceDir)) {
    foreach ($candSrv in @(
        (Join-Path $CandidateBundleRoot "RFAB_Online_Server"),
        (Join-Path $ScriptDir "RFAB_Online_Server"),
        (Join-Path $CandidateRepoRoot "RFAB_Online_Server"),
        (Join-Path $CandidateRepoRoot "dist_ready\RFAB_Online_Server")
    )) {
        if (Test-Path -LiteralPath (Join-Path $candSrv "SkyrimTogetherServer.exe")) {
            $ServerSourceDir = $candSrv
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($ModSourceDir) -or -not (Test-Path -LiteralPath $ModSourceDir)) {
    throw "Не найдена папка готового мода RFAB_Online_MO2 рядом с установщиком!"
}

# 2. Проверка целостности дистрибутива (0 LFS-заглушек, валидные PE/ESP/UI)
Write-Step "Шаг 1/7. Проверка целостности чистого релиза RFAB Online"
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
                break
            }
        }
    }
}

$SrcStrDir = Join-Path $ModSourceDir "SkyrimTogetherReborn"
$SrcSeExe = Join-Path $SrcStrDir "SkyrimSE.exe"
$SrcStExe = Join-Path $SrcStrDir "SkyrimTogether.exe"
$HasValidSe = (Test-Path -LiteralPath $SrcSeExe) -and ((Get-Item -LiteralPath $SrcSeExe).Length -gt 200)
$HasValidSt = (Test-Path -LiteralPath $SrcStExe) -and ((Get-Item -LiteralPath $SrcStExe).Length -gt 200)
if ($HasValidSe -and $HasValidSt) {
    if ((Get-Item -LiteralPath $SrcSeExe).LastWriteTimeUtc -ge (Get-Item -LiteralPath $SrcStExe).LastWriteTimeUtc) {
        Copy-Item -LiteralPath $SrcSeExe -Destination $SrcStExe -Force
    } else {
        Copy-Item -LiteralPath $SrcStExe -Destination $SrcSeExe -Force
    }
} elseif ($HasValidSe) {
    Copy-Item -LiteralPath $SrcSeExe -Destination $SrcStExe -Force
} elseif ($HasValidSt) {
    Copy-Item -LiteralPath $SrcStExe -Destination $SrcSeExe -Force
} else {
    throw "В папке '$SrcStrDir' не найден скомпилированный SkyrimSE.exe! Скачайте полный архив RFAB_Online_v1.0.0_Full_Installer.zip со страницы Releases."
}

$RequiredModFiles = @(
    "SkyrimTogether.esp",
    "SKSE\Plugins\RFAB_Online_Bindings.json",
    "SkyrimTogetherReborn\SkyrimSE.exe",
    "SkyrimTogetherReborn\SkyrimTogether.exe",
    "SkyrimTogetherReborn\TPProcess.exe",
    "SkyrimTogetherReborn\EarlyLoad.dll",
    "SkyrimTogetherReborn\libcef.dll",
    "SkyrimTogetherReborn\UI\index.html"
)
foreach ($rel in $RequiredModFiles) {
    $full = Join-Path $ModSourceDir $rel
    if (-not (Test-Path -LiteralPath $full)) {
        throw "В дистрибутиве отсутствует обязательный файл: $full"
    }
    $len = (Get-Item -LiteralPath $full).Length
    if ($len -lt 200 -and ($rel.EndsWith(".exe") -or $rel.EndsWith(".dll") -or $rel.EndsWith(".esp"))) {
        throw "Файл $full повреждён ($len байт). Скачайте полный архив RFAB_Online_v1.0.0_Full_Installer.zip со страницы Releases!"
    }
}
Write-Ok "Дистрибутив проверен: все бинарники, плагины, CEF и UI целые."

# 3. Автоматическое закрытие старого сервера + проверка закрытия игры и MO2 (чтобы ничего не блокировало чистую замену файлов)
Get-Process -Name "SkyrimTogetherServer" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
if (-not $NonInteractive) {
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    while ((Get-Process -Name "SkyrimSE" -ErrorAction SilentlyContinue) -or
           (Get-Process -Name "SkyrimTogether" -ErrorAction SilentlyContinue) -or
           (Get-Process -Name "TPProcess" -ErrorAction SilentlyContinue)) {
        $ansGame = [System.Windows.Forms.MessageBox]::Show(
            "Обнаружена запущенная игра Skyrim!`n`nПожалуйста, закройте игру прямо сейчас, чтобы установщик мог начисто удалить старые файлы и записать новые, затем нажмите ОК.",
            "RFAB Online - Закройте Skyrim",
            [System.Windows.Forms.MessageBoxButtons]::OKCancel,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )
        if ($ansGame -eq [System.Windows.Forms.DialogResult]::Cancel) {
            throw "Установка отменена пользователем."
        }
    }
    while (Get-Process -Name "ModOrganizer" -ErrorAction SilentlyContinue) {
        $ans = [System.Windows.Forms.MessageBox]::Show(
            "Обнаружен запущенный Mod Organizer 2!`n`nПожалуйста, ЗАКРОЙТЕ окно Mod Organizer 2 прямо сейчас (иначе при выходе он перезапишет настройки), затем нажмите ОК.",
            "RFAB Online - Закройте Mod Organizer 2",
            [System.Windows.Forms.MessageBoxButtons]::OKCancel,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )
        if ($ans -eq [System.Windows.Forms.DialogResult]::Cancel) {
            throw "Установка отменена пользователем."
        }
    }
}

# 4. Автопоиск папки сборки RFAB на всех локальных дисках ПК
Write-Step "Шаг 2/7. Автопоиск сборки RFAB, Mod Organizer 2 и SkyrimSE.exe (1.5.97)"
if ([string]::IsNullOrWhiteSpace($RfabDir)) {
    $AutoRfabCandidates = New-Object System.Collections.Generic.List[string]
    $RegProbe = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
    if (Test-Path -LiteralPath $RegProbe) {
        $regTitlePath = (Get-ItemProperty -LiteralPath $RegProbe -Name "TitlePath" -ErrorAction SilentlyContinue).TitlePath
        if (-not [string]::IsNullOrWhiteSpace($regTitlePath)) {
            $AutoRfabCandidates.Add($regTitlePath)
            $parentReg = Split-Path -Parent $regTitlePath
            if ($parentReg) { $AutoRfabCandidates.Add($parentReg) }
        }
    }
    foreach ($knownRoot in @(
        "G:\RFAB Online SkyrimSE",
        "G:\RFAB",
        "D:\RFAB Online SkyrimSE",
        "D:\RFAB",
        "D:\Games\RFAB",
        "C:\RFAB",
        "C:\Games\RFAB",
        "E:\RFAB",
        "F:\RFAB"
    )) {
        $AutoRfabCandidates.Add($knownRoot)
    }
    foreach ($drv in ([System.IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady })) {
        $rootPath = $drv.RootDirectory.FullName
        Get-ChildItem -LiteralPath $rootPath -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            if ($_.Name -match 'RFAB|Skyrim') { $AutoRfabCandidates.Add($_.FullName) }
            Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue | Where-Object {
                $_.Name -match 'RFAB|Skyrim'
            } | ForEach-Object { $AutoRfabCandidates.Add($_.FullName) }
        }
    }
    foreach ($candDir in ($AutoRfabCandidates | Select-Object -Unique)) {
        if (-not [string]::IsNullOrWhiteSpace($candDir) -and (Test-Path -LiteralPath $candDir)) {
            if ((Test-Path -LiteralPath (Join-Path $candDir "ModOrganizer.exe")) -or
                (Test-Path -LiteralPath (Join-Path $candDir "MO2\ModOrganizer.exe"))) {
                $RfabDir = $candDir
                Write-Ok "Автоматически найдена папка сборки RFAB: $RfabDir"
                break
            }
        }
    }
}
if ([string]::IsNullOrWhiteSpace($RfabDir)) {
    if ($NonInteractive) {
        throw "Не удалось автоматически найти папку RFAB (ModOrganizer.exe). Укажите параметр -RfabDir."
    }
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Выберите КОРНЕВУЮ папку вашей установленной сборки RFAB (где находится папка MO2 или ModOrganizer.exe)"
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

# 5. ПОЛНОЕ ЧИСТОЕ УДАЛЕНИЕ любых старых версий RFAB_Online перед установкой с нуля (НИКАКОГО наслаивания на старые файлы!)
Write-Step "Шаг 3/7. Чистое удаление старых файлов и установка свежего $ModsDir\RFAB_Online"
$TargetModDir = Join-Path $ModsDir "RFAB_Online"
$LegacyModDir = Join-Path $ModsDir "RFAB_Online_MO2"
$FullModSrc = [System.IO.Path]::GetFullPath($ModSourceDir)
$FullTargetMod = [System.IO.Path]::GetFullPath($TargetModDir)

if ((Test-Path -LiteralPath $LegacyModDir) -and ([System.IO.Path]::GetFullPath($LegacyModDir) -ne $FullModSrc)) {
    Remove-Item -LiteralPath $LegacyModDir -Recurse -Force
    Write-Ok "Полностью удалена устаревшая папка мода: $LegacyModDir"
}

if ($FullModSrc -ne $FullTargetMod) {
    if (Test-Path -LiteralPath $TargetModDir) {
        Remove-Item -LiteralPath $TargetModDir -Recurse -Force
        Write-Ok "Полностью очищена предыдущая установка мода: $TargetModDir"
    }
    New-Item -ItemType Directory -Path $TargetModDir -Force | Out-Null
    & robocopy ($ModSourceDir.TrimEnd('\')) ($TargetModDir.TrimEnd('\')) /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "Ошибка копирования файлов мода (robocopy код $LASTEXITCODE)."
    }
}
Write-Ok "Чистая версия мода RFAB_Online установлена с нуля в $TargetModDir"

# 6. Активация +RFAB_Online и *SkyrimTogether.esp (+ *SkyrimTogetherQuestPatches.esp) с оптимальным порядком до Requiem.esp
Write-Step "Шаг 4/7. Включение +RFAB_Online и *SkyrimTogether.esp в профиле MO2"
$HasQuestPatchesEsp = Test-Path -LiteralPath (Join-Path $TargetModDir "SkyrimTogetherQuestPatches.esp")
$ProfileDirs = @(Get-ChildItem -LiteralPath $ProfilesDir -Directory -ErrorAction SilentlyContinue | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName "modlist.txt")) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName "plugins.txt"))
})
if ($ProfileDirs.Count -eq 0) {
    throw "В '$ProfilesDir' не найдено ни одного профиля MO2 с modlist.txt и plugins.txt."
}

function Insert-StPluginsBeforeRequiem([string[]]$Lines, [bool]$StarPrefix, [bool]$WithQuestPatches) {
    $stripNames = @("SkyrimTogether.esp", "*SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp", "*SkyrimTogetherQuestPatches.esp")
    $clean = New-Object System.Collections.Generic.List[string]
    foreach ($l in $Lines) {
        if ($l.Trim() -notin $stripNames) {
            $clean.Add($l)
        }
    }
    # Place right before Requiem.esp (so Requiem/RFAB spell localizations & balance win on vanilla cloak spells while SkyrimTogether gets a rock-solid low slot!), or after master .esm/.esl block
    $idx = -1
    for ($i = 0; $i -lt $clean.Count; $i++) {
        $rawName = $clean[$i].Trim().TrimStart('*')
        if ($rawName -ieq "Requiem.esp") {
            $idx = $i
            break
        }
    }
    if ($idx -lt 0) {
        $idx = $clean.Count
    }
    $stEntry = if ($StarPrefix) { "*SkyrimTogether.esp" } else { "SkyrimTogether.esp" }
    $qpEntry = if ($StarPrefix) { "*SkyrimTogetherQuestPatches.esp" } else { "SkyrimTogetherQuestPatches.esp" }
    $clean.Insert($idx, $stEntry)
    if ($WithQuestPatches) {
        $clean.Insert($idx + 1, $qpEntry)
    }
    return $clean
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

    $plLines = [System.IO.File]::ReadAllLines($pluginsPath, [System.Text.Encoding]::UTF8)
    $newPl = Insert-StPluginsBeforeRequiem -Lines $plLines -StarPrefix $true -WithQuestPatches $HasQuestPatchesEsp
    [System.IO.File]::WriteAllLines($pluginsPath, $newPl, (New-Object System.Text.UTF8Encoding($false)))

    if (Test-Path -LiteralPath $loadOrderPath) {
        $loLines = [System.IO.File]::ReadAllLines($loadOrderPath, [System.Text.Encoding]::UTF8)
        $newLo = Insert-StPluginsBeforeRequiem -Lines $loLines -StarPrefix $false -WithQuestPatches $HasQuestPatchesEsp
        [System.IO.File]::WriteAllLines($loadOrderPath, $newLo, (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-Ok "Профиль '$($pd.Name)': активирован +RFAB_Online и *SkyrimTogether.esp"
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
    $srcLines = if (Test-Path -LiteralPath $loFile) {
        [System.IO.File]::ReadAllLines($loFile, [System.Text.Encoding]::UTF8)
    } else {
        [System.IO.File]::ReadAllLines($pFile, [System.Text.Encoding]::UTF8) | ForEach-Object { $_.Trim().TrimStart('*') }
    }
    foreach ($line in $srcLines) {
        $t = $line.Trim()
        if ([string]::IsNullOrEmpty($t) -or $t.StartsWith("#")) { continue }
        if ($enabledSet.Contains($t) -and $seen.Add($t)) {
            $orderedEnabled.Add($t)
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
Write-Ok "Синхронизирован активный список плагинов ($($ActiveLoadOrder.Count) шт.) -> $TargetModDir\loadorder.txt"

# 7. Патч совместимости конфигов модпака RFAB (EngineFixes.toml, SkyrimSoulsRE.ini, [RFAB] Autosave.ini, po3_Tweaks.ini, BetterStealing.dll)
Write-Step "Шаг 5/7. Настройка совместимости конфигов сборки RFAB"
$CompatRoots = @((Join-Path $Mo2BaseDir "overwrite"), $ModsDir, (Join-Path $GameRoot "Data")) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -Unique

function Ensure-SectionKeys([string]$Content, [string]$Section, [array]$Pairs) {
    $nl = if ($Content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $cur = $Content
    $missing = New-Object System.Collections.Generic.List[object]
    foreach ($pair in $Pairs) {
        $k = [string]$pair[0]; $v = [string]$pair[1]
        $pat = "(?mi)^(\s*$([regex]::Escape($k))\s*=\s*)([^\r\n#;]+)(.*)$"
        if ([regex]::IsMatch($cur, $pat)) {
            $cur = [regex]::Replace($cur, $pat, "`${1}$v`${3}")
        } else {
            $missing.Add(@($k, $v))
        }
    }
    if ($missing.Count -gt 0) {
        $addLines = ($missing | ForEach-Object { "$($_[0]) = $($_[1])$nl" }) -join ""
        $secPat = "(?mi)^(\s*\[$([regex]::Escape($Section))\][^\r\n]*\r?\n)"
        $m = [regex]::Match($cur, $secPat)
        if ($m.Success) {
            $insertAt = $m.Index + $m.Length
            $cur = $cur.Substring(0, $insertAt) + $addLines + $cur.Substring($insertAt)
        } else {
            $prefix = if ([string]::IsNullOrEmpty($cur) -or $cur.EndsWith("`n")) { "" } else { $nl }
            $cur = "$cur$prefix[$Section]$nl$addLines"
        }
    }
    return $cur
}

$EngineFixesFiles = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "EngineFixes.toml" -Recurse -ErrorAction SilentlyContinue
}
foreach ($ef in ($EngineFixesFiles | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($ef.FullName, [System.Text.Encoding]::UTF8)
    $updated = $raw
    $hasV7Mm = [regex]::IsMatch($updated, '(?mi)^\s*\[MemoryManager\]')
    if ($hasV7Mm) {
        $updated = Ensure-SectionKeys $updated "MemoryManager" @(
            @("OverrideVanilla", "false"),
            @("OverrideScaleform", "false")
        )
        foreach ($pair in @(@("MemoryManager", "false"), @("ScaleformAllocator", "false"))) {
            $k = $pair[0]; $v = $pair[1]
            $updated = [regex]::Replace($updated, "(?mi)^(\s*$([regex]::Escape($k))\s*=\s*)([^\r\n#;]+)(.*)$", "`${1}$v`${3}")
        }
        $updated = Ensure-SectionKeys $updated "Patches" @(
            @("RegularQuicksaves", "true"),
            @("MaxStdio", "8192")
        )
    } else {
        $updated = Ensure-SectionKeys $updated "Patches" @(
            @("MemoryManager", "false"),
            @("ScaleformAllocator", "false"),
            @("RegularQuicksaves", "true"),
            @("MaxStdio", "8192")
        )
    }
    $updated = Ensure-SectionKeys $updated "SaveGameMaxSize" @(
        @("SaveGameMaxSize", "true")
    )
    if ($updated -ne $raw) {
        Backup-Once $ef.FullName
        [System.IO.File]::WriteAllText($ef.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Патч EngineFixes.toml применён: $($ef.FullName)"
    } else {
        Write-Ok "EngineFixes.toml проверен: $($ef.FullName)"
    }
}

$SkyrimSoulsFiles = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "SkyrimSoulsRE.ini" -Recurse -ErrorAction SilentlyContinue
}
foreach ($sf in ($SkyrimSoulsFiles | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($sf.FullName, [System.Text.Encoding]::UTF8)
    $updated = [regex]::Replace($raw, '(?mi)^(\s*fSlowMotionMultiplier\s*=\s*)([^\r\n;#]+)(.*)$', '${1}1.000000${3}')
    $updated = [regex]::Replace($updated, '(?mi)^(\s*bEnableSlowMotion[A-Za-z0-9_]*\s*=\s*)([^\r\n;#]+)(.*)$', '${1}false${3}')
    if ($updated -ne $raw) {
        Backup-Once $sf.FullName
        [System.IO.File]::WriteAllText($sf.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Отключён локальный SlowMotion в SkyrimSoulsRE.ini: $($sf.FullName)"
    } else {
        Write-Ok "SkyrimSoulsRE.ini проверен: $($sf.FullName)"
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
        Write-Ok "[RFAB] Autosave.ini проверен: $($af.FullName)"
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
        Write-Ok "po3_Tweaks.ini проверен: $($pf.FullName)"
    }
}

$BetterStealingFiles = foreach ($cr in $CompatRoots) {
    Get-ChildItem -LiteralPath $cr -Filter "BetterStealing.dll" -Recurse -ErrorAction SilentlyContinue
}
foreach ($bs in ($BetterStealingFiles | Select-Object -Unique -Property FullName)) {
    Rename-Item -LiteralPath $bs.FullName -NewName "BetterStealing.dll.mohidden" -Force
    Write-Ok "Скрыт конфликтующий плагин BetterStealing.dll -> $($bs.FullName).mohidden"
}

try {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) {
        New-NetFirewallRule -DisplayName "RFAB Online Server (UDP 10578)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 10578 -ErrorAction SilentlyContinue | Out-Null
        New-NetFirewallRule -DisplayName "RFAB Online Server (TCP 10578)" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 10578 -ErrorAction SilentlyContinue | Out-Null
        Write-Ok "Порт 10578 (UDP/TCP) открыт в Брандмауэре Windows"
    }
} catch {}

# 8. Запись пути к RFAB SkyrimSE.exe в реестр Windows + добавление ярлыка в ModOrganizer.ini
Write-Step "Шаг 6/7. Привязка пути SkyrimSE.exe (1.5.97) и настройка кнопки 'RFAB Online - Play' в MO2"
$RegPath = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
if (-not (Test-Path -LiteralPath $RegPath)) {
    New-Item -Path $RegPath -Force | Out-Null
}
Set-ItemProperty -LiteralPath $RegPath -Name "TitlePath" -Value $GameRoot -Type String
Set-ItemProperty -LiteralPath $RegPath -Name "TitleExe" -Value $SkyrimExe -Type String
Write-Ok "Реестр TiltedEvolution привязан к: $SkyrimExe"

Backup-Once $Mo2Ini
$ClientExe = Join-Path $TargetModDir "SkyrimTogetherReborn\SkyrimSE.exe"
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
            if ($decodedBin.IndexOf("SkyrimTogetherReborn/", [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or $decodedBin.IndexOf("SkyrimTogetherReborn\", [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $existingIdx = $idx
            }
        } elseif ($iniRawLines[$i] -match '^\s*(\d+)\\title\s*=\s*RFAB Online') {
            $idx = [int]$Matches[1]
            if ($existingIdx -eq 0) { $existingIdx = $idx }
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
Write-Ok "Кнопка запуска 'RFAB Online - Play' настроена в Mod Organizer 2"

# 9. Автономная чистая установка выделенного сервера ПРЯМО В <RfabDir>\RFAB_Online_Server (не зависит от папки загрузок/репозитория!)
Write-Step "Шаг 7/7. Чистая автономная установка выделенного сервера в $RfabDir\RFAB_Online_Server"
$InstalledServerDir = Join-Path $RfabDir "RFAB_Online_Server"
if (-not [string]::IsNullOrWhiteSpace($ServerSourceDir) -and (Test-Path -LiteralPath (Join-Path $ServerSourceDir "SkyrimTogetherServer.exe"))) {
    $SavedServerIni = $null
    $ExistingIniPath = Join-Path $InstalledServerDir "config\STServer.ini"
    if (Test-Path -LiteralPath $ExistingIniPath) {
        $SavedServerIni = [System.IO.File]::ReadAllBytes($ExistingIniPath)
    }

    if ([System.IO.Path]::GetFullPath($ServerSourceDir) -ne [System.IO.Path]::GetFullPath($InstalledServerDir)) {
        if (Test-Path -LiteralPath $InstalledServerDir) {
            Remove-Item -LiteralPath $InstalledServerDir -Recurse -Force
            Write-Ok "Полностью очищена старая папка сервера: $InstalledServerDir"
        }
        New-Item -ItemType Directory -Path $InstalledServerDir -Force | Out-Null
        & robocopy ($ServerSourceDir.TrimEnd('\')) ($InstalledServerDir.TrimEnd('\')) /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) {
            throw "Ошибка копирования файлов сервера (robocopy код $LASTEXITCODE)."
        }
    }

    $SrvDataDir = Join-Path $InstalledServerDir "Data"
    $SrvCfgDir = Join-Path $InstalledServerDir "config"
    New-Item -ItemType Directory -Path $SrvDataDir -Force | Out-Null
    New-Item -ItemType Directory -Path $SrvCfgDir -Force | Out-Null

    if ($null -ne $SavedServerIni -and $SavedServerIni.Length -gt 20) {
        [System.IO.File]::WriteAllBytes((Join-Path $SrvCfgDir "STServer.ini"), $SavedServerIni)
        Write-Ok "Сохранены ваши пользовательские настройки STServer.ini"
    }

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
Write-Host "  Для подключения ХОСТУ (на этом ПК):  127.0.0.1:10578        " -ForegroundColor Yellow
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
    $_.IPAddress -ne "127.0.0.1" -and -not $_.IPAddress.StartsWith("169.254.")
} | ForEach-Object {
    Write-Host "  IP для ДРУЗЕЙ ($($_.InterfaceAlias)):  $($_.IPAddress):10578" -ForegroundColor Green
}
Write-Host "  Настройки HP/баланса/пароля: $SrvDir\config\STServer.ini" -ForegroundColor Gray
Write-Host "==============================================================" -ForegroundColor Cyan

$env:TILTED_ACCEPT_EULA = "true"
& (Join-Path $SrvDir "SkyrimTogetherServer.exe")
'@
    $RunServerPs1 = $RunServerTemplate.Replace("__ACTIVE_PROFILE_DIR__", $ActiveProfileDir)
    $Utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path $InstalledServerDir "Run-Server.ps1"), $RunServerPs1, $Utf8Bom)

    $StartServerBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online Dedicated Server (Port 10578)`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Run-Server.ps1`"`r`npause`r`n"
    [System.IO.File]::WriteAllText((Join-Path $InstalledServerDir "START_SERVER.bat"), $StartServerBat, [System.Text.Encoding]::ASCII)

    $ShortcutServerBat = "@echo off`r`nchcp 65001 >nul`r`ntitle RFAB Online Dedicated Server (Port 10578)`r`npowershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $InstalledServerDir 'Run-Server.ps1')`"`r`npause`r`n"
    [System.IO.File]::WriteAllText((Join-Path $RfabDir "START_RFAB_SERVER.bat"), $ShortcutServerBat, [System.Text.Encoding]::ASCII)
    Write-Ok "Сервер автономно установлен в: $InstalledServerDir"
    Write-Ok "Ярлык запуска сервера для хоста: $(Join-Path $RfabDir 'START_RFAB_SERVER.bat')"
} else {
    Write-Ok "Установлен клиентский пакет (для подключения к серверу хоста)."
}

Write-Host "`n==============================================================" -ForegroundColor Green
Write-Host "  УСПЕШНО! Чистая установка RFAB Online завершена!            " -ForegroundColor Green
Write-Host "==============================================================" -ForegroundColor Green
Write-Host "  Архив установщика в Загрузках теперь можно спокойно удалить —" -ForegroundColor Gray
Write-Host "  все файлы мода и сервера полностью скопированы в папку RFAB!" -ForegroundColor Gray
Write-Host ""
Write-Host "  1. [ТОЛЬКО ХОСТ] Запустите сервер:" -ForegroundColor White
Write-Host "     $RfabDir\START_RFAB_SERVER.bat" -ForegroundColor Yellow
Write-Host "  2. [ХОСТ И ДРУЗЬЯ] Откройте Mod Organizer 2 ->" -ForegroundColor White
Write-Host "     выберите справа вверху 'RFAB Online - Play' -> Нажмите 'Запустить'" -ForegroundColor Yellow
Write-Host "  3. В игре нажмите ПРАВЫЙ CTRL (или F2) -> Подключиться (Connect):" -ForegroundColor White
Write-Host "     - Хост вводит:   127.0.0.1:10578" -ForegroundColor Cyan
Write-Host "     - Друзья вводят: <IP хоста из окна сервера / Radmin VPN>:10578" -ForegroundColor Cyan
Write-Host "     (В одну группу все игроки добавляются автоматически!)" -ForegroundColor Green
$global:LASTEXITCODE = 0
exit 0
