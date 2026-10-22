# Phase 0: анализ порта Tilted Evolution на Skyrim SE 1.5.97 + RFAB

> **Редакторская врезка при интеграции (2026-10-22, Arena-агент).**
> Документ написан агентом на ПК владельца и интегрирован сюда из
> `origin/main` @ `4cba32cd` без правок текста ниже. Исходный статус
> «черновик Phase 0, код ещё не изменялся» **устарел**: фичи из §5–§6
> (HP-множитель, RFAB XP sync) реализованы и перенесены атомарными
> коммитами на сохранённую историю. Часть «нужно сделать» из §3.4–§3.5
> (VersionDb format 1, SKSE 2.0.20) к моменту написания уже была
> реализована коммитами Phase 1 — не переделывать. Канонические доки
> проекта: `docs/phase-1-report.md`, `docs/phase-1-midfunction-audit.md`,
> ревью интеграции: `docs/review-agent-main-2026-10-22.md`.

Статус: черновик Phase 0. Код ещё не изменялся. *(исходная строка документа — см. врезку выше)*

Цель: портировать `rfortier/TiltedEvolution-rwf` на Skyrim SE 1.5.97.0.8, SKSE64 2.0.20, Address Library 1.5.97 и RFAB, исправив ошибки оригинала. До подтверждения этого документа не начинать правки кода.

## 1. База и ограничения

- Репозиторий: `G:\RFAB_Online`, ветка `port/1.5.97-phase0`, база `a7d06156`, remote `rfortier/TiltedEvolution-rwf`.
- Эталон 1.5.97: коммит `8eaca858^` = `5c5837f6`; `8eaca858` — миграция на AE. `6f963497` не является предком HEAD.
- Целевая игра: Skyrim SE 1.5.97.0.8, SKSE64 2.0.20, Address Library `version-1-5-97-0.bin` (format 1).
- Address Library format 1: 778 674 записи; 3 081 уникальный ID из `Code/client` найден, отсутствующих нет.
- RFAB.esp: 31 115 357 байт, 89 802 записи; разбор сохранён в `RFAB_extract/rfab_records.tsv`.
- Ограничения: отдельная ветка, атомарные коммиты, рискованные изменения за флагами, не выдумывать offsets/IDs, непроверенное явно помечать, отчёт пользователю на русском.
- `xmake 3.1.2` доступен (`G:\Tools\xmake\xmake\xmake.exe`, `XMAKE_GLOBALDIR=G:\xmake`), сборка `releasedbg` с `--runtimes=MT`. Skyrim SE не установлена, проверка в игре пока невозможна.
- `RFAB_ref` — частичный клон с пустым worktree; исходники доступны через `git -C RFAB_ref show HEAD:...`. Извлечённые заголовки/данные — в `RFAB_extract`.

## 2. Подтверждённая матрица layout

Все offsets ниже — для 1.5.97/SKSE 2.0.20. Они ещё не проверены в игре.

| Класс | Цель 1.5.97 | Текущее состояние | Действие |
|---|---:|---:|---|
| `ExtraDataList` / `BaseExtraList` | `0x18` | `0x18` | Готово: vtable/virtual destructor убраны |
| `TESObjectREFR` | `0x98`, `extraData 0x70`, `loadedState 0x68` | `0x98` | Готово |
| `Actor` | `0x2B0` | `0x2B0` | Готово |
| `PlayerCharacter` | `0xBE0` | `0xBE0` | Готово |
| `TESObjectCELL` | `0x140`, `worldspace 0x120` | `0x140` | Готово |
| `TESQuest` | `0x268`, `objectives 0xF8` | `0x268` | Stage storage совместим; нужен игровой тест |
| `TESLeveledList` | `0x28`, `entries 0x8` | `0x28` | Компонент приведён к цели |
| `TESActorBaseData` | `0x58`, `factions 0x40` | `0x58`, `factions 0x40` | Не менять |
| `TESNPC` | без отдельного сдвига | совпадает с pre-AE | Не менять |

