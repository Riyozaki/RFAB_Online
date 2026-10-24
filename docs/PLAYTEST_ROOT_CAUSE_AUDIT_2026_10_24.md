# RFAB Online — Полный аудит первопричин и архитектурная стабилизация (Playtest 2026-10-24)

> **Статус базового релиза `v1.0.0`:** живой релиз `v1.0.0` (`a1965f674f40357d624c364db5aa75da77fe57fd`, 4 архива) полностью сохранён на GitHub Releases без изменений; его точный манифест и `SHA-256` контрольные суммы зафиксированы в [`docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json`](./V1.0.0_RELEASE_BACKUP_MANIFEST.json), а в `Tools/rfab/Build-CloudRelease.ps1` и `.github/workflows/release.yml` установлена жёсткая защита от перезаписи `v1.0.0` (новая стабилизированная ветка собирается в релиз `v1.1.0`).

Данный документ фиксирует результаты полного реверс-инжиниринга и аудита исходного кода клиента (`Code/client`), сервера (`Code/server`), протокола (`Code/encoding`), UI (`Code/skyrim_ui`), плагинов (`SkyrimTogether.esp`), а также бинарников и скриптов самой сборки **RFAB** (`RFAB.dll`, `FaceGenFixes.dll`, `DifficultyBalance.dll`, `BashBugFix.dll`, `HookDamage.dll`, `SkyrimSoulsRE`, `EngineFixes`, `RFAB.esp`, `RFAB_BizarreAdventure.esp`) для **Skyrim SE `1.5.97.0.8`**.

---

## 1. Приоритет №1 — «Файл сохранения поврежден и не может быть загружен» и цикл перезагрузки чекпоинта (Скриншоты 1 и 3)

### Доказанные первопричины (4 накладывающихся дефекта в `v1.0.0`)

1. **Прерывание `void`-функции `BGSSaveLoadGame::SaveGame` ПОСЛЕ открытия и обнуления `.ess`-файла (`Code/client/Games/Skyrim/SaveLoad.cpp:102-113`):**
   - В `v1.0.0` перехватчики `HookSaveCallSite1` (`0x00586DE0 + 0x18B`) и `HookSaveCallSite2` (`0x005875F0 + 0x138`) перехватывали внутренний `E8`-вызов `BGSSaveLoadGame::SaveGame` (`ID 34676`, `0x0057CCC0`), имеющий тип возврата **`void`**.
   - К моменту входа в `+0x18B` / `+0x138` внешние обёртки `Save_Impl` (`ID 34818`, `0x586DE0`) и `SaveGame2` (`ID 34823`, `0x5875F0`) **уже создали файл `<SaveName>.ess.tmp` / `<SaveName>.ess` на диске**.
   - Когда `RunSaveCallSiteCommon` делал `if (!coord.HandleEngineSaveAttempt(saveName)) return;` без вызова `apOriginal`, внешняя функция не знала об отмене (возврата `bool` нет!) и штатно закрывала/переименовывала **пустой/обрезанный `.ess`-файл без заголовка `TESV_SAVEGAME`**.
   - Любое перехваченное сохранение (`F5`, сохранение через меню, несинхронный автосейв `RFAB.dll`) оставляло на диске битый `.ess`-файл, при попытке чтения которого Skyrim выдавал модальное окно **«Файл сохранения поврежден и не может быть загружен.»** (Скриншот 1).

2. **Конфликт `ExecuteConsoleSaveOrLoadCommand` с `SkyrimSoulsRE` (`SaveHandler.cpp`) и суффиксом `.ess`:**
   - `BGSSaveLoadManager::SaveNamed` и `LoadNamed` выполняли консольные команды `save "<slot>"` / `load "<slot>"` через `Script::CompileAndRun` (`ID 21416`).
   - В модпаке RFAB установлен `SkyrimSoulsRE`, чей `SaveHandler::SaveGame_Hook` перехватывает `Console::SaveGameHandler + 0xC4`, сохраняет имя слота, ставит отложенную задачу `QueueSaveLoadTask(0xF0004000)` (`ID 34859`) на следующий тик `ProcessEvents` (`ID 34862`) и немедленно возвращает `true`.
   - В результате `ScopedSaveLoadAllowance` в `ExecuteAuthorizedSave` успевал разрушиться **до** фактической записи сейва на следующем кадре, `OnNotifyPrepareSave` преждевременно отправлял серверу `CoordinatedSaveAck{Success = true}`, а поскольку контекст движка `apUnk0[0xBB0 / 8]` передавал имя файла **с расширением `.ess`** (`"RFAB_MP_..._0.ess"`), строгое сравнение без нормализации суффикса (`fileName == m_authorizedPendingSaveSlot`) возвращало **`false`** и обрезало в 0 байт даже сам групповой чекпоинт `RFAB_MP_*`!
   - Аналогично при загрузке чекпоинта (`NotifyExecuteLoad`) `HandleEngineLoadAttempt("RFAB_MP_..._0.ess")` получал `false` из-за суффикса `.ess`, блокировал собственную загрузку и, видя префикс `RFAB_MP_`, отправлял обратно серверу `RequestCoordinatedSaveLoad` («Общая загрузка чекпоинта для всех игроков...»), на что сервер снова отвечал `NotifyExecuteLoad` («Синхронная загрузка чекпоинта группы») — порождая бесконечный пинг-понг каждые `1.5` с (буквально две чередующиеся строки на Скриншоте 3!), а флаг `aCheckForMods = true` вызывал блокирующее модальное окно **«Это сохранение использует контент, в настоящее время отсутствующий...»**.

