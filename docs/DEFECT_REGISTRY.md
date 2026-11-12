# RFAB Online — единый реестр дефектов плейтестов

**Назначение.** Один живой документ, куда сведены **все** кейсы, когда-либо зафиксированные в
`docs/*AUDIT*.md`, `PLAYTEST*.md` и коммитах. Раньше каждый разбор жил в своём файле, и было
непонятно, что из этого реально исправлено в текущем коде, что покрыто тестом, а что проверено
только моделью. Этот реестр — единственная точка сверки. Старые аудиты остаются как исторический
контекст, но **статус смотрим здесь**.

**Дата сверки:** 2026-11-04 · **Ревизия:** `main` после мержа PR #17 (`c80c6882`) + ветка `arena/c6f6a538-rfab-online`.

## Легенда статусов

| Метка | Значение |
|---|---|
| 🧪 **тест** | Поведение закреплено тестом в быстрой петле (`run-core-tests.sh`) — падает при регрессе |
| ✅ **код** | Фикс лично найден в текущем коде (указано место); поведенческого теста нет |
| ⚠️ **модель** | Правило доказано симулятором сессии, но замена движковых вызовов в живой игре не проверена |
| 🔬 **нужен плейтест** | Движковая часть (хуки, 3D, Papyrus) — проверить можно только в игре вдвоём |
| ❌ **открыт** | Дефект не закрыт или закрытие не подтверждено |

> Правило ведения: меняешь поведение системы — обнови строку здесь и приложи тест. Строка без
> теста и без ссылки на код считается непроверенной.

---

## 1. Сейвы / загрузка / чекпоинты

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| S1 | «Файл сохранения поврежден» + цикл перезагрузки чекпоинта | `SaveGameMaxSize=true` + `MemoryManager=false` в EngineFixes; поверхностная валидация `.ess`; ложный `sSaveGameCorrupt` при перехвате загрузки | `Setup-RfabOnline.ps1` (миграция EngineFixes.toml), `SaveLoad.cpp` | ✅ код · 🔬 плейтест |
| S2 | Вылет при F5 в коопе (`0xE06D7363` `__non_rtti_object` в EngineFixes) | Сохранение из потока не-main + RTTI через границу модуля | `CoordinatedSaveService`, WinMain-only pump | ✅ код · 🔬 плейтест |
| S3 | F5 блокировался у всех до создания группы | `IsLocalPlayerAuthorizedLeader` возвращал `empty()` вместо хоста | `CoordinatedSaveService.cpp` | ✅ код |
| S4 | Отменённый барьер доходил до коммита; «готов» от не-ожидаемого игрока открывал мир; ворота оставались взведёнными после restore; разыменование пустого optional | 4 дефекта в `SaveSnapshotCoordinator` | `Code/common/Save/Barrier.h` | 🧪 тест (`rfab_save_barrier.cpp`, проверено мутацией) |
| S5 | Чекпоинт коммитится без подтверждения всех пиров | Двухфазный коммит | `Save::Coordinator` + `SessionSim` инвариант | 🧪 тест + ⚠️ модель |

## 2. Переходы локаций / спавн / материализация

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| C1 | Детерминированный CTD пира при смене локации (`0xC0000005`) | `TESObjectREFR::Disable()` — latent Papyrus ABI, `R8` трактовался как `RefrOrInventoryObj*` | `TESObjectREFR.cpp` → `DisableImpl()`/`EnableImpl()` напрямую | ✅ код · 🔬 плейтест |
| C2 | UAF на удалённом актёре послеunload | `CancelServerAssignment` не снимал `FormIdComponent` | `CharacterService.cpp` | ✅ код |
| C3 | CTD при unload 3D головы (`~BSLightingShaderProperty`) | Кросс-аллокаторное разрушение `BSMaskedShaderMaterial` | `FaceGenSystem.cpp` (guard + virtual dtor flags=1) | ✅ код · 🔬 плейтест |
| C4 | CTD при одновременном переходе (null `parentCell`) | `MoveTo`/`Actor::Create` без null-guard | `TESObjectREFR.cpp`, `Actor.cpp`, `CharacterService.cpp` | ✅ код |
| C5 | CTD `SkyrimSE.exe+0x6EDFC2` на save-load (`RCX=0`) | `DisableImpl()` + `Delete()` до нативного разрушения мира; двойной purge | `DetachAllActorsForEngineLoad()` (`CharacterService.cpp:1098`) | ✅ код · 🔬 плейтест |
| C6 | Бессмертные невидимые враги при входе в интерьер с downed-союзником | `WaitingFor3D`-спавн без `FormIdComponent` | `CreateCharacterForEntity`/`RunSpawnUpdates` (`CharacterService.cpp:2441`) | ✅ код |
| C7 | Бессмертные невидимые враги после reconnect в данже | `OnConnected` удалял нативных `TESLevCharacter` `0xFF` | `IsCreatedByNetwork()` + `SetRemote(false)` | ✅ код (2 call-site) · 🔬 плейтест |
| C8 | NPC навсегда `IsRemote()` после выхода пира | `OnRemoveCharacter` не сбрасывал `SetRemote(false)` | `CharacterService.cpp` | ✅ код |
| C9 | Предикат «network-created vs нативный leveled 0xFF» и решения «удалить/отсоединить/перерегистрировать» были инлайн в ~6 местах | Решающая логика вперемешку с entt/движком; корень C7 | `Code/common/RemoteLifecycle.h` (вынесено: `IsNetworkCreatedForm`, `DecideFormDisposition`, `DecideEntityDisposition`; предикат делегирован всеми 20 call-site) | 🧪 тест (`rfab_remote_lifecycle.cpp`, 7 кейсов — добавлено в этой ветке; вынос доказуемо faithful) · 🔬 плейтест (движковая часть) |