Ключевые offsets `Actor` после исправления `ExtraDataList`: `actorValueOwner 0xB0`, `actorState 0xB8`, `flags1 0xE0`, `currentProcess 0xF0`, `flags2 0x1FC`, `unk194 0x270`, `actorLock 0x27C`.

Ключевые offsets `PlayerCharacter`: `objectives 0x580`, `pSkills 0x9B0`, `locationForm 0xAC8`, `baseTints 0xB10`, `overlayTints 0xB28`.

`TESObjectREFR` до исправления: `size 0xA0`, `loadedState 0x68`, `extraData 0x70`. После исправления: `size 0x98`, offsets до `extraData` не меняются.

## 3. Критические исправления

### 3.1 ExtraDataList: корневая причина сдвига +8

Текущий `ExtraDataList` имеет виртуальный деструктор, из-за чего в начале объекта появляется vptr и размер становится `0x20`. В 1.5.97 `BaseExtraList` не имеет vtable: `data 0x0`, `bitfield 0x8`, `lock 0x10`, итого `0x18`.

Исправление:

- убрать virtual destructor/marker vtable из `ExtraDataList`;
- сохранить `data`, `PresenceBitfield`, `BSRecursiveLock` и текущий API `New()`;
- проверить, что нигде нет `delete` через базовый указатель и не требуется полиморфное удаление;
- обновить `static_assert` для `ExtraDataList`, `TESObjectREFR`, `Actor`, `PlayerCharacter`, `TESObjectCELL`.

Это автоматически возвращает `TESObjectREFR` с `0xA0` на `0x98`, `Actor` с `0x2B8` на `0x2B0`, `PlayerCharacter` с `0xBE8` на `0xBE0`, `TESObjectCELL` с `0x148` на `0x140`.

### 3.2 TESQuest: stage storage совместим с 1.5.97

Проверка CommonLibSSE 1.5.97 показала, что `0xE8`/`0xF0` — это два указателя:

- `BSSimpleList<TESQuestStage>* executedStages` на `0xE8`;
- `BSSimpleList<TESQuestStage*>* waitingStages` на `0xF0`.

Текущие `GameValueList<Stage>* pExecutedStages` / `GameValueList<Stage*>* pWaitingStages` layout-совместимы: `TESQuestStage` — 8 байт (`index 0x0`, `uint8_t flags 0x2`, `pad3`, `pad4`), плюс next-указатель. `GameList<Objective> objectives` на `0xF8` оставить как есть.

Не менять это на `GameList<Stage>` на `0xE8`; такая гипотеза ошибочна. `IsStageDone` можно оставить на native ID `25011`, но поведение проверить в игре; fallback — обход `executedStages`/`waitingStages`.

Проверить также `TESQuest::Kill`: текущая версия меняет `flags` на `Completed`, pre-AE использовала `Disabled`; это поведенческое изменение, не layout, требует отдельного решения.

### 3.3 PlayerCharacter::pSkills: неверная косвенность

В 1.5.97:

- `pSkills` — `PlayerSkills*` на `0x9B0`;
- `PlayerSkills` — 8 байт, содержит `StatData* data`;
- `StatData`: `levelPoints 0x0`, `levelPointsMax 0x4`, `LevelData levelData[18]` с `0x8` (по 12 байт: `level`, `points`, `pointsMax`), `legendaryLevel[18]` с `0xE0`.

Текущий `Skills` по полям фактически совпадает с `StatData`, но `pSkills` объявлен как `Skills**`, а `GetSkillExperience` использует `(*pSkills)->skills[aSkill].xp`. После переноса на 1.5.97 это неверно.

План:

- ввести/использовать `PlayerSkills { StatData* data; }` и объявить `PlayerSkills* pSkills` на `0x9B0`;
- `GetSkillExperience` переписать на `pSkills->data->levelData[aSkill].points` (или эквивалентный typed wrapper);
- `Skills` можно оставить как представление `StatData` или переименовать, чтобы не путать с `PlayerSkills`;
- проверить `AddSkillExperience`, `HookAddSkillExperience`, `LastUsedCombatSkill`, `difficulty`.

`difficulty` после сдвига оказывается на `0xAF8`, что соответствует неизвестному полю SKSE рядом с `0xB00`; отдельного изменения не требуется, но `SetDifficulty` надо проверить в игре.

