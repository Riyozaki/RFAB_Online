# Спецификация низкоуровневого порта на Skyrim SE 1.5.97.0.8

> **Целевой бинарник:** `SkyrimSE.exe 1.5.97.0.8` (Steam / GOG / Downgrade — любой дистрибутив с PE-версией `1.5.97.0`)
> **SKSE64:** `2.0.20` (`skse64_1_5_97.dll`)
> **Address Library:** `version-1-5-97-0.bin` (`Format = 1`, `PointerSize = 8`, `778 674` записей)
> **Верификационные источники истины:**
> 1. `version-1-5-97-0.bin` (`778 674` точных пар `SE_ID -> RVA`)
> 2. `NetScriptFramework.SkyrimSE.1_5_97_0.bin` из сборки `drake1ch/RFAB` (`16 656` C++-типов с точными `sizeof`/полями, `187 817` функций `[Begin..End]`, `590 857` глобалов/RTTI)
> 3. `Vermunds/SkyrimSoulsRE` тег `2.1.3` (последний официальный релиз под `1.5.97` до AE-порта `828b9dc`)
> 4. До-AE коммит `9d78319c` в истории `TiltedEvolution` + база бинарного сопоставления `se_ae.csv` / Diaphora (`offsets-1-6-318-0.csv` ↔ `1.5.97`)

---

## 1. Полная таблица Address Library ID (`3 083 / 3 083` = `100%`)

Файл [`docs/se-1597-id-map.csv`](./se-1597-id-map.csv) содержит **100% всех `3 083` уникальных идентификаторов** `POINTER_SKYRIMSE` / `VersionDbPtr` / `RTTI` в кодовой базе `Code/`, где **каждая запись проверена на точное вхождение и совпадение `RVA` с `version-1-5-97-0.bin`** (`Tools/rfab/verify_se1597_port.py --addrlib ...` даёт `0` пропусков и `0` ошибок).

### Важные нетривиальные исправления, выявленные перекрёстным бинарным аудитом