## 3. Бой / урон / здоровье

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| D1 | Один удар засчитывался дважды (97→95→93) | Удар не опознавался, дубль пакета снимал HP снова | `Authority::HitLedger` + `HitId` (`Code/common/Authority/Hits.h`) | 🧪 тест + ⚠️ модель (дефект №11) |
| D2 | Чужой урон откатывался назад | Периодическое показание владельца возвращало снятое HP; таймер 650/700 мс не работает | `HealthRevision` + `Authority::ClassifyHealthSnapshot` (`Health.h`) | 🧪 тест + ⚠️ модель (дефект №14) |
| D3 | Удар щитом ~300 чистого урона по чужим NPC | Не синхронизировались `kDamageResist`/резисты → броня RFAB = 0 на клиенте атакующего | `GetEssentialActorValues`/`BroadcastActorValues` (+resists) | ✅ код · 🔬 плейтест |
| D4 | ×9 HP / деление базы на 3 при передаче владения | Немасштабированное `InitialActorValues` до `OnLocalComponentAdded` | `ActorExtension::NativeBaseMaxHealth` (идемпотентная база) | ✅ код · 🔬 плейтест |
| D5 | Сложность «Мастер» резала урон вдвое и удваивала входящий | `uDifficulty=4` вместо эталонного RFAB Адепт=2 | `GameServer.cpp`, `STServer.ini`, миграция | ✅ код |
| D6 | Фонтан чисел 205/349 при downed | `ForceActorValue` принимал дельту, передавали `1-curHp` | `DownedStateService` (точное целевое `1.0f`) | ✅ код · 🔬 плейтест |

## 4. NPC AI / агрессия / экипировка

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| A1 | 2H оружие в одной руке, лучники не стреляют | `AddOrRemoveItem` форсил `rightEquipSlot` для всех `isWorn` | `TESObjectREFR.cpp:1095` (только 1H/стафы) | ✅ код · 🔬 плейтест |
| A2 | NPC замирают в анимации замаха / очень медленные атаки | Порча base `kWeaponSpeedMult` через `SetActorValue` | `ActorValueService.cpp:542` (`ForceActorValue DAMAGE`) | ✅ код |
| A3 | NPC кружат в агрессии, не атакуя после передачи владения | Залипшие transient `meleeAttackState`/`staggered`/`weaponState` | `CharacterService::TakeOwnership` (sanitize) | ✅ код · 🔬 плейтест |
| A4 | Бесконечный aggro-цикл над downed-игроком | `StopCombat`+`StartCombat` каждые 0.5 с | `HookUpdateTarget` + `IsDownedPlayer` (`CombatController.cpp:67`) | ✅ код · 🔬 плейтест |
| A5 | Враги игнорируют живого игрока, толпятся над downed | Downed-клон оставался `kAlive` валидной целью | `IsDownedPlayer()` + `kBleedout`, `HookUpdateDetectionState` | ✅ код · 🔬 плейтест |

