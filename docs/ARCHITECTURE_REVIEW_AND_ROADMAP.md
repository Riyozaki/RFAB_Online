# Полное код-ревью репозитория `RFAB_Online` и поэтапный план работ

> **АКТУАЛЬНОЕ ДОПОЛНЕНИЕ (Playtest 2026-10-24 -> Releases `v1.1.0` & `v1.2.0`):**
> - Полный аудит собственных сложностей RFAB («Странник» `x2/x0.5`, «Довакин» `x1/x1`, «Легенда» `x1/x2` + «Проклятый» `x0.5` исходящий) и устранения 100% ванильных перезаписей контента/статов/перков/камер/ассетов RFAB (`v1.2.0`) задокументирован в **[`docs/RFAB_DIFFICULTY_AND_VANILLA_OVERRIDE_AUDIT_2026_10_24.md`](./RFAB_DIFFICULTY_AND_VANILLA_OVERRIDE_AUDIT_2026_10_24.md)**.
> - Полный разбор всех багов кооп-плейтеста `v1.0.0` (повреждённые `.ess`-сейвы, цикл перезагрузки чекпоинта, нокаут `205`/`349`, фиолетовое лицо, алтари RFAB, удар щитом `~300`, «Кольцо авантюриста», выброшенные предметы, саммоны, заклинания на союзников, опыт RFAB и анимации) задокументирован в **[`docs/PLAYTEST_ROOT_CAUSE_AUDIT_2026_10_24.md`](./PLAYTEST_ROOT_CAUSE_AUDIT_2026_10_24.md)**.
> - Неизменяемый манифест базового рабочего релиза `v1.0.0` сохранён в **[`docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json`](./V1.0.0_RELEASE_BACKUP_MANIFEST.json)**.

> **Дата первичного ревью:** 2026-10-22
> **Проверенные ветки репозитория:**
> - `origin/main` (`4cba32cd`) — базовая ветка предыдущего агента (собиралась MSVC, но содержит мусорные бинарники и критические пробелы порта)
> - `origin/arena/014f2836-rfab-online` (`e2406a5d`) — частичный маппинг ID (`2989/3083`), без сборки MSVC и без адаптации RFAB
> - `origin/arena/fcbffe0c-rfab-online` (`3bc18b13`) — наброски RFAB без порта ID (`0/3083`) и патчей (`0/11`)
> - `origin/arena/ada174eb-rfab-online` (`23cd6230`) — наброски `HealthScalingService` со смертельным багом при дисконнекте, без порта ID (`0/3083`)
> - Upstream `rfortier/TiltedEvolution-rwf` (`a7d06156`) + история `TiltedEvolution` (`2 980` коммитов вплоть до SE `1.5.97` `9d78319c`)
> - Полная сборка `drake1ch/RFAB` (`RFAB.esp`, `872` `.psc`-скрипта, `NetScriptFramework.SkyrimSE.1_5_97_0.bin`, `StorageUtilData/*`, SKSE-плагины)

---

## Часть 1. Что уже сделано ХОРОШО (и что НЕЛЬЗЯ ломать)

### 1. Сильные стороны upstream-базы `rfortier/TiltedEvolution-rwf` (`a7d06156`)
1. **Готовая поддержка модифицированных анимационных графов (`MODDED_BEHAVIOR_COMPATIBILITY`, PR `#659`, `#797`, `#811`)**:
   - В отличие от старого ванильного Skyrim Together, форк `rfortier` уже снимает жёсткий лимит на число переменных `hkbBehaviorGraph`, корректно определяет гуманоидов/драконов при установленном **Nemesis**, поддерживает beast-формы и хранит `ActionsReplayCache` (резко снижая «скольжение» NPC в Т-позе). Это идеальный фундамент под анимационный стек `RFAB` (`Nemesis` + `OAR` + `AMR`).
