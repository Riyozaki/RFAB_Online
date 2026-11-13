# Upstream reconciliation (N7)

Baseline: fork base `5a99a0b6`; upstream `refs/upstream/dev` at `13ff0b5a`; range contains 131 commits (122 non-merge). There is no shared Git history, so commits are evaluated selectively by comparing their patch and intended behavior with the fork; cherry-picking the range is not a safe reconciliation strategy. This document is an evidence ledger, not a claim that every upstream change has been ported. Revisit it when advancing the upstream baseline.

## Audited behavior

| Upstream | Result | Evidence / action |
|---|---|---|
| `f609305e` | Present, adapted | `Code/client/Games/Skyrim/TESObjectREFR.cpp`: save/load index bound checks; fork additionally checks word-variable type. |
| `25bedc9d` | Present, adapted | `Code/server/Services/CharacterSocialAndMountSubservice.cpp`: respawn handling is guarded after its service move. |
| `e0002073` | Present | `Code/server/Services/DebugService.cpp`: copy modifiers before stopping them. |
| `af7f37e6` | Present | `Code/client/Messages/SpellCastRequest.h`: value-initialized desired target. |
| `5a4a8eae` | Present, adapted | `Code/client/Services/Generic/CharacterService.cpp`: loading guard in remote materialization; spawn path uses world model. |
| `cc4a108a` | Present, adapted | `Code/client/Games/Skyrim/PlayerCharacter.cpp` and `Code/client/Services/Generic/MapService.cpp`: worldspace/cell validation. |
| `43bfd173` | Present | `Code/client/Games/Skyrim/Forms.cpp` null-entry cleanup; warning in `Code/client/Services/Generic/SaveLoad.cpp`. |
| `69fe8e67` | Present, adapted | `Code/client/Games/Skyrim/Magic/MagicTarget.cpp`: scripted slow-time exclusion is constrained to constant-effect spells. |
| `83d8080f` | Ported | `Code/client/Games/Skyrim/Events/EventDispatcher.h` carries started/failed event state; `Code/client/Services/Generic/QuestService.cpp` ignores failed starts before broadcasting. Prevents one client's failed start from stopping the party quest. |
| `92980e22` | Behavior present | `Code/server/Services/CharacterService.cpp` reconciles on grid changes and uses dragon-aware `CellIdComponent::IsInRange`; no separate PlayerService grid handler is needed. |
| `5aec3487` | Present | `Code/client/Services/Generic/MagicService.cpp` and `Code/client/Games/Skyrim/Forms/MagicItem.{h,cpp}` suppress duplicate summon casts. |
| `4ffec0a7` | Ported | `Code/client/Games/Skyrim/Actor.cpp`: commanded summon actors do not emit death-item inventory changes. |
| `a813c61e` | Ported | `Code/immersive_launcher/loader/PathRerouting.cpp`: KiLoader dependency directory added to DLL search directories. |
| `2e52a704` | Ported, adapted | `Code/client/Services/Generic/PlayerService.cpp`, `Code/client/Services/PlayerService.h`: respawn, knockdown, and god-mode deadlines use `steady_clock`, not frame deltas; retains fork's teleport guards and connected/downed-system conditions. |
| `25a0049a` | Present, adapted | `Code/client/Games/Skyrim/BSCore/BSAtomic.{h,cpp}` and `Code/client/Games/Skyrim/ExtraData/ExtraDataList.{h,cpp}` use the engine's read/write lock functions for `ExtraDataList`; static layout and SE 1.5.97 ID-map gates pass. |
| `1f488f6f` | Core invariant present, not equivalent wholesale | Fork already uses server-granted ownership epochs (`Code/common/Authority/Lease.h`, `Code/client/Services/Generic/CharacterService.cpp`, `Code/server/Services/CharacterService.cpp`) and explicit waiting-for-assignment state. Upstream's 52-file protocol redesign is not cherry-picked; individual race fixes still need comparison against the fork's ownership lifecycle. |
| `59669245` | Present, adapted | `Code/client/Services/Generic/ObjectService.cpp` excludes the faction player-inventory and stolen-goods containers; `ObjectService::IsFactionCrimeContainer` checks loaded faction crime data, while the two quest-owned chest exclusions remain. |
| `ea11e634` | Core design present; patch-level review pending | Fork has an explicit `OwnershipSystem` (`Code/client/Systems/OwnershipSystem.cpp`) and epoch-based ownership decisions. Still compare upstream's exact threading and stale-epoch changes. |
| `121c25b5` | Behavior present | `Code/client/Components/WaitingFor3D.h` and `Code/client/Services/Generic/CharacterService.cpp` defer equipment application until actor 3D is available. |
| `23901108` | Present, adapted independently | `Code/common/PlaybackClock.h` implements adaptive per-entity jitter/interval estimation with a bounded playback delay; interpolation and tests are in `Code/client/Components/InterpolationComponent.h` and `Code/tests/rfab_stage2_physics_ownership_anim.cpp`. The same goal is implemented in the fork's existing playback architecture; upstream's protocol/action-event redesign is not implied by this match. |
| `390071ca` | Partially covered; audit remains | Fork has intrusive smart pointer support (`Code/client/Games/Skyrim/BSCore/BSTSmartPointer.h`) and guarded animation-manager handling, but this specific upstream leak fix has not been fully compared line-by-line. |
| `8770b370` | Not applicable to the fork's current hook map | Upstream change concerns ID 14375 (`CreateTemplateActorBase`); no matching hook or `SetLeveledNpc` implementation found in `Code/client`. Recheck when adding NPC template hooks. |
| `42a518a0` | Ported, strengthened | `Code/server/Services/InventoryService.cpp`: non-owner mutation of an owned NPC now requires sender/entity cell-range validation, and valid remote NPC looting/pickpocketing is broadcast even when `UpdateClients=false` (drop replay suppressed). Coverage added to `Tools/rfab/linux-server/harness/main.cpp` for out-of-range rejection, authoritative inventory update, and observer broadcast. |

## Deferred / not yet classified

The remaining commits in the 122 non-merge range have not all received a code-level audit. In particular, do not treat the following as resolved merely because they were assessed as important candidates: ownership handoff (`ea11e634`, `21d85fa6`, `b1f70409`, `121c25b5`, `e44ab60c`, `2e74896c`), animation/reference safety (`390071ca`, `25a0049a`), remote playback/cutscenes (`23901108`, `ea0f6c39`, `e9214a87`, `5a72cdcd`), menu input (`e5e8cafe`), and the UI/settings changes (`ed46728c`, `0698f159`, `9f9ee187`, `cae24433`). Build-system, localization, documentation, and engine-version-only commits require separate applicability review. N7 remains open until the full range is classified and high-risk missing behavior is either adapted or explicitly ruled out with evidence.

## Validation scope

No direct upstream merge was performed. Every port is adapted to the fork's service layout and SE 1.5.97 address map. Windows engine behavior and multiplayer behavior still require Windows CI and, where noted by upstream, in-game testing; source-level equivalence is not a substitute for a playtest.
