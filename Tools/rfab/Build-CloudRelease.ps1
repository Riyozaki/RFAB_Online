param(
    [string]$VersionTag = "v1.0.0",
    [string]$BuildMode = "releasedbg"
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."))
Set-Location -LiteralPath $RepoRoot

Write-Host "==============================================================" -ForegroundColor Cyan
Write-Host "  RFAB Online $VersionTag — Clean Cloud Windows x64 Build     " -ForegroundColor Cyan
Write-Host "==============================================================" -ForegroundColor Cyan

Write-Host "`n==> [Step 1/7] Initializing Git Submodules..." -ForegroundColor Cyan
& git submodule sync --recursive
if ($LASTEXITCODE -ne 0) { throw "git submodule sync failed ($LASTEXITCODE)" }
& git submodule update --init --force --recursive --depth=1
if ($LASTEXITCODE -ne 0) { throw "git submodule update failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 2/7] Verifying Skyrim SE 1.5.97 RVA & Struct Invariants..." -ForegroundColor Cyan
& python "Tools\rfab\verify_se1597_port.py"
if ($LASTEXITCODE -ne 0) { throw "verify_se1597_port.py failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 3/7] Configuring xmake (Windows x64 /MT $BuildMode)..." -ForegroundColor Cyan
& xmake config --plat=windows --arch=x64 --mode=$BuildMode --yes -vD
if ($LASTEXITCODE -ne 0) { throw "xmake config failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 4/7] Compiling Full Client, Server & Test Suite from Scratch..." -ForegroundColor Cyan
& xmake -y
if ($LASTEXITCODE -ne 0) { throw "xmake build failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 5/7] Running Native Windows x64 Unit Tests (TPTests.exe)..." -ForegroundColor Cyan
$TpTests = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "build\windows\x64\$BuildMode") -Filter "TPTests.exe" -Recurse | Select-Object -First 1
if (-not $TpTests) { throw "TPTests.exe not found under build\windows\x64\$BuildMode!" }
& $TpTests.FullName
if ($LASTEXITCODE -ne 0) { throw "TPTests.exe failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 6/7] Installing Runtime Distribution & Building Verified ZIP Bundle..." -ForegroundColor Cyan
& xmake install -o distrib
if ($LASTEXITCODE -ne 0) { throw "xmake install failed ($LASTEXITCODE)" }

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "Tools\rfab\Package-Release.ps1") -VersionTag $VersionTag -BuildMode $BuildMode
if ($LASTEXITCODE -ne 0) { throw "Package-Release.ps1 failed ($LASTEXITCODE)" }

Write-Host "`n==> [Step 7/7] Publishing Official Clean Release $VersionTag to GitHub Releases..." -ForegroundColor Cyan
$NotesFile = Join-Path $RepoRoot "release_artifacts\RELEASE_NOTES.md"
$ReleaseNotes = @"
# RFAB Online $VersionTag — Чистый автономный релиз (Skyrim SE ``1.5.97.0.8``)

Полностью собранный с нуля на чистой облачной машине ``Windows x64`` автономный установщик **RFAB Online** (клиент для **Mod Organizer 2** + выделенный кооп-сервер).
Никаких консольных команд, ``git``, ``xmake`` или ручного переименования/копирования файлов **не требуется**!

---

## Установка в 2 клика (одинаково и Хосту, и Друзьям)
1. Скачайте один архив ниже: **``RFAB_Online_${VersionTag}_Full_Installer.zip``** и распакуйте его в любую временную папку.
2. Закройте игру и **Mod Organizer 2**, затем дважды нажмите на **``INSTALL_RFAB_ONLINE.bat``**:
   - Установщик **начисто удаляет** любые старые файлы прошлых тестов в ``MO2\mods\RFAB_Online`` и ``RFAB_Online_Server`` (никакого наслаивания на старые версии!).
   - Автоматически находит сборку **RFAB (``SkyrimSE.exe 1.5.97``)**, устанавливает готовый нативный лаунчер ``SkyrimSE.exe``, активирует ``+RFAB_Online``, ``*SkyrimTogether.esp`` и ``*SkyrimTogetherQuestPatches.esp`` в правильном порядке до ``Requiem.esp``, настраивает ``EngineFixes.toml``, ``SkyrimSoulsRE.ini``, ``[RFAB] Autosave.ini``, скрывает конфликтующий ``BetterStealing.dll`` и создаёт кнопку **``RFAB Online - Play``** в MO2.
   - Устанавливает автономный сервер прямо в ``<Папка_RFAB>\RFAB_Online_Server`` и создаёт ярлык **``START_RFAB_SERVER.bat``** (после установки скачанный ZIP-архив можно спокойно удалить).

## Как запустить и играть
1. **[Только Хост]** Запустите ``<Папка_RFAB>\START_RFAB_SERVER.bat`` (в окне сразу отобразится IP-адрес для друзей, например из **Radmin VPN**).
2. **[И Хост, и Друзья]** Откройте **Mod Organizer 2** -> выберите **``RFAB Online - Play``** -> нажмите **«Запустить»**.
3. В игре загрузите сохранение в открытом мире -> нажмите **Правый CTRL** (или **F2**) -> **Подключиться (``Connect``)**:
   - **Хост вводит:** ``127.0.0.1:10578``
   - **Друзья вводят:** ``<IP_хоста>:10578``
   - Все игроки автоматически добавляются в общую группу!

---

## Что внутри сборки ``$VersionTag``
- **Строго под движок Skyrim SE ``1.5.97.0.8``:** проверены все ``3 099`` ID Address Library, размеры и смещения C++-структур ``1.5.97`` и нативные заголовки ``HEDR 1.70`` в ``SkyrimTogether.esp`` + ``SkyrimTogetherQuestPatches.esp``.
- **Статическая линковка ``/MT`` + ``--do-not-de-elevate`` в CEF:** не требует обновления ``VC_redist.x64`` у друзей и не перезапускает ванильный ``SkyrimSE.exe`` при запуске от имени Администратора.
- **Кооп-баланс RFAB без дублирования врагов (``1x``):** враги не размножаются на число игроков; масштабируется их здоровье (``1`` игрок = ``x1 HP``, ``2`` игрока = ``x3 HP``, ``3`` игрока = ``x4 HP``, настраивается в ``STServer.ini``).
- **Равный опыт (XP) всей группе**, **ранение на колено при ``0 HP`` до конца боя** (поднятие на ``E`` / автоподъём после победы / общий откат при гибели всей группы) и **умные координированные сохранения хоста (``F5`` + автосейв после боя)**.
"@
[System.IO.File]::WriteAllText($NotesFile, $ReleaseNotes, (New-Object System.Text.UTF8Encoding($false)))

$CommitSha = (& git rev-parse HEAD).Trim()
$InstallerZip = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Full_Installer.zip"
$PdbZip       = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_PDB_Symbols.zip"
$ShaFile      = Join-Path $RepoRoot "release_artifacts\SHA256SUMS.txt"
$RelTitle     = "RFAB Online $VersionTag — Чистый автономный установщик (Skyrim SE 1.5.97.0.8)"

& gh release view $VersionTag 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    & gh release edit $VersionTag --title $RelTitle --notes-file $NotesFile --latest
    & gh release upload $VersionTag $InstallerZip $PdbZip $ShaFile --clobber
    if ($LASTEXITCODE -ne 0) { throw "gh release upload failed ($LASTEXITCODE)" }
} else {
    & gh release create $VersionTag $InstallerZip $PdbZip $ShaFile --target $CommitSha --title $RelTitle --notes-file $NotesFile --latest
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
}

# Remove obsolete pre-fix test release v1.0.0-rfab if present so Releases page stays spotless
& gh release view "v1.0.0-rfab" 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) {
    & gh release delete "v1.0.0-rfab" --yes --cleanup-tag
}

Write-Host "`n[PUBLISHED] https://github.com/Riyozaki/RFAB_Online/releases/tag/$VersionTag" -ForegroundColor Green
$global:LASTEXITCODE = 0
exit 0
