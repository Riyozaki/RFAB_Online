# RFAB Online

Мультиплеерная платформа для модпака **[Skyrim Requiem for a Balance (RFAB)](https://github.com/drake1ch/RFAB)** на базе форка [`rfortier/TiltedEvolution-rwf`](https://github.com/rfortier/TiltedEvolution-rwf), адаптированная строго под **Skyrim Special Edition `1.5.97.0.8`** (`SKSE64 2.0.20`, `version-1-5-97-0.bin`).

---

## Ключевые проектные принципы и возможности

- **Строгий таргет `Skyrim SE 1.5.97.0.8`:** полная побайтовая совместимость с `SkyrimSE.exe 1.5.97.0.8`, `skse64_1_5_97.dll` (`2.0.20`), `version-1-5-97-0.bin` (`3 096 / 3 096` проверенных call-site'ов, `9 / 9` `static_assert` ABI-структур) и полным стеком SKSE-плагинов `RFAB` (`EngineFixes.dll`, `SSEDisplayTweaks.dll`, `SkyrimSoulsRE.dll`, `po3_Tweaks.dll`, `OAR`, `AMR`, `RFAB.dll`). Версии `1.6.x` (AE) намеренно не поддерживаются.
- **Устойчивость к будущим обновлениям `drake1ch/RFAB` (Zero Hardcoded FormIDs):** динамическое разрешение форм через `TESForm::LookupByEditorID` (`SE 14464`), внешний JSON-манифест [`Data/SKSE/Plugins/RFAB_Online_Bindings.json`](./Data/SKSE/Plugins/RFAB_Online_Bindings.json), живое чтение таблиц `Data/SKSE/Plugins/StorageUtilData/{Experience.ini, Quests.ini, BossList.json, RaceList.json}` и автонастройка `EngineFixes.toml` / `[RFAB] Autosave.ini`.
- **Равное распределение опыта и полная прогрессия `RFAB`:** серверный дедуплицированный журнал `ServerXpLedger` начисляет **равное количество опыта (`1×`)** всем игрокам за убийства мобов/боссов, завершение квестов и чтение книг опыта (`XPBooksReaded`), синхронно обновляя `RFAB_XP_Amount`, ванильный пул `PlayerCharacter::pSkills->data->xp` по `Experience.ini`, счётчики прогрессии и фазы боссов `RFAB_Global_*`.
- **Честный баланс врагов (`1×` спавнов, `(N + 1)×` HP):** количество врагов **никогда не умножается** на число игроков (включая `1-к-1` дедупликацию скриптовых засад `RFAB_AmbushScript` / `0xFF000000`), а максимальный HP врагов масштабируется безопасно (`1` игрок = `1× HP`, `2` игрока = `3× HP`, `3` игрока = `4× HP`, настраивается в `STServer.ini`) без риска мгновенной смерти раненых мобов при выходе игрока.
- **Система `0 HP Downed-on-Knee` и честные границы боя (`strict_rfab_leash`):** при `0 HP` игрок падает на колено без срабатывания одиночного `REQ_DeathHandler` и без поломки реген-статов/баффов `RFAB` (все положительные изменения `kHealth` замораживаются напрямую в C++-хуках движка `SE 37513`), враги переключают агро на живых игроков, союзник может поднять раненого за `~4.0s` (`E`) прямо в бою, честная победа автоматически поднимает павших с `~30% HP`, побег за `120 м` без убийства врагов не абузит воскрешение, а падение всей группы (`Total Party Wipe`) автоматически откатывает последний чекпоинт.
- **Умные двухфазные сохранения и снапшоты мира (`Host Sync + Post-Combat Auto`):** двухфазный протокол `PREPARE -> ACK -> COMMIT` для ручных сохранений/загрузок Хоста (`F5` / `F9`), автоматический чекпоинт после боя, кольцевой буфер слотов `RFAB_MP_<Session>_Slot<K>` и атомарный откат `ServerWorldSnapshot` (`Epoch N`).

---

## Статус реализации

- [x] **Этап 0 — Ревизия репозитория, очистка истории артефактов и 100% верификация баз данных**
- [x] **Этап 1 — Чистый бинарный порт клиента и лаунчера на `Skyrim SE 1.5.97.0.8` (`3 096 / 3 096` проверенных вызовов, `9 / 9` C++-структур, `11 / 11` низкоуровневых хуков, совместимость с `MO2 + EngineFixes + SSEDisplayTweaks + SkyrimSoulsRE`)**
- [x] **Этап 2 — Исправление критических багов ядра `TiltedEvolution-rwf`, стабилизация синхронизации врагов/квестов/снарядов и безопасное масштабирование HP врагов (`SetScaledHealthSafely`)**
- [x] **Этап 3 — Адаптивный слой совместимости с `drake1ch/RFAB`, двухфазные синхронные сохранения/снапшоты мира, система `0 HP Downed-on-Knee`, строгие границы боя (`strict_rfab_leash`) и дедупликация скриптовых спавнов `1×`**
- [x] **Этап 4 — Полная интеграция прогрессии `RFAB` (`ServerXpLedger`, двойной пул XP + уровни `Experience.ini`, книги `XPBooksReaded`, глобальные счётчики и фазы боссов `RFAB_Global_*`) и многопроходная верификация (`TPTests`: `15 / 15` тест-сьютов, `2 208` проверок)**

---

## Документация и инженерные артефакты

- [`docs/RFAB_PORT_NOTES.md`](./docs/RFAB_PORT_NOTES.md) — **Главный практический справочник `Stages 1–4`**: архитектура всех сервисов, настройки `STServer.ini`, двухфазные сохранения, система `Downed-on-Knee` и кастомизация `RFAB_Online_Bindings.json`.
- [`docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md`](./docs/ARCHITECTURE_REVIEW_AND_ROADMAP.md) — **Архитектурное ревью и дорожная карта**: аудит веток репозитория, разбор исправленных багов и план этапов.
- [`docs/SE_1597_PORT_SPEC.md`](./docs/SE_1597_PORT_SPEC.md) — **Низкоуровневая спецификация порта на `1.5.97.0.8`**: все проверки `sizeof`/`offsetof` C++-структур, `11` внутрифункциональных патчей `VersionDbPtr` и правила совместимости с SKSE-плагинами `RFAB`.
- [`docs/se-1597-id-map.csv`](./docs/se-1597-id-map.csv) — **Полная бинарно верифицированная таблица Address Library ID** (`AE 1.6.x -> SE 1.5.97.0.8` + точные `RVA` из `version-1-5-97-0.bin`).
- [`docs/RFAB_ADAPTATION_SPEC.md`](./docs/RFAB_ADAPTATION_SPEC.md) — **Спецификация механик `drake1ch/RFAB`**: формулы XP/HP, `REQ_DeathHandler`, засады, счётчики и боссы.
- [`Data/SKSE/Plugins/RFAB_Online_Bindings.json`](./Data/SKSE/Plugins/RFAB_Online_Bindings.json) — **Внешний манифест привязок `EditorID`**, позволяющий адаптироваться к новым релизам `drake1ch/RFAB` без перекомпиляции.
- [`Tools/rfab/`](./Tools/rfab/) — CLI-инструменты верификации и подготовки модпака (`verify_se1597_port.py`, `patch_rfab_modpack_compat.py`, `parse_rfab_esp.py`).
- [`BUILD.md`](./BUILD.md) — инструкция по локальной сборке клиента и сервера через `xmake` / Visual Studio 2022 + `pnpm`.
