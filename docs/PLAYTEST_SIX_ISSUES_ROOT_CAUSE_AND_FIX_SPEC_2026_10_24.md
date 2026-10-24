# Playtest 6-Issue Root-Cause Analysis & Architectural Fix Specification (2026-10-24)

## Scope & Context

Active co-op playtest audit against `Riyozaki/RFAB_Online` (`v1.1.0`, commit `b6b127e176cb178b3e11920d849fffb89fe1fdba`) targeting **Skyrim Special Edition `1.5.97.0`** + **Requiem for a Balance (`drake1ch/RFAB`)**.

Every crash RVA, struct offset, and engine call below was cross-verified against:
- `version-1-5-97-0.bin` (`778,674` SE `1.5.97` Address Library entries),
- `NetScriptFramework.SkyrimSE.1_5_97_0.bin`,
- `powerof3/CommonLibSSE` (SE `1.5.97` struct layouts),
- `Vermunds/SkyrimSoulsRE` (`2.1.3`),
- `Exit-9B/CustomSkills` (`v2.0.2`), and
- `drake1ch/RFAB` (`RFAB.dll`, `SkyrimSoulsRE.ini`, `[RFAB] Perks Menu SE`, `Extended UI SE RUS`).

---

## Issue 1 — Repeatable Save-Load CTD at `SkyrimSE.exe+0x6EDFC2` (`FaultAddr=0x1F0`, `RCX=0x0`) & Pre-Party `F5` Gate

### Crash Signature
- **Exception**: `EXCEPTION_ACCESS_VIOLATION` (`0xC0000005`) reading `[RCX + 0x1F0]` (`Actor::race`) with `RCX = 0x0`, `RAX = 0x141669918` (`SE_ID = 262516`, `MovementControllerNPC::vftable{for IMovementMotionDrivenControl}`).
- **Instruction**: `SkyrimSE.exe+0x6EDFC2` (`SE_ID = 40691`, `IMovementMotionDrivenControl::Func2 + 0x22`).
- **Call Stack**:
  1. `SkyrimSE.exe+0x5C7E66` (`SE_ID = 36016`, `BSTaskPool::HandleTask + 0xF86`)
  2. `SkyrimSE.exe+0x298A2E` (`SE_ID = 19374`, `TESObjectREFR::Disable + 0x37E`)
  3. `SkyrimSE.exe+0x6F2200` (`SE_ID = 40786`, `MovementControllerNPC::~MovementControllerNPC + 0x10`)
  4. `SkyrimSE.exe+0x6EDFC2` (`SE_ID = 40691`)