3. **Порча таблицы `ChangeForms` динамическими `0xFFxxxxxx` клонами (`Forms.cpp:103` и `Actor.cpp:191,197`):**
   - `TESForm::SetSkipSaveFlag(true)` в `Forms.cpp:103` записывал **`unk10 = 0xFFFF`** (`TESForm::inGameFormFlags` по смещению `+0x18`), выставляя одновременно биты `kWantsDelete | kForcedPersistent (0x02) | kRefOriginalPersistent (0x40) | kRefPermanentlyDeleted (0x80)` вместо флага `RecordFlags::kTemporary` (`1 << 14` в `TESForm::flags`).
   - Сразу после этого `Actor::Create` вызывал **`pActor->MarkChanged(0x40000000)`**, принудительно регистрируя временного сетевого `Character` (`0xFF000xxx`) в `BGSSaveLoadGame::changeForms`, тогда как его базовый `TESNPC` (`0xFF000yyy`, созданный в куче через `IFormFactory::Create<TESNPC>()`) в `.ess` никогда не сериализуется.
   - При этом `PreSavePurgeDynamicActors()` вызывал `pActor->Delete()` синхронно прямо внутри `Save_Impl` за наносекунды до сериализации, не снимая `ChangeFlags` (`UnsetChanged(0xFFFFFFFF)`) и не давая главному циклу движка ни одного кадра на `GarbageCollection`. Попадание хотя бы одной записи `ACHR` с несуществующим `baseForm = 0xFF000yyy` в `.ess` делает сохранение нечитаемым при загрузке (`Character::LoadGame` -> `TESForm::GetById` == `nullptr` -> «Файл сохранения поврежден»).

4. **Неисправленный `[RFAB] Autosave.ini` (`Save after combat = 1`):**
   - В `drake1ch/RFAB` конфиг `SKSE/Plugins/[RFAB] Autosave.ini` использует ключи `Enable autosave = 0` и **`Save after combat = 1`**, тогда как `Setup-RfabOnline.ps1` и `patch_rfab_modpack_compat.py` искали несуществующий ключ `Enabled = true`. После каждого боя `RFAB.dll` запускал `_0_RFABAutoSave`, который обрезался в `HookSaveCallSite2`.

### Чистое архитектурное решение
- **Полностью удалены** внутренние `void`-перехватчики `HookSaveCallSite1` и `HookSaveCallSite2`. Ни один `.ess`-файл больше никогда не прерывается после открытия дескриптора.
- Блокировка несанкционированных сохранений/загрузок выполняется строго **на внешних точках входа до создания файлов**: `HookQueueSaveLoadTask` (`ID 34859`), `HookSaveImpl` (`ID 34818`), `HookSaveGame2` (`ID 34823`), `HookLoadImpl` (`ID 34819`) и `HookLoadCallSite` (возвращающий `bool` до `kMessage_PreLoadGame`). Сопоставление имён слотов нормализует регистр, пути и суффикс `.ess`.
- **Прямое выполнение через `BGSSaveLoadManager::ProcessEvents` (`ID 34862`, аналогично `skse64`):** консольный `CompileAndRun("save/load ...")` полностью убран. При получении `NotifyPrepareSave` клиент:
  1. На кадре $T$ вызывает `PreSavePurgeDynamicActors()`, который для всех временных `0xFFxxxxxx` сетевых клонов вызывает `UnsetChanged(0xFFFFFFFF)`, очищает `inGameFormFlags` и удаляет референсы;
  2. Ждёт **2 полных кадра `ProcessEvents`**, чтобы `GarbageCollection` движка гарантированно завершил очистку ячеек и `BGSSaveLoadGame`;
  3. Внутри `HookProcessEvents` (`ID 34862`) под `ScopedSaveLoadAllowance` напрямую вызывает `RealSaveImpl(apThis, 2, 0, slotName.c_str())`;
  4. Проверяет на диске, что `<slotName>.ess` существует, имеет размер `>= 256` байт и валидный заголовок **`TESV_SAVEGAME`**, и **только после этого** отправляет `CoordinatedSaveAck{Success = true}` и восстанавливает удалённых клонов на следующем кадре.
