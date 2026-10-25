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

function Remove-DirectoryClean([string]$DirPath) {
    if (-not (Test-Path -LiteralPath $DirPath)) { return }
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Get-ChildItem -LiteralPath $DirPath -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
                try { $_.Attributes = 'Normal' } catch {}
            }
            Remove-Item -LiteralPath $DirPath -Recurse -Force -ErrorAction Stop
            if (-not (Test-Path -LiteralPath $DirPath)) { return }
        } catch {
            if ($attempt -eq 5) { throw }
            Start-Sleep -Milliseconds 450
        }
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
    throw "В папке '$SrcStrDir' не найден скомпилированный SkyrimSE.exe! Скачайте полный архив RFAB_Online_v1.2.0_Full_Installer.zip со страницы Releases."
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
        throw "Файл $full повреждён ($len байт). Скачайте полный архив RFAB_Online_v1.2.0_Full_Installer.zip со страницы Releases!"
    }
}
Write-Ok "Дистрибутив проверен: все бинарники, плагины, CEF и UI целые."

# 3. Автоматическое закрытие старого сервера и осиротевших CEF-процессов + проверка закрытия игры и MO2
$runningSrv = @(Get-Process -Name "SkyrimTogetherServer" -ErrorAction SilentlyContinue)
if ($runningSrv.Count -gt 0) {
    $runningSrv | Stop-Process -Force -ErrorAction SilentlyContinue
    $runningSrv | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
}
if (-not (Get-Process -Name "SkyrimSE" -ErrorAction SilentlyContinue) -and
    -not (Get-Process -Name "SkyrimTogether" -ErrorAction SilentlyContinue)) {
    $orphanTp = @(Get-Process -Name "TPProcess" -ErrorAction SilentlyContinue)
    if ($orphanTp.Count -gt 0) {
        $orphanTp | Stop-Process -Force -ErrorAction SilentlyContinue
        $orphanTp | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
    }
}
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

# 4. Умный поиск всех установок MO2/RFAB + удобное окно подтверждения/выбора папки (читает точный gamePath из ModOrganizer.ini!)
Write-Step "Шаг 2/7. Определение папки сборки RFAB, Mod Organizer 2 и SkyrimSE.exe (1.5.97)"

function Get-SkyrimExeDetails([string]$ExePath) {
    if ([string]::IsNullOrWhiteSpace($ExePath) -or -not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        return $null
    }
    $fullExe = [System.IO.Path]::GetFullPath($ExePath)
    $exeDir  = Split-Path -Parent $fullExe

    $probeFiles = @($fullExe)
    $cdxSibling = [System.IO.Path]::ChangeExtension($fullExe, ".cdx")
    if (Test-Path -LiteralPath $cdxSibling -PathType Leaf) {
        $probeFiles += $cdxSibling
    }

    $realVersions = New-Object System.Collections.Generic.List[string]
    foreach ($pf in $probeFiles) {
        try {
            $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($pf)
            $rawCands = @(
                "$($vi.ProductVersion)",
                "$($vi.ProductMajorPart).$($vi.ProductMinorPart).$($vi.ProductBuildPart).$($vi.ProductPrivatePart)",
                "$($vi.FileVersion)",
                "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
            )
            foreach ($rc in $rawCands) {
                if ([string]::IsNullOrWhiteSpace($rc)) { continue }
                $nums = @([regex]::Matches($rc, '\d+') | ForEach-Object { [int]$_.Value })
                if ($nums.Count -lt 3) { continue }
                $p3 = if ($nums.Count -ge 4) { $nums[3] } else { 0 }
                # Bethesda leaves FILEVERSION = 1.0.0.0 while storing 1.5.97.0 in ProductVersion / standalone wrappers
                if (($nums[0] -eq 0 -or $nums[0] -eq 1) -and $nums[1] -eq 0 -and $nums[2] -eq 0 -and $p3 -eq 0) {
                    continue
                }
                $norm = "$($nums[0]).$($nums[1]).$($nums[2]).$p3"
                if (-not $realVersions.Contains($norm)) {
                    $realVersions.Add($norm)
                }
            }
        } catch {}
    }

    foreach ($v in $realVersions) {
        if ($v -match '^1\.5\.97(\.|$)') {
            return [PSCustomObject]@{ ExePath = $fullExe; Version = $v; IsSe1597 = $true }
        }
    }
    if ($realVersions.Count -gt 0) {
        return [PSCustomObject]@{ ExePath = $fullExe; Version = $realVersions[0]; IsSe1597 = $false }
    }

    if ((Test-Path -LiteralPath (Join-Path $exeDir "skse64_1_5_97.dll")) -or
        (Test-Path -LiteralPath (Join-Path $exeDir "Data\SKSE\Plugins\version-1-5-97-0.bin"))) {
        return [PSCustomObject]@{ ExePath = $fullExe; Version = "1.5.97.0 (Standalone)"; IsSe1597 = $true }
    }

    return [PSCustomObject]@{ ExePath = $fullExe; Version = "1.0.0.0"; IsSe1597 = $false }
}

