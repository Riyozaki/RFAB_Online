param(
    [string]$VersionTag = "v1.0.0",
    [string]$BuildMode = "releasedbg"
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$env:PYTHONIOENCODING = "utf-8"

$script:PackagesReady = $false
$CacheMarker = Join-Path $env:LOCALAPPDATA ".xmake\packages\.rfab_cached_ok"

function Emit-Annotation([string]$Level, [string]$Title, [string]$Msg) {
    $clean = $Msg -replace "\r", "" -replace "\n", " | " -replace "%", "%25"
    if ($clean.Length -gt 3500) { $clean = $clean.Substring($clean.Length - 3500) }
    Write-Host "::${Level} title=${Title}::${clean}"
}

function Invoke-LoggedCmd([string]$StepName, [string]$CmdString) {
    Write-Host "`n==> $StepName" -ForegroundColor Cyan
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
            $errMatches = @($allLines | Where-Object { $_ -match '(?i)(error\s+[A-Z]*\d+|fatal error|unresolved external|error:|Traceback|Assertion|Exception)' })
            if ($errMatches.Count -gt 0) {
                Emit-Annotation "warning" "DIAG_MATCHES" ($errMatches -join "`n")
            }
            Emit-Annotation "warning" "DIAG_TAIL" ($tailShow -join "`n")
            Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
            throw "$StepName exited with code $code"
        }
        Remove-Item -LiteralPath $stepLog -Force -ErrorAction SilentlyContinue
    } elseif ($code -ne 0) {
        throw "$StepName exited with code $code (no log file)"
    }
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

    $NotesFile = Join-Path $RepoRoot "release_artifacts\RELEASE_NOTES.md"
    $ReleaseNotes = @"
# RFAB Online $VersionTag — Чистый автономный релиз (Skyrim SE ``1.5.97.0.8``)

Полностью собранный с нуля на чистой облачной машине ``Windows x64`` автономный установщик **RFAB Online** (клиент для **Mod Organizer 2** + выделенный кооп-сервер).
Никаких консольных команд, ``git``, ``xmake`` или ручного переименования/копирования файлов **не требуется**!

## 📦 Состав релиза
| Файл | Назначение |
|---|---|
| ``RFAB_Online_${VersionTag}_Full_Installer.zip`` | **Основной архив**: полная чистая установка клиента + сервера (GUI-установщик ``INSTALL_RFAB_ONLINE.bat``) |
| ``RFAB_Online_${VersionTag}_Quick_Update.zip`` | **Быстрое обновление** для тех, кто уже играл (клиент + сервер + ``UPDATE_RFAB_ONLINE.bat``, ≈15 МБ) |
| ``RFAB_Online_${VersionTag}_PDB_Symbols.zip`` | Отладочные символы (``.pdb``) для разработчиков и диагностики крашей |
| ``SHA256SUMS.txt`` | Контрольные суммы ``SHA-256`` всех архивов выше |

## 🛠 Установка в 2 клика (одинаково и Хосту, и Друзьям)
1. Скачайте **``RFAB_Online_${VersionTag}_Full_Installer.zip``** и распакуйте его в любую временную папку.
2. Закройте игру и **Mod Organizer 2**, затем дважды нажмите на **``INSTALL_RFAB_ONLINE.bat``**:
   - Откроется удобное окно выбора папки сборки RFAB (автоматически найдёт ``ModOrganizer.exe`` на ваших дисках и покажет точный путь к ``SkyrimSE.exe 1.5.97`` прямо из ``ModOrganizer.ini``, либо позволит выбрать любую папку кнопкой **«Обзор...»**).
   - Установщик **начисто удаляет** любые старые файлы прошлых тестов в ``MO2\mods\RFAB_Online`` и ``RFAB_Online_Server`` (никакого наслаивания на старые версии!).
   - Ставит готовый нативный лаунчер ``SkyrimSE.exe``, включает ``+RFAB_Online``, ``*SkyrimTogether.esp`` и ``*SkyrimTogetherQuestPatches.esp`` строго перед ``Requiem.esp``, настраивает ``EngineFixes.toml``, ``SkyrimSoulsRE.ini``, ``[RFAB] Autosave.ini``, скрывает конфликтующий ``BetterStealing.dll`` и добавляет кнопку **``RFAB Online - Play``** в MO2.
   - Ставит автономный сервер прямо в ``<Папка_RFAB>\RFAB_Online_Server`` и создаёт ярлык **``START_RFAB_SERVER.bat``** (после установки скачанный ZIP-архив можно спокойно удалить).