- При синхронной загрузке в `HookProcessEvents` вызывается `RealLoadImpl(apThis, slotName.c_str(), -1, 0, /*aCheckForMods=*/false)` — без модального окна отсутствующего контента и без перехвата со стороны `SkyrimSoulsRE`.
- Исправлены `TESForm::SetSkipSaveFlag` (выставляет `flags |= (1u << 14)`, не трогая `unk10`) и `Actor::Create` (убран `MarkChanged(0x40000000)`, добавлен `UnsetChanged(0xFFFFFFFF)`).
- В `EncounterService` на сервере добавлена защита от повторной рассылки `NotifyExecuteLoad` для одного и того же вайпа/эпохи (устранён цикл спама чекпоинта).
- При старте клиента и в установщике `Setup-RfabOnline.ps1` старые 0-байтные/обрезанные `.ess`-файлы без сигнатуры `TESV_SAVEGAME` аккуратно переносятся в резервную подпапку `Saves/Quarantined_Corrupt_v100/`.
- В `Setup-RfabOnline.ps1` и `patch_rfab_modpack_compat.py` отключаются оба ключа `Enable autosave = 0` и `Save after combat = 0` в `[RFAB] Autosave.ini`.

---

## 2. Нокаут («Downed-on-Knee»), фонтан чисел `205`/`349` и вращение камеры (Скриншот 3)

### Первопричины
1. **`Actor::ForceActorValue` принимает целевое значение, а не дельту (`Actor.cpp:753` vs `DownedStateService.cpp:136,332,666`):**
   - `Actor::ForceActorValue` внутри сам вычисляет `aValue - initialValue`.
   - `DownedStateService` передавал `1.0f - curHp` вместо `1.0f`. При `curHp = -204` первый тик ставил HP в `1 - (-204) = +205` (зелёные `205` в `FloatingDamageV2.dll`), а следующий тик ставил `1 - 205 = -204` (`∆ = -409` / красные `349`), порождая бесконечный фонтан огромных зелёных и красных цифр урона/исцеления каждый кадр!
2. **Каждые `0.85` с повторно вызывались `SetWeaponDrawnEx(false)` и `SendAnimationEvent("bleedoutStart")` (`DownedStateService.cpp:614-621`):**
   - Постоянный сброс графа анимации в `bleedoutStart` и повторное убирание оружия раз в `0.85` с срывали камеру 3-го лица во вращение и вызывали спам системного уведомления Skyrim/RFAB `«Вы не можете продолжать бой.»`.

### Решение
- Все вызовы `ForceActorValue(..., kHealth, ...)` в `DownedStateService` передают **точное целевое значение** (`1.0f` при удержании на колене и только если `std::abs(curHp - 1.0f) > 0.25f`; `reviveHp` при поднятии).
- Вход в анимацию `bleedoutStart` и убирание оружия выполняются **однократно** при переходе в состояние нокаута (с мягкой перепроверкой только при перезагрузке 3D-модели или реальном выходе из bleedout-состояния спустя `>= 4.0` с).

---

## 3. Фиолетовое лицо локального персонажа (Скриншот 4) и визуальный баг / HUD после смены локации (Скриншот 2)

### Первопричины
1. **Мутация общего кэшированного `BSMaskedShaderMaterial` in-place (`FaceGenSystem.cpp:87-122`):**
   - В сборке RFAB активен плагин `FaceGenFixes.dll`, пересобирающий шейдерные материалы оттенков кожи (`BSLightingShaderMaterialFacegen`, RTTI `NiMaskedShaderRTTI`).
   - Движок Skyrim кэширует экземпляры `BSShaderMaterial` в глобальной хэш-таблице материалов. Когда `FaceGenSystem::Update` создавал текстуру с пустым именем `BSFixedString("")` и напрямую перезаписывал `pMaterial->renderedTexture = pTexture` на существующем `pLightingShader->material` без клонирования материала через `BSShaderProperty::SetMaterial(..., true)`, общий материал получал фиолетовую `TintMask`-текстуру друга-аргонианина и окрашивал голову локального норда в ярко-пурпурный цвет (Скриншот 4)!
