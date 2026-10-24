# RFAB Online — Thread-Safety, UAF, Re-Entrancy & Null-Pointer Hardening Audit (SE 1.5.97)

## Overview

During a full proactive audit of the Skyrim SE `1.5.97.0.8` client and encoding subsystems in **RFAB Online**, six concrete classes of cross-thread data races, hash-table pointer invalidation (UAF), mid-hook combat/Papyrus re-entrancy, and null-pointer dereferences were identified and hardened.

In Skyrim SE `1.5.97`, `World::Update()` executes inside `HookVMUpdate` (`SkyrimVM::Update`, SE ID `53115`, gated on `apThis->inactive == 0`), while:
- Combat and magic hooks (`HookDamageActor` in `Actor.cpp`, `HookAddTarget` in `MagicTarget.cpp`, `MagicItem::IsBuffSpell`) execute on **Havok / combat / projectile worker threads**;
- Animation graph hooks (`BehaviorVar::Patch`, `SaveAnimationVariables`, `LoadAnimationVariables`, `HookPerformAction`) execute across **Havok behavior-graph worker threads**;
- Overlay UI callbacks (`OverlayClient::OnProcessMessageReceived`) execute on the **CEF IPC thread**.

---

## 1. `RfabAdapterService::EvaluateCoopAction` & `GetNoAllyDamageAllowedSpellsList` — Thread-Safe Combat Snapshot

### Root Cause
- `HookDamageActor` (`Actor.cpp:1134`) and `HookAddTarget` (`MagicTarget.cpp:126`) invoke `RfabAdapterService::EvaluateCoopAction` directly from combat/magic worker threads.
- Previously, `EvaluateCoopAction` iterated `m_world.view<FormIdComponent, PlayerComponent>()` (`entt::registry` sparse sets) and `PartyService::GetPartyMembers()` (`TiltedPhoques::Vector<uint32_t>`) without synchronization while `World::Update()` concurrently spawned/despawned entities and processed party packets.
- Additionally, `MagicItem::IsBuffSpell()` (`MagicItem.cpp:117`) lazily resolved and wrote `m_pNoAllyDamageAllowedSpellsList` without atomic or mutex protection.

### Fix
- Added `RfabAdapterService::CoopCombatSnapshot` (`Connected`, `InParty`, `AllyProtectionEnabled`, `FormIdToPlayerId`, `PartyMemberIds`) guarded by `mutable std::mutex m_coopSnapshotMutex`.
- `RefreshCoopCombatSnapshot()` rebuilds the snapshot strictly on the `World::Update()` thread (`OnUpdate`, `OnConnected`, `OnDisconnected`, `OnPartyJoined`, `OnPartyLeft`, `NotifyPartyLeadershipChanged`, `SetLocalAllyProtectionEnabled`, and `CharacterService` `PlayerComponent` add/remove paths).
- `EvaluateCoopAction` copies the compact snapshot under `m_coopSnapshotMutex` and never touches `entt::registry` or `PartyService` from worker threads.
- Converted `m_pNoAllyDamageAllowedSpellsList` to `std::atomic<BGSListForm*>` with double-checked `m_allowedSpellsMutex` initialization and pre-warmed it in `OnConnected`.

---

## 2. `DownedStateService` — Deferring Heavy Engine/Papyrus/Network Side Effects Out of Damage Hooks

### Root Cause
- `FinalizeLethalHitCushion` is called synchronously inside `HookDamageActor` and `HookRestoreActorValue` when a hit reduces the local player to `<= 1.0f` HP.
- Calling `EnterLocalDownedState()` synchronously inside the damage hook invoked `PlayerCamera::ForceThirdPerson()`, `InterruptCast()`, `SetWeaponDrawnEx(false)`, Papyrus `StopCombat()`, `RedirectHostileAggroAwayFromPlayer()` (`StartCombatEx` + `m_world.view<FormIdComponent>()`), and `m_transport.Send(pkt)` mid-combat-calculation and potentially off the `World::Update()` thread.