### 3.4 VersionDb: format 1

`VersionDb` сейчас принимает только format 2 (`if (format != 2) return false;`). Целевой `version-1-5-97-0.bin` — format 1. Нужно:

- принимать format 1 и 2 с одинаковым алгоритмом;
- проверить чтение заголовка, count, records, IDs;
- подтвердить, что все 3 081 уникальных ID из `Code/client` находятся;
- покрыть тестом/утилитой парсинга.

### 3.5 SKSE64 2.0.20

`ScriptExtender.cpp` содержит `kSKSEMinBuild = 20100` с комментарием AE-only; для 2.0.20 (build 20020) это rejects. Нужно:

- снизить/сделать настраиваемым минимум до 20020;
- проверить `GetSKSEStyleExeVersion()` для 1.5.97;
- обновить сообщение `main.cpp` про 1.6.X и код выхода;
- проверить `StartSKSE` и загрузку плагинов.

## 4. Вторичные вопросы

### 4.1 TESForm: виртуальные слоты и сигнатуры

Проверка CommonLibSSE 1.5.97 подтвердила текущую схему:

- `TESForm` содержит слоты `0x04..0x3A`; `unk_39/unk_3A` соответствуют `GetObjectTypeName`/`QAvailableInGame`, а не первым слотам `TESObjectREFR`;
- `TESObjectREFR` начинается с `0x3B`; `GetParentCell`/`SetParentCell` — `0x97/0x98`, `IsDead` — `0x99`;
- `TESForm::GetName(const BSFixedString& = {})` на `0x2E` — корректный wrapper: настоящая функция `GetTextForParsedSubTag`, пустой тег даёт полное имя; no-arg версия не нужна;
- `GetFormEditorID` на `0x32` корректен; вызовы только в debug views;
- `TESObjectREFR::VirtIsDead(bool)` на `0x99` соответствует `IsDead(UInt8)`.

Не менять эти слоты без повторной проверки по CommonLibSSE 1.5.97.

### 4.2 TESLeveledList

SKSE 2.0.20:

- `Entry* entries 0x8`;
- `chanceValue 0x10`, `flags 0x11`, `length 0x12`, `unk0B 0x13`;
- `unk18 0x18`, `chanceGlobal 0x20`;
- `sizeof 0x28`, `Entry 0x18`.

`Components/TESLeveledList.h` приведён к цели: `Entry* entries 0x8`, `chanceValue 0x10`, `flags 0x11`, `length 0x12`, `unk0B 0x13`, `unk18 0x18`, `chanceGlobal 0x20`. Прямых обращений к полям не найдено; `TESLevCharacter` использует компонент только как opaque-поле.

Новый `Forms/TESLeveledList.h` с `pLeveledListA` на `0x8` и `LEVELED_OBJECT` (`pForm`, `count`, `level`, `padC`, `pItemExtra`, `0x18`) соответствует цели и используется в `TESLevItem`/`Actor.cpp` (уровневые доспехи). Его не ломать.

Ранее предполагавшиеся `0x18/0x29/0x30` для `TESLeveledList` не подтверждаются; цель — `0x28`.

### 4.3 TESActorBaseData и TESNPC

`TESActorBaseData` уже соответствует 1.5.97: `factions 0x40`, `sizeof 0x58`. Текущие добавления — только `IsEssential`/`SetEssential`, layout не меняется.

`TESNPC` отличается от pre-AE только `FormType`, `GetTemplateBase` и косметикой; отдельного сдвига `+0x10` нет.

### 4.4 TESObjectCELL

Цель: `sizeof 0x140`, `worldspace 0x120`. Текущие `0x148/0x128` объясняются только `ExtraDataList`; после 3.1 статические assert должны совпасть. Проверить `Data048` (SKSE: `BSExtraData*`, `void*`, два `UInt32`, итого `0x18`) и `worldspace`/`parentCell` при загрузке клеток.

## 5. HP scaling

Цель: единое масштабирование здоровья для игроков/партий, совместимое с RFAB, без побочного изменения урона.