2. **Потеря FaceGen/экипировки после смены локации и раздутый блок имени в HUD группы (Скриншот 2):**
   - После перехода между локациями движок пересоздаёт 3D-граф (`NiNode` / `BSFaceGenNiNode`) удалённого игрока, но `aFaceGenComponent.Generated` оставался `true` навсегда, поэтому тинты и шейдер головы/тела не применялись к новому `NiNode`.
   - В `Code/skyrim_ui/src/app/components/group/group.component.scss` имя участника группы (`2 👑 хочу есть`) не имело `white-space: nowrap` и ограничения ширины с `text-overflow: ellipsis`, из-за чего русское имя с пробелом переносилось на две строки с крупным кеглем поверх экрана.

### Решение
- В `FaceGenSystem::Update`:
  - Текстура тинтов создаётся с уникальным именем `STR_FaceTint_<FormID>`.
  - Шейдерный материал клонируется по эталонному паттерну `RaceMenu` (`skee64/TintMaskInterface.cpp`): `pOldMaterial->Create()` -> `CopyMembers(pOldMaterial)` -> установка `renderedTexture` -> вызов **`BSShaderProperty::SetMaterial(pNewMaterial, true)` (`SE ID 98897`, `0x1291D40`)** + **`BSLightingShaderProperty::InitializeShader(pTriBasedGeom)` (`SE ID 99862`, `0x12C56D0`)** -> освобождение временного `pNewMaterial`. Локальный игрок (`0x14`) полностью изолирован от чужих `TintMask`.
  - В `FaceGenComponent` отслеживается указатель на актуальный `BSFaceGenNiNode*`: при пересоздании 3D-модели после смены локации тинты автоматически применяются заново, а удалённый персонаж обновляет экипировку после загрузки 3D.
- В `group.component.scss` и собранном бандле `dist_ready/RFAB_Online_MO2/SkyrimTogetherReborn/UI/` добавлены `white-space: nowrap`, компактный `max-width` и `text-overflow: ellipsis`.

---

## 4. Алтари богов и Даэдра из RFAB не работают

### Первопричины
1. **5 устаревших loose-скриптов `.pex` в `dist_ready/RFAB_Online_MO2/scripts/` перебивали скрипты RFAB в VFS Mod Organizer 2:**
   - Мод `RFAB_Online` устанавливается с высоким приоритетом в MO2 и содержал ванильный/STR-скрипт `scripts/TempleBlessingScript.pex` (а также `C00TrainerScript.pex`, `HallofStoriesDiskScript.pex`, `HallofStoriesKeyholeScript.pex`, `ResourceFurnitureScript.pex`).
   - В RFAB именно кастомный `TempleBlessingScript.pex` (через `PapyrusIniManipulator` и `[RFAB] Blessings.ini`) выдаёт благословения всех 18 алтарей Аэдра и Даэдра! Перекрытие этого `.pex` полностью ломало алтари.
2. **Сетевая ретрансляция активации алтарей (`ObjectService.cpp`):**
   - В `RFAB_BizarreAdventure.esp` скрипт универсального алтаря `RFAB_BA_UniversalShrine.psc` вызывает `OpenBlessingMenu()` в `OnActivate(ObjectReference akActionRef)` **без проверки `akActionRef == Game.GetPlayer()`**. При сетевой ретрансляции `Activate` на алтарь меню благословений открывалось у второго игрока или сбивало локальную активацию.
3. **Грязные оверрайды в `SkyrimTogether.esp`:**
   - Плагин `SkyrimTogether.esp` содержал ненужные ванильные оверрайды перков двуручного оружия `Sweep` (`0x0003AE9F`, `0x0003AEA2`, `0x0003AEA3`) и NPC `Ancano` (`0x0001C48E`).

### Решение
- Удалены все 5 конфликтующих loose `.pex`/`.psc` скриптов из `dist_ready/RFAB_Online_MO2/scripts/` (плюс добавлена автоматическая зачистка и проверка в `Package-Release.ps1` и `Setup-RfabOnline.ps1`), что возвращает оригинальную механику всех алтарей Аэдра и Даэдра из RFAB.
- В `ObjectService` отключена сетевая пересылка активаций алтарей/святилищ/камней-хранителей (благословения персональны для каждого игрока).
- Из `GameFiles/Skyrim/SkyrimTogether.esp` и `dist_ready/RFAB_Online_MO2/SkyrimTogether.esp` вычищены ванильные оверрайды `0x3AE9F/A2/A3` и `0x1C48E`.

