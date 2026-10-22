# RFAB Online — Технический справочник порта `Skyrim SE 1.5.97.0.8` и систем `Stages 1–4`

Актуальная сводная документация по архитектуре, настройкам сервера, синхронным сохранениям, системе ранения «на колене» (`0 HP Downed-on-Knee`) и адаптивности к будущим обновлениям сборки **[drake1ch/RFAB](https://github.com/drake1ch/RFAB)**.

---

## 1. Обзор реализованных подсистем (`Stages 1–4`)

| Подсистема | Ключевые файлы | Назначение |
|---|---|---|
| **Бинарный порт `SE 1.5.97.0.8`** | `Code/client/**`, `Code/immersive_launcher/**`, `docs/se-1597-id-map.csv`, `Tools/rfab/verify_se1597_port.py` | `3 096 / 3 096` проверенных вызовов (`3 085` уникальных ID из `version-1-5-97-0.bin`), `9 / 9` `static_assert` структур (`ModManager`, `TESGlobal`, `CombatController`, `BSAnimationGraphManager` и др.), совместимость с `MO2 + EngineFixes + SSEDisplayTweaks + SkyrimSoulsRE`. |
| **Адаптивность к обновлениям `drake1ch/RFAB`** | `Data/SKSE/Plugins/RFAB_Online_Bindings.json`, `Code/common/RfabModpackCompat.h`, `Code/client/Services/RfabAdapterService.*`, `Tools/rfab/patch_rfab_modpack_compat.py` | **Нулевая жёсткая привязка к численным `FormID` `RFAB.esp`**: разрешение форм на лету через `TESForm::LookupByEditorID` (`SE 14464`, включено в `po3_Tweaks.ini`), чтение внешнего манифеста `RFAB_Online_Bindings.json` и живых таблиц `StorageUtilData/{Experience.ini, Quests.ini, BossList.json, RaceList.json, RFAB_XP_Settings.json}`. |
| **Двухфазные серверные сохранения и снапшоты** | `Code/server/Services/SaveSnapshotService.*`, `Code/client/Services/CoordinatedSaveService.*`, `Code/client/Games/Skyrim/SaveLoad.*` | Двухфазный барьер `PREPARE -> ACK -> COMMIT` для ручного сохранения/загрузки Хостом, авточекпоинт после завершения боя, кольцевой буфер слотов `RFAB_MP_<Session>_Slot<K>` + атомарный `ServerWorldSnapshot` (`Epoch N`) с откатом при `Total Party Wipe`. |
| **Система `0 HP Downed-on-Knee` и честные границы боя** | `Code/server/Services/EncounterService.*`, `Code/client/Services/DownedStateService.*`, `Code/client/Games/Skyrim/Actor.cpp` | Перехват летального урона до смерти (`REQ_DeathHandler` не срабатывает, баффы и статы сохраняются), полная блокировка всех положительных изменений `kHealth` на уровне C++-хуков движка (`SE 37513 ActorValueOwner::RestoreActorValue`, `HookRegenAttributes`, `HookApplyActorEffect`), сброс агро врагов на живых игроков, реанимация союзником (`~4.0s` удержания `E`), автоподъём (`~30% HP`) строго после честной победы и перезагрузка чекпоинта при падении всей группы. |
| **Полная прогрессия `RFAB` и дедупликация спавнов `1×`** | `Code/server/Services/RfabProgressionService.*`, `Code/server/Services/CharacterService.cpp`, `Code/client/Services/RfabAdapterService.*` | Дедуплицированный серверный журнал `ServerXpLedger` (убийства, квесты, книги `XPBooksReaded`), синхронное обновление `RFAB_XP_Amount` и ванильной шкалы `PlayerCharacter::pSkills->data->xp` по `Experience.ini`, синхронизация счётчиков и фаз боссов `RFAB_Global_*`, `1-к-1` дедупликация скриптовых засад `PlaceActorAtMe` (`0xFF000000`). |

---

## 2. Настройки сервера (`STServer.ini` / консоль сервера)

### Секция `[Gameplay]`
- `bAutoScaleEnemyHealth = true` — автоматический пересчёт HP врагов по числу активных игроков (`1` игрок = `1×`, `N >= 2` игроков = `(N + fHealthExtraBase) * fHealthMultiplier`, т.е. по умолчанию `2p = 3×`, `3p = 4×`, `4p = 5×`). Напарники, саммоны, поднятая нежить и лошади никогда не масштабируются; при выходе игрока процент HP сохраняется без мгновенной смерти раненых мобов (`SetScaledHealthSafely`).
- `fHealthMultiplier = 1.0` (`0.1 .. 20.0`) — базовый множитель HP врагов поверх формулы группы.
- `fHealthExtraBase = 1.0` (`0.0 .. 5.0`) — базовое слагаемое кривой `(N + ExtraBase)`.
- `bDeduplicateScriptedSpawns = true` — `1-к-1` дедупликация скриптовых спавнов/засад `RFAB_AmbushScript` (`0xFF000000`), сохраняющая все реальные спавны владельца локации (`1×` врагов), но отсекающая дубликаты от других клиентов.
- `bDownedSystem = true` — включает состояние «на колене» при `0 HP` вместо одиночной смерти `RFAB`.
- `fReviveChannelSeconds = 4.0` — время удержания клавиши взаимодействия (`E`) рядом с раненым союзником в бою (в секундах).
- `fReviveHealthFraction = 0.30` — доля `MaxHP`, с которой поднимается спасённый игрок (плюс краткая неуязвимость после подъёма).
- `fCombatLeashRadius = 8400.0` (`120 м`) — радиус честной границы боя `strict_rfab_leash`: побег за радиус без убийства заагренных врагов **не** засчитывается как победа и **не** воскрешает павших союзников на `0 HP`.

### Секция `[SaveSync]`
- `bCoordinatedSaves = true` — включает двухфазную серверную синхронизацию сохранений/загрузок и перехват локальных одиночных сохранений (`Autosave` от `RFAB.dll` подавляется на время сессии).
- `bAutoPostCombatSave = true` — автоматически создаёт синхронный серверный чекпоинт после победы в значимом бою.
- `fPostCombatSaveCooldownSeconds = 180.0` — минимальный интервал между автоматическими боевыми чекпоинтами.
- `uSaveRingBufferSlots = 5` — размер кольцевого буфера слотов сохранений (`Slot0 .. Slot4`).

### Управление с клиента (Хост / Лидер группы)
- `F5` (Quicksave) — инициирует двухфазное синхронное сохранение (`SaveKind::kHostQuick`) у всех игроков и атомарный `ServerWorldSnapshot` на сервере (`EngineFixes.toml` автоматически настраивается на `RegularQuicksaves = true`).
- `F9` (Quickload) — инициирует синхронный откат `ServerWorldSnapshot` до последнего подтверждённого `Epoch` и одновременную загрузку соответствующего слота `RFAB_MP_<Session>_Slot<K>` у всех игроков.
- **Total Party Wipe (все игроки на `0 HP` одновременно)** — сервер автоматически откатывает мир к последнему чекпоинту и рассылает всем клиентам команду синхронной загрузки без повреждения квестов, боссов или XP.

---

## 3. Адаптация к будущим обновлениям `drake1ch/RFAB` без перекомпиляции

Все имена `EditorID` глобальных переменных, уровневых списков и путей к таблицам вынесены в файл:

```
Data/SKSE/Plugins/RFAB_Online_Bindings.json
```

Если в будущей версии `drake1ch/RFAB` появятся новые глобальные счётчики (`RFAB_Counter_*`), фазовые переменные новых боссов (`RFAB_Global_*`) или изменится порядок форм внутри `RFAB.esp`:
1. Численные `FormID` обновлять **не нужно** — `RfabAdapterService` разрешает формы по строковому `EditorID` через движковый хеш-словарь `TESForm::LookupByEditorID` (`SE 14464`), который в `RFAB` заполняется плагином `po3_Tweaks` (`Load EditorIDs = true`).
2. Любые новые глобальные переменные с префиксами из `"synced_global_prefixes"` (`RFAB_Counter_*`, `RFAB_Global_*`, `RFAB_Boss_*`, `RFAB_Quest_*`, `TFOTG_*`) подхватываются **автоматически** при сканировании `ModManager::globals` (с автоматическим отсечением локальных MCM/UI настроек из `"excluded_global_prefixes"`), а при добавлении совершенно нового префикса в `RFAB` достаточно дописать одну строку в `"synced_global_prefixes"` или `"editor_ids"` в `Data/SKSE/Plugins/RFAB_Online_Bindings.json`.
3. Таблицы опыта за уровни, квесты, боссов и расы считываются напрямую из `Data/SKSE/Plugins/StorageUtilData/{Experience.ini, Quests.ini, BossList.json, RaceList.json}` установленного модпака при запуске.

---

## 4. Скрипты проверки и подготовки (`Tools/rfab/`)

- Проверка совместимости папки модпака MO2 (dry-run / автопатч конфигов):
  ```bash
  python3 Tools/rfab/patch_rfab_modpack_compat.py --root "/path/to/MO2/RFAB"
  python3 Tools/rfab/patch_rfab_modpack_compat.py --root "/path/to/MO2/RFAB" --apply
  ```
- Полная побайтовая проверка всех `3 096` вызовов `SE 1.5.97.0.8` против `version-1-5-97-0.bin`:
  ```bash
  python3 Tools/rfab/verify_se1597_port.py --strict-migrated --addrlib "/path/to/version-1-5-97-0.bin"
  ```