Реализовано (сборка проходит, поведение в игре не проверено из-за отсутствия Skyrim SE):

- настройка сервера `Gameplay:fHealthMultiplier` (float, по умолчанию `1.0`, clamp `0.1..10.0`); `Gameplay:uDifficulty` не используется как множитель HP, так как difficulty меняет урон и уже синхронизируется;
- сервер не хранит базовый `MaxHealth` и не масштабирует его отдельно: множитель передаётся клиентам через `ServerSettings.HealthMultiplier`, а сервер хранит/рассылает уже масштабированные значения владельца;
- клиент масштабирует max health всех локально симулируемых акторов через `ForceActorValue(PERMANENT, kHealth, scaledMax)` и восстанавливает процент текущего HP через `ForceActorValue(DAMAGE, ...)`;
- `ActorValuesComponent.AppliedHealthBonus` предотвращает повторное масштабирование; при переходе remote->local бонус инициализируется, если у сущности ещё есть `RemoteComponent`;
- при потере `LocalComponent` и при disconnect здоровье восстанавливается до множителя `1.0`;
- wire format `ServerSettings` изменён добавлением float; отдельный protocol bump не добавлен, совместимость старых/новых клиентов зависит от build auth.

Ограничения: инициализация при remote->local эвристическая: если предыдущий владелец ещё не применил множитель или множитель недавно изменился, возможен пропуск/двойное масштабирование. Требуется проверка `GetActorPermanentValue(kHealth)`/`ForceActorValue(PERMANENT, ...)` после повторных изменений и совместимости с RFAB `ModBaseActorValue`.

Тесты: игрок с multiplier 1/2/5, NPC, remote player, ownership transfer, уровень/перк RFAB, смена настройки, respawn, сохранение/загрузка, урон и смерть.

## 6. Синхронизация RFAB XP

RFAB XP — локальная Papyrus-система, не нативная:

- `RFAB_XP_Handler.psc`: `PlayerXP`, `MultiplierXP`, `OnKill`, `OnBookRead`, `OnQuestComplete`, `ModXP`, `VanilaXPGain`;
- `RFAB_XP_Alias.psc`: PO3-события, `OnActorKilled`, фильтры `IsValidXPKill`;
- XP также меняется через RFAB-книги/квесты и `CustomReward`.

Свойства `PlayerXP`/`MultiplierXP` подтверждены по VMAD `QUST 0x060CB75F` в `RFAB.esp`: `PlayerXP` -> `GLOB 0x06AD45BB` (`RFAB_XP_Amount`), `MultiplierXP` -> `GLOB 0x06112815` (`RFAB_XP_Multiplier`). Индекс `06` — из текущей тестовой сборки, поэтому в клиенте форма ищется через `ModManager::GetByName("RFAB.esp")` + `Mod::GetFormId(0xAD45BB)`, без жёсткого runtime ID.

Реализовано (сборка проходит, поведение в игре не проверено — Skyrim SE не установлен):

- клиент раз в секунду читает `RFAB_XP_Amount`, считает положительную дельту и отправляет `SyncRfabExperienceRequest` (только при наличии party);
- сервер ретранслирует дельту через `NotifySyncRfabExperience` остальным участникам party; настройка `Gameplay:bEnableRfabXpSync` (по умолчанию `true`) отключает синхронизацию; дельта ограничена `1 000 000` за сообщение;
- получатель обновляет `PlayerXP` и вызывает нативную `Game.SetPlayerExperience` через `GlobalPapyrusFunction`; `RFAB_XP_Handler.ModXP` не вызывается, поэтому множитель RFAB не применяется повторно;
- echo suppression: при удалённом применении `m_lastRfabExperience` сдвигается на дельту, а не перечитывается из глобала; при отключении baseline сбрасывается; XP, полученный вне party, не отправляется задним числом;
- добавлены `SyncRfabExperienceRequest`/`NotifySyncRfabExperience`, opcode'ы и round-trip тесты; `ServerSettings` не менялся, но новые opcode'ы требуют одинаковой сборки клиента и сервера.

Ограничения:

- нет ledger/`AwardId`/дедупликации и snapshot при reconnect: протокол рассчитан на упорядоченный TCP в рамках сессии; при повторной доставке возможен двойной XP;
- нет серверной проверки источника и rate-limit, кроме верхней границы дельты;
- `Game.SetPlayerExperience` может не вызвать RFAB `OnLevelIncrease`/`ShowLevelUp` так же, как `VanilaXPGain`; `fXPLevelUpBase`, `ShowLevelUp` и UI требуют проверки;
- polling может пропустить начисление и последующее списание между тиками; отрицательные дельты игнорируются и только сбрасывают baseline;
- RFAB UI может читать `PlayerXP` до/после обновления глобала; порядок `PlayerXP` -> `SetPlayerExperience` требует проверки.

Сборка `xmake -y` проходит; `TPTests` — 6/32, `Console_Tests` — 6/6, `Resources_Tests` — 13/13. Игровые тесты: убийство обычного/босса, книга, квест, `CustomReward`, level-up, несколько игроков в party, disconnect/reconnect, отсутствие двойного XP, отсутствие эха, RFAB UI.

## 7. Launcher и RFAB DLL

- `EngineFixes.dll` блокируется `Code/immersive_launcher/stubs/DllBlocklist.cpp` с комментарием, что ломает hooks; для RFAB-сборки нужно решить: оставить блок, обновить EngineFixes или проверить совместимость.
- Sumwunn `DINPUT8.dll` и `DInputHook` вместе не проверялись; возможен конфликт загрузчика.
- `tbb.dll` присутствует; проверить, что RFAB/плагины не подменяют её несовместимой версией.
- `RFAB.dll` — нативный API не реверс-инженерен; не вызывать его функции до отдельного анализа.
- Порядок загрузки: `CrtStartupHooks.cpp` → `InitClient()` → `BeginMain()` → `LoadScriptExender()` → `StartSKSE`; проверить, что RFAB.esp/скрипты загружаются после SKSE и до создания мира.

## 8. Риски и открытые вопросы

- Сборка проходит, но игра не установлена: offsets и поведение не проверены в рантайме.
- `TESQuest` stage storage проверен по CommonLibSSE 1.5.97 и совместим; `IsNonSyncableQuest`/debug views всё ещё требуют игрового теста.
- `TESForm` vtable проверен: `GetName`/`GetFormEditorID`/`VirtIsDead` на корректных слотах; runtime-проверка debug views остаётся.
- `TESLeveledList::Components` приведён к цели и собирается; прямых использований нет.
- HP multiplier может конфликтовать с RFAB `ModBaseActorValue`, `SetActorValue` и синхронизацией max values.
- RFAB XP сейчас ретранслируется как delta без ledger/`AwardId`; нет дедупликации, rate-limit и snapshot при reconnect. Для anti-cheat/валидации нужен более строгий источник.
- `Game.SetPlayerExperience` может не обновлять RFAB `fXPLevelUpBase`/UI так же, как `ModXP`; нужен тест. Клиент применяет XP через нативную функцию и обновляет `PlayerXP` глобал.
- `EngineFixes`/`DINPUT8`/`RFAB.dll` совместимость неизвестна.
- `VersionDb` format 1 проверен на реальном `version-1-5-97-0.bin`: 778 674 записи, 0 ошибок.
- `TiltedScript`/`cpp-httplib` не являются gitlink'ами и не нужны для сборки; `cpp-httplib` приходит из xmake package.

## 9. План проверки

1. Собрать проект (после установки toolchain): `xmake`/CMake, все static_assert.
2. Запустить 1.5.97 + SKSE 2.0.20 + Address Library: лог не должен содержать reject по build/format.
3. Логировать `sizeof`/`offsetof` для `ExtraDataList`, `TESObjectREFR`, `Actor`, `PlayerCharacter`, `TESObjectCELL`, `TESQuest`, `TESLeveledList`, `TESActorBaseData`.
4. Проверить базовый сценарий: загрузка save, движение, бой, инвентарь, клетки, крафт, заклинания.
5. Проверить `TESQuest`: запуск/сдача квестов, `IsNonSyncableQuest`, stage sync, debug view.
6. Проверить skill XP: бой/крафт, `pSkills`, отсутствие падений.
7. Проверить HP multiplier: локальный игрок, NPC, remote player, respawn, смена настройки, сохранение.
8. Проверить RFAB XP: kill/book/quest/custom, level-up, party, reconnect, отсутствие двойного начисления и эха.
9. Проверить launcher: SKSE стартует, RFAB.esp/скрипты загружаются, нет конфликтов DLL.
10. Проверить save/load и стабильность 30+ минут в party.