> 💡 **Уже играли в прошлую версию?** Скачайте ``RFAB_Online_${VersionTag}_Quick_Update.zip``, распакуйте и запустите **``UPDATE_RFAB_ONLINE.bat``** — клиент и сервер обновятся без полной переустановки.

## 🎮 Как запустить и играть
1. **[Только Хост]** Запустите ``<Папка_RFAB>\START_RFAB_SERVER.bat`` (в окне сразу отобразится IP-адрес для друзей, например из **Radmin VPN**).
2. **[И Хост, и Друзья]** Откройте **Mod Organizer 2** -> выберите **``RFAB Online - Play``** -> нажмите **«Запустить»**.
3. В игре загрузите сохранение в открытом мире -> нажмите **Правый CTRL** (или **F2**) -> **Подключиться (``Connect``)**:
   - **Хост вводит:** ``127.0.0.1:10578``
   - **Друзья вводят:** ``<IP_хоста>:10578``
   - Все игроки автоматически добавляются в общую группу!

| Клавиша | Действие |
|---|---|
| **Правый CTRL** / **F2** | Открыть оверлей (подключение, сессия, отладка) |
| **F5** | Сохранить игру для **всей группы** (любой игрок) |
| **F9** | Загрузить последний групповой сейв (**только Хост**) |
| **E** (у раненого союзника) | Поднять игрока с колена (≈4 с) |

## ⚖️ Правила кооператива
- **Опыт** — равными долями всем игрокам группы (мобы, боссы, квесты, книги опыта).
- **Враги** — один комплект на группу (засады **не** дублируются), HP масштабируется: ``2`` игрока = ``×3 HP``, ``3`` игрока = ``×4 HP``.
- **Смерть** — ``0 HP`` = «Downed-on-Knee» до конца боя или до поднятия союзником; честная победа поднимает павших с ≈``30% HP``; Total Party Wipe откатывает последний чекпоинт.
- **Сохранения** — ``F5`` у любого игрока сохраняет группу, ``F9``/загрузка — только Хост, автоматический чекпоинт после боя и авто-переподключение после загрузки.

## 🩹 Что исправлено по итогам плейтеста
1. HP уходил в минус без анимации смерти -> единый C++-хук ``RestoreActorValue`` + система ``Downed-on-Knee``.
2. Нестабильный опыт за убийства -> серверный журнал XP и двойной пул: теперь ``490/490`` паритет у всех участников.
3. Краш ``0xC0000005`` при загрузке старого сейва и ``F9`` -> UAF динамических NPC при откате ``BGSSaveLoadGame`` исправлен очисткой/восстановлением акторов вокруг каждого save/load.
4. «Голые» NPC у подключившегося друга -> рекурсивный резолвер экипировки из уровневых списков + досинхронизация инвентаря.
5. Путаница синхронизации сейвов -> единые правила ``host_sync_plus_postcombat_auto`` (см. выше).

---
Полная документация и исходники: **github.com/Riyozaki/RFAB_Online**
"@
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
        Invoke-LoggedCmd "[7/7] Update GitHub Release Metadata $VersionTag" `
            "gh release edit $VersionTag --title `"$RelTitle`" --notes-file `"$NotesFile`" --latest"
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
    Emit-Annotation "notice" "RELEASE_READY" "Published https://github.com/Riyozaki/RFAB_Online/releases/tag/$VersionTag"
    $global:LASTEXITCODE = 0
    exit 0
} catch {
    $errMsg = $_.Exception.Message
    Emit-Annotation "error" "CI_EXCEPTION" $errMsg
    exit 1
}