2. **Продвинутая синхронизация квестов, квестовых предметов, диалогов и сцен (`#769`, `#805`, `#815`, `#852+`)**:
   - Поддержано продвижение стадий квестов участниками группы с защитой от повторных триггеров стадий, синхронизация сцен/субтитров и квестовых предметов.
3. **Улучшенная синхронизация экипировки NPC и combat targeting (`#616`, `#783`, `640c8fe2`)**:
   - Исправлена проблема «голых NPC» при борьбе клиентов за ownership, переведены сырые указатели `CombatController` на `NiPointer<Actor>`, исправлен use-after-free `BGSAttackDataMap` при смене ячейки (`#726`).

### 2. Полезные наработки из `main` (`4cba32cd`) и других веток `RFAB_Online`
1. **Загрузчик `VersionDb` формата `1` (`Libraries/TiltedReverse/.../VersionDb.h`) и `SKSE64 2.0.20` (`main`)**:
   - Чтение `version-1-5-97-0.bin` (формат `1`) реализовано корректно, без переполнений и проверено нами на всех **`778 674`** записях `version-1-5-97-0.bin`.
   - `FileVersion.inl` дополнен fallback-чтением `VS_FIXEDFILEINFO`, не зависящим от языковой таблицы ресурсов `SkyrimSE.exe 1.5.97`, а `main.cpp` явно проверяет `major == 1 && minor == 5 && revision == 97` до установки хуков.
2. **Часть C++-разметок `1.5.97` в `main`**:
   - Корректно откачены размеры `ExtraDataList` (`0x18`), `TESObjectREFR` (`0x98`), `Actor` (`0x2B0`), `PlayerCharacter` (`0xBE0`), `TESObjectCELL` (`0x140`), `TESLeveledList` (`0x28`), `Projectile` (`0x1D8`, `fPower == 0x190`).
3. **Сетевые сообщения `SyncRfabExperienceRequest` / `NotifySyncRfabExperience` (`main`) и точечные багфиксы `014f2836`**:
   - Каркас сериализации пакетов в `Code/encoding/` чистый; в `014f2836` также найдены полезные проверки на `nullptr` и утечки, которые мы аккуратно перенесём.

---

## Часть 2. Что СЛОМАНО / СДЕЛАНО ПЛОХО (полный перечень проблем)

### Блок А. Мусор и структурный беспорядок в репозитории *(исправлено в текущем коммите!)*
1. В Git в `main` были закоммичены **`38+ МБ` бинарников** (`RFAB_extract/RFAB.esp`, `skse64_1_5_97.dll`, `tbb.dll`, `DINPUT8.dll`, `version-1-5-97-0.bin`), корневой дамп `RFAB_ref_files.txt` (`2.44 МБ`) и черновик `PHASE0_ANALYSIS.md`.
2. В `.gitmodules` был добавлен сломанный гигантский сабмодуль `RFAB_ref` (`drake1ch/RFAB`), ломающий `git submodule update --init --recursive`.
3. Одновременно существовали директории **`Tools/` и `tools/`**, отличающиеся только регистром букв — на файловой системе **Windows NTFS** при `git checkout` это вызывает коллизию путей!

### Блок Б. Критические блокеры запуска на `Skyrim SE 1.5.97.0.8` (100% гарантированный CTD в `main`)
1. **`0` из `3 083` Address Library ID переведены на `1.5.97` в `main` (`fcbffe0c`, `ada174eb`)**, а в `014f2836` оставались неразрешёнными **`94` ID** + `2` скрытые ошибки (`AE 400180 -> SE 692121`, `AE 210860 -> SE 548568`, `AE 104655 -> SE 97929`, сплит `AE 77226 -> SE 75591 + SE 75595`). **Теперь в [`docs/se-1597-id-map.csv`](./se-1597-id-map.csv) разрешены и бинарно проверены все `3 083 / 3 083` (`100%`) ID!**
2. **Пропущенная структура `BSAnimationGraphManager.h` (во ВСЕХ 4 ветках!)**:
   - В `main` и всех остальных ветках остались оффсеты AE `1.6.629+` (`lock == 0xA0`, `animationGraphIndex == 0xB0`), тогда как в `1.5.97` **`sizeof(BSAnimationGraphManager) == 0xB0`**, **`lock == 0x98`**, **`animationGraphIndex == 0xA8`**! Каждая синхронизация анимации читает память за пределами heap-блока и портит мьютекс!
