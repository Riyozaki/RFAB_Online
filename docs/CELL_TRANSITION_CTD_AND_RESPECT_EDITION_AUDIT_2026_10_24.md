# Root-Cause Audit: Peer CTD on Location/Cell Transition (`v1.0.0` → `v1.1.0`) & RFAB Respect Edition Compatibility (2026-10-24)

## 1. Reported Symptom & Context

- **Reported symptom**: When Player A enters another location (interior door, dungeon, tavern, or exterior grid boundary), **Player B's (friend's) Skyrim SE crashes to desktop immediately** (*«друг начал вылетать когда захожу в другую локацию, до этого не было такого в версии 1.0»*).
- **Modpack / MO2 profile verified from user screenshot**:
  - **Mod Organizer 2 v2.4.2**, **RFAB Respect Edition** (`Default` profile, `44` active mods, `30` active plugins).
  - Key load-order positions:
    - `05` `Unofficial Skyrim Special Edition Patch.esp` (ESM-flagged)
    - `06` `Skyrim Project Optimization - Full Version.esm` (`.esm`)
    - **`07` `RFAB.esp` (ESM-flagged!)**
    - `10` `SkyrimTogether.esp` (priority 19)
    - `FE:003` `SkyrimTogetherQuestPatches.esp` (priority 20)
    - `17` `SkyrimOutfitSystem.esp` (priority 29, `[x] Skyrim Outfit System RU`)
    - `[x] Disable Quick Saves` active (`[ ] Seamless Saving` disabled), `[x] RFAB_Online` active.

---

## 2. Exact Root Causes of the `v1.1.0` Crash on Peer Location Transition

Diffing `c316488e40906185e6b1528aa2d34e56deb18ac3..HEAD` (`v1.0.0..v1.1.0`) exposed **four compounding regressions** triggered on **Player B's PC** the millisecond Player A enters a different cell/location:

