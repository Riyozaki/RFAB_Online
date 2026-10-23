param(
    [string]$RfabDir = "",
    [switch]$SkipBuild,
    [switch]$SkipFriendArchive,
    [switch]$NonInteractive
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Write-Step([string]$Msg) { Write-Host "`n==> $Msg" -ForegroundColor Cyan }
function Write-Ok([string]$Msg)   { Write-Host "  [OK] $Msg" -ForegroundColor Green }
function Write-Warn([string]$Msg) { Write-Host "  [!]  $Msg" -ForegroundColor Yellow }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "..\.."))
Set-Location -LiteralPath $RepoRoot

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "  RFAB Online (SE 1.5.97.0.8) - Полная сборка и автодеплой    " -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

# 1. Сборка C++ клиента (SkyrimSE.exe + EarlyLoad.dll + TPProcess.exe) и сервера (SkyrimTogetherServer.exe + STServer.dll)
$InstallDir = Join-Path $RepoRoot "build\package"
if (-not $SkipBuild) {
    Write-Step "Шаг 1/4. Компиляция клиента и сервера (xmake -y -> единый BUILD_COMMIT)"
    $XmakeCmd = Get-Command "xmake.exe" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
    if (-not $XmakeCmd) {
        foreach ($candXmake in @(
            "$env:USERPROFILE\xmake\xmake.exe",
            "$env:LOCALAPPDATA\xmake\xmake.exe",
            "C:\Program Files\xmake\xmake.exe",
            "C:\xmake\xmake.exe"
        )) {
            if (Test-Path -LiteralPath $candXmake) { $XmakeCmd = $candXmake; break }
        }
    }
    if (-not $XmakeCmd) {
        throw "Не найден xmake.exe! Установите xmake или добавьте его в PATH."
    }
    Write-Ok "Используется xmake: $XmakeCmd"

    & $XmakeCmd -y
    if ($LASTEXITCODE -ne 0) {
        throw "Ошибка сборки xmake -y (код выхода $LASTEXITCODE)."
    }

    if (Test-Path -LiteralPath $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    & $XmakeCmd install -o $InstallDir
    if ($LASTEXITCODE -ne 0) {
        Write-Warn "xmake install вернул код $LASTEXITCODE — продолжаем прямое копирование артефактов из build\windows\x64\..."
    }
    Write-Ok "Сборка C++ успешно завершена!"
}

# 2. Сбор всех свежесобранных бинарников в dist_ready\RFAB_Online_MO2 и dist_ready\RFAB_Online_Server
Write-Step "Шаг 2/4. Обновление dist_ready (нативный SkyrimSE.exe + UI + Сервер)"
$DistMo2 = Join-Path $RepoRoot "dist_ready\RFAB_Online_MO2"
$DistStr = Join-Path $DistMo2 "SkyrimTogetherReborn"
$DistSrv = Join-Path $RepoRoot "dist_ready\RFAB_Online_Server"
$DistPdb = Join-Path $RepoRoot "dist_ready\DebugSymbols_PDB"

New-Item -ItemType Directory -Path $DistStr -Force | Out-Null
New-Item -ItemType Directory -Path $DistSrv -Force | Out-Null
New-Item -ItemType Directory -Path $DistPdb -Force | Out-Null

# Если в уже установленном MO2 есть живые LFS-ассеты (libcef.dll, SkyrimTogether.esp, .pex), подтягиваем их при необходимости
$FallbackInstalledMod = ""
foreach ($probeMo2Mod in @(
    "G:\RFAB Online SkyrimSE\MO2\mods\RFAB_Online",
    (Join-Path $RfabDir "MO2\mods\RFAB_Online"),
    (Join-Path $RfabDir "mods\RFAB_Online")
)) {
    if (-not [string]::IsNullOrWhiteSpace($probeMo2Mod) -and (Test-Path -LiteralPath $probeMo2Mod)) {
        $FallbackInstalledMod = $probeMo2Mod
        break
    }
}
if ($FallbackInstalledMod) {
    Get-ChildItem -LiteralPath $DistMo2 -File -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Length -le 200 } | ForEach-Object {
        $relPath = $_.FullName.Substring($DistMo2.Length).TrimStart('\', '/')
        $candidateLive = Join-Path $FallbackInstalledMod $relPath
        if ((Test-Path -LiteralPath $candidateLive) -and ((Get-Item -LiteralPath $candidateLive).Length -gt 200)) {
            Copy-Item -LiteralPath $candidateLive -Destination $_.FullName -Force
        }
    }
}

# Копируем CEF/Discord/UI файлы из xmake install (если есть)
$PkgBin = Join-Path $InstallDir "bin"
if (Test-Path -LiteralPath $PkgBin) {
    & robocopy ($PkgBin.TrimEnd('\')) ($DistStr.TrimEnd('\')) /E /NFL /NDL /NJH /NJS /NP /XF "*Server*" /R:2 /W:1 | Out-Null
}

function Find-NewestBuiltArtifact([string]$FileName) {
    $searchDirs = @(
        (Join-Path $RepoRoot "build\windows\x64"),
        $PkgBin
    ) | Where-Object { Test-Path -LiteralPath $_ }
    $hits = foreach ($sd in $searchDirs) {
        Get-ChildItem -LiteralPath $sd -Filter $FileName -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 200 }
    }
    return ($hits | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1)
}

# Нативный SkyrimSE.exe + совместимая копия SkyrimTogether.exe
$BuiltLauncher = Find-NewestBuiltArtifact "SkyrimSE.exe"
if (-not $BuiltLauncher) { $BuiltLauncher = Find-NewestBuiltArtifact "SkyrimTogether.exe" }
if (-not $BuiltLauncher) { throw "Не найден собранный лаунчер SkyrimSE.exe в папке build\windows\x64!" }

Copy-Item -LiteralPath $BuiltLauncher.FullName -Destination (Join-Path $DistStr "SkyrimSE.exe") -Force
Copy-Item -LiteralPath $BuiltLauncher.FullName -Destination (Join-Path $DistStr "SkyrimTogether.exe") -Force
Write-Ok "Упакован нативный лаунчер: $($BuiltLauncher.FullName) -> SkyrimSE.exe (+ SkyrimTogether.exe)"

foreach ($clientBin in @("EarlyLoad.dll", "TPProcess.exe")) {
    $hit = Find-NewestBuiltArtifact $clientBin
    if (-not $hit) { throw "Не найден собранный бинарник клиента: $clientBin" }
    Copy-Item -LiteralPath $hit.FullName -Destination (Join-Path $DistStr $clientBin) -Force
    Write-Ok "Упакован клиентский модуль: $clientBin"
}

foreach ($serverBin in @("SkyrimTogetherServer.exe", "STServer.dll")) {
    $hit = Find-NewestBuiltArtifact $serverBin
    if (-not $hit) { throw "Не найден собранный бинарник сервера: $serverBin" }
    Copy-Item -LiteralPath $hit.FullName -Destination (Join-Path $DistSrv $serverBin) -Force
    Write-Ok "Упакован серверный модуль: $serverBin"
}

foreach ($pdbName in @("SkyrimSE.pdb", "SkyrimTogether.pdb", "EarlyLoad.pdb", "TPProcess.pdb", "SkyrimTogetherServer.pdb", "STServer.pdb")) {
    $pdbHit = Find-NewestBuiltArtifact $pdbName
    if ($pdbHit) {
        Copy-Item -LiteralPath $pdbHit.FullName -Destination (Join-Path $DistPdb $pdbName) -Force
    }
}

# Гарантируем свежие Data\SKSE\Plugins\RFAB_Online_Bindings.json и собранный UI
Copy-Item -LiteralPath (Join-Path $RepoRoot "Data\SKSE\Plugins\RFAB_Online_Bindings.json") -Destination (Join-Path $DistMo2 "SKSE\Plugins\RFAB_Online_Bindings.json") -Force
if (Test-Path -LiteralPath (Join-Path $RepoRoot "Code\skyrim_ui\dist\UI\index.html")) {
    & robocopy (Join-Path $RepoRoot "Code\skyrim_ui\dist\UI") (Join-Path $DistStr "UI") /MIR /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
    Write-Ok "Обновлён оверлей UI из Code\skyrim_ui\dist\UI"
}

# 3. Автоматическая установка в сборку RFAB + синхронизация сервера + упаковка архива для друзей
Write-Step "Шаг 3/4. Запуск автоустановщика Setup-RfabOnline.ps1"
$SetupArgs = @{
    ModSourceDir    = $DistMo2
    ServerSourceDir = $DistSrv
}
if (-not [string]::IsNullOrWhiteSpace($RfabDir)) { $SetupArgs["RfabDir"] = $RfabDir }
if ($SkipFriendArchive) { $SetupArgs["SkipFriendArchive"] = $true }
if ($NonInteractive)    { $SetupArgs["NonInteractive"]    = $true }

& (Join-Path $ScriptDir "Setup-RfabOnline.ps1") @SetupArgs