3. **Пропущенная структура `CombatController.h` (в `main`, `fcbffe0c`, `ada174eb`)**:
   - Оставлены AE-смещения (`0xA0 / 0xB8 / sizeof 0xE0`) вместо `1.5.97` (`0x98 / 0xB0 / sizeof 0xD8`), что гарантирует краш в любом бою.
4. **Не портированы все `11` внутрифункциональных патчей (`VersionDbPtr`)**:
   - `BSGraphicsRenderer.cpp` (`InitWindows`, `Keyboard`, `StopTimer`, `Renderer_Init`), `BSTaskletManager.cpp`, `SkillsMenu.cpp`, `UI.cpp`, `Projectile.cpp` (`Xbyak` stack-unwind хак под стек AE `1.6.x`).
   - В `BSGraphicsRenderer.cpp` безусловная запись `WS_OVERLAPPEDWINDOW` конфликтует с `SSEDisplayTweaks.dll` из `RFAB` (смещение курсора мыши и рамка окна).

### Блок В. Серверные и сетевые баги синхронизации (upstream + `main`)
1. **Инверсия знака урона/лечения на сервере (`Code/server/Services/ActorValueService.cpp:78`)**:
   - На клиенте урон имеет `DeltaHealth < 0`, но сервер считает `currentHealth - message.DeltaHealth` — **ПРИБАВЛЯЯ урон к здоровью моба на сервере!** При входе второго игрока в ячейку или смене владельца полумёртвый босс внезапно получает `MaxHP + полученный урон`! Должно быть `currentHealth + message.DeltaHealth` с зажатием `std::clamp(..., 0.f, maxHealth)`.
2. **Падение сервера (`SIGSEGV` / Access Violation) при передаче ownership отключившегося игрока (`Code/server/Services/CharacterService.cpp:512`)**:
   - В `TransferOwnership` вызывается `characterOwnerComponent.pOwner->Send(notify)` без проверки `if (characterOwnerComponent.pOwner)` — если предыдущий владелец вылетел/вышел, `pOwner == nullptr` мгновенно роняет `SkyrimTogetherServer`!
3. **Борьба за ownership и рассинхрон HP/смерти врагов**:
   - Нет защиты от перезаписи HP врага устаревшим пакетом не-владельца и нет принудительной синхронизации финального состояния при `IsDead`.

### Блок Г. Критические ошибки логики RFAB, Опыта и Масштабирования врагов в `main` / `ada174eb`
1. **Полностью сломанная прокачка уровней при синхронизации опыта RFAB (`main`, `fcbffe0c`, `ada174eb`)**:
   - Менялся только `RFAB_XP_Amount` (`0x00AD45BB` — число в тексте меню), но **не менялся `PlayerCharacter::pSkills->data->xp` (`Game.SetPlayerExperience`)**, не вызывались уведомления `UILib` / `ShowLevelUp`, квестовый XP умножался в `N` раз на группу из `N` игроков, книги опыта позволяли бесконечный дюп при передаче предмета, а игроки вне `Party` не получали XP вовсе.
2. **Опаснейшие ошибки масштабирования HP врагов в `main` и `ada174eb`**:
   - В `main`: дефолт `1.0f`, положительный `ForceActorValue(DAMAGE, ...)` искажал реальный пул здоровья почти вдвое, скейлились **удалённые игроки, саммоны игрока и компаньоны**, возникали гонки между клиентами.
   - В `ada174eb`: при отключении игрока уменьшение `PermanentMod` до лечения `DamageMod` мгновенно убивало (`CurrentHP <= 0`) загруженных NPC и напарников вокруг игрока!
