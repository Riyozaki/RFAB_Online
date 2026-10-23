param(
    [string]$VersionTag = "v1.0.0",
    [string]$BuildMode = "releasedbg"
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$TranscriptPath = Join-Path $env:TEMP "rfab_cloud_build_transcript.txt"
Start-Transcript -Path $TranscriptPath -Force | Out-Null

function Emit-StepNotice([string]$Msg) {
    Write-Host "::notice title=RFAB_BUILD::$Msg"
}

function Invoke-NativeStep([string]$StepName, [scriptblock]$CmdBlock) {
    Write-Host "`n==> $StepName" -ForegroundColor Cyan
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $CmdBlock
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) {
        throw "$StepName failed with exit code $code"
    }
    Emit-StepNotice "OK: $StepName"
}

try {
    $RepoRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "..\.."))
    Set-Location -LiteralPath $RepoRoot

    Invoke-NativeStep "[1/7] Submodules init" {
        & git submodule sync --recursive
        if ($LASTEXITCODE -ne 0) { throw "git submodule sync failed ($LASTEXITCODE)" }
        & git submodule update --init --force --recursive --depth=1
    }

    Invoke-NativeStep "[2/7] SE 1.5.97 RVA & Struct Audit" {
        & python "Tools\rfab\verify_se1597_port.py"
    }

    Invoke-NativeStep "[3/7] xmake config (Windows x64 /MT $BuildMode)" {
        & xmake config --plat=windows --arch=x64 --mode=$BuildMode --yes -vD
    }

    Invoke-NativeStep "[4/7] xmake build (Client + Launcher + Server + Tests)" {
        & xmake -y
    }

    $TpTests = Get-ChildItem -LiteralPath (Join-Path $RepoRoot "build\windows\x64\$BuildMode") -Filter "TPTests.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $TpTests) {
        throw "TPTests.exe not found under build\windows\x64\$BuildMode!"
    }
    Invoke-NativeStep "[5/7] Native Windows x64 Unit Tests (TPTests.exe)" {
        & $TpTests.FullName
    }

    Invoke-NativeStep "[6/7] xmake install + Package-Release.ps1 + Smoke & Clean-Wipe Verification" {
        & xmake install -o distrib
        if ($LASTEXITCODE -ne 0) { throw "xmake install failed ($LASTEXITCODE)" }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "Tools\rfab\Package-Release.ps1") -VersionTag $VersionTag -BuildMode $BuildMode
    }

    $NotesFile = Join-Path $RepoRoot "release_artifacts\RELEASE_NOTES.md"
    $ReleaseNotes = @"
# RFAB Online $VersionTag — Чистый автономный релиз (Skyrim SE ``1.5.97.0.8``)

Полностью собранный с нуля на облачной машине ``Windows x64`` автономный установщик **RFAB Online** (клиент для **Mod Organizer 2** + выделенный кооп-сервер).
Никаких консольных команд, ``git``, ``xmake`` или ручного переименования/копирования файлов **не требуется**!

---

## Установка в 2 клика (одинаково и Хосту, и Друзьям)
1. Скачайте один архив ниже: **``RFAB_Online_${VersionTag}_Full_Installer.zip``** и распакуйте его в любую временную папку.
2. Закройте игру и **Mod Organizer 2**, затем дважды нажмите на **``INSTALL_RFAB_ONLINE.bat``**:
   - Откроется удобное окно выбора папки сборки RFAB (автоматически найдёт ``ModOrganizer.exe`` на ваших дисках и покажет найденный ``SkyrimSE.exe 1.5.97`` прямо из ``ModOrganizer.ini``, либо даст выбрать любую папку кнопкой **«Обзор...»**).
   - Установщик **начисто удаляет** любые старые файлы прошлых тестов в ``MO2\mods\RFAB_Online`` и ``RFAB_Online_Server`` (никакого наслаивания на старые версии!).
   - Ставит готовый нативный лаунчер ``SkyrimSE.exe``, включает ``+RFAB_Online``, ``*SkyrimTogether.esp`` и ``*SkyrimTogetherQuestPatches.esp`` строго перед ``Requiem.esp``, настраивает ``EngineFixes.toml``, ``SkyrimSoulsRE.ini``, ``[RFAB] Autosave.ini``, скрывает конфликтующий ``BetterStealing.dll`` и добавляет кнопку **``RFAB Online - Play``** в MO2.
   - Ставит автономный сервер прямо в ``<Папка_RFAB>\RFAB_Online_Server`` и создаёт ярлык **``START_RFAB_SERVER.bat``** (после установки скачанный ZIP-архив можно спокойно удалить).

