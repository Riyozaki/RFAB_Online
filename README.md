# RFAB Online

<div align="center">

### 🎮 Кооперативный мультиплеер для Skyrim SE 1.5.97 + [Skyrim Requiem for a Balance (RFAB)](https://github.com/drake1ch/RFAB)

[![CI](https://github.com/Riyozaki/RFAB_Online/actions/workflows/release.yml/badge.svg)](https://github.com/Riyozaki/RFAB_Online/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/Riyozaki/RFAB_Online?include_prereleases&label=release)](https://github.com/Riyozaki/RFAB_Online/releases/latest)
[![Game](https://img.shields.io/badge/Skyrim%20SE-1.5.97.0.8-2a2a3a)](https://github.com/Riyozaki/RFAB_Online/releases/latest)
[![SKSE](https://img.shields.io/badge/SKSE64-2.0.20-blue)](https://github.com/Riyozaki/RFAB_Online/releases/latest)

*Играйте вдвоём–втроём в Requiem for a Balance: общие квесты, общий опыт, честные бои, «Downed-on-Knee» вместо мгновенной смерти и синхронные сохранения всей группы.*

[**📥 Скачать релиз**](https://github.com/Riyozaki/RFAB_Online/releases/latest) · [**⚡ Быстрый старт**](#-быстрый-старт) · [**🎮 Управление**](#-управление) · [**📚 Документация**](#-документация-и-инженерные-артефакты)

</div>

---

## 🔥 Что это

**RFAB Online** — готовый к игре кооператив «из коробки» для модпака **RFAB** на базе форка [`rfortier/TiltedEvolution-rwf`](https://github.com/rfortier/TiltedEvolution-rwf), строго под **Skyrim Special Edition `1.5.97.0.8`** (`SKSE64 2.0.20`, `version-1-5-97-0.bin`).

Всё уже собрано, упаковано и проверено: нативный лаунчер `SkyrimSE.exe`, клиент для **Mod Organizer 2**, автономный выделенный сервер и GUI-установщик с чистой вычисткой старых файлов. Никаких консольных команд, `git`, `xmake` или ручного копирования файлов не требуется.

---

## 📦 Быстрый старт

### 1. Установка (одинакова для Хоста и Друзей)

1. Скачайте **[`RFAB_Online_v1.0.0_Full_Installer.zip`](https://github.com/Riyozaki/RFAB_Online/releases/latest)** и распакуйте в любую папку.
2. Закройте игру и **Mod Organizer 2**, затем запустите **`INSTALL_RFAB_ONLINE.bat`**:
   - откроется окно выбора папки сборки RFAB (автопоиск `ModOrganizer.exe` по дискам + точный путь к `SkyrimSE.exe 1.5.97` из `ModOrganizer.ini`, либо кнопка **«Обзор...»**);
   - установщик **начисто удаляет** любые старые файлы прошлых тестов (`MO2\mods\RFAB_Online`, `RFAB_Online_Server`) — никакого наслоения на старые версии;
   - ставит нативный лаунчер `SkyrimSE.exe`, включает `+RFAB_Online`, `*SkyrimTogether.esp` и `*SkyrimTogetherQuestPatches.esp` строго перед `Requiem.esp`, настраивает `EngineFixes.toml`, `SkyrimSoulsRE.ini`, `[RFAB] Autosave.ini`, скрывает конфликтующий `BetterStealing.dll` и добавляет кнопку **`RFAB Online - Play`** в MO2;
   - ставит автономный сервер в `<Папка_RFAB>\RFAB_Online_Server` и создаёт ярлык **`START_RFAB_SERVER.bat`**.

> 💡 **Уже играли в прошлую версию?** Достаточно **[`RFAB_Online_v1.0.0_Quick_Update.zip`](https://github.com/Riyozaki/RFAB_Online/releases/latest)** (≈15 МБ): распакуйте и запустите `UPDATE_RFAB_ONLINE.bat` — он обновит клиент и сервер без полной переустановки.

### 2. Запуск и игра

1. **[Только Хост]** Запустите `START_RFAB_SERVER.bat` — в окне сразу появится IP-адрес для друзей (например, из **Radmin VPN**).
2. **[Все]** Откройте **Mod Organizer 2** → выберите **`RFAB Online - Play`** → **«Запустить»**.
3. В игре загрузите открытый мир → нажмите **`Правый Ctrl`** (или **`F2`**) → **`Connect`**:
   - **Хост:** `127.0.0.1:10578`
   - **Друзья:** `<IP_хоста>:10578`
   - все игроки автоматически попадают в общую группу.

---

## ✨ Возможности

| | |
|---|---|
| 🤝 **Честный кооп** | Количество врагов **никогда не умножается** на число игроков (включая скриптовые засады `1×`), зато HP врагов масштабируется: `2` игрока = `×3 HP`, `3` игрока = `×4 HP` (настраивается в `STServer.ini`). |
| 📈 **Общий опыт** | Равное распределение XP (`1×`) **каждому** игроку за мобов, боссов, квесты и книги опыта — с синхронным обновлением `RFAB_XP_Amount`, ванильного пула и уровней `Experience.ini`. |
| 🙏 **Downed-on-Knee** | При `0 HP` игрок падает на колено (без срабатывания одиночного `REQ_DeathHandler`), враги переключаются на живых, союзник поднимает раненого за `~4 с` на **`E`**, а честная победа поднимает павших с `~30% HP`. Вся группа пала — автоматический откат последнего чекпоинта. |
| 💾 **Умные сохранения** | Двухфазный протокол `PREPARE → ACK → COMMIT`: **`F5`** у любого игрока сохраняет всю группу, **`F9`**/загрузка — только Хост, автоматический чекпоинт после боя и авто-переподключение после загрузки. |
| 🧩 **Адаптивность** | Ноль захардкоженных FormID: динамическое разрешение форм через `TESForm::LookupByEditorID` + внешний манифест [`RFAB_Online_Bindings.json`](./Data/SKSE/Plugins/RFAB_Online_Bindings.json) — будущие обновления [`drake1ch/RFAB`](https://github.com/drake1ch/RFAB) подхватываются без перекомпиляции. |
| 🛡 **Стабильность** | Исправлены корневые баги `TiltedEvolution` (UAF-краш при загрузке старого сейва с динамическими NPC, «голые» NPC у друзей, отрицательный HP, нестабильный XP, путаница синхронизации сейвов) — подробности ниже. |

---

## 🎮 Управление

| Клавиша | Действие |
|---|---|
| **`Правый Ctrl`** или **`F2`** | Открыть оверлей (подключение, сессия, отладка) |
| **`F5`** | Сохранить игру для **всей группы** (любой игрок) |
| **`F9`** | Загрузить последний групповой сейв (**только Хост**) |
| **`E`** (у раненого союзника) | Поднять игрока с колена (~4 с) |

> Хост должен войти в открытый мир **до** подключения друзей. После загрузки сейва клиент **автоматически переподключается** к серверу.

---

## ⚖️ Правила кооператива

- **Опыт** — равными долями всем участникам группы (в т.ч. за квесты и книги опыта `RFAB_XP_Books_XP`).
- **Враги** — один комплект на группу (никаких двойных/тройных спавнов), HP растут с числом игроков.
- **Смерть** — `0 HP` = «на колене» до конца боя или до поднятия; полный провал группы откатывает чекпоинт.
- **Побег** — уход на `120 м` без убийства врагов не абузит воскрешение (`strict_rfab_leash`).
- **Лут и инвентарь** — синхронизируются; экипировка NPC восстанавливается по базовой раскладке уровневых списков (фикс «голых» NPC).

---

## 🖥 Сервер и настройки

Конфигурация сервера — `<Папка_RFAB>\RFAB_Online_Server\STServer.ini`:

| Параметр | Смысл |
|---|---|
| `port` (по умолчанию `10578`) | UDP-порт подключения |
| `MaxPlayers` | максимум игроков в сессии |
| `EnemyHealthScalePerPlayer` | множитель HP врагов на каждого следующего игрока (`×3` для 2-х, `×4` для 3-х по умолчанию) |
| `SaveSlots` | размер кольцевого буфера групповых сейвов `RFAB_MP_<Session>_Slot<K>` |
| `LogLevel` / `GameLogLevel` | детализация логов сервера/игры |

Полный разбор всех параметров — в [`docs/RFAB_PORT_NOTES.md`](./docs/RFAB_PORT_NOTES.md).

---

## 🩹 Что исправлено по итогам плейтеста

| # | Проблема | Решение |
|---|---|---|
| 1 | HP уходил в минус, анимации смерти не было | Унифицированный C++-хук `RestoreActorValue` (`SE 37513`): фатальный удар смягчается до `1 HP` + система `Downed-on-Knee` |
| 2 | Нестабильный опыт (`200 / 3×30 / 0` за волков) | Двойной пул XP `RFAB_XP_Amount` + прямая запись ванильного пула, серверный дедуплицированный журнал, `490/490` паритет у всех игроков |
| 3 | Краш `0xC0000005` при загрузке старого сейва → `F9` | UAF динамических NPC (`0xFFxxxxxx`) при откате `BGSSaveLoadGame`: очистка/восстановление динамических акторов вокруг каждого save/load |
| 4 | «Голые» NPC у подключившегося друга | Рекурсивный резолвер экипировки из уровневых списков (`TESLeveledList`, детерминированный seed) + досинхронизация инвентаря |
| 5 | Путаница синхронизации сейвов | Правила `host_sync_plus_postcombat_auto`: `F5` у любого игрока = групповой сейв, `F9` — только Хост, авто-чекпоинт после боя, авто-реконнект |

---

## 📈 Статус реализации

- [x] **Этап 0** — Ревизия репозитория, очистка истории артефактов и 100% верификация баз данных
- [x] **Этап 1** — Чистый бинарный порт клиента и лаунчера на `Skyrim SE 1.5.97.0.8` (`3 096 / 3 096` проверенных вызовов, `9 / 9` C++-структур, `11 / 11` низкоуровневых хуков)
- [x] **Этап 2** — Исправление критических багов ядра `TiltedEvolution-rwf`, стабилизация синхронизации врагов/квестов/снарядов и безопасное масштабирование HP врагов
- [x] **Этап 3** — Адаптивный слой совместимости с `drake1ch/RFAB`, двухфазные синхронные сохранения/снапшоты мира, `0 HP Downed-on-Knee`, строгие границы боя и дедупликация скриптовых спавнов `1×`
- [x] **Этап 4** — Полная интеграция прогрессии `RFAB` (`ServerXpLedger`, двойной пул XP, книги `XPBooksReaded`, фазы боссов `RFAB_Global_*`) и многопроходная верификация (`TPTests`: `15 / 15` сьютов, `2 208` проверок)

---

## 📚 Документация и инженерные артефакты

- [`docs/RFAB_PORT_NOTES.md`](./docs/RFAB_PORT_NOTES.md) — **Главный практический справочник `Stages 1–4`**: архитектура всех сервисов, настройки `STServer.ini`, двухфазные сохранения, система `Downed-on-Knee` и кастомизация `RFAB_Online_Bindings.json`.
- [`docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md`](./docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md) — **Архитектурное ревью и дорожная карта**: аудит веток репозитория, разбор исправленных багов и план этапов.
- [`docs/SE_1597_PORT_SPEC.md`](./docs/SE_1597_PORT_SPEC.md) — **Низкоуровневая спецификация порта на `1.5.97.0.8`**: все проверки `sizeof`/`offsetof` C++-структур, `11` внутрифункциональных патчей `VersionDbPtr` и правила совместимости с SKSE-плагинами `RFAB`.
- [`docs/se-1597-id-map.csv`](./docs/se-1597-id-map.csv) — **Полная бинарно верифицированная таблица Address Library ID** (`AE 1.6.x -> SE 1.5.97.0.8` + точные `RVA` из `version-1-5-97-0.bin`).
- [`docs/RFAB_ADAPTATION_SPEC.md`](./docs/RFAB_ADAPTATION_SPEC.md) — **Спецификация механик `drake1ch/RFAB`**: формулы XP/HP, `REQ_DeathHandler`, засады, счётчики и боссы.
- [`Data/SKSE/Plugins/RFAB_Online_Bindings.json`](./Data/SKSE/Plugins/RFAB_Online_Bindings.json) — **Внешний манифест привязок `EditorID`**, позволяющий адаптироваться к новым релизам `drake1ch/RFAB` без перекомпиляции.
- [`Tools/rfab/`](./Tools/rfab/) — CLI-инструменты верификации и подготовки модпака (`verify_se1597_port.py`, `patch_rfab_modpack_compat.py`, `parse_rfab_esp.py`).
- [`BUILD.md`](./BUILD.md) — инструкция по локальной сборке клиента и сервера через `xmake` / Visual Studio 2022 + `pnpm`.

---

## 🛠 Сборка из исходников

```powershell
git clone https://github.com/Riyozaki/RFAB_Online.git
# требуется: Visual Studio 2022, xmake, Node.js + pnpm
powershell -File Tools\rfab\Build-CloudRelease.ps1   # полный облачный пайплайн сборки + упаковки + тестов
```

Подробнее — в [`BUILD.md`](./BUILD.md). CI-пайплайн [`.github/workflows/release.yml`](./.github/workflows/release.yml) собирает и публикует релиз автоматически на чистой Windows-машине (`7` этапов: депы → пакеты MO2 → `xmake` → `TPTests` → упаковка → верификация → публикация).

---

## 🙏 Благодарности

- [`rfortier/TiltedEvolution-rwf`](https://github.com/rfortier/TiltedEvolution-rwf) и [Tilted Online](https://tilted.ph) — базовая кооперативная платформа.
- [`drake1ch/RFAB`](https://github.com/drake1ch/RFAB) — модпак **Skyrim Requiem for a Balance**.
- [SKSE Team](https://skse.silverlock.org/) (`ianpatt/skse64`), [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG), [aers/EngineFixesSkyrim64](https://github.com/aers/EngineFixesSkyrim64) — инфраструктура моддинга SE.

<sub>Проект не связан с Bethesda Softworks. Требуется легальная копия Skyrim SE <code>1.5.97.0.8</code>, Mod Organizer 2 и модпак RFAB.</sub>