---

## 5. Удар щитом (~300 чистого урона), баланс сложности и расчёт урона / HP

### Доказанные первопричины (дизассемблирование `RFAB.dll` + `DifficultyBalance.dll` + `Actor.cpp`)
1. **Дефолтная сложность `uDifficulty = 4` (`Мастер`) в `GameServer.cpp` и `STServer.ini`:**
   - В `RFAB.esp` + `DifficultyBalance.dll` эталонной `1.0x / 1.0x` сложностью RFAB является **Адепт (`uDifficulty = 2`, `fDiffMultHPByPCN = 1.0`, `fDiffMultHPToPCN = 1.0`)**, тогда как на `Мастере` (`4`) RFAB выставляет **`fDiffMultHPByPCM = 0.50`** (режет весь урон оружия и магии игроков **вдвое**) и **`fDiffMultHPToPCM = 2.00`** (удваивает входящий урон по игрокам!). В сочетании с `×3 HP` врагов на 2 игроков обычное оружие требовало в **6 раз** больше ударов, чем задумано в RFAB!
2. **Почему удар щитом наносил ровно `~300` урона по врагам, принадлежащим хосту (`RFAB.dll` `0x1800A471A`–`0x1800A4807` + `Actor.cpp:723,1172`):**
   - В `RFAB.dll` хук `Actor::ProcessHit` (`0x1800A4380`) при `HitData.flags & kBash` по неблокирующей цели записывает в `HitData.physicalDamage` (`+0x54`) базовый урон баша щитом `100.0` (`BashPower_*Shield_Damage`) плюс `+50.0` за каждый перк ветки Блокирования (`PowerfulBashes`, `OverpoweringBashes`, `DisarmingBash`, `Champion`) = **`250`–`300` физ. урона ДО расчёта брони (`CalculateArmorReduction` `0x18007B3D0`)**.
   - В кооперативе лидер группы (`хочу есть`) владел почти всеми NPC в локации. Когда второй игрок (воин со щитом, `IsRemote() == true` для этих NPC) бил врага, `HookDamageActor` запускал расчёт попадания **локально на клиенте атакующего**, где:
     - `HookAddTarget` (`MagicTarget.cpp:181`) блокировал все способности/ауры брони RFAB (`ArmorFFSelf*`, `PerkArmorFFSelf*`, `RFAB_Spell_*AutoCloak`) на удалённых NPC;
     - `Actor::GetEssentialActorValues()` (`Actor.cpp:723`) сериализовал **только `Health`, `Stamina`, `Magicka`**, никогда не передавая **`kDamageResist` (броню `39`)** и резисты стихий/магии (`40..44`)!
   - В результате на клиенте не-владельца удалённые враги имели **нулевую способность-броню**, и `250`–`300` физ. урона баша проходили без снижения бронёй RFAB (и без штрафа `0.5x` оружия)!
3. **Тройное умножение / деление максимального HP при передаче владения NPC (`CharacterService.cpp:524,574` и `ActorValueService.cpp:97`):**
   - `RequestServerAssignment` снимал **немасштабированное** `InitialActorValues` до вызова `OnLocalComponentAdded`. На не-владельце `SeedUnscaledMaxHealthFromRemote` делил немасштабированное HP на `3` (`150 / 3 = 50`), а при перехвате владения уже отмасштабированным NPC умножал `450 * 3 = 1350` (`×9 HP`)!

### Решение
- Стандартная сложность сервера (`GameServer.cpp`, `STServer.ini` и авто-миграция в `Setup-RfabOnline.ps1`) переведена на **`uDifficulty = 2` (Адепт = эталон RFAB `1.0x / 1.0x`)**.
- В `GetEssentialActorValues()`, `BroadcastActorValues()` и `OnActorValueChanges()` добавлена синхронизация боевых защитных характеристик NPC (`kDamageResist`, `kPoisonResist`, `kFireResist`, `kElectricResist`, `kFrostResist`, `kMagicResist`) через `ForceMode::PERMANENT`, благодаря чему `CalculateArmorReduction` в `RFAB.dll` на клиенте атакующего учитывает реальную броню и резисты врага при ударах оружием, щитом и магией.
- Масштабирование максимального HP NPC привязано к идемпотентному нативному базовому значению `ActorExtension::NativeBaseMaxHealth` (снимается один раз до любых сетевых модификаций `PERMANENT`), что исключает `×9 HP` и деление базы на `3` при любых передачах владения.