## Как запустить и играть
1. **[Только Хост]** Запустите ``<Папка_RFAB>\START_RFAB_SERVER.bat`` (в окне сразу отобразится IP-адрес для друзей, например из **Radmin VPN**).
2. **[И Хост, и Друзья]** Откройте **Mod Organizer 2** -> выберите **``RFAB Online - Play``** -> нажмите **«Запустить»**.
3. В игре загрузите сохранение в открытом мире -> нажмите **Правый CTRL** (или **F2**) -> **Подключиться (``Connect``)**:
   - **Хост вводит:** ``127.0.0.1:10578``
   - **Друзья вводят:** ``<IP_хоста>:10578``
   - Все игроки автоматически добавляются в общую группу!
"@
    [System.IO.File]::WriteAllText($NotesFile, $ReleaseNotes, (New-Object System.Text.UTF8Encoding($false)))

    $CommitSha    = (& git rev-parse HEAD).Trim()
    $InstallerZip = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_Full_Installer.zip"
    $PdbZip       = Join-Path $RepoRoot "release_artifacts\RFAB_Online_${VersionTag}_PDB_Symbols.zip"
    $ShaFile      = Join-Path $RepoRoot "release_artifacts\SHA256SUMS.txt"
    $RelTitle     = "RFAB Online $VersionTag - Full Clean Standalone Installer (Skyrim SE 1.5.97.0.8)"

    Invoke-NativeStep "[7/7] Publish GitHub Release $VersionTag" {
        & cmd.exe /c "gh release view $VersionTag >nul 2>&1"
        if ($LASTEXITCODE -eq 0) {
            & gh release edit $VersionTag --title $RelTitle --notes-file $NotesFile --latest
            if ($LASTEXITCODE -ne 0) { throw "gh release edit failed ($LASTEXITCODE)" }
            & gh release upload $VersionTag $InstallerZip $PdbZip $ShaFile --clobber
            if ($LASTEXITCODE -ne 0) { throw "gh release upload failed ($LASTEXITCODE)" }
        } else {
            & gh release create $VersionTag $InstallerZip $PdbZip $ShaFile --target $CommitSha --title $RelTitle --notes-file $NotesFile --latest
            if ($LASTEXITCODE -ne 0) { throw "gh release create failed ($LASTEXITCODE)" }
        }
        & cmd.exe /c "gh release view v1.0.0-rfab >nul 2>&1"
        if ($LASTEXITCODE -eq 0) {
            & gh release delete "v1.0.0-rfab" --yes --cleanup-tag
        }
        $global:LASTEXITCODE = 0
    }

    Stop-Transcript | Out-Null
    Write-Host "`n[ALL 7 STEPS SUCCEEDED] Published $VersionTag!" -ForegroundColor Green
    $global:LASTEXITCODE = 0
    exit 0
} catch {
    $errMsg = $_.Exception.Message
    Stop-Transcript -ErrorAction SilentlyContinue | Out-Null
    Write-Host "::error title=CI_EXCEPTION::$errMsg"
    if (Test-Path -LiteralPath $TranscriptPath) {
        $tailLines = Get-Content -LiteralPath $TranscriptPath -Tail 120
        $joined = ($tailLines -join " | ")
        if ($joined.Length -gt 3500) { $joined = $joined.Substring($joined.Length - 3500) }
        Write-Host "::error title=TRANSCRIPT_TAIL::$joined"
    }
    exit 1
}