function Resolve-Mo2AndSkyrimPreview([string]$InputPath) {
    if ([string]::IsNullOrWhiteSpace($InputPath)) { return $null }
    $p = $InputPath.Trim().Trim('"')
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        $p = Split-Path -Parent $p
    }
    if (-not (Test-Path -LiteralPath $p)) { return $null }

    $mo2 = $null
    if (Test-Path -LiteralPath (Join-Path $p "ModOrganizer.exe")) {
        $mo2 = Join-Path $p "ModOrganizer.exe"
    } elseif (Test-Path -LiteralPath (Join-Path $p "MO2\ModOrganizer.exe")) {
        $mo2 = Join-Path $p "MO2\ModOrganizer.exe"
    } else {
        $found = Get-ChildItem -LiteralPath $p -Filter "ModOrganizer.exe" -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $mo2 = $found.FullName }
    }
    if (-not $mo2) { return $null }

    $mo2Folder = Split-Path -Parent $mo2
    $iniFile = Join-Path $mo2Folder "ModOrganizer.ini"
    $gameExeFound = ""
    $gameVerFound = ""
    $gameIs1597 = $false
    $isRfabPack = $false

    if (Test-Path -LiteralPath $iniFile) {
        $baseD = $mo2Folder
        $gpRaw = ""
        foreach ($ln in [System.IO.File]::ReadAllLines($iniFile, [System.Text.Encoding]::UTF8)) {
            if ($ln -match '^\s*base_directory\s*=\s*(.+)$') { $baseD = Decode-Mo2IniValue $Matches[1] }
            elseif ($ln -match '^\s*gamePath\s*=\s*(.+)$')   { $gpRaw = Decode-Mo2IniValue $Matches[1] }
        }
        if ([string]::IsNullOrWhiteSpace($baseD) -or -not (Test-Path -LiteralPath $baseD)) { $baseD = $mo2Folder }
        $profsD = Join-Path $baseD "profiles"
        if (Test-Path -LiteralPath $profsD) {
            $profHit = Get-ChildItem -LiteralPath $profsD -Filter "plugins.txt" -Recurse -Depth 2 -ErrorAction SilentlyContinue | Where-Object {
                ([System.IO.File]::ReadAllText($_.FullName) -match 'Requiem\.esp|\[RFAB\]')
            } | Select-Object -First 1
            if ($profHit) { $isRfabPack = $true }
        }
        $tryExes = New-Object System.Collections.Generic.List[string]
        if (-not [string]::IsNullOrWhiteSpace($gpRaw)) {
            $tryExes.Add((Join-Path ($gpRaw.Replace('%BASE_DIR%', $baseD)) "SkyrimSE.exe"))
        }
        $tryExes.Add((Join-Path $p "SkyrimSE.exe"))
        $parentM = Split-Path -Parent $mo2Folder
        if ($parentM) { $tryExes.Add((Join-Path $parentM "SkyrimSE.exe")) }
        foreach ($te in $tryExes) {
            $det = Get-SkyrimExeDetails $te
            if ($null -ne $det) {
                $gameExeFound = $det.ExePath
                $gameVerFound = $det.Version
                $gameIs1597   = $det.IsSe1597
                break
            }
        }
    }

    # Prefer the outer RFAB root folder if MO2 lives inside <Root>\MO2
    $installRoot = if ((Split-Path -Leaf $mo2Folder) -ieq "MO2") { Split-Path -Parent $mo2Folder } else { $mo2Folder }
    return [PSCustomObject]@{
        RootDir  = $installRoot
        Mo2Exe   = $mo2
        SkyrimExe = $gameExeFound
        Version  = $gameVerFound
        IsSe1597 = $gameIs1597
        IsRfab   = $isRfabPack
    }
}