## 5. Downed-on-Knee / камера / меню

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| K1 | Вращение камеры + спам «Вы не можете продолжать бой» | Повторные `bleedoutStart`/`SetWeaponDrawn(false)` каждые 0.85 с | `DownedStateService` (однократный вход) | ✅ код · 🔬 плейтест |
| K2 | CTD SkillsMenu + блокировка Tab | Boot-order race `g_IsSoulsREActive`; снятие `kPausesGame` у StatsMenu/TweenMenu | `SkillsMenu.cpp`/`UI.cpp` (runtime-проверка SoulsRE) | ✅ код · 🔬 плейтест |
| K3 | Блокировка управления после сцены/квеста | `UEFlag::kMenu` не восстанавливался | `RfabAdapterService::OnUpdate` (ControlMap self-heal) | ✅ код · 🔬 плейтест |
| K4 | Правила нокдауна/подъёма были инлайн в сервисе и не покрыты тестами | Решающая логика вперемешку с движковыми вызовами | `Code/common/Downed/Revive.h` (вынесено: health-gate, выбор цели, канал подъёма, расчёт лечения, grace) | 🧪 тест (`rfab_downed_revive.cpp`, 12 кейсов — добавлено в этой ветке; вынос доказуемо faithful) · 🔬 плейтест (движковая часть) |

## 6. Внешность / FaceGen / HUD

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| F1 | Фиолетовое лицо локального персонажа | Мутация общего кэшированного `BSMaskedShaderMaterial` in-place | `FaceGenSystem.cpp` (клон + `SetMaterial` + `InitializeShader`) | ✅ код · 🔬 плейтест |
| F2 | Потеря FaceGen/экипировки после смены локации | `Generated=true` навсегда, тинты не применялись к новому NiNode | `FaceGenComponent` (отслеживание `BSFaceGenNiNode*`) | ✅ код |
| F3 | Имя в HUD группы на две строки поверх экрана | Нет `nowrap`/`ellipsis` | `group.component.scss` | ✅ код (UI теперь собирается из исходников) |
| F4 | RaceMenu hair color не синхронизировался | Общий `BGSColorForm`/`HairTint` | `CoopHairColorSync.h`, v3.1.2 | ✅ код · 🔬 плейтест |

## 7. RFAB-совместимость (алтари, кольца, заклинания, дроп)

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| R1 | Алтари Аэдра/Даэдра не работают | 5 loose `.pex` перебивали скрипты RFAB; сетевая ретрансляция активации | Удалены `.pex`; `ObjectService` (без пересылки алтарей) | ✅ код · 🔬 плейтест |
| R2 | «Кольцо авантюриста» ломается при надевании | Papyrus на чужом клоне слал `RequestInventoryChanges` владельцу | Запрет `InventoryChangeEvent` от живых Remote-акторов | ✅ код · 🔬 плейтест |
| R3 | Выброшенные предметы не видны другим | `bEnableItemDrops=false`; не реализован подбор | `InventoryService.cpp` + `Actor::DropOrPickUpObject` | ✅ код · 🔬 плейтест |
| R4 | Заклинания игроков друг на друга не работают | Хардкод 1 лечения + 3 баффов | `MagicItem::IsHealingSpell/IsBuffSpell` (динамический анализ) | ✅ код · 🔬 плейтест |
| R5 | Защита союзников «Единство в битве» | 0 урона по союзникам/саммонам, лечение работает | `CoopDamagePipeline` + ally-protection | ✅ код · ⚠️ модель |
| R6 | XP квеста двоится / личный множитель применяется дважды / утечка XP при открытом меню | Дедупликация квестового XP в обоих порядках прихода; нормализация множителя | `CoopExperienceLedger.h` | 🧪 тест (`rfab_experience_ledger.cpp`, 10 кейсов — добавлено в этой ветке; поведение проверено корректным) |

## 8. Владение / авторитет (переписано в PR #17)

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| O1 | Два симулятора у NPC / ноль симуляторов / удалённые боссы | 3 механизма владения + ручная сборка перехода в ~20 местах | `Code/common/Authority/Lease.h\|cpp` (единая модель) | 🧪 тест (22 кейса) + ⚠️ модель |
| O2 | Приколоченную сущность можно было отпустить чужим пакетом | `Release`/`Handoff` не знали правила pinned | `Authority::Lease` (правило в модуле) | 🧪 тест (проверено мутацией) |
| O3 | Эпоха пира обгоняла серверную / ехала назад | Ручной инкремент эпохи | `OwnerComponent::Apply` — единственная точка | 🧪 тест + ⚠️ модель |

## 9. Протокол / wire