---

## 6. Мгновенная поломка «Кольца авантюриста» (`0x06112812`) при надевании

### Доказанные первопричины (`RFAB_ExperienceRing.psc` + `RFAB.dll` `0x18009DFF0` + `InventoryService.cpp`)
1. Скрипт `RFAB_ExperienceRing.psc` на `Кольце авантюриста` запоминает `_owner` в `OnEquipped` и вызывает `Break()` (`RemoveItem(SelfBase, 1)` + звук разрушения) при любом `OnUnequipped`.
2. В `RFAB.dll` (`0x18009E01B`) механика ношения **двух колец** работает **только для `PlayerCharacter` (`0x14`)**, тогда как клон игрока `RemotePlayerA` (`0xFF000xxx`) на клиенте друга имеет только 1 слот кольца. Кроме того, `InventoryService::OnNotifyEquipmentChanges` (`InventoryService.cpp:238-256`) при надевании **любого** предмета брони снимал (`UnEquip`) **всю надетую броню и кольца** с `RemotePlayerA` и надевал заново!
3. На клиенте друга `RemotePlayerA` тут же получал `OnUnequipped` -> `RFAB_ExperienceRing.Break()` -> вызывал `RemotePlayerA.RemoveItem(RFAB_Ring_Adventurer, 1)` из потока Papyrus VM (вне `ScopedInventoryOverride`).
4. Ни `HookRemoveInventoryItem` (`TESObjectREFR.cpp:1045`), ни `InventoryService::OnInventoryChangeEvent`, ни серверный `InventoryService::OnInventoryChanges` **не проверяли владение живым `Actor`!** Клиент друга отправлял серверу `RequestInventoryChanges{ServerId = PlayerA, Count = -1}`, сервер пересылал `NotifyInventoryChanges` обратно владельцу, и **клиент самого Игрока A удалял и ломал кольцо прямо у него на пальце сразу после экипировки**!

### Решение
- В `HookAddInventoryItem`, `HookRemoveInventoryItem`, `HookActorAddInventoryItem` и `InventoryService::OnInventoryChangeEvent` запрещена генерация `InventoryChangeEvent` от живых `Remote`-акторов (Papyrus-скрипты на чужих клонах больше никогда не могут мутировать инвентарь владельца; лут мёртвых тел и контейнеров мира полностью сохранён).
- На сервере (`Server::InventoryService`) добавлена проверка владельца живого персонажа для `OnEquipmentChanges` и не-лутовых `OnInventoryChanges`.
- В `InventoryService::OnNotifyEquipmentChanges` убрано тотальное снятие всей брони: снимаются только предметы с реально пересекающимся `TESObjectARMO::slotType`, а невидимые скриптовые кольца вроде `RFAB_Ring_Adventurer` (`0x112812`) и вторые кольца RFAB не вызывают снятия колец на `RemotePlayer`.

---

## 7. Выброшенные в мир предметы не видны другим игрокам

### Первопричины
- В `Code/server/Services/InventoryService.cpp:14` параметр `Gameplay:bEnableItemDrops` по умолчанию имел значение **`false`** (и отсутствовал в `STServer.ini`), поэтому сервер принудительно обнулял `notify.Drop = false`.
- В `Actor::DropOrPickUpObject` (`Actor.cpp:1350`) была не реализована ветка подбора выброшенного временного предмета (`// TODO: pick up`), из-за чего при включении дропа подбор предмета вторым игроком оставлял бы копию лежать на земле у первого.

### Решение
- `Gameplay:bEnableItemDrops` включён по умолчанию (`true`) в коде сервера, в `STServer.ini` и при обновлении через `Setup-RfabOnline.ps1`.
- Реализована полная синхронизация цикла «выбросил на землю -> друг увидел и подобрал -> временный `0xFFxxxxxx` объект исчез с земли у всех игроков без дюпа».

---

## 8. Заклинания игроков друг на друга не работают

### Первопричины
- В `MagicItem.cpp:62-84` функции `IsHealingSpell()` и `IsBuffSpell()` проверяли захардкоженный список из **1 ванильного эффекта лечения (`0x1CEB0`)** и **3 ванильных заклинаний (`Courage`, `Rally`, `CallToArms`)**!
- В `MagicTarget.cpp:120,145` все остальные дружественные заклинания на удалённых/локальных игроков отбрасывались (`return false`), блокируя 99% заклинаний лечения, баффов, щитов и диспэлов из RFAB.
- В `MagicService.cpp:74` потоковые заклинания лечения пропускали `SpellCastRequest`, не воспроизводя визуал и эффект каста.