$DiscoveredInstalls = New-Object System.Collections.Generic.List[object]
$SeenMo2 = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

function Add-CandidateRoot([string]$Cand) {
    $info = Resolve-Mo2AndSkyrimPreview $Cand
    if ($null -ne $info -and $SeenMo2.Add($info.Mo2Exe)) {
        $DiscoveredInstalls.Add($info)
    }
}

if (-not [string]::IsNullOrWhiteSpace($RfabDir)) {
    Add-CandidateRoot $RfabDir
}
$RegProbe = "HKCU:\Software\TiltedPhoques\TiltedEvolution\Skyrim Special Edition"
if (Test-Path -LiteralPath $RegProbe) {
    $regTitlePath = (Get-ItemProperty -LiteralPath $RegProbe -Name "TitlePath" -ErrorAction SilentlyContinue).TitlePath
    if (-not [string]::IsNullOrWhiteSpace($regTitlePath)) {
        Add-CandidateRoot $regTitlePath
        $parentReg = Split-Path -Parent $regTitlePath
        if ($parentReg) { Add-CandidateRoot $parentReg }
    }
}
foreach ($drv in ([System.IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady })) {
    $rootPath = $drv.RootDirectory.FullName
    Get-ChildItem -LiteralPath $rootPath -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        Add-CandidateRoot $_.FullName
        Get-ChildItem -LiteralPath $_.FullName -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            Add-CandidateRoot $_.FullName
        }
    }
}

$SortedInstalls = @($DiscoveredInstalls | Sort-Object -Property @{Expression={$_.IsRfab};Descending=$true}, @{Expression={$_.IsSe1597};Descending=$true})
if ([string]::IsNullOrWhiteSpace($RfabDir) -and $SortedInstalls.Count -gt 0) {
    $RfabDir = $SortedInstalls[0].RootDir
}

