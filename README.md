# RFAB Online

<div align="center">

### 🎮 Кооперативный мультиплеер для Skyrim SE `1.5.97.0.8` + [Requiem for a Balance (RFAB)](https://github.com/drake1ch/RFAB)

[![Build & Release](https://github.com/Riyozaki/RFAB_Online/actions/workflows/release.yml/badge.svg)](https://github.com/Riyozaki/RFAB_Online/actions/workflows/release.yml)
[![Latest Release](https://img.shields.io/github/v/release/Riyozaki/RFAB_Online?label=актуальный%20релиз&color=brightgreen)](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7)
[![Game Version](https://img.shields.io/badge/Skyrim%20SE-1.5.97.0.8-2a2a3a)](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7)
[![SKSE64](https://img.shields.io/badge/SKSE64-2.0.20-blue)](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7)

*Проходите Requiem for a Balance вдвоём–вчетвером без потери механик RFAB: собственные сложности («Странник», «Довакин», «Легенда», «Проклятый»), режим «Единство в битве» без урона по союзникам, общие квесты и опыт, «Downed-on-Knee» вместо мгновенной смерти и синхронные сохранения всей группы.*

[**📥 Скачать v3.1.7**](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7) · [**⚡ Установка и обновление**](#-быстрый-старт-и-обновление) · [**🎮 Управление (Хост и Гости)**](#-управление-и-права-хоста) · [**📋 Changelog**](./CHANGELOG.md) · [**📚 Документация**](#-документация-и-инженерные-артефакты)

</div>

---

## 🔥 О проекте

**RFAB Online** — готовый к игре кооперативный мультиплеер «из коробки», специально адаптированный под модпак **[Skyrim Requiem for a Balance (RFAB)](https://github.com/drake1ch/RFAB)** (включая **RFAB Respect Edition** и любые обновлённые версии RFAB) и движок **Skyrim Special Edition `1.5.97.0.8`** (`SKSE64 2.0.20`, `version-1-5-97-0.bin`).

- **0 перезаписей баланса и контента RFAB:** плагины коопа полностью очищены от ванильных оверрайдов (всего 5 собственных форм в `SkyrimTogether.esp`), сохраняя 100% перков, алтарей, заклинаний, колец, призывов, камер и интерфейса RFAB при любом порядке загрузки (в том числе когда `RFAB.esp` помечен ESM-флагом на слоте `07` в **RFAB Respect Edition**) за счёт встроенного бинарного индексатора `.esm/.esp/.esl` (`EspEdidIndex`).
- **Установка и обновление в 2 клика:** готовый нативный лаунчер `SkyrimSE.exe`, клиент под **Mod Organizer 2**, русский оверлей и выделенный сервер ставятся автоматически без `git`, консольных команд и ручного копирования файлов.
- **Чёткая политика версий:** в разделе [Releases](https://github.com/Riyozaki/RFAB_Online/releases) доступен актуальный релиз **[`v3.1.7` (`Latest`)](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7)** и неприкосновенные архивные бэкапы всех стабильных веток (**[`v2.0.2-backup`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v2.0.2-backup)**, **[`v2.0.2`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v2.0.2)**, **[`v2.0.1-backup`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v2.0.1-backup)**, **[`v2.0.1`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v2.0.1)**, **[`v1.2.0`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v1.2.0)**, **[`v1.1.0`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v1.1.0)**, **[`v1.0.0`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v1.0.0)**).

---

## 📦 Быстрый старт и обновление

| Архив в релизе [`v3.1.7` (`Latest`)](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7) | Назначение |
|---|---|
| **[`RFAB_Online_v3.1.7_Full_Installer.zip`](https://github.com/Riyozaki/RFAB_Online/releases/download/v3.1.7/RFAB_Online_v3.1.7_Full_Installer.zip)** | **Полная установка с нуля или чистое обновление** (`INSTALL_RFAB_ONLINE.bat`): клиент MO2 + UI + выделенный сервер + автонастройка конфигов |
| **[`RFAB_Online_v3.1.7_Quick_Update.zip`](https://github.com/Riyozaki/RFAB_Online/releases/download/v3.1.7/RFAB_Online_v3.1.7_Quick_Update.zip)** | **Быстрое обновление за 10 секунд** (`UPDATE_RFAB_ONLINE.bat`, ≈15 МБ): обновляет все `.exe`/`.dll`/`.esp`, `UI` и сервер поверх установленного мода |
| **[`RFAB_Online_v3.1.7_PDB_Symbols.zip`](https://github.com/Riyozaki/RFAB_Online/releases/download/v3.1.7/RFAB_Online_v3.1.7_PDB_Symbols.zip)** | Отладочные символы `.pdb` и [`SHA256SUMS.txt`](https://github.com/Riyozaki/RFAB_Online/releases/download/v3.1.7/SHA256SUMS.txt) |

### 1. Чистая установка (одинаково для Хоста и Друзей)

1. Скачайте **[`RFAB_Online_v3.1.7_Full_Installer.zip`](https://github.com/Riyozaki/RFAB_Online/releases/tag/v3.1.7)** и распакуйте архив в любую временную папку.
2. Закройте игру и **Mod Organizer 2**, затем запустите **`INSTALL_RFAB_ONLINE.bat`**:
   - откроется удобное окно выбора папки сборки RFAB (автоматически находит `ModOrganizer.exe` на дисках и считывает точный путь к `SkyrimSE.exe 1.5.97` из `ModOrganizer.ini`);
   - установщик автоматически закрывает фоновый сервер, **полностью удаляет любые старые файлы прошлых версий** в `MO2\mods\RFAB_Online` и `RFAB_Online_Server` (сохраняя ваши настройки `STServer.ini`), очищает устаревшие хвосты в `MO2\overwrite` и ставит новую версию начисто;
   - активирует `+RFAB_Online`, `*SkyrimTogether.esp` и `*SkyrimTogetherQuestPatches.esp` строго **до `RFAB.esp`**, настраивает `EngineFixes.toml`, `SkyrimSoulsRE.ini`, `[RFAB] Autosave.ini`, скрывает конфликтующий `BetterStealing.dll` и создаёт профиль запуска **`RFAB Online - Play`** в MO2;
   - устанавливает выделенный сервер в `<Папка_RFAB>\RFAB_Online_Server` и создаёт ярлык **`START_RFAB_SERVER.bat`**.

### 2. Легкое обновление с прошлой версии

Перейти на свежую сборку можно **любым из двух способов** — никакого ручного удаления папок не требуется:
- **Через `INSTALL_RFAB_ONLINE.bat` (`Full_Installer.zip`):** полностью сносит старую версию клиента и сервера, сохраняет и мигрирует `STServer.ini`, после чего ставит чистую новую сборку.
- **Через `UPDATE_RFAB_ONLINE.bat` (`Quick_Update.zip`, ≈15 МБ):** мгновенно закрывает старый сервер, удаляет устаревший `UI` и конфликтующие файлы старых сборок и обновляет бинарники клиента (`SkyrimSE.exe`, `EarlyLoad.dll`, `TPProcess.exe`), `.esp`, `UI` и сервер.

### 3. Запуск и подключение

1. **[Только Хост]** Запустите `<Папка_RFAB>\START_RFAB_SERVER.bat` — в окне сразу отобразится IP-адрес для друзей (например, из **Radmin VPN**).
2. **[Хост и Друзья]** Откройте **Mod Organizer 2** → выберите **`RFAB Online - Play`** → нажмите **«Запустить»**.
3. Загрузите сохранение в открытом мире → нажмите **`Правый CTRL`** (или **`F2`**) → **«Подключиться» (`Connect`)**:
   - **Хост вводит:** `127.0.0.1:10578`
   - **Друзья вводят:** `<IP_хоста>:10578`
   - все игроки автоматически объединяются в общую кооп-группу!

---

## ✨ Ключевые механики RFAB Online

| Механика | Как работает в кооперативе |
|---|---|
| 🛡 **«Единство в битве» (строго у Хоста)** | Встроенный в верхнюю панель **Правого CTRL** аналог способности Красноречия RFAB. Когда режим **ВКЛ** (по умолчанию), игроки группы и их саммоны наносят **0 физического урона**, **0 магического урона** и **0 негативных эффектов** (крики, оглушение, яды, плащи, дебаффы) друг другу, но **на 100% лечат и баффают** союзников и их призывы (включая все 18 заклинаний из `RFAB_List_NoAllyDamage_AllowedSpells`). Переключать режим может **только Хост** (для дуэлей и тестов билдов). |
| ⚔️ **Сложности RFAB и «Проклятый»** | Режимы **«Странник»** (`×2 / ×0.5`), **«Довакин»** (`×1 / ×1`), **«Легенда»** (`×1 / ×2`) и метка **«Проклятый»** (`×0.5` к исходящему урону игрока) работают индивидуально у каждого игрока без перезаписи сервером; урон призывных существ остаётся `1.0x`, как в `DifficultyBalance.dll` / `RFAB.dll`. |
| 🐉 **Честный спавн и масштаб HP** | Враги и скриптовые засады **не умножаются** (`1×` спавн), а их здоровье масштабируется по числу игроков: `1` игрок = `×1 HP`, `2` игрока = `×3 HP`, `3` игрока = `×4 HP` (настраивается в `STServer.ini`). |
| 📈 **Общий опыт + личный кошелёк** | Опыт за врагов, боссов, квесты и книги `XPBooksReaded` начисляется всем участникам группы с учётом персонального `RFAB_XP_Multiplier` (кольца, камни), при этом личный кошелёк `RFAB_XP_Amount` защищён от перезаписи чужим `RFAB_Menu`. |
| 🙏 **Downed-on-Knee вместо смерти** | При падении до `0 HP` точный перехват летального урона (`ScopedLethalPlayerDamageIntercept`) ставит персонажа на колено без вызова одиночного `REQ_DeathHandler` и без искажения `% MaxHP` перков RFAB: союзник может поднять его клавишей **`E`** (~4 с), либо группа побеждает в бою и павшие встают с `30% HP`. Если пала вся группа — сервер синхронно откатывает чекпоинт, снимая враждебные яды/дебаффы и полностью сохраняя еду, эликсиры и благословения RFAB. |
| 🧰 **Серверный авторитет боя и лута (`v2.0.2`)** | Серверный реестр `CoopLootLedger` защищает сундуки боссов и тела врагов от дюпа при одновременном взятии предметов (сохраняя квестовые ключи/когти для всей группы), окно согласования урона (`650 мс`) не даёт устаревшим пакетам откатывать HP босса назад, а фазы бесплотности боссов RFAB защищены на стороне сервера. |
| 🧭 **Тактический HUD отряда и 3D-метки** | Лёгкий D3D11/ImGui-интерфейс отряда (`F11` / `/hud`), общая полоса здоровья, фазы и цели агро активного босса RFAB, 3D-маяки раненых напарников и контекстные 3D-метки по прицелу (`F10` / `Alt + СКМ`). |
| 💾 **Безопасные синхронные сейвы** | Двухфазный барьер `PREPARE → ACK → COMMIT` через нативную очередь `BGSSaveLoadManager`, автонастройка `EngineFixes.toml` (`SaveGameMaxSize = false` при `MemoryManager = false`, `_setmaxstdio(8192)`), полная очистка временных `0xFFxxxxxx`-клонов без порчи статических форм и глубокая бинарная проверка LZ4-структуры `.ess` с авто-карантином повреждённых файлов. |
| 🚪 **Бесшовные переходы и `O(1)` ядро** | Разделение `ServerWorldModel` / `ClientWorldView`, `O(1)` индексы сетевых сущностей и объектов, гистерезис 3D-моделей при смене экипировки, кросс-ячеечный перенос `0xFF00xxxx`-клонов и спящий режим мировых NPC на сервере гарантируют плавную игру без вылетов при переходах между локациями. |

---

## 🎮 Управление и права Хоста

| Действие / Клавиша | Кто может использовать | Описание |
|---|---|---|
| **`Правый CTRL`** или **`F2`** | **Все игроки** | Открыть оверлей кооператива (подключение, чат, группа, настройки) |
| **Кнопка «Единство в битве: ВКЛ / ВЫКЛ»** *(или `/allyprotect`)* | **Только Хост** | Включить/выключить защиту союзников от урона и дебаффов для всей группы (у гостей отображается текущий статус Хоста) |
| **`F5`** *(быстрое сохранение)* | **Любой игрок** | Запустить синхронное сохранение прогресса **для всей группы** |
| **`F9`** *(быстрая загрузка)* | **Только Хост** | Синхронно загрузить последний групповой чекпоинт у всех игроков (с авто-реконнектом) |
| **Удерживать `E`** *(у раненого)* | **Все живые союзники** | Поднять упавшего на колено напарника (~4 секунды) |
| **`F10`** / **`Alt + СКМ`** *(тактический пинг)* | **Все игроки** | Поставить контекстную 3D-метку для группы (`Shift + F10` — опасность, `Ctrl + F10` — очистить) |
| **`F11`** *(или `/hud`)* | **Все игроки** | Переключить режим тактического HUD отряда (`Полный` → `Компактный` → `Только 3D-метки` → `Скрыт`) |

---

## 🖥 Настройка выделенного сервера (`STServer.ini`)

Файл конфигурации расположен в `<Папка_RFAB>\RFAB_Online_Server\config\STServer.ini`:

| Параметр | По умолчанию | Назначение |
|---|---|---|
| `uPort` | `10578` | UDP-порт сервера |
| `uMaxPlayerCount` | `8` | Максимальное число игроков |
| `uDifficulty` | `2` | Базовый индекс Adept (`1.0x / 1.0x`), не затирающий личные сложности RFAB |
| `bAutoPartyJoin` | `true` | Автоматическое объединение всех подключившихся игроков в группу Хоста |
| `bEnableItemDrops` | `true` | Видимость и синхронизация выброшенных на землю предметов |
| `bEnablePvp` | `false` | Защита игроков вне группы (внутри группы дружеским огнём управляет Хост кнопкой **«Единство в битве»**) |
| `bAutoScaleEnemyHealth` | `true` | Автоматическое масштабирование HP врагов (`fHealthMultiplier * (Players + fHealthExtraBase)`) |
| `bEnableDownedSystem` | `true` | Система ранения `0 HP Downed-on-Knee` и поднятия на `E` |
| `bEnableCoordinatedSaves` | `true` | Синхронные сохранения группы (`F5` / `F9` / автосейв после боя) |

---

## 📚 Документация и инженерные артефакты

- **[`CHANGELOG.md`](./CHANGELOG.md)** — подробный журнал изменений по всем релизам (`v3.1.7`, `v2.0.2`, `v2.0.1`, `v1.2.0`, `v1.1.0`, `v1.0.0`).
- **[`docs/V2.0.2_RELEASE_BACKUP_MANIFEST.json`](./docs/V2.0.2_RELEASE_BACKUP_MANIFEST.json)**, **[`docs/V2.0.1_RELEASE_BACKUP_MANIFEST.json`](./docs/V2.0.1_RELEASE_BACKUP_MANIFEST.json)** и **[`docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json`](./docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json)** — криптографические манифесты (`SHA-256`) неприкосновенных резервных релизов `v2.0.2-backup`, `v2.0.1-backup` и `v1.0.0`.
- **[`docs/RFAB_ADAPTATION_SPEC.md`](./docs/RFAB_ADAPTATION_SPEC.md)** — спецификация интеграции механик RFAB и кросс-версионной совместимости с будущими обновлениями `drake1ch/RFAB` (`EspEdidIndex`, рекурсивное обнаружение конфигов, плагин-привязанные идентификаторы боссов и квестов).
- **[`docs/PLAYTEST_SIX_ISSUES_ROOT_CAUSE_AND_FIX_SPEC_2026_10_24.md`](./docs/PLAYTEST_SIX_ISSUES_ROOT_CAUSE_AND_FIX_SPEC_2026_10_24.md)** — полный бинарный разбор и архитектурная спецификация исправлений 6 багов кооп-плейтеста (`+0x6EDFC2` при загрузке сейва, `StatsMenu` / `Tab`, агро на раненого, невидимые бессмертные враги и боевой ИИ / скорость атак NPC).
- **[`docs/THREAD_SAFETY_AND_REENTRANCY_HARDENING.md`](./docs/THREAD_SAFETY_AND_REENTRANCY_HARDENING.md)** — аудит потокобезопасности, защиты от `Use-After-Free` и реентерабельности всех сервисов клиента.
- **[`docs/CELL_TRANSITION_CTD_AND_RESPECT_EDITION_AUDIT_2026_10_24.md`](./docs/CELL_TRANSITION_CTD_AND_RESPECT_EDITION_AUDIT_2026_10_24.md)** — корневой разбор и устранение вылетов напарника при смене локации (`DisableImpl` ABI, `FormIdComponent`, `FaceGen`) + аудит совместимости с **RFAB Respect Edition** (`07 RFAB.esp`).
- **[`docs/CORRUPT_SAVE_ROOT_CAUSE_AUDIT_2026_10_24.md`](./docs/CORRUPT_SAVE_ROOT_CAUSE_AUDIT_2026_10_24.md)** — разбор и устранение всех 4 причин ошибки `sSaveGameCorrupt` (`EngineFixes` `ScrapHeap` `128 МБ / 64 МБ`, бинарная валидация SE v12 `.ess` и перехват `0x0058AE30`).
- **[`docs/RFAB_COOP_ALLY_PROTECTION_AND_RELEASE_FIX_2026_10_24.md`](./docs/RFAB_COOP_ALLY_PROTECTION_AND_RELEASE_FIX_2026_10_24.md)** — разбор механики *«Единство в битве»* в `RFAB.esp`, серверной авторизации Хоста и кодировки релизов.
- **[`docs/RFAB_DIFFICULTY_AND_VANILLA_OVERRIDE_AUDIT_2026_10_24.md`](./docs/RFAB_DIFFICULTY_AND_VANILLA_OVERRIDE_AUDIT_2026_10_24.md)** — аудит сложностей RFAB (*Странник / Довакин / Легенда / Проклятый*) и полной зачистки ванильных перезаписей.
- **[`docs/PLAYTEST_ROOT_CAUSE_AUDIT_2026_10_24.md`](./docs/PLAYTEST_ROOT_CAUSE_AUDIT_2026_10_24.md)** — корневой разбор проблем живого кооп-плейтеста (сейвы, FaceGen, алтари, кольца, саммоны, анимации).
- **[`docs/RFAB_PORT_NOTES.md`](./docs/RFAB_PORT_NOTES.md)** и **[`docs/SE_1597_PORT_SPEC.md`](./docs/SE_1597_PORT_SPEC.md)** — архитектура сервисов и бинарная спецификация порта под `Skyrim SE 1.5.97.0.8` (`docs/se-1597-id-map.csv`).
- **[`Data/SKSE/Plugins/RFAB_Online_Bindings.json`](./Data/SKSE/Plugins/RFAB_Online_Bindings.json)** — внешний манифест `EditorID`-привязок для совместимости с обновлениями `drake1ch/RFAB` без перекомпиляции.
- **[`VERSION`](./VERSION)** — единственный источник номера версии (его читают сборка, установщик, релизный пайплайн и README); поднимается только вместе с новым тегом `vX.Y.Z`.
- **[`BUILD.md`](./BUILD.md)** — инструкция по самостоятельной сборке клиента, UI и сервера (`xmake` + Visual Studio 2022 + Node.js).

---

## 🙏 Благодарности

- [`drake1ch/RFAB`](https://github.com/drake1ch/RFAB) — авторам модпака **Skyrim Requiem for a Balance**.
- [`rfortier/TiltedEvolution-rwf`](https://github.com/rfortier/TiltedEvolution-rwf) и команде **Tilted Online** — за базовую платформу Skyrim Together Reborn.
- Командам **SKSE64** (`ianpatt/skse64`), **CommonLibSSE-NG** и **SSE Engine Fixes** (`aers/EngineFixesSkyrim64`).

<sub>Проект является фанатской некоммерческой модификацией и не связан с Bethesda Softworks. Требуется установленный Skyrim SE <code>1.5.97.0.8</code>, Mod Organizer 2 и сборка RFAB.</sub>