### Exact Root Cause
1. `PurgeDynamicActorCleanly(pActor, true)` (`Code/client/Services/Generic/CharacterService.cpp:96..99`) invoked synchronous `apActor->DisableImpl()` (detaching `MovementControllerNPC`'s `Actor*` owner pointer to `nullptr`) **immediately followed by** `apActor->Delete()`, which queued an asynchronous `Disable` + `Delete` task into `BSTaskPool`.
2. `PrepareEngineForWorldLoad()` (`Code/client/Games/SaveLoad.cpp:54..68`) ran immediately prior to `BGSSaveLoadGame::LoadGame`, calling `TransportService::Close()` (synchronously firing `CharacterService::OnDisconnected` -> `PurgeAllDynamicActorsForWorldLoad()`) and then calling `PurgeAllDynamicActorsForWorldLoad()` a **second time**.
3. When `BGSSaveLoadGame::LoadGame` subsequently tore down the world and flushed `BSTaskPool`, the queued `TESObjectREFR::Disable` task executed `~MovementControllerNPC` on a controller whose `Actor*` owner (`RCX`) was already `0x0`.
4. Separately, `CoordinatedSaveService::IsLocalPlayerAuthorizedLeader()` (`CoordinatedSaveService.cpp:47..52`) returned `partyService.GetPlayers().empty()` when `!partyService.IsInParty()`, causing `F5` saves to be blocked for **all** connected players before forming a party, whereas the server's `SaveSnapshotService::IsPlayerAuthorizedHost()` already authorized the first/lowest-ID host.

### Architectural Fix
- Introduce `CharacterService::DetachAllActorsForEngineLoad()` (and guard `OnDisconnected` while an engine world load is in progress) so `PrepareEngineForWorldLoad()` detaches ECS components, clears `SetRemote(false)` / `SetPlayer(false)`, and sets `SetSkipSaveFlag(true)` + `UnsetChanged(0xFFFFFFFF)` **without** calling `DisableImpl()` or queueing `BSTaskPool` `Delete()` tasks before `BGSSaveLoadGame::LoadGame` natively destroys the world.
- Remove synchronous `DisableImpl()` before `Delete()` in live-world `PurgeDynamicActorCleanly()` (`TESObjectREFR::Delete()` already queues the engine's ordered `Disable` + `Delete` sequence) and restrict `Delete()` strictly to network-created actors (`ActorExtension::IsCreatedByNetwork()`).
- Align `CoordinatedSaveService::IsLocalPlayerAuthorizedLeader()` with `RfabAdapterService::IsLocalPlayerAllyProtectionHost()` and `SaveSnapshotService::IsPlayerAuthorizedHost()` when no party has been formed yet.

---

## Issue 2 — Skills Menu (`StatsMenu`) CTD & `Tab` (`TweenMenu`) Lockout

### Exact Root Cause
1. **Boot-order race on `g_IsSoulsREActive`**:
   - `SkillsMenu.cpp` (`s_skillsMenuInit`) and `UI.cpp` (`s_s`: `Hook_UI_Register` + `FavoritesCanProcess` `SE 50644 + 0x15`) run inside `TiltedPhoques::Initializer` during `InstallHooks2()` (`[BOOT 3.1/4]`), **before** `StartSKSE()` (`[BOOT 4/4]`) loads `SkyrimSoulsRE.dll`, `CustomSkills.dll`, and `RFAB.dll`.
   - At `[BOOT 3.1/4]`, `stubs::g_IsSoulsREActive` is always `false` and `GetModuleHandleW(L"SkyrimSoulsRE.dll")` is always `nullptr`.
2. **`StatsMenu` crash**:
   - In `drake1ch/RFAB`, `SkyrimSoulsRE.ini` explicitly sets `bStatsMenu = false` because `[RFAB] Perks Menu SE` (`CustomSkills.dll` `v2.0.2`), `RFAB.dll` (`PerkPlacementMenuOpenCloseManager`), and `Extended UI SE RUS` (`exui_statsmenu.psc`) require `StatsMenu`'s native paused sky-dome camera lifecycle.
   - Because `s_skillsMenuInit` ran unconditionally at `[BOOT 3.1/4]` and `UI.cpp`'s `WrappedMenuCreator` stripped `IMenu::kPausesGame` from `"StatsMenu"`, opening the Skills menu forced an unpaused state into `CustomSkills.dll` / `StatsMenu` and crashed the client.
3. **`Tab` (`TweenMenu`) lockout**:
   - `WrappedMenuCreator` stripped `IMenu::kPausesGame` from `"TweenMenu"` **before** `SkyrimSoulsRE::CreateMenu("TweenMenu")` inspected `menu->PausesGame()`, preventing `SkyrimSoulsRE` from marking `TweenMenu` with `MenuFlagEx::kUnpaused` (`GetUnpausedMenuCount() == 0`) and desynchronizing `TweenMenu` camera/control state.
   - Additionally, synced quest stages invoking `Game.DisablePlayerControls(..., abDisableMenu = true, ...)` (or `.ess`-persisted `ControlMap::enabledControls` at `+0x118`) can leave `UEFlag::kMenu` (`1 << 3`, which gates `Tab` / `TweenMenu` exclusively) cleared even after normal movement/looking/fighting/activate controls resume.

### Architectural Fix
- Defer all SKSE-plugin-dependent UI checks to runtime (when `UI::Register` / menu creators actually run after `StartSKSE()`):
  - When `SkyrimSoulsRE.dll` is present, let `SkyrimSoulsRE` own menu unpausing (`SkyrimSoulsRE.ini`: `bTweenMenu = true`, `bCustomMenu = true`, `bStatsMenu = false`, etc.) without stripping `kPausesGame` behind its back or double-patching `FavoritesMenu` / `StatsMenu`.
  - Never unpause `"StatsMenu"` or patch `StatsMenu::ProcessMessage` when `SkyrimSoulsRE.dll`, `CustomSkills.dll`, or `RFAB.dll` is loaded.
- Add a non-invasive free-roam `ControlMap` self-heal in `RfabAdapterService::OnUpdate` (1 Hz on the main VM thread): when the player is alive, not downed, not in a scene, no blocking UI/overlay is open, and `kMovement | kLooking | kActivate | kFighting` are all enabled in `ControlMap::enabledControls` (`+0x118`), restore `UEFlag::kMenu | UEFlag::kMainFour` (`0x108`) via `BSInputEnableManager::Get()->EnableOtherEvent` (`SE 514705` / `SE 67245`, `ControlMap::ToggleControls`).

---

## Issue 3 & Issue 4 — NPCs Stuck in Aggression Stance, Failing to Attack, Swinging 2H Weapons 1-Handed, or Attacking Very Slowly

### Exact Root Causes (Four Compounding Defects)
1. **2H Weapons, Bows/Crossbows, Shields, and Arrows/Bolts Force-Equipped into `rightEquipSlot` (`TESObjectREFR.cpp:887`)**:
   - `TESObjectREFR::AddOrRemoveItem()` unconditionally passed `DefaultObjectManager::Get().rightEquipSlot` (`1H RightHand`, `0x13F42`) with `aForceEquip = true` for **every** `isWorn` item during `SetActorInventory()`.
   - Because `aForceEquip = true` bypasses slot validation in `ActorEquipManager::EquipObject`, 2-handed greatswords (`kTwoHandSword = 5`), battleaxes/warhammers (`kTwoHandAxe = 6`), bows (`kBow = 7`), and crossbows (`kCrossbow = 9`) were forced into `equippedObjects[kRight]` instead of `BothHands` (`0x13F45`), and `TESAmmo` arrows/bolts were passed `rightEquipSlot` instead of `nullptr`.
   - Consequences: 2H NPCs swung 2-handed weapons 1-handed or stalled in `CombatBehaviorMelee`, and archers/crossbowmen stood in aggression pose unable to fire.
2. **Base `WeaponSpeedMult` (`85`) / `LeftWeaponSpeedMult` (`132`) Corruption (`ActorValueService.cpp:557`) & Missing Havok Speed Vars (`AnimationGraphDescriptor_Master_Behavior.cpp:321`)**:
   - In Skyrim SE, `kWeaponSpeedMult` (`85`) has base `0.0f` (`0.0f` = `1.0x`; any non-zero value such as `0.15f` is treated literally as `0.15x` = **6.6x slower**, and `<= 0.0f` freezes attack animations mid-windup).
   - `ActorValueService::OnActorValueChanges()` called `pActor->SetActorValue(key, value)` (overwriting **base** AV) for `kWeaponSpeedMult` (`85`), `kLeftWeaponSpeedMult` (`132`), and `kSpeedMult` (`30`) while `OnActorMaxValueChanges()` simultaneously mutated `PERMANENT` modifiers, compounding Requiem/RFAB Mass Effect & stamina modifiers across sync/ownership handoffs.
   - `AnimationGraphDescriptor_Master_Behavior` also omitted `kweaponSpeedMult` (`47`), `k2HMweaponSpeedMult` (`141`), and `kleftWeaponSpeedMult` (`197`) from `FloatLookupTable` (and `Falmer_Master_Behavior` / `DraugrBehavior` omitted combat hand/stance/attack vars).
3. **Stale Transient `ActorState` Attack/Stagger/Recoil/Draw Bits Across Ownership Handoff (`CharacterService::TakeOwnership`)**:
   - While an NPC is remote, `AnimationSystem::Update()` copies raw `actorState.flags1` (`meleeAttackState` bits `28..31`, `knockState` bits `25..27`) and `actorState.flags2` (`weaponState` bits `5..7`, `recoil` bits `10..11`, `staggered` bit `13`) from mid-action packets.
   - Upon `TakeOwnership()` (Remote -> Local), if `meleeAttackState != kNone`, `staggered == 1`, `recoil != 0`, or `weaponState == kDrawing` was left set without a matching local animation end event, Skyrim's `CombatBehaviorMelee` waited indefinitely for the phantom attack/stagger/draw to finish, leaving the NPC circling in aggression stance without ever attacking.
4. **0.5s `StopCombat()` + `StartCombat()` Loop in `DownedStateService` (see Issue 6 below)** resetting enemy `CombatController` twice per second before any swing could execute.

### Architectural Fix
- In `TESObjectREFR::AddOrRemoveItem()`:
  - Pass `nullptr` (native `BGSEquipType::GetEquipSlot()`) for armor/shields, ammo (`count = arEntry.Count`), torches, and 2-handed/ranged weapons (`kTwoHandSword`, `kTwoHandAxe`, `kBow`, `kCrossbow`, or `BGSEquipType::equipSlot` != 1H slots).
  - Pass `rightEquipSlot` / `leftEquipSlot` (`count = 1`) **only** for 1-handed melee weapons and staves (`kHandToHandMelee`, `kOneHandSword`, `kOneHandDagger`, `kOneHandAxe`, `kOneHandMace`, `kStaff`), supporting dual-wield (`isWorn && isWornLeft`) cleanly.
- In `ActorValueService`:
  - Never overwrite base `kWeaponSpeedMult` (`85`), `kLeftWeaponSpeedMult` (`132`), or `kSpeedMult` (`30`) via `SetActorValue`; route dynamic changes through `ForceActorValue(DAMAGE, ...)` (and guard against near-zero/negative `WeaponSpeedMult` poison states on local ownership).
- Sync `kweaponSpeedMult`, `k2HMweaponSpeedMult`, `kleftWeaponSpeedMult` in `AnimationGraphDescriptor_Master_Behavior` (and missing combat state vars in `DraugrBehavior` & `Falmer_Master_Behavior`).
- In `CharacterService::TakeOwnership()` (and `OnAssignCharacter` local claim): sanitize stuck transient `meleeAttackState`, `knockState`, `recoil`, `staggered`, and mid-transition `weaponState` on living NPCs and reset stuck `IsAttacking`/`IsStaggering`/`IsRecoiling` graph states so local AI immediately attacks.

---

## Issue 5 — Immortal Invisible Enemies on Cell Re-entry While Ally Is Downed or After Player Leaves & Reconnects

### Exact Root Causes (Four Specific Defects)
1. **Missing `FormIdComponent` on Interior `WaitingFor3D` Spawns (`CharacterService::RunSpawnUpdates`, line `2308`)**:
   - When Player B enters/re-enters an interior dungeon while Player A is downed inside, `OnCharacterSpawn` stages custom/leveled `0xFFxxxxxx` enemies during the door loading screen (`pLocalPlayer->parentCell == nullptr`) into `WaitingFor3D` without `FormIdComponent`.
   - Once the cell finishes loading, `RunSpawnUpdates()` (`else if (remoteComponent.CachedRefId == 0)`) called `CreateCharacterForEntity(entity)` and set `remoteComponent.CachedRefId = pActor->formID` **without emplacing `FormIdComponent`**.
   - Without `FormIdComponent`: (a) `waitingView = m_world.view<FormIdComponent, WaitingFor3D>()` never ran `SetActorInventory()` / `EquipOutfit()`, leaving creatures/ghosts/armored NPCs without their worn body/skin mesh (**invisible**); and (b) damage/death handlers could never map `formID <-> ServerId` while `IsRemote() == true` blocked local damage (**immortal**).
2. **Reconnect (`OnConnected`, lines `419..427`) Calling `DisableImpl()` + `Delete()` on Local `TESLevCharacter` `0xFFxxxxxx` Dungeon Enemies**:
   - When a player disconnected and reconnected in a dungeon, `OnConnected` matched **all** `(formId & 0xFF000000) == 0xFF000000` forms — including Skyrim/Requiem/RFAB's own cell-spawned `TESLevCharacter` enemies — and called `PurgeDynamicActorCleanly(pActor, true)` + `continue`.
   - `DisableImpl()` detached their 3D `NiNode` (**invisible**), while `ObjectReference::Delete()` is a no-op on non-`PlaceAtMe` cell-spawned `TESLevCharacter` refs, leaving their AI/hitboxes active in combat and skipping server registration (**immortal invisible enemies**).
3. **`OnRemoveCharacter` (`CharacterService.cpp:1072`) Leaving `SetRemote(true)` Stuck on Pre-Placed NPCs**:
   - When `NotifyRemoveCharacter` arrived on peer disconnect/cell-exit, `DeleteTempActor()` ignored `< 0xFF000000` NPCs and `DeleteRemoteEntityComponents()` stripped `RemoteComponent` **without** resetting `pActor->GetExtension()->SetRemote(false)`, leaving those NPCs permanently `IsRemote() == true` with no owner (`HookRestoreActorValue` and `HookDamageActor` blocking damage = **immortal**).
4. **Server Ownership Gate Blocking Non-Leader Standing Ally When Party Leader Is Downed (`Code/server/Services/CharacterService.cpp:250`)**:
   - `OnAssignCharacterRequest` required `partyService.IsPlayerLeader(acMessage.pPlayer)` before invoking `CoopOwnership::EvaluateVoluntaryClaim`, preventing a non-leader standing party member re-entering a cell from taking ownership of enemies from a downed party leader.

### Architectural Fix
- Track `ActorExtension::IsCreatedByNetwork()` (`std::atomic<uint32_t> onlineFlags`) strictly on actors spawned by STR via `Actor::Create(pNpc)`. Only network-created actors are deleted on disconnect/reconnect; native `TESLevCharacter` `0xFFxxxxxx` dungeon enemies have `SetRemote(false)` restored and re-register cleanly via `ProcessNewEntity()`.
- Emplace `FormIdComponent` immediately inside `CreateCharacterForEntity()` / `RunSpawnUpdates()`, and also defer pre-placed (`FormId != GameId{}`) `CharacterSpawnRequest` packets if `pLocalPlayer->parentCell == nullptr` or `pActor->IsStillLoading()`.
- In `OnRemoveCharacter()`, reset `pActor->GetExtension()->SetRemote(false)` on non-network-created actors before stripping `RemoteComponent`.
- In `Server::CharacterService::OnAssignCharacterRequest()`, allow any standing party member to claim enemy ownership when `ownerDowned || !ownerValidInRange` (in addition to `IsPlayerLeader`).

---

## Issue 6 — Downed-Player Aggro Ping-Pong Loop & Enemies Standing Over Downed Player Ignoring Living Player

### Exact Root Cause
1. `DownedStateService::RedirectNearbyHostileAggro()` (`DownedStateService.cpp:449..499, 700..705`) ran every `0.5s` and called `pActor->StartCombatEx(pBestLivingAlly)` (`StopCombat()` + `StartCombat()`), destroying and recreating `CombatController` twice per second (causing endless taunt/aggro animations without attacks), or called `StopCombat()` while `0x14` remained `kAlive` at `1.0 HP` so enemies immediately re-detected `0x14` on the next AI tick (infinite `0.5s` aggro loop).
2. On allies' clients, `OnNotifyPlayerDownedState()` (`lines 740..757`) played `BleedoutStart` on the downed remote player clone (`0xFFxxxxxx`) while leaving it `kAlive` as a valid combat target without redirecting ally-owned hostiles, causing enemies owned by Player B to swarm and stand over Player A's downed clone while ignoring Player B.

### Architectural Fix
- Maintain atomic `ActorExtension::IsDownedPlayer()` on both local `0x14` (while downed) and downed remote player clones (`0xFFxxxxxx`), and mark downed remote player clones with `ACTOR_LIFE_STATE::kBleedout` (`flags1` bits `21..24` = `8`) while downed.
- In `HookUpdateDetectionState` (`SE 41626`, `ActorKnowledge::UpdateDetectionState`): suppress NPC detection updates onto any actor with `IsDownedPlayer() == true` and clamp `DetectionState::level = -1000`, eliminating aggro re-acquisition loops when no living ally is nearby.
- Activate `HookUpdateTarget` (`SE 32489`, `CombatController::UpdateTarget`): after `RealUpdateTarget`, if the selected target has `IsDownedPlayer() == true`, seamlessly switch `CombatController::SetTarget` (`SE 32488`) to `RfabAdapterService::FindBestStandingPlayerTargetFor(...)` (without calling `StopCombat()`), or clear target if no standing ally is in range.
- In `DownedStateService`: replace `StartCombatEx` (`StopCombat` + `StartCombat`) with `SetCombatTargetEx` (`CombatController::SetTarget`) when already in combat, and redirect locally-owned hostiles immediately on both local `EnterDownedState` and remote `OnNotifyPlayerDownedState(IsDowned = true)`.