if (-not $NonInteractive) {
    Add-Type -AssemblyName System.Drawing | Out-Null
    $form = New-Object System.Windows.Forms.Form
    $form.Text = "Установка RFAB Online — Выбор папки сборки RFAB"
    $form.Size = New-Object System.Drawing.Size(680, 290)
    $form.StartPosition = "CenterScreen"
    $form.FormBorderStyle = "FixedDialog"
    $form.MaximizeBox = $false
    $form.TopMost = $true

    $lblTitle = New-Object System.Windows.Forms.Label
    $lblTitle.Location = New-Object System.Drawing.Point(18, 16)
    $lblTitle.Size = New-Object System.Drawing.Size(630, 40)
    $lblTitle.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $lblTitle.Text = "Укажите папку вашей сборки RFAB (где лежит ModOrganizer.exe или папка MO2):"
    $form.Controls.Add($lblTitle)

    $cmbPath = New-Object System.Windows.Forms.ComboBox
    $cmbPath.Location = New-Object System.Drawing.Point(18, 60)
    $cmbPath.Size = New-Object System.Drawing.Size(515, 28)
    $cmbPath.Font = New-Object System.Drawing.Font("Segoe UI", 10)
    foreach ($inst in $SortedInstalls) {
        [void]$cmbPath.Items.Add($inst.RootDir)
    }
    if (-not [string]::IsNullOrWhiteSpace($RfabDir)) {
        $cmbPath.Text = $RfabDir
    }
    $form.Controls.Add($cmbPath)

    $btnBrowse = New-Object System.Windows.Forms.Button
    $btnBrowse.Location = New-Object System.Drawing.Point(542, 58)
    $btnBrowse.Size = New-Object System.Drawing.Size(108, 30)
    $btnBrowse.Font = New-Object System.Drawing.Font("Segoe UI", 9)
    $btnBrowse.Text = "Обзор..."
    $form.Controls.Add($btnBrowse)

    $lblStatus = New-Object System.Windows.Forms.Label
    $lblStatus.Location = New-Object System.Drawing.Point(18, 102)
    $lblStatus.Size = New-Object System.Drawing.Size(632, 85)
    $lblStatus.Font = New-Object System.Drawing.Font("Segoe UI", 9)
    $form.Controls.Add($lblStatus)

    $btnOk = New-Object System.Windows.Forms.Button
    $btnOk.Location = New-Object System.Drawing.Point(370, 200)
    $btnOk.Size = New-Object System.Drawing.Size(280, 38)
    $btnOk.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
    $btnOk.Text = "Установить начисто"
    $btnOk.DialogResult = [System.Windows.Forms.DialogResult]::OK
    $form.AcceptButton = $btnOk
    $form.Controls.Add($btnOk)

    $btnCancel = New-Object System.Windows.Forms.Button
    $btnCancel.Location = New-Object System.Drawing.Point(250, 200)
    $btnCancel.Size = New-Object System.Drawing.Size(110, 38)
    $btnCancel.Font = New-Object System.Drawing.Font("Segoe UI", 9)
    $btnCancel.Text = "Отмена"
    $btnCancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    $form.CancelButton = $btnCancel
    $form.Controls.Add($btnCancel)

    $updatePreview = {
        $pInfo = Resolve-Mo2AndSkyrimPreview $cmbPath.Text
        if ($null -eq $pInfo) {
            $lblStatus.ForeColor = [System.Drawing.Color]::Firebrick
            $lblStatus.Text = "ModOrganizer.exe пока не найден по этому пути.`nНажмите «Обзор...» и укажите файл ModOrganizer.exe вашей сборки RFAB (или вставьте путь в строку выше)."
            $btnOk.Enabled = $false
        } elseif ($pInfo.IsSe1597) {
            $lblStatus.ForeColor = [System.Drawing.Color]::DarkGreen
            $packTag = if ($pInfo.IsRfab) { "Профиль Requiem / RFAB подтверждён" } else { "Mod Organizer 2 найден" }
            $lblStatus.Text = "[OK] $packTag`nMO2: $($pInfo.Mo2Exe)`nДвижок из ModOrganizer.ini: $($pInfo.SkyrimExe) (версия $($pInfo.Version))"
            $btnOk.Enabled = $true
        } else {
            $lblStatus.ForeColor = [System.Drawing.Color]::DarkOrange
            $lblStatus.Text = "MO2 найден: $($pInfo.Mo2Exe)`nВнимание: проверьте, что в ModOrganizer.ini указан SkyrimSE.exe версии 1.5.97 (сейчас: $($pInfo.Version))."
            $btnOk.Enabled = $true
        }
    }

    $cmbPath.Add_TextChanged($updatePreview)
    $cmbPath.Add_SelectedIndexChanged($updatePreview)
    $btnBrowse.Add_Click({
        $ofd = New-Object System.Windows.Forms.OpenFileDialog
        $ofd.Title = "Выберите ModOrganizer.exe вашей сборки RFAB (или откройте папку сборки)"
        $ofd.Filter = "Mod Organizer 2 (ModOrganizer.exe)|ModOrganizer.exe|Любой файл в папке сборки (*.*)|*.*"
        $ofd.CheckFileExists = $false
        $ofd.CheckPathExists = $true
        $ofd.ValidateNames = $false
        $ofd.FileName = "ModOrganizer.exe"
        if ($ofd.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
            $picked = Split-Path -Parent $ofd.FileName
            $resolved = Resolve-Mo2AndSkyrimPreview $picked
            $cmbPath.Text = if ($resolved) { $resolved.RootDir } else { $picked }
        }
    })

    & $updatePreview
    if ($form.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        throw "Установка отменена пользователем."
    }
    $RfabDir = $cmbPath.Text.Trim().Trim('"')
}

