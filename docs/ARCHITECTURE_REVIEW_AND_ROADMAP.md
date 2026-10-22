# Полное код-ревью репозитория `RFAB_Online` и поэтапный план работ

> **Дата ревью:** 2026-10-22
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

### Этап 1 — Чистый бинарный порт клиента на `Skyrim SE 1.5.97.0.8` (Zero AE Residue)
1. Применить проверенную таблицу `docs/se-1597-id-map.csv` ко всем `3 083` точкам `POINTER_SKYRIMSE` / `VersionDbPtr` / `RTTI` в `Code/client/` (0 оставшихся AE ID по `Tools/rfab/verify_se1597_port.py --strict-migrated`).
2. Исправить C++-структуры **`BSAnimationGraphManager.h`** (`lock = 0x98`, `animationGraphIndex = 0xA8`, `sizeof = 0xB0`) и **`CombatController.h`** (`targetSelectors = 0x98`, `pActiveTargetSelector = 0xB0`, `sizeof = 0xD8`).
3. Портировать все **11 `VersionDbPtr` патчей** с рантайм-проверкой сигнатур инструкций, чистым хуком `SE 33672 + 0x0` в `Projectile.cpp` (без `Xbyak` AE-хака) и защитой совместимости с `SkyrimSoulsRE.dll`, `SSEDisplayTweaks.dll` и `EngineFixes.dll`.

### Этап 2 — Исправление багов ядра `TiltedEvolution-rwf` и стабилизация синхронизации врагов
1. Исправить инверсию знака `DeltaHealth` в `Code/server/Services/ActorValueService.cpp` (`+ message.DeltaHealth` + `clamp`).
2. Исправить null-pointer dereference `pOwner->Send(...)` в `CharacterService::TransferOwnership` при дисконнекте игрока + перенести проверенные фиксы `nullptr`/утечек памяти клиента.
3. Усилить синхронизацию врагов и владения (`Ownership`): исключить перезапись здоровья/позиции врага устаревшими пакетами не-владельцев, гарантировать доставку `DeathStateChange`, точную синхронизацию снарядов (`SE 33672`) и `DamageActorValue` (включая `RFAB_PapyrusFunctions.ScriptDamage`).

### Этап 3 — Строгое масштабирование HP врагов (`1p=1×`, `2p=3×`, `3p=4×`) при `1×` числе врагов
1. Удалить сломанный прототип из `Client/ActorValueService.cpp` и внедрить чистый модуль `EnemyHealthScalingService` (сервер сообщает точное число активных игроков $N$ при каждом входе/выходе игрока).
2. Реализовать строгий фильтр **только враждебных NPC** (исключая игроков, удалённых игроков, саммонов, нежить игрока и напарников) и **безопасный двухшаговый алгоритм рескейла** (`PERMANENT -> DAMAGE` при росте $N$, `DAMAGE -> PERMANENT` при убывании $N$), гарантирующий сохранение точного `% HP` и **нулевой шанс смерти NPC при дисконнекте**.
3. Обеспечить **строго `1×` число врагов** (никакого размножения спавнов на число игроков, включая защиту от дублирования скриптовых засад `RFAB_AmbushScript`).

### Этап 4 — Полная интеграция с `drake1ch/RFAB` и равное распределение опыта (`100%` всем игрокам)
1. Внедрить серверный дедуплицированный реестр `ServerXpLedger` и клиентский `RfabXpService`:
   - равное начисление опыта **всем игрокам на сервере поровну** за убийства, квесты, книги опыта и особые награды;
   - синхронное обновление **обоих** пулов (`RFAB_XP_Amount` + `PlayerCharacter::pSkills->data->xp`), показ HUD-уведомлений и срабатывание повышения уровня по `Experience.ini`;
   - синхронизация меток `Infamy = -1` на убитых врагах и `XPBooksReaded` на книгах опыта (0% эксплойтов и 0% `N×` умножения на квестах!).
2. Автоматически переключать `REQ_Combat_NoDeathHandling = 1` на время сетевой сессии (никаких принудительных перезагрузок сейва при смерти одного игрока!).
3. Синхронизировать глобальные счётчики прогрессии RFAB (`FalmersKilled`, `DragonsKilled`, `TFOTG_Kill_Counter`) и фазы боссов (`RFAB_Global_CurrentUmbraBossPhase`).

### Этап 5 — Финальная верификация, unit/offline тесты и подготовка сборки
1. Покрыть unit-тестами математику скейлинга HP (все переходы $1 \leftrightarrow 2 \leftrightarrow 3 \leftrightarrow 4$ игроков на `100%`, `50%`, `1 HP` мобах), `ServerXpLedger` (убийства, квесты на 3 игроков, книги, реконнект) и инверсию `DeltaHealth`.
2. Прогнать полный `Tools/rfab/verify_se1597_port.py --addrlib ... --strict-migrated` (`0` AE ID, `100%` `static_assert` `1.5.97`), проверить Linux-сборку сервера (`SkyrimTogetherServer`) в песочнице и подготовить готовую инструкцию сборки клиента Windows MSVC (`xmake`) + конфиг совместимости `EngineFixes.toml` для `MO2 RFAB`.