| # | Симптом | Первопричина | Где фикс | Статус |
|---|---|---|---|---|
| W1 | Порча анимаций/внешности у поздно зашедших | `StringCache` не реплицировался поздним игрокам; assert в release молчал | `CachedString`/`StringCache` (курсор на игрока) | 🧪 тест (`rfab_string_cache_protocol.cpp`) |
| W2 | Удалённый overflow → случайные вылеты | `String_to_VectorBool` не проверял длину пакета | `AnimationVariables` + fuzz | 🧪 тест (`rfab_wire_fuzz.cpp`) |
| W3 | Висячие TLV-view | `TlvContainerBuilder::Bytes()` возвращал новый вектор | Исправлено + тест | 🧪 тест |
| W4 | **Ложное падение петли после смены флагов** | Объекты не пересобирались при правке Makefile (только `-MMD -MP` по исходникам) | `linux-core/Makefile` (зависимость от `$(MAKEFILE_LIST)`) | 🧪 **исправлено в этой ветке** |

## 9a. Найдено ревью 2026-11-12 (`main` @ `e342a2e4`, v4.0.0)

| # | Симптом | Первопричина | Где | Статус |
|---|---|---|---|---|
| N1 | Случайные **зависания** игры (иногда затем CTD), в т.ч. при открытых меню | `ExtraDataList::lock` в SE — это движковый `BSReadWriteLock`, а клиент берёт его своим алгоритмом `BSRecursiveLock` (CAS 0→1 / tid). Если игровой поток-читатель заходит во время нашего захвата, при отпускании освобождение теряется, счётчик застревает на 1 читателе → следующий write-lock движка крутится вечно. Путь: `RunFactionsUpdates` каждые 2 с → `Actor::GetFactions` → `extraData.GetByType` **для игрока**. Апстрим нашёл и починил ровно это в `25a0049a` (#937, 27.10.2026). Вдобавок в v2.1.0 (`94e3a757`) из `GetByType` удалена проверка `ScopedExtraDataOverride`, поэтому хуки инвентаря, которые явно просят «не брать лок» (`PlayerCharacter.cpp:192`, `TESObjectREFR.cpp:1597`), всё равно его берут, даже когда движок уже держит read-lock на этом списке (кольцевой захват) | `Games/Skyrim/ExtraData/ExtraDataList.{h,cpp}`, `Games/Primitives.cpp` | ✅ код · 🔬 плейтест — портирован апстрим #937: `Games/Skyrim/BSCore/BSAtomic.{h,cpp}` (движковые `LockForRead/UnlockRead/LockForWrite/UnlockWrite`, SE-ID `66976/66982/66977/66983`, сверено с CommonLibSSE и RVA SKSE64 2.0.20), `BSSpinLock` для `BSAnimationGraphManager`/`Actor`/`TESObjectCELL`, guard `ScopedExtraDataOverride` возвращён; самописный `BSRecursiveLock` удалён. Исходники закреплены в `rfab_v401_review_stability.cpp` |
| N2 | После F9/кооп-загрузки или долгого сидения в меню у части игроков перестают синхронизироваться анимации/тинты других — до реконнекта | Надёжные пакеты, пришедшие на паузе, кладутся в очередь, а очередь (а) при переполнении в 4096 **выкидывает самый старый надёжный пакет**, (б) **очищается целиком** в `PrepareEngineForWorldLoad`. Сервер сдвигает курсор `StringCache` для игрока в момент отправки, без подтверждения, поэтому потерянный `StringCacheUpdate` уже не придёт повторно, а все следующие клиент отклоняет как «дыру» | `CoopWorldStabilizer.h:432,491`, `TransportService.cpp:139-148`, `CoordinatedSaveService.cpp` (`ClearPausedInboundPackets`), `server/Services/StringCacheService.cpp:38-45` | 🧪 тест · 🔬 плейтест — (1) надёжные пакеты больше не выбрасываются: при 8192 в очереди пауза перестаёт читать сокет (GNS буферизует сам, как в апстриме); (2) `ServerReferencesMoveRequest` на паузе **не выбрасывается**, а сливается в один запрос (последняя позиция + до 64 действий анимации на сущность) и применяется после очереди — раньше терялись надёжные `ActionEvents` после каждого меню; (3) перезагрузка мира в сессии выкидывает только мировые пакеты, сессионные (`StringCacheUpdate`, список игроков, группа, настройки) сохраняются. `PausedInboundCoalescer.h`, `CoopWorldStabilizer::MustSurviveWorldReload`, тесты `rfab_v401_review_stability.cpp`. Подтверждение от клиента для курсора `StringCache` на сервере — не делалось (нужно менять протокол) |
| N3 | Предметы на земле пропадают/дюпаются у пиров | Подбор у удалённого актёра ищет **любой** `0xFF`-реф с тем же base в радиусе 2048 ед. (~29 м) и делает ему `Disable+Delete`: под удаление попадает чужой/другой экземпляр | `Games/Skyrim/Actor.cpp` `DropOrPickUpObject` (~2232-2295) | ⚠️ смягчено · 🧪 тест — выбор теперь: точное совпадение размера стопки в радиусе 768 ед., иначе только в 256 ед., иначе ничего не удаляется (`common/DroppedItemMatch.h`). Полное решение — серверная идентичность выброшенного рефа — ❌ открыт |
| N4 | После загрузки сейва в сессии NPC/клоны иногда стартуют с чужой «последней анимацией» | `ActorExtension::SetLatestAnimation` хранит действия в глобальной карте по `formID`; при загрузке мира карта не чистится (`DetachAllActorsForEngineLoad`), а `0xFF`-ID переиспользуются движком | `Games/ActorExtension.h`, `CharacterService.cpp` | ✅ код — `ClearAllLatestAnimations()` в `CharacterService::BeginEngineWorldLoad` |
| N5 | Фризы/вылеты в StatsMenu у пользователей (issue #32); «самопроизвольно» меняются конфиги других модов | Лаунчер при **каждом** запуске переписывает `EngineFixes.toml`, `SkyrimSoulsRE.ini`, `[RFAB] Autosave.ini` в `Data/` и во всех `MO2/mods/*`, и переименовывает `BetterStealing.dll` в `.mohidden`. Вокруг одного StatsMenu сплелись 5 механизмов: правка ini SoulsRE, NOP-патчи `51638/51645`, рантайм-трамплин в код `RFAB.dll`, обёртка `UI::Register`, paused-pump | `RfabModpackCompat.h:1236-1400`, `immersive_launcher/Launcher.cpp:120-138`, `Interface/Menus/SkillsMenu.cpp`, `Interface/UI.cpp` | ❌ открыт |
| N6 | README требует невыполнимого порядка загрузки (ESP раньше ESM-флагнутого `RFAB.esp`) — issue #32 | При этом `SkyrimTogether.esp` содержит 4 новые записи и **0 оверрайдов**, `SkyrimTogetherQuestPatches.esp` (ESL) — 1 оверрайд `SCEN`; порядок по факту почти не важен | `README.md:24,44`, `Setup-RfabOnline.ps1` | ❌ открыт (документация) |
| N7 | Уже исправленные в апстриме баги продолжают жить в форке | База форка — апстрим `5a99a0b6` (28.02.2026); с тех пор в `tiltedphoques/TiltedEvolution@dev` вышло **127 коммитов**. Часть перенесена вручную (IsStillLoading, BSTSmartPointer, RemoveNullEntries, hkStepInfo), часть — нет (#937 выше). Сверки нет | весь `Code/client` | ❌ открыт (нужна сверка «коммит апстрима → есть/нет/неприменимо») |

## 10. Что НЕ проверено живой игрой (очередь на плейтест вдвоём)

Всё, что помечено 🔬, — движковая часть: хуки, 3D-граф, Papyrus VM, FaceGen. Модель и тесты
доказывают **правила**, но не то, что движковый вызов встал на нужное место. Приоритет проверки —
по `docs/PLAYTEST.md`:

1. **D1/D2** — моб не тает вдвое, чужой урон не откатывается (главное в PR #17).
2. **C1/C5/C7** — переходы локаций и reconnect без CTD и без бессмертных невидимок.
3. **A1–A5** — NPC атакуют, 2H в двух руках, нет aggro-цикла над downed.
4. **S1/S2/S4** — F5 и загрузка без порчи и без вылета.
5. **R1–R4** — алтари, кольцо, дроп, заклинания друг на друга.

---

## Как пользоваться этим реестром

- Нашёл баг в плейтесте → найди строку (или добавь) → посмотри статус. Если 🧪 — значит регресс,
  и тест должен падать; запусти петлю. Если 🔬 — приложи лог/дамп, это движковая часть.
- Чинишь → переведи строку в 🧪 (добавь поведенческий тест) или обнови место фикса.
- Не плоди новый «финальный аудит» отдельным файлом — обновляй этот реестр.