if ([string]::IsNullOrWhiteSpace($RfabDir)) {
    throw "Не удалось автоматически найти папку RFAB (ModOrganizer.exe). Укажите параметр -RfabDir."
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
    $det = Get-SkyrimExeDetails $cand
    if ($null -eq $det) { continue }
    if ($det.IsSe1597) {
        $SkyrimExe = $det.ExePath
        Write-Ok "Подтверждён SkyrimSE.exe версии $($det.Version) -> $SkyrimExe"
        break
    } elseif ([string]::IsNullOrWhiteSpace($BadVersionFound)) {
        $BadVersionFound = "$($det.Version) ($($det.ExePath))"
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
    Remove-DirectoryClean $LegacyModDir
    Write-Ok "Полностью удалена устаревшая папка мода: $LegacyModDir"
}

if ($FullModSrc -ne $FullTargetMod) {
    if (Test-Path -LiteralPath $TargetModDir) {
        Remove-DirectoryClean $TargetModDir
        Write-Ok "Полностью очищена предыдущая установка мода: $TargetModDir"
    }
    New-Item -ItemType Directory -Path $TargetModDir -Force | Out-Null
    & robocopy ($ModSourceDir.TrimEnd('\')) ($TargetModDir.TrimEnd('\')) /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "Ошибка копирования файлов мода (robocopy код $LASTEXITCODE)."
    }
}
# Очистка MO2\overwrite от возможных остатков старых версий, чтобы новая версия мода никогда не перекрывалась!
$Mo2OverwriteDir = Join-Path $Mo2BaseDir "overwrite"
if (Test-Path -LiteralPath $Mo2OverwriteDir) {
    foreach ($staleRel in @(
        "scripts\TempleBlessingScript.pex",
        "scripts\C00TrainerScript.pex",
        "scripts\ResourceFurnitureScript.pex",
        "scripts\PetFramework_ParentQuestScript.pex",
        "scripts\petframework_petquest.pex",
        "meshes\Interface\logo\logo.nif",
        "meshes\Interface\logo\logo01ae.nif",
        "SkyrimTogetherReborn\UI"
    )) {
        $stalePath = Join-Path $Mo2OverwriteDir $staleRel
        if (Test-Path -LiteralPath $stalePath) {
            Remove-Item -LiteralPath $stalePath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
Write-Ok "Чистая версия мода RFAB_Online установлена с нуля в $TargetModDir"

# 6. Активация +RFAB_Online и *SkyrimTogether.esp (+ *SkyrimTogetherQuestPatches.esp) строго ДО RFAB.esp
Write-Step "Шаг 4/7. Включение +RFAB_Online и *SkyrimTogether.esp в профиле MO2"
$HasQuestPatchesEsp = Test-Path -LiteralPath (Join-Path $TargetModDir "SkyrimTogetherQuestPatches.esp")
$ProfileDirs = @(Get-ChildItem -LiteralPath $ProfilesDir -Directory -ErrorAction SilentlyContinue | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName "modlist.txt")) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName "plugins.txt"))
})
if ($ProfileDirs.Count -eq 0) {
    throw "В '$ProfilesDir' не найдено ни одного профиля MO2 с modlist.txt и plugins.txt."
}