### Fix
- Converted `m_isLocalDowned`, `m_pendingEnterDownedTransition`, `m_allowAuthorizedHeal`, and `m_isGraceActive` to `std::atomic<bool>` (`ScopedReviveHealAllowance` now uses `exchange`/`store` with acquire-release ordering).
- `FinalizeLethalHitCushion` now strictly clamps health to `1.0f` (`kDownedFloorHealth`), queues `HealthChangeEvent` via `m_world.GetRunner().Trigger(...)`, and atomically latches `m_isLocalDowned = true` + `m_pendingEnterDownedTransition = true` so any subsequent multi-hit or DoT tick in the same frame is immediately absorbed.
- At the top of `DownedStateService::OnUpdate` on the `World::Update()` thread, `m_pendingEnterDownedTransition` is atomically consumed and `CompleteEnterLocalDownedState()` safely executes camera switch, control lock, bleedout graph events, aggro redirection, and network packet transmission.

---

## 3. `AnimationGraphDescriptorManager` & `BehaviorVar` — Pointer-Stable Registry & Zero Heap Leaks

### Root Cause
- `AnimationGraphDescriptorManager::m_descriptors` stored descriptors in `TiltedPhoques::Map<uint64_t, AnimationGraphDescriptor>` (`tsl::hopscotch_map` — open-addressed flat storage) without locking, returning raw `const AnimationGraphDescriptor*` pointers via `GetDescriptor()`.
- When `BehaviorVar::Patch` registered a custom modded behavior hash on a Havok worker thread (`Register()`), `tsl::hopscotch_map` rehashed and invalidated raw `const AnimationGraphDescriptor*` pointers concurrently dereferenced in `TESObjectREFR::SaveAnimationVariables` / `LoadAnimationVariables`.
- `BehaviorVar::Add` (`BehaviorVar.cpp:299,311`) allocated `new AnimationGraphDescriptor()` and `new AnimationGraphDescriptorManager::Builder(...)` on the heap and leaked both on every registration, and `BehaviorVar::Patch` (`line 330`) dereferenced `PlayerCamera::Get()` without a null check.
- `HookPerformAction` (`Animation.cpp:28,39`) dereferenced `apAction`, `apAction->actor`, and `apAction->action` without null guards.

### Fix
- Replaced `tsl::hopscotch_map` in `AnimationGraphDescriptorManager` with node-based `std::unordered_map<uint64_t, AnimationGraphDescriptor>` (C++ standard `[unord.req.general]/9`: element pointers/references remain valid across insertions and bucket rehashes) guarded by `mutable std::shared_mutex m_mutex` (`std::shared_lock` for `GetDescriptor`/`GetDescriptors`, `std::unique_lock` for `Register`/`UpdateKey`/`Update`).
- Changed `GetDescriptors()` to return a snapshot by value under `std::shared_lock`.
- Eliminated both heap allocations in `BehaviorVar::Add` (stack construction + `std::move`) and added null guards in `BehaviorVar::Patch` and `HookPerformAction`.

---

## 4. `TESObjectCELL` — `BSSpinLock` (`+0x118`) Synchronization & `baseForm` Null Safety

### Root Cause
- Verified against CommonLibSSE for Skyrim SE `1.5.97` (`sizeof(TESObjectCELL) == 0x140`, pre-1.6.629 layout without `ExtraDataList` vtable): `TESObjectCELL+0x118` (immediately before `TESWorldSpace* worldspace` at `+0x120`) is Bethesda's `mutable BSSpinLock spinLock` (`_owningThread` + `_lockCount`, identical layout and semantics to `BSRecursiveLock` in `Games/Primitives.{h,cpp}`).
- `TESObjectCELL::GetRefsByFormTypes` (`TESObjectCELL.cpp:8..21`) and `Actor::DropOrPickUpObject` (`Actor.cpp:1461`) iterated `refData.refArray[0..capacity)` without locking `TESObjectCELL::lock`, racing background cell-loader / reference-insertion threads, and `TESObjectCELL::GetRefsByFormTypes` dereferenced `pRef->baseForm->formType` without checking `pRef->baseForm`.