| AE ID | SE 1.5.97 ID | RVA (`1.5.97`) | Файл | Почему автоматические эвристики ошибаются |
|---:|---:|---|---|---|
| `77226` *(вызов #1, `initWindowLoc`)* | **`75591`** | `0xD71F00` (`+ 0xA9 + 1 = 0xD71FAA`) | `BSGraphicsRenderer.cpp:82` | В AE `1.6.x` компилятор заинлайнил `BSGraphics::InitWindows` внутрь `Renderer::Init` (`77226`), а в `1.5.97` это **две разные функции**: `InitWindows` = **`75591`**, `Renderer::Init` = **`75595`**. Наивная замена `77226 -> 75595` затирает машинный код `Renderer::Init + 0xAA` константой `WS_OVERLAPPEDWINDOW` и роняет игру до создания окна! |
| `77226` *(вызов #2, `renderInit`)* | **`75595`** | `0xD72810` (`+ 0x0`) | `BSGraphicsRenderer.cpp:94` | Истинная точка входа `BSGraphics::Renderer::Init` в `1.5.97`. |
| `400180` | **`692121`** | `0x1EBCDC8` | `RTTI.cpp:5617` | В `1.5.97` класс назывался `BSGameDataSystemUtility` (переименован в `BSWin32GameDataSystemUtility` в AE). Сосед по интервалу `(692135..692137)` — `692136` — это **чужой RTTI** `BSMsgDialogSystemUtility`! |
| `210860` | **`548568`** | `0x1653DF0` | `RTTI.cpp` / VTable | Исправляет опечатку `548856` в `CommonLibSSE-NG` (`VTABLE_CombatInventoryItemStaff_0`). |
| `104655` | **`97929`** | `0x1250F60` | `BSScript.h:274` | `BSScript::NF_util::NativeFunctionBase::dtor` (`0x1250F60`), а **не** scalar deleting destructor `97927` (`0x1250ED0`), который при случайном младшем бите `RDX` вызывает `free(this)`. |
| `36548` | **`35548`** | `0x5AE010` | `UI.cpp:132` | `Main::Init` (пропуск вступительного ролика). |
| `37717` | **`36707`** | `0x5FA750` | `Actor.cpp:823` | `Actor::SetPlayerTeammate`. |
| `392578` | **`513937`** | `0x1E153E8` | `RTTI.cpp:613` | `RTTI_TESSoundFile`. |

---

## 2. Полная ревизия C++ Layout / Struct Offsets под `1.5.97.0.8`

В обновлении `1.6.629` Bethesda добавила виртуальный деструктор (`vftable`, `+8` байт) в `BSExtraDataList` и расширила `BSAnimationGraphManager` и `CombatController`. Ветка `main` (`4cba32cd`) откатила `ExtraDataList`, `TESObjectREFR`, `Actor`, `PlayerCharacter`, `TESObjectCELL` и `TESLeveledList`, **но пропустила ДВЕ КРИТИЧЕСКИЕ СТРУКТУРЫ**, гарантирующие краш в бою и при кастомных анимациях RFAB:

### 2.1. Структуры, уже приведённые к `1.5.97` в `main` (подтверждены по `NetScriptFramework.SkyrimSE.1_5_97_0.bin`)
- **`ExtraDataList`** (`ExtraDataList.h`): `sizeof == 0x18` (`+0x00 data`, `+0x08 bitfield`, `+0x10 lock` — без `vtable`).
- **`TESObjectREFR`** (`TESObjectREFR.h`): `sizeof == 0x98` (`+0x60 parentCell`, `+0x68 loadedState`, `+0x70 extraData`, `+0x90 scale`).
- **`Actor`** (`Actor.h`): `sizeof == 0x2B0` (`+0x98 magicTarget`, `+0xB0 actorValueOwner`, `+0xB8 actorState`, `+0xE0 flags1`, `+0xF0 currentProcess`, `+0x158 pCombatController`, `+0x188 addedSpells`, `+0x1C0 magicItems`, `+0x1E0 equippedShout`, `+0x1FC flags2`, `+0x270 unk194`, `+0x27C actorLock`).
- **`PlayerCharacter`** (`PlayerCharacter.h`): `sizeof == 0xBE0` (`+0x580 objectives`, `+0x9B0 pSkills`, `+0xAC8 locationForm`, `+0xAF8 difficulty`, `+0xB10 baseTints`, `+0xB28 overlayTints`).
- **`TESObjectCELL`** (`TESObjectCELL.h`): `sizeof == 0x140` (`+0x40 cellFlags`, `+0x80 refData`, `+0xB8 objectList`, `+0x118 lock`, `+0x120 worldspace`, `+0x128 loadedCellData`; `LoadedCellData::encounterZone == +0x160`).
- **`TESLeveledList`** (`TESLeveledList.h`): `sizeof == 0x28` (`+0x08 entries`, `+0x10 chanceValue`, `+0x11 flags`, `+0x12 length`, `+0x20 chanceGlobal`).
- **`Projectile`** (`Projectile.h`): наследует `TESObjectREFR` (`0x98`), поэтому `0x98 + 0xF8 == 0x190 == offsetof(Projectile, fPower)`, `sizeof(Projectile) == 0x1D8`, `sizeof(LaunchData) == 0xA8` — **совпадает с `1.5.97` байт в байт**.

### 2.2. КРИТИЧЕСКИЕ СТРУКТУРЫ, СЛОМАННЫЕ В `main` (ОБЯЗАТЕЛЬНЫ К ИСПРАВЛЕНИЮ В ЭТАПЕ 1!)

1. **`BSAnimationGraphManager`** (`Code/client/Games/Skyrim/BSAnimationGraphManager.h` — сломан во **ВСЕХ 4** предыдущих ветках: `main`, `014f2836`, `fcbffe0c`, `ada174eb`!):
   - Upstream-коммит `373291bf` (`fix: animations with new skyrim update`) увеличил `void* pad_ptrs2[8]` до `void* pad_ptrs2[9]` под AE `1.6.629+`, сдвинув `lock` с `0x98` на `0xA0`, а `animationGraphIndex` — с `0xA8` на **`0xB0`**.
   - По данным `NetScriptFramework.SkyrimSE.1_5_97_0.bin`: **`sizeof(BSAnimationGraphManager) == 0xB0`** (`+0x40 Graphs`, `+0x98 UpdateLock`, `+0xA0 DependentManagerLock`, `+0xA8 ActiveGraph`, `+0xAC GenerateDepth`)!
   - В `main` чтение `animationGraphIndex` по смещению `+0xB0` читает **4 байта ЗА ПРЕДЕЛАМИ heap-аллокации объекта**, а взятие `lock` по `+0xA0` портит соседний мьютекс без синхронизации с потоками Havok/OAR/AMR/Nemesis -> мгновенный CTD на анимациях!
   - **Точное исправление:** вернуть `void* pad_ptrs2[8];`, `static_assert(offsetof(BSAnimationGraphManager, lock) == 0x98);`, `static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0xA8);`.

2. **`CombatController`** (`Code/client/Games/Skyrim/Combat/CombatController.h` — сломан в `main`, `fcbffe0c`, `ada174eb`):
   - В `main` остались AE-оффсеты: `targetSelectors == 0xA0`, `pActiveTargetSelector == 0xB8`, `sizeof == 0xE0`.
   - В `1.5.97`: **`targetSelectors == 0x98`**, **`pActiveTargetSelector == 0xB0`**, **`sizeof(CombatController) == 0xD8`**.
   - Без исправления `CombatController.cpp` при каждом пересчёте агро врагов читает/пишет со сдвигом `+8` байт -> поломка AI врагов и CTD в бою.

---

## 3. Все 11 внутрифункциональных патчей (`VersionDbPtr`) под `1.5.97.0.8`

В `main` **НИ ОДИН** из 11 низкоуровневых патчей внутри тела функций не был портирован под `1.5.97` — при запуске игры они затирают случайные инструкции `SkyrimSE.exe 1.5.97`.

| # | Файл | Назначение | AE ID + Offset (сейчас в `main`) | Точный SE `1.5.97` ID + Offset / Способ установки |
|---|---|---|---|---|
| 1 | `BSGraphicsRenderer.cpp:82` | `InitWindows` (`dwStyle` окна) | `77226 + 0x174 + 1` | **`SE 75591` (`0xD71F00`) `+ 0xA9 + 1`** — применять **ТОЛЬКО** при флаге `+force_windowed`, чтобы НЕ ломать Borderless Fullscreen и координаты мыши `SSEDisplayTweaks.dll` в RFAB! |
| 2 | `BSGraphicsRenderer.cpp:86` | `BSWin32KeyboardDevice::Initialize` (`DISCL_NONEXCLUSIVE`) | `68781 + 0x55 + 2` | **`SE 67471` (`0xC1A060`) `+ 0x55 + 2`** (проверка ожидаемого байта перед записью). |
| 3 | `BSGraphicsRenderer.cpp:93` | `Hook_StopTimer` (рендер D3D11 UI!) | `77246 + 0x9` | **`SE 75461` (`0xD6A2B0`)** — верифицированный поиск `0xE8` вызова `StopTimer` в `[Begin..End]` (критичен для D3D11 оверлея TiltedUI!). |
| 4 | `BSGraphicsRenderer.cpp:94` | `Hook_Renderer_Init` (D3D11 device + WndProc) | `77226 + 0x0` | **`SE 75595` (`0xD72810`) `+ 0x0`** (`TP_HOOK_IMMEDIATE`). |
| 5 | `BSInputDeviceManager.cpp:19` | `PollInputDevices` | `68617 + 0x0` | **`SE 67315` (`0xC16660`) `+ 0x0`** (`TP_HOOK_IMMEDIATE`). |
| 6 | `BSRandom.cpp:17,20` | `BSRandom::UnsignedInt` / `GetGenerator` | `68276`, `14774` | **`SE 67018` (`0xC081A0`)**, **`SE 14599` (`0x198FA0`)** (`+0x0`). |
| 7 | `BSTaskletManager.cpp:32` | Именование потоков `BSTaskletManager` | `69554 + 0x63` | **`SE 68203` (`0xC39950`)** — верифицированный `SwapCall` с проверкой `0xE8` в `[Begin..End]`. |
| 8 | `BSThread.cpp:78,83` | `BSThread::Init` + `SetThreadName` | `68261`, `69066` | **`SE 67003` (`0xC07DE0`)**, **`SE 67740` (`0xC26240`)** (`+0x0`). |
| 9 | `SkillsMenu.cpp:11,19` | Разморозка `StatsMenu` | `52510` (`+0x84E`, `+0xA10`, `+0x1040`), `52518` (`+0x46`, `+0x4A`) | **`SE 51638` (`+0x929` 6B, `+0xAEC` 4B, `+0xFBC` 2B)** и **`SE 51645` (`+0x46` 4B, `+0x4A` 2B)** + обязательный гард **`if (!stubs::g_IsSoulsREActive)`**! |
| 10 | `UI.cpp:126,132,138` | `UI_AddToActiveQueue`, пропуск интро, `FavoritesMenu` | `82082 + 0x682`, `36548 + 0xFE`, `51538 + 0x15` | **`SE 79945` (`0xEBE630`, верифицированный поиск `0xE8` `AddToActiveQueue`)**, **`SE 35548` (с проверкой `0x74/0x75`)**, **`SE 50644` (`0x8781C0 + 0x15`, `0x9090` при `!g_IsSoulsREActive`)**. |
| 11 | `Projectile.cpp:131` | Перехват `Actor::Fire` (стрелы / арбалеты / каст) | `34452 + 0x374` (`Xbyak` unwind stack hack под фрейм AE!) | **Полный отказ от хрупкого AE `Xbyak` stack-unwind хака!** Чистый function-entry detour `TP_HOOK` по началу функции **`SE 33672` (`0x5506C0 + 0x0`, `ActorMagicCaster::Fire` / `TESObjectWEAP::Fire`)**: убирает дублирование стрел и исключает порчу стека `1.5.97`! |

---

## 4. Совместимость с SKSE-плагинами сборки `RFAB` (`EngineFixes.dll`, `SkyrimSoulsRE.dll`, `SSEDisplayTweaks.dll`)

1. **`SSE Engine Fixes` (`EngineFixes.dll` для `1.5.97`)**:
   - **КАТЕГОРИЧЕСКИ НЕЛЬЗЯ** блокировать `EngineFixes.dll` в `DllBlocklist.cpp` или выключать `AnimationLoadSignedCrash = true`: сборка `RFAB` содержит тысячи анимаций Nemesis/OAR и **гарантированно вылетает при загрузке (`int16` overflow счётчика анимаций)** без `AnimationLoadSignedCrash = true`!
   - Рекомендуемый профиль совместимости `EngineFixes.toml` для игроков `RFAB Online`:
     ```toml
     [Patches]
     MemoryManager = false          # Не конфликтовать с аллокатором TiltedOnline / Skyrim
     ScaleformAllocator = false     # Не конфликтовать с UI Scaleform / SkyUI / CustomMenu
     MaxStdio = 8192                # Критично для сотен .esp/.bsa в MO2 RFAB

     [Fixes]
     AnimationLoadSignedCrash = true # ОБЯЗАТЕЛЬНО true для RFAB!
     ```
2. **`SkyrimSoulsRE.dll`**:
   - В `RFAB` может быть активен `SkyrimSoulsRE.dll` (`v2.1.x` под `1.5.97`). `SkillsMenu.cpp` и `UI.cpp` обязаны проверять `stubs::g_IsSoulsREActive` и не патчить те же инструкции повторно.
3. **`SSEDisplayTweaks.dll`**:
   - Управляет окном, VSync, high-FPS физикой Havok и Borderless Fullscreen в `RFAB`. `BSGraphicsRenderer.cpp` не должен принудительно перезаписывать `dwStyle = WS_OVERLAPPEDWINDOW` в обычном игровом режиме.