### Решение
- `MagicItem::IsHealingSpell()` и `MagicItem::IsBuffSpell()` переписаны на динамический анализ `EffectItem` / `EffectSetting`: любое невраждебное (`!IsHostile()` / не-`Detrimental`) заклинание, восстанавливающее здоровье или накладывающее положительный эффект на цель/область/касание, корректно распознаётся и синхронизируется между игроками (включая потоковые касты).

---

## 9. Призванные и поднятые существа работают не все

### Первопричины (анализ всех 266 заклинаний призыва `RFAB.esp` + `RFAB_BizarreAdventure.esp`)
1. **Синхронизация гибридных/скриптовых призывов RFAB на клон заклинателя (`MagicTarget.cpp:69` + `MagicService.cpp:455`):**
   - В RFAB духовные/спектральные призывы (`Summon Spectral Arrow/Warrior`, `RFAB_Script_SummonSpirit`, `RFAB_CastSpellOnEffectStart_Summon`, призывы с `REQ_Magic_Toggle` и `Mirel_REUS_RemoveItemSummon*`) имеют нулевой эффект архетипа `kScript (1)`, а сам `kSummonCreature (18)` идёт вторым эффектом или вызывается дочерним `Spell.Cast(akCaster)`.
   - `AddTargetData::ShouldSync()` проверял только один `pEffect`, пересылал обёртку на `RemotePlayerA` у второго игрока, там срабатывал `REQ_Magic_FinishSpell` / `Dispel`, а `MagicService::OnRemoveSpellEvent` (не имевший проверки `if (pActor->GetExtension()->IsRemote()) return;`) отправлял `RemoveSpellRequest` обратно Игроку A и **мгновенно убивал его только что призванного духа**!
2. **Задержка `commandingActor` и реанимация трупов (`kReanimate = 22`) (`CharacterService.cpp:200,736`):**
   - `OnActorAdded` регистрировал саммона до того, как `SummonCreatureEffect` выставлял `commandingActor = 0x14` (`IsPlayerSummon == false`).
   - При поднятии мёртвого тела (`kReanimate`) труп уже был зарегистрирован на сервере с `IsDead = true` и `IsPlayerSummon = false` — переход в живого саммона игрока не отслеживался.
   - На клиенте друга `OnCharacterSpawn` (`CharacterService.cpp:736`) прописывал чужому саммону `commandingActor = PlayerCharacter::Get()` (`0x14`), забивая лимит призыва второго игрока!

### Решение
- `AddTargetData::ShouldSync()` проверяет **весь список эффектов `MagicItem`** (а также скриптовые обёртки призыва/тогглов) и никогда не реплицирует эффекты самопризыва на `Remote`-клонов; в `MagicService::OnRemoveSpellEvent` добавлена защита `if (pActor->GetExtension()->IsRemote()) return;`.
- В `CharacterService` добавлено отслеживание отложенного появления `commandingActor == 0x14` и оживления трупов через `kReanimate` (`IsDead: true -> false`), а на удалённых клиентах чужим саммонам выставляется `commandingActor` их реального владельца (`RemotePlayer`), не затрагивая лимит призыва локального `0x14`.

---

## 10. Опыт за врагов и прогрессия уровней в RFAB

### Первопричины (верифицировано по `RFAB_XP_Handler.psc:90-145` и `RFAB_XP_Alias.psc`)
1. **Двойной пул опыта RFAB (`RFAB_XP_Amount` + `pSkills->data->xp`) и утечка траты очков в `RFAB_Menu`:**
   - В `RFAB_XP_Handler.psc:90-93` функция `ModXP(aiXP)` начисляет опыт одновременно в `RFAB_XP_Amount` (`0x00AD45BB` — персональный кошелёк очков прокачки для `RFAB_Menu` / `CustomMenu`) и в `pSkills->data->xp` (шкала уровня персонажа).
   - Поскольку трата очков в `RFAB_Menu` индивидуальна для каждого игрока, `RFAB_XP_Amount` **никогда не должен** синхронизироваться сырым значением через `GlobalValueSync` (иначе открытие меню одним игроком перезаписывает кошелёк второго), а при открытом `CustomMenu` возвраты/перераспределение очков внутри меню не должны транслироваться в группу как новый боевой опыт.