### Fix
- Marked `TESObjectCELL::lock` `mutable` and added `static_assert(offsetof(TESObjectCELL, lock) == 0x118);`.
- Acquired `BSScopedLock<BSRecursiveLock> cellLock(...)` across `refData.refArray` iteration in both `TESObjectCELL::GetRefsByFormTypes` and `Actor::DropOrPickUpObject` (releasing the lock before `Disable()`/`Delete()` in `DropOrPickUpObject`), and added `!pRef->baseForm` null checks.

---

## 5. `OverlayClient` & `PartyService` — CEF IPC Thread Marshaling & Null Overlay Guards

### Root Cause
- `OverlayClient::OnProcessMessageReceived` runs on the CEF IPC thread. While `connect`, `disconnect`, `acceptPartyInvite`, and `setAllyProtection` queued work onto `World::Get().GetRunner().Queue(...)`, `launchParty`, `leaveParty`, `createPartyInvite`, `kickPartyMember`, `changePartyLeader`, `revealPlayers`, `sendMessage`, `setTime`, `teleportToPlayer`, and `toggleDebugUI` mutated `PartyService`, `MagicService`, `TransportService`, `DebugService`, and `entt::dispatcher` directly from the CEF thread.
- `PartyService::OnPartyInfo` and `PartyService::OnPartyInvite` dereferenced `GetOverlayApp()->ExecuteAsync(...)` without checking for `nullptr`.

### Fix
- Marshaled all remaining UI event handlers in `OverlayClient.cpp` (including `SetServerPassword` inside `ProcessConnectMessage`) onto `World::Get().GetRunner().Queue(...)`.
- Added `if (auto* pOverlayApp = ...)` guards in `PartyService.cpp`.

---

## 6. `ObjectService` & `PlayerService` — Defensive Null-Pointer Hardening

### Root Cause & Fix
- `ObjectService::OnCellChange` (`ObjectService.cpp:93`): guarded `PlayerCharacter::Get()` and `pPlayer->parentCell` before dereferencing `pCell->formID`.
- `ObjectService::OnActivate` (`ObjectService.cpp:326`) & `ObjectService::OnLockChange` (`ObjectService.cpp:400..411`): guarded `pObject` against `nullptr` and logged `pCell->formID` instead of `pObject->parentCell->formID` (`pObject->parentCell` is `nullptr` on persistent world references resolved via `GetParentCellEx()`).
- `ObjectService::OnActivateNotify` (`ObjectService.cpp:369`): guarded `pObject->baseForm` before reading `formType`.
- `PlayerService.cpp`: guarded `PlayerCharacter::Get()` and `pPlayer->currentProcess` across `OnNotifyPlayerRespawn`, `OnPlayerLevelEvent`, `RunRespawnUpdates`, `RunPostDeathUpdates`, `RunLevelUpdates`, `RunBeastFormDetection`, and `ToggleDeathSystem`.

---

## Verification

- **ThreadSanitizer (`g++ -std=c++20 -O2 -pthread -fsanitize=thread`) & AddressSanitizer/UBSan (`-fsanitize=address,undefined`)**: Verified concurrent worker/main-thread stress tests covering `CoopCombatSnapshot`, atomic `DownedStateService` latch deferral, and node-stable `std::shared_mutex` + `std::unordered_map` descriptor registration across 3,000+ concurrent insertions with zero races, zero UAFs, and zero leaks.
- **`TPTests` (`Code/tests/rfab_se1597_hardening_audit.cpp`)**: Added live multi-threaded Catch2 stress tests on `AnimationGraphDescriptorManager::Get()` plus regression checks for all 6 subsystems.
- **SE `1.5.97` Port Verifier (`Tools/rfab/verify_se1597_port.py`)**: Passed (`3,104` active SE IDs, `0` unmigrated AE sites, all struct `sizeof`/`offsetof` assertions valid including `offsetof(TESObjectCELL, lock) == 0x118`).