3. **Дублирование засадных врагов (`RFAB_AmbushScript.psc`) и конфликт смерти (`REQ_DeathHandler.psc`)**:
   - Не был отключён `REQ_Combat_NoDeathHandling` при сетевой игре (смерть одного игрока принудительно перезагружает сейв и выбрасывает его с сервера!) и не были дедуплицированы скриптовые засады `PlaceActorAtMe`.

---

## Часть 3. Детальный поэтапный план работ

### Этап 0 — Ревизия, очистка репозитория и верификация баз данных *(ВЫПОЛНЕНО В ТЕКУЩЕМ КОММИТЕ)*
- [x] Очистить репозиторий от всех бинарников (`*.dll`, `RFAB.esp`, `version-1-5-97-0.bin`), сломанного сабмодуля `RFAB_ref`, `RFAB_ref_files.txt`, дублирующейся папки `tools/` (коллизия NTFS) и черновиков.
- [x] Защитить `.gitignore` и аккуратно разложить чистые справочные таблицы RFAB в `docs/rfab/` и утилиты проверки в `Tools/rfab/`.
- [x] Сформировать и на `100%` верифицировать полную таблицу **`3 083 / 3 083`** адресов SE `1.5.97` ([`docs/se-1597-id-map.csv`](./se-1597-id-map.csv)), все `16 656` структур движка ([`docs/SE_1597_PORT_SPEC.md`](./SE_1597_PORT_SPEC.md)) и все механики `RFAB` ([`docs/RFAB_ADAPTATION_SPEC.md`](./RFAB_ADAPTATION_SPEC.md)).

### Этап 1 — Чистый бинарный порт клиента на `Skyrim SE 1.5.97.0.8` (Zero AE Residue) *(ВЫПОЛНЕНО И ПРОВЕРЕНО)*
- [x] Применена проверенная таблица `docs/se-1597-id-map.csv` ко всем `3 090` точкам `POINTER_SKYRIMSE` / `VersionDbPtr` / `RTTI` в `Code/client/` (`0` оставшихся AE ID по `Tools/rfab/verify_se1597_port.py --strict-migrated` и C++20 `VersionDb(1,5,97,8)` тесту).
- [x] Исправлены C++-структуры **`BSAnimationGraphManager.h`** (`lock = 0x98`, `animationGraphIndex = 0xA8`, `sizeof = 0xB0`) и **`CombatController.h`** (`targetSelectors = 0x98`, `pActiveTargetSelector = 0xB0`, `sizeof = 0xD8`), добавлен `Actor::IsPlayerTeammate()`.
- [x] Портированы все **11 `VersionDbPtr` патчей** с рантайм-проверкой и чистым хуком входа `SE 33672` (`ActorMagicCaster::Fire`) в `Projectile.cpp`, обеспечена совместимость с `SkyrimSoulsRE.dll`, `SSEDisplayTweaks.dll`, `EngineFixes.dll` (`CRLF/LF` regex) и `MO2` (`VersionDb` `(1,5,97,8)` -> `version-1-5-97-0.bin`), восстановлены git-сабмодули `Libraries/Tilted*`.