2. **Персональный множитель опыта `RFAB_XP_Multiplier` (`0x00112815`):**
   - `RFAB_XP_Handler::CalculateXP(aiXP)` умножает базовый опыт врага на персональный `RFAB_XP_Multiplier` убийцы (изменяемый «Кольцом авантюриста», Камнями-хранителями и бонусами расы/знака) и округляет вниз до кратного `5` (`iMultiplied - (iMultiplied % 5)`).
   - Раньше в сеть уходило уже умноженное значение убийцы, из-за чего напарник без кольца получал чужой бонус (или терял свой собственный), а при дублирующемся срабатывании `OnActorKilled` на не-владельце срабатывал откат опыта.

### Решение
- `RfabAdapterService` нормализует исходящую дельту опыта делением на локальный `RFAB_XP_Multiplier` отправителя перед отправкой на сервер и умножает входящую базовую долю опыта на персональный `RFAB_XP_Multiplier` получателя с точным округлением RFAB до кратного `5`, начисляя дельту и в `RFAB_XP_Amount`, и в `pSkills->data->xp`.
- `RFAB_XP_Amount`, `RFAB_XP_Multiplier` и `RFAB_XP_Bonus` жёстко исключены из `GlobalValueSync` как на уровне `RfabModpackCompat.h` и `RfabAdapterService::OnGlobalValueSync`, так и в обоих манифестах `RFAB_Online_Bindings.json`.
- При открытом `CustomMenu` (`RFAB_Menu`) подавляется отправка локальных изменений кошелька XP, а `CharacterService::KillActorSuppressingDuplicateXp` выставляет `ForceActorValue(kInfamy, -1.0f)` до вызова `Kill()` на удалённых клиентах, предотвращая двойное срабатывание `RFAB_XP_Alias::OnActorKilled`.

---

## 11. Синхронизация анимаций, интерполяция и общая стабильность

### Первопричины и решения
1. **Очередь анимаций `AnimationSystem::Update` (`AnimationSystem.cpp`):** раньше обрабатывалось не более **1 события анимации за кадр**, из-за чего быстрые серии ударов MCO/RFAB, блок, баш и уклонения накапливали многосекундную задержку. Теперь за кадр дренируется до 8 причинно-следственных переходов с автоматическим сбросом устаревшего хвоста (>16 событий старше 2 секунд).
2. **Перезапись дискретных переменных графа старыми снапшотами (`InterpolationSystem.cpp`):** интерполятор позиций с задержкой 200 мс перезаписывал булевы/целочисленные флаги комбо и стоек `LastRanAction` устаревшими значениями. Теперь дискретные `Booleans`/`Integers` берутся строго из актуального `LastRanAction`, а `Floats` (`SpeedSampled`, углы смешивания) и `Direction` (`DeltaAngle`) плавно интерполируются между точками.
3. **Трансляция `ActionEvent::TargetId` (`AnimationSystem.cpp` + `ActionEvent.cpp`):** локальные `FormID` целей ударов и парных анимаций упаковываются с сетевым маркером `0xFF800000u | serverId` при сериализации и безопасно разрешаются обратно в локальный `Actor*` на клиентах получателей.

---

## 12. Сводка релиза `v1.1.0` и сохранность `v1.0.0`

- **Защита рабочего релиза `v1.0.0`:** манифест релиза и SHA-256 всех 4 архивов сохранены в `docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json`; `Tools/rfab/Build-CloudRelease.ps1` содержит жёсткий предохранитель, запрещающий перезапись тега `v1.0.0`, а `.github/workflows/release.yml` и `Tools/rfab/Package-Release.ps1` собирают и публикуют новый релиз `v1.1.0`.
- **Чистка `SkyrimTogether.esp` и скриптов:** удалены 5 конфликтующих ванильных `.pex`-скриптов (`TempleBlessingScript.pex`, `C00TrainerScript.pex`, `ResourceFurnitureScript.pex`, `PetFramework_ParentQuestScript.pex`, `petframework_petquest.pex`) и 4 грязных оверрайда ванильных записей из `SkyrimTogether.esp` (`6420` -> `1783` байт, `HEDR 1.7000`, `numRecords = 10`). Проверки закреплены в `Tools/rfab/verify_release_bundle.py`, а `Setup-RfabOnline.ps1` удаляет устаревшие `.pex` при `Quick_Update` с `v1.0.0`.
- **Верификация SE `1.5.97.0`:** `Tools/rfab/verify_se1597_port.py --addrlib version-1-5-97-0.bin --strict --strict-migrated` подтверждает **3103 / 3103 (100%)** вызовов Address Library IDs без единого AE-остатка.