### Root Cause #1 (Primary Deterministic `0xC0000005` Crash): Latent Papyrus ABI Mismatch in `TESObjectREFR::Disable()` Called by `PurgeDynamicActorCleanly`
- **Mechanism**:
  1. When Player A enters another location, the server (`Code/server/Services/CharacterService.cpp:155, 179`) sends `NotifyRemoveCharacter` to Player B.
  2. On Player B's client, `CharacterService::OnRemoveCharacter` calls `CharacterService::DeleteTempActor(formId)`.
  3. In **`v1.0.0`**, `DeleteTempActor` **only** called `pActor->Delete()` (which invokes non-latent `ObjectReference::Delete` and defers teardown to Skyrim's main-thread cleanup queue). `TESObjectREFR::Disable()` (`TESObjectREFR.cpp:353`) was dead upstream code never called on remote players or temporary actors.
  4. In **`v1.1.0`**, `DeleteTempActor` called `PurgeDynamicActorCleanly(pActor)`, which added `apActor->Disable()` right before `apActor->Delete()`.
  5. `TESObjectREFR::Disable()` (`Code/client/Games/Skyrim/TESObjectREFR.cpp:353`) invoked `ObjectReference.Disable(bool abFadeOut = true)` via **non-latent** `PAPYRUS_FUNCTION` (`R8 = const TESObjectREFR*`, `stackID = 0`).
  6. In Skyrim SE `1.5.97` (`ObjectReference.psc:177`), **`ObjectReference.Disable` is a LATENT VM native** (unlike `DisableNoWait`): its C++ handler expects **`R8 = const RefrOrInventoryObj*`** (`{ const TESObjectREFR* pRefr; TESForm* pInventoryForm; uint16_t itemCount; }`) and suspends a live Papyrus `stackID` when `abFadeOut == true`. Passing `apActor` in `R8` caused `SkyrimSE.exe` to dereference `[apActor + 0x00]` (**`Actor::vtable` in `.rdata`**) as `TESObjectREFR* self.pRefr` → **immediate `0xC0000005` Access Violation CTD** on Player B's PC!
- **Fix**:
  - Replaced `TESObjectREFR::Disable()` and `TESObjectREFR::Enable()` in `Code/client/Games/Skyrim/TESObjectREFR.cpp` with direct C++ engine calls `const_cast<TESObjectREFR*>(this)->DisableImpl()` (`vtable[0x89]`) and `const_cast<TESObjectREFR*>(this)->EnableImpl()` (`SE ID 19373`) — zero `BSScript::IVirtualMachine` involvement and zero latent ABI hazard anywhere in the client.
  - Restored `v1.0.0`'s clean deferred `apActor->Delete()` behavior for normal runtime cell-change despawns (`aImmediateDisable = false` in `DeleteTempActor` and `CancelServerAssignment`), invoking `DisableImpl()` only during pre-save / world-load / initial-connect purges (`aImmediateDisable = true`).

### Root Cause #2: Missing `FormIdComponent` Removal in `CharacterService::CancelServerAssignment`
- **Mechanism**:
  - In `v1.1.0` (`Code/client/Services/Generic/CharacterService.cpp:1885-1903`), when a temporary `0xFFxxxxxx` remote actor unloaded while the local player was in an exterior `WorldSpace`, `CancelServerAssignment` deleted `pActor` via `PurgeDynamicActorCleanly`, set `remoteComponent.CachedRefId = 0`, and emplaced `WaitingFor3D`, **but omitted `m_world.remove<FormIdComponent>(aEntity)`**.
  - Whenever `FormIdComponent` remained attached (e.g. during `OnNotifyRespawn` or within the same frame), `RunSpawnUpdates()` (`entt::exclude<FormIdComponent>`) could not see the entity while `RunRemoteUpdates()` (`interpolatedEntities`, `animatedView`, `facegenView`, `waitingView`) continued invoking `ForcePosition`, `ForceAction`, `LoadAnimationVariables`, and `FaceGenSystem::Update` on the destroyed `Actor*`.
  - Additionally, `OnCharacterSpawn` (`line 725`) ignored incoming `CharacterSpawnRequest` packets if an unloaded `WaitingFor3D` placeholder (`CachedRefId == 0`, no `FormIdComponent`) still held `RemoteComponent`.
- **Fix**:
  - Atomically strip `FormIdComponent`, `EarlyAnimationBufferComponent`, and `PlayerComponent` inside `CancelServerAssignment`'s exterior `WaitingFor3D` transition, copy `FormIdComponent::Id` by value before calling `CancelServerAssignment`, clear `m_cachedRemoteSpawns` in `OnNotifyRespawn`, and replace unloaded `WaitingFor3D` placeholders cleanly when a fresh `CharacterSpawnRequest` arrives.

### Root Cause #3: Premature / Cross-Allocator Destruction of `BSMaskedShaderMaterial` in `FaceGenSystem::Update`
- **Mechanism**:
  - In `v1.1.0` (`Code/client/Systems/FaceGenSystem.cpp:149-164`), `FaceGenSystem::Update` cloned `BSMaskedShaderMaterial` via `pMaterial->Create()`, called `SetShaderMaterial(pLightingShader, pClonedMaterial, true)`, and **unconditionally** called `vtbl[0](pClonedMaterial, 0)` + `Memory::Free(pClonedMaterial)` (`RealFormFree(..., aAligned = false)`).
  - If `SetShaderMaterial` (or RFAB's `FaceGenFixes.dll` hook on `BSLightingShaderMaterialFacegen`) adopted `pClonedMaterial` directly (`pLightingShader->material == pClonedMaterial`), destroying `pClonedMaterial` left a dangling pointer inside `pLightingShader->material` that crashed inside `~BSLightingShaderProperty()` the moment the peer's 3D head mesh unloaded on location transition. Even when cloned separately, manual `Memory::Free(..., false)` bypassed `SkyrimSE.exe`'s own virtual scalar deleting destructor (`flags = 1`) and alignment flag.
- **Fix**:
  - Guarded cleanup strictly with `if (pLightingShader->material != pClonedMaterial)` and invoked `SkyrimSE.exe`'s own virtual scalar deleting destructor with `flags = 1` (`vtbl[0](pClonedMaterial, 1)`), eliminating both dangling-pointer UAF on actor unload and cross-heap/alignment mismatches under `EngineFixes.dll`.

### Root Cause #4: Null `parentCell` Dereference Guards During Simultaneous Cell Transitions
- **Mechanism**:
  - If Player A entered Player B's location while Player B was simultaneously on a loading screen (`PlayerCharacter::Get()->parentCell == nullptr`), `TESObjectREFR::MoveTo` dereferenced `apCell->worldspace` without a null check (`nullptr` CTD), and `Actor::Create` dereferenced `pPlayer->parentCell` unchecked.
- **Fix**:
  - Added null guards in `TESObjectREFR::MoveTo`, `Actor::Create`, `CharacterService::OnCharacterSpawn`, and `CharacterService::RunSpawnUpdates`, staging custom `CharacterSpawnRequest` spawns into `WaitingFor3D` (`CachedRefId = 0`) while `parentCell == nullptr` and materializing them automatically once the cell finishes loading.

---

## 3. RFAB Respect Edition Load-Order & Modlist Audit (`07 RFAB.esp` ESM-Flagged)

1. **`RFAB.esp` at index `07` (ESM-flagged after `06 Skyrim Project Optimization - Full Version.esm`)**:
   - Verified **zero** hardcoded `0x06xxxxxx` FormID assumptions in `Code/`:
     - `RfabAdapterService`: resolves `RFAB_XP_Amount`, `RFAB_XP_Multiplier`, `RFAB_Global_NoDeathHandling`, and `RFAB_List_NoAllyDamage_AllowedSpells` (`0x005CAA` → `0x07005CAA`) dynamically via `EditorID` + `ModManager::Get()->GetByName("RFAB.esp")->GetFormId(...)`.
     - `InventoryService` (`RFAB_ExperienceRing` `0x00112812` → `0x07112812`) & `ObjectService` (13 RFAB personal shrines/altars): explicitly scoped via `ModManager::Get()->GetByName("RFAB.esp")->GetFormId(localId)` so they work at `0x07` (or any index) and never collide with local IDs of other plugins.
   - Added a dedicated C++ unit test in `Code/tests/rfab_se1597_hardening_audit.cpp` simulating the exact **30-plugin RFAB Respect Edition `loadorder.txt`** (`07 RFAB.esp`, `10 SkyrimTogether.esp`, `FE:003 SkyrimTogetherQuestPatches.esp`, `17 SkyrimOutfitSystem.esp`).
2. **`SkyrimTogether.esp` (`10`) & `SkyrimTogetherQuestPatches.esp` (`FE:003`) loading after ESM-flagged `RFAB.esp` (`07`)**:
   - Verified binary record tables of both plugins:
     - `SkyrimTogether.esp` contains **5 records total**, 100% custom `0x0200xxxx` records (**0 overrides** of `Skyrim.esm` or `RFAB.esp`).
     - `SkyrimTogetherQuestPatches.esp` contains **2 records total** (`TES4` + `SCEN 0x0007165F` MQ206 Alduin's Bane Time-Wound co-op anti-softlock scene).
   - Therefore, MO2 keeping ESM-flagged `RFAB.esp` at `07` above non-ESM `SkyrimTogether.esp` (`10`) causes **zero gameplay, balance, perk, shrine, camera, or NPC conflicts** with RFAB Respect Edition.
3. **`Disable Quick Saves`, `Skyrim Outfit System RU`, & MO2 Conflict Flags**:
   - `Disable Quick Saves` (`SKSE\Plugins\EngineFixes.toml`) is automatically patched by `Setup-RfabOnline.ps1` and `EarlyLoad`/`Launcher` (`MemoryManager = false`, `SaveGameMaxSize = false`, `RegularQuicksaves = true`, `MaxStdio = 8192`).
   - `Skyrim Outfit System RU` (`17 SkyrimOutfitSystem.esp`) hooks skinning strictly on local `PlayerCharacter` (`0x14`) and is 100% compatible with remote `0xFFxxxxxx` co-op avatars.
   - MO2's `+` conflict icon on `RFAB_Online` reflects only the 2 intentional co-op Dragon Claw puzzle scripts (`HallofStoriesDiskScript.pex`, `HallofStoriesKeyholeScript.pex`) overriding `- ЯДРО -` (`USSEP`), with zero overrides over `RFAB` itself.