## 10. Порядок реализации

1. `ExtraDataList` + все static_assert/size cascades; сборка.
2. `VersionDb` format 1 + `kSKSEMinBuild` + сообщение `main.cpp`; сборка/запуск.
3. `TESQuest` stage storage проверен; код оставлен без изменений, нужен игровой тест квестов.
4. `PlayerSkills` + `GetSkillExperience`; тест skill XP.
5. HP multiplier: настройка, сообщения, применение, тесты.
6. RFAB XP: polling, relay, применение, тесты; ledger/snapshot — отдельный этап.
7. Launcher/RFAB DLL совместимость.
8. Документация и финальный чеклист.

Каждый пункт — отдельный атомарный коммит; HP/XP/launcher — за фича-флагами до подтверждения.

## 11. Ключевые файлы

- `Code/client/Games/Skyrim/ExtraData/ExtraDataList.h/.cpp`
- `Code/client/Games/Skyrim/TESObjectREFR.h`
- `Code/client/Games/Skyrim/Actor.h/.cpp`
- `Code/client/Games/Skyrim/PlayerCharacter.h/.cpp`
- `Code/client/Games/Skyrim/Forms/TESObjectCELL.h`
- `Code/client/Games/Skyrim/Forms/TESQuest.h/.cpp`
- `Code/client/Games/Skyrim/Components/TESActorBaseData.h`
- `Code/client/Games/Skyrim/Components/TESLeveledList.h`
- `Code/client/Games/Skyrim/Forms/TESLeveledList.h`
- `Code/client/Games/Skyrim/Forms/TESLevItem.h`
- `Code/client/Games/Skyrim/Forms/TESLevCharacter.h`
- `Code/client/Games/Skyrim/Forms/TESNPC.h`
- `Code/client/Games/Skyrim/Forms/TESForm.h`
- `Code/client/Services/Generic/QuestService.cpp`
- `Code/client/Services/Debug/Views/QuestDebugView.cpp`
- `Code/client/VersionDb.h/.cpp`
- `Code/client/ScriptExtender.cpp`
- `Code/client/main.cpp`
- `Code/client/Services/PapyrusService.h`
- `Code/client/Services/Generic/PapyrusService.cpp`
- `Code/server/Services/ActorValueService.cpp`
- `Code/client/Services/Generic/ActorValueService.cpp`
- `Code/server/Services/CharacterService.cpp`
- `Code/client/Services/Generic/CharacterService.cpp`
- `Code/encoding/Structs/ServerSettings.h/.cpp`
- `Code/encoding/Messages/NotifyActorMaxValueChanges.h`
- `Code/encoding/Messages/RequestActorMaxValueChanges.h`
- `Code/encoding/Messages/NotifySyncExperience.h`
- `Code/encoding/Messages/SyncExperienceRequest.h`
- `Code/encoding/Messages/SyncRfabExperienceRequest.h/.cpp`
- `Code/encoding/Messages/NotifySyncRfabExperience.h/.cpp`
- `Code/immersive_launcher/stubs/DllBlocklist.cpp`
- `RFAB_extract/SKSE_GameReferences.h`
- `RFAB_extract/SKSE_GameData.cpp`
- `RFAB_extract/version-1-5-97-0.bin`
- `RFAB_extract/RFAB.esp`
- `RFAB_ref` (git object): `src/skse64/skse64/GameForms.h`, `GameFormComponents.h`, `GameReferences.h`, `GameBSExtraData.h`
- `RFAB_ref` (git object): `MO2/mods/RFAB/scripts/source/RFAB_XP_Handler.psc`, `RFAB_XP_Alias.psc`, `RFAB_LevelUpBonuses.psc`