### Этап 2 — Исправление багов ядра `TiltedEvolution-rwf`, стабилизация синхронизации врагов, базовый HP-скейлинг и равная рассылка XP *(ВЫПОЛНЕНО И ПРОВЕРЕНО)*
- [x] Исправлена инверсия знака `DeltaHealth` в `Code/server/Services/ActorValueService.cpp` (`currentHealth + message.DeltaHealth` + `std::clamp(..., 0.f, maxHealth)`) + проверка владельца при периодических апдейтах + точный клиентский `actualDelta = healthAfter - healthBefore` в `Actor.cpp` (`HookDamageActor`).
- [x] Исправлены `SIGSEGV` при передаче ownership отключившегося игрока (`CharacterService::TransferOwnership`), `IsNonSyncableQuest` (`||` -> `&&`) и `nullptr`-краши в `QuestService`, утечка в `ExtraDataList::Delete`, остановка `ServerListService` и серверные заголовки (`ActorValueService.cpp`, `ServerListService.cpp`).
- [x] Включено автоматическое серверное масштабирование HP врагов (`bAutoScaleEnemyHealth = true`: `1p = 1×`, `2p = 3×`, `3p = 4×`, $N \ge 2 \implies (N + 1)\times$) с фильтром игроков/напарников/саммонов/лошадей и безопасным двухшаговым рескейлом (`SetScaledHealthSafely`), исключающим мгновенную смерть раненых NPC при дисконнекте, а также равная рассылка опыта `RFAB` всем подключённым игрокам с защитой `kInfamy = -1.0f` и дедупликацией квестового XP (`TPTests`: `Passed all 10 test cases with 2075 assertions`).

### Этап 3 — Адаптивность к обновлениям `drake1ch/RFAB`, двухфазные сохранения, `0 HP Downed-on-Knee` и `1×` спавны *(ВЫПОЛНЕНО И ПРОВЕРЕНО)*
- [x] Внедрены `Data/SKSE/Plugins/RFAB_Online_Bindings.json`, `Code/common/RfabModpackCompat.h`, `RfabAdapterService` (`TESForm::LookupByEditorID` `SE 14464` + живой парсинг `StorageUtilData/`) и `Tools/rfab/patch_rfab_modpack_compat.py` + автопатч `EngineFixes.toml` (`RegularQuicksaves = true`) и подавление одиночного `[RFAB] Autosave.ini`.
- [x] Внедрены `SaveSnapshotService` и `CoordinatedSaveService` (`BGSSaveLoadManager` `SE 516860` / `34818` / `34819`): двухфазный протокол `PREPARE -> ACK -> COMMIT`, кольцевые слоты `RFAB_MP_<Session>_Slot<K>`, авточекпоинт после боя и атомарный откат `ServerWorldSnapshot` при `Total Party Wipe`.
- [x] Внедрены `EncounterService` и `DownedStateService` с заморозкой всех положительных мутаций `kHealth` в `ActorValueOwner::RestoreActorValue` (`SE 37513`, subobject `+0xB0`), `HookRegenAttributes` и `HookApplyActorEffect`, реанимацией союзником за `~4.0s` (`E`), автоподъёмом на `~30% HP` после честной победы, защитой `strict_rfab_leash` (`120 м`) и `1-к-1` дедупликацией скриптовых спавнов `0xFF000000`.

### Этап 4 — Полная интеграция с `drake1ch/RFAB` и равное распределение опыта (`100%` всем игрокам) *(ВЫПОЛНЕНО И ПРОВЕРЕНО)*
- [x] Внедрены `RfabProgressionService` (`ServerXpLedger` для убийств, квестов и книг `XPBooksReaded` `BGSListForm::AddForm` `SE 20470`) и синхронизация двойного пула XP (`RFAB_XP_Amount` + `PlayerCharacter::pSkills->data->xp` по `Experience.ini`) + глобалов прогрессии и фаз боссов `RFAB_Global_*`.

### Этап 5 — Многопроходная верификация и документация *(ВЫПОЛНЕНО И ПРОВЕРЕНО)*
- [x] Пройдены все 5 контуров верификации: `TPTests` (`15 / 15` тест-сьютов, `2 208` проверок), `verify_client_real` (`3 092` RVAs + ABI asserts), `verify_se1597_port.py --strict-migrated` (`3 096 / 3 096` вызовов, `9 / 9` `static_assert`), C++20 сборка всех серверных и клиентских TU и справочник [`docs/RFAB_PORT_NOTES.md`](./RFAB_PORT_NOTES.md).