function Insert-StPluginsBeforeRfab([string[]]$Lines, [bool]$StarPrefix, [bool]$WithQuestPatches) {
    $stripNames = @("SkyrimTogether.esp", "*SkyrimTogether.esp", "SkyrimTogetherQuestPatches.esp", "*SkyrimTogetherQuestPatches.esp")
    $clean = New-Object System.Collections.Generic.List[string]
    foreach ($l in $Lines) {
        if ($l.Trim() -notin $stripNames) {
            $clean.Add($l)
        }
    }
    # Place strictly BEFORE RFAB.esp (or Requiem.esp) and AFTER all .esm/.esl/USSEP master plugins,
    # guaranteeing RFAB.esp wins 100% of records while SkyrimTogether gets a deterministic early slot!
    $idx = -1
    $lastMasterIdx = -1
    for ($i = 0; $i -lt $clean.Count; $i++) {
        $trimmed = $clean[$i].Trim()
        if ($trimmed.StartsWith("#") -or [string]::IsNullOrWhiteSpace($trimmed)) {
            continue
        }
        $rawName = $trimmed.TrimStart('*')
        if ($rawName.EndsWith(".esm", [System.StringComparison]::OrdinalIgnoreCase) -or
            $rawName.EndsWith(".esl", [System.StringComparison]::OrdinalIgnoreCase) -or
            ($rawName -ieq "Unofficial Skyrim Special Edition Patch.esp")) {
            $lastMasterIdx = $i
        }
        if ($idx -lt 0 -and (($rawName -ieq "RFAB.esp") -or ($rawName -ieq "Requiem.esp"))) {
            $idx = $i
        }
    }
    if ($idx -lt 0) {
        $idx = if ($lastMasterIdx -ge 0) { $lastMasterIdx + 1 } else { $clean.Count }
    } elseif ($lastMasterIdx -ge $idx) {
        $idx = $lastMasterIdx + 1
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
    $newPl = Insert-StPluginsBeforeRfab -Lines $plLines -StarPrefix $true -WithQuestPatches $HasQuestPatchesEsp
    [System.IO.File]::WriteAllLines($pluginsPath, $newPl, (New-Object System.Text.UTF8Encoding($false)))

    if (Test-Path -LiteralPath $loadOrderPath) {
        $loLines = [System.IO.File]::ReadAllLines($loadOrderPath, [System.Text.Encoding]::UTF8)
        $newLo = Insert-StPluginsBeforeRfab -Lines $loLines -StarPrefix $false -WithQuestPatches $HasQuestPatchesEsp
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
    # Guard against PowerShell single-element `@( @("Key","Val") )` array unrolling into `@("Key","Val")`!
    if ($Pairs.Count -eq 2 -and ($Pairs[0] -is [string]) -and ($Pairs[1] -is [string])) {
        $Pairs = @( , @($Pairs[0], $Pairs[1]) )
    }
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
    $updated = [regex]::Replace($raw, '(?m)^\s*(?:S\s*=\s*a|t\s*=\s*r|f\s*=\s*a)\s*\r?\n?', '')
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
            @("FormCaching", "false"),
            @("TreeLODReferenceCaching", "false"),
            @("RegularQuicksaves", "true"),
            @("CleanSkseCosaves", "true"),
            @("MaxStdio", "8192")
        )
    } else {
        $updated = Ensure-SectionKeys $updated "Patches" @(
            @("MemoryManager", "false"),
            @("ScaleformAllocator", "false"),
            @("FormCaching", "false"),
            @("TreeLODReferenceCaching", "false"),
            @("RegularQuicksaves", "true"),
            @("CleanSkseCosaves", "true"),
            @("MaxStdio", "8192")
        )
    }
    $updated = [regex]::Replace($updated, '(?mi)^\s*\[SaveGameMaxSize\]\s*$', '[Experimental]')
    # CRITICAL: EngineFixes v5/v6 PatchSaveGameMaxSize() lacks v7's MemoryManager/ScrapHeap guard!
    # Setting SaveGameMaxSize = true while MemoryManager = false forces Win32FileType to allocate 128 MB
    # from vanilla's 64 MB ScrapHeap -> nullptr -> truncated saves & "Файл сохранения поврежден"!
    $updated = Ensure-SectionKeys $updated "Experimental" @(
        , @("SaveGameMaxSize", "false")
    )
    if ($updated -ne $raw) {
        Backup-Once $ef.FullName
        [System.IO.File]::WriteAllText($ef.FullName, $updated, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "Патч EngineFixes.toml применён: $($ef.FullName)"
    } else {
        Write-Ok "EngineFixes.toml проверен: $($ef.FullName)"
    }
}

function Test-ValidSkyrimEssFile([string]$EssPath) {
    try {
        $fi = New-Object System.IO.FileInfo($EssPath)
        if (-not $fi.Exists -or $fi.Length -lt 256) { return $false }
        $fs = [System.IO.File]::Open($EssPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $magic = [System.Text.Encoding]::ASCII.GetString($br.ReadBytes(13))
            if ($magic -cne "TESV_SAVEGAME") { return $false }
            $hdrSize = [uint32]$br.ReadUInt32()
            if ($hdrSize -lt 36 -or $hdrSize -gt 65536 -or ([int64]17 + [int64]$hdrSize) -ge $fi.Length) { return $false }
            $hdr = $br.ReadBytes([int]$hdrSize)
            if ($hdr.Length -ne [int]$hdrSize) { return $false }
            $pos = 0
            $ver = [System.BitConverter]::ToUInt32($hdr, 0)
            if ($ver -lt 7 -or $ver -gt 15) { return $false }
            $pos = 8
            for ($s = 0; $s -lt 4; $s++) {
                if ($pos + 2 -gt $hdr.Length) { return $false }
                $sLen = [int][System.BitConverter]::ToUInt16($hdr, $pos)
                $pos += 2 + $sLen
                if ($pos -gt $hdr.Length) { return $false }
                if ($s -eq 0) {
                    $pos += 4
                    if ($pos -gt $hdr.Length) { return $false }
                }
            }
            $pos += 18
            if ($pos + 8 -gt $hdr.Length) { return $false }
            $ssW = [int64][System.BitConverter]::ToUInt32($hdr, $pos)
            $ssH = [int64][System.BitConverter]::ToUInt32($hdr, $pos + 4)
            $pos += 8
            if ($ssW -gt 16384 -or $ssH -gt 16384) { return $false }
            $compType = [uint16]0
            if ($ver -ge 12) {
                if ($pos + 2 -gt $hdr.Length) { return $false }
                $compType = [System.BitConverter]::ToUInt16($hdr, $pos)
                $pos += 2
                if ($compType -gt 2) { return $false }
            }
            if ($pos -ne $hdr.Length) { return $false }
            $bpp = if ($ver -ge 12) { [int64]4 } else { [int64]3 }
            $bodyOff = [int64]17 + [int64]$hdrSize + ($bpp * $ssW * $ssH)
            if ($fi.Length -le ($bodyOff + 16)) { return $false }
            if ($ver -ge 12 -and $compType -ne 0) {
                [void]$fs.Seek($bodyOff, [System.IO.SeekOrigin]::Begin)
                $uncompLen = [int64]$br.ReadUInt32()
                $compLen = [int64]$br.ReadUInt32()
                if ($uncompLen -lt 64 -or $compLen -lt 16) { return $false }
                if (($bodyOff + 8 + $compLen) -gt $fi.Length) { return $false }
            } elseif ($fi.Length -lt ($bodyOff + 64)) {
                return $false
            }
            return $true
        } finally {
            $fs.Dispose()
        }
    } catch {
        return $false
    }
}

function Quarantine-CorruptSavesInDir([string]$SavesDir) {
    if ([string]::IsNullOrWhiteSpace($SavesDir) -or -not (Test-Path -LiteralPath $SavesDir)) { return 0 }
    $moved = 0
    $qDir = Join-Path $SavesDir "Quarantined_Corrupt_v100"
    Get-ChildItem -LiteralPath $SavesDir -File -ErrorAction SilentlyContinue | ForEach-Object {
        $n = $_.Name
        $isBad = $false
        if ($n.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase) -or
            $n.EndsWith(".ess.tmp", [System.StringComparison]::OrdinalIgnoreCase)) {
            if (-not (Test-ValidSkyrimEssFile $_.FullName)) { $isBad = $true }
        }
        if ($isBad) {
            New-Item -ItemType Directory -Path $qDir -Force | Out-Null
            $dst = Join-Path $qDir $n
            Move-Item -LiteralPath $_.FullName -Destination $dst -Force -ErrorAction SilentlyContinue
            $moved++
            if ($n.EndsWith(".ess", [System.StringComparison]::OrdinalIgnoreCase)) {
                $sksePath = [System.IO.Path]::ChangeExtension($_.FullName, ".skse")
                if (Test-Path -LiteralPath $sksePath) {
                    Move-Item -LiteralPath $sksePath -Destination (Join-Path $qDir ([System.IO.Path]::GetFileName($sksePath))) -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
    return $moved
}

$TotalQuarantinedSaves = 0
$DocsSavesRoot = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Skyrim Special Edition\Saves"
$CandidateSaveDirs = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $DocsSavesRoot) {
    $CandidateSaveDirs.Add($DocsSavesRoot)
    Get-ChildItem -LiteralPath $DocsSavesRoot -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -ine "Quarantined_Corrupt_v100" -and $_.Name -ine "_RFAB_CorruptQuarantine"
    } | ForEach-Object { $CandidateSaveDirs.Add($_.FullName) }
}
if (Test-Path -LiteralPath (Join-Path $Mo2Dir "profiles")) {
    Get-ChildItem -LiteralPath (Join-Path $Mo2Dir "profiles") -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $profSaves = Join-Path $_.FullName "saves"
        if (Test-Path -LiteralPath $profSaves) { $CandidateSaveDirs.Add($profSaves) }
    }
}
foreach ($sd in ($CandidateSaveDirs | Select-Object -Unique)) {
    $TotalQuarantinedSaves += (Quarantine-CorruptSavesInDir $sd)
}
if ($TotalQuarantinedSaves -gt 0) {
    Write-Ok "Перемещено повреждённых/обрезанных .ess сохранений в карантин Quarantined_Corrupt_v100: $TotalQuarantinedSaves"
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
    Get-ChildItem -LiteralPath $cr -Filter "*Autosave.ini" -Recurse -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -ieq "[RFAB] Autosave.ini"
    }
}
foreach ($af in ($AutosaveFiles | Select-Object -Unique -Property FullName)) {
    $raw = [System.IO.File]::ReadAllText($af.FullName, [System.Text.Encoding]::UTF8)
    $updated = [regex]::Replace($raw, "(?mi)^(\s*Enabled\s*=\s*)([^\r\n;#]+)(.*)$", "`${1}false`${3}")
    foreach ($asKey in @("Enable autosave", "Save after combat", "Save on location change", "Save on sleep/wait")) {
        $updated = [regex]::Replace($updated, "(?mi)^(\s*$([regex]::Escape($asKey))\s*=\s*)([^\r\n;#]+)(.*)$", "`${1}0`${3}")
    }
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
            Remove-DirectoryClean $InstalledServerDir
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
        $TargetIniFile = Join-Path $SrvCfgDir "STServer.ini"
        [System.IO.File]::WriteAllBytes($TargetIniFile, $SavedServerIni)
        $rawSavedIni = [System.IO.File]::ReadAllText($TargetIniFile, [System.Text.Encoding]::UTF8)
        $migratedIni = $rawSavedIni
        if ($migratedIni -match '4=Master \(Requiem default\)') {
            $migratedIni = $migratedIni.Replace('4=Master (Requiem default)', '2=Adept (RFAB/Requiem 1.0x standard!), 4=Master')
            $migratedIni = [regex]::Replace($migratedIni, '(?mi)^(\s*uDifficulty\s*=\s*)4(\s*)$', '${1}2${2}')
        }
        if ($migratedIni -notmatch '(?mi)^\s*bEnableItemDrops\s*=') {
            $migratedIni = [regex]::Replace($migratedIni, '(?mi)^(\s*uDifficulty\s*=\s*[^\r\n]+\r?\n)', "`$1bEnableItemDrops = true`r`n")
        }
        if ($migratedIni -match '(?mi)^\s*bEnableMiscQuestSync\s*=\s*false') {
            $migratedIni = [regex]::Replace($migratedIni, '(?mi)^(\s*bEnableMiscQuestSync\s*=\s*)false(\s*)$', '${1}true${2}')
        } elseif ($migratedIni -notmatch '(?mi)^\s*bEnableMiscQuestSync\s*=') {
            $migratedIni = [regex]::Replace($migratedIni, '(?mi)^(\s*bEnableItemDrops\s*=\s*[^\r\n]+\r?\n)', "`$1bEnableMiscQuestSync = true`r`n")
        }
        if ($migratedIni -ne $rawSavedIni) {
            [System.IO.File]::WriteAllText($TargetIniFile, $migratedIni, (New-Object System.Text.UTF8Encoding($false)))
        }
        Write-Ok "Сохранены и обновлены настройки STServer.ini (сложность Adept 1.0x, видимые предметы на земле)"
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
    if ($seen.Add("SkyrimTogether.esp")) {
        $rfabIdx = -1
        for ($i = 0; $i -lt $outList.Count; $i++) {
            if (($outList[$i] -ieq "RFAB.esp") -or ($outList[$i] -ieq "Requiem.esp")) { $rfabIdx = $i; break }
        }
        if ($rfabIdx -ge 0) { $outList.Insert($rfabIdx, "SkyrimTogether.esp") } else { $outList.Add("SkyrimTogether.esp") }
    }
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
