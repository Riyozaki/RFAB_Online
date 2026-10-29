#!/usr/bin/env python3
"""
Regression test suite for RFAB Online v1.2.0-hotfix.1:
  1. Verifies immutable v1.2.0 release backup manifest & Build-CloudRelease.ps1 clobber guards.
  2. Verifies UTF-8 BOM on all Tools/rfab/*.ps1 scripts (Windows PowerShell 5.1 Cyrillic safety).
  3. Verifies WeatherService non-leader StatsMenu/CustomMenu/interior ForceWeather guards.
  4. Verifies MO2 ModOrganizer.ini gamePath= priority in PathSelection.cpp & UNICODE_STRING bounds in FileMapping.cpp.
  5. Verifies D3D11 Render-Thread isolation of RfabAdapterService::OnDrawTacticalHud().
  6. Verifies version-agnostic StatsMenu pause detection & SkyrimSoulsRE.ini bStatsMenu=false self-healer.
"""
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    # 1. Backup manifest & release script protection
    manifest_path = ROOT / "docs" / "V1.2.0_RELEASE_BACKUP_MANIFEST.json"
    assert manifest_path.is_file(), f"Missing {manifest_path}"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["tag_name"] == "v1.2.0"
    assert manifest["target_commitish"] == "b431be0da38764ce1f4d658329934b4a8ccc5692"
    assert len(manifest["assets"]) == 4

    build_ps1 = (ROOT / "Tools" / "rfab" / "Build-CloudRelease.ps1").read_bytes()
    assert build_ps1.startswith(b"\xef\xbb\xbf"), "Build-CloudRelease.ps1 must retain UTF-8 BOM"
    build_txt = build_ps1[3:].decode("utf-8")
    for tag in ("v1.0.0", "v1.1.0", "v1.2.0", "v1.2.0-backup"):
        assert f'"{tag}"' in build_txt, f"Build-CloudRelease.ps1 must protect {tag}"

    for ps1 in sorted((ROOT / "Tools" / "rfab").glob("*.ps1")):
        assert ps1.read_bytes().startswith(b"\xef\xbb\xbf"), f"Missing UTF-8 BOM on {ps1}"

    # 2. WeatherService non-leader StatsMenu CTD fix
    weather_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "WeatherService.cpp").read_text(encoding="utf-8")
    assert "CanMutateOutdoorSkyWeather" in weather_cpp
    assert 'BSFixedString("StatsMenu")' in weather_cpp
    assert 'BSFixedString("CustomMenu")' in weather_cpp
    assert "IsLocalPlayerInExteriorWorldspace" in weather_cpp
    assert "else\n            SetCachedWeather();" not in weather_cpp.replace("\r\n", "\n")

    # 3. MO2 ModOrganizer.ini gamePath= priority & FileMapping UNICODE_STRING byte-length fix
    path_sel_cpp = (ROOT / "Code" / "immersive_launcher" / "oobe" / "PathSelection.cpp").read_text(encoding="utf-8")
    assert "RfabCompat::ResolveMo2ConfiguredGamePath" in path_sel_cpp

    file_map_cpp = (ROOT / "Code" / "immersive_launcher" / "stubs" / "FileMapping.cpp").read_text(encoding="utf-8")
    assert "DllName->Length / sizeof(wchar_t)" in file_map_cpp
    assert "std::wcsncmp(TARGET_NAME L\".exe\", DllName->Buffer, DllName->Length)" not in file_map_cpp

    # 4. RfabAdapterService::OnDrawTacticalHud render-thread snapshot isolation
    rfab_cpp = (ROOT / "Code" / "client" / "Services" / "RfabAdapterService.cpp").read_text(encoding="utf-8")
    assert "RefreshTacticalHudSnapshot();" in rfab_cpp
    draw_idx = rfab_cpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept")
    assert draw_idx != -1
    draw_body = rfab_cpp[draw_idx:]
    assert "m_tacticalHudSnapshotMutex" in draw_body
    for forbidden in ("BSFixedString", "GetMenuOpen", "m_world.view", "TESForm::GetById", "WorldPtToScreenPt3", "GetPartyService"):
        assert forbidden not in draw_body, f"Render-thread hazard '{forbidden}' found inside OnDrawTacticalHud!"

    # 5. Version-agnostic StatsMenu pause detection & SkyrimSoulsRE.ini bStatsMenu = false patcher
    skills_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Interface" / "Menus" / "SkillsMenu.cpp").read_text(encoding="utf-8")
    ui_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Interface" / "UI.cpp").read_text(encoding="utf-8")
    assert "HasCustomSkillsOrRfabFilesUnderData" in skills_cpp
    assert "ShouldKeepStatsMenuPausedRuntime()" in ui_cpp

    spec = importlib.util.spec_from_file_location(
        "patch_rfab_modpack_compat", ROOT / "Tools" / "rfab" / "patch_rfab_modpack_compat.py"
    )
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    patched_souls, changed = mod.patch_skyrim_souls_ini(
        "[UNPAUSED_MENUS]\nbStatsMenu = true\nbCustomMenu = true\n[SLOWMOTION]\nfSlowMotionMultiplier = 0.25\n"
    )
    assert changed
    assert "bStatsMenu = false" in patched_souls
    assert "bCustomMenu = true" in patched_souls
    assert "fSlowMotionMultiplier = 1.000000" in patched_souls

    # 6. v2.0.1 Pillar 1: GamePtr<T> uninitialized m_pPointer / move-assignment Release() fix (all 4 0x65D2CB CTDs)
    primitives_h = (ROOT / "Code" / "client" / "Games" / "Primitives.h").read_text(encoding="utf-8")
    assert "T* m_pPointer{nullptr};" in primitives_h
    assert "GamePtr(std::nullptr_t) noexcept" in primitives_h
    assert "m_pPointer(aRhs.m_pPointer)" in primitives_h

    # 7. v2.0.1 Pillar 2: Fort Greymoor post-wipe AI & WorldView rollback synchronization
    downed_cpp = (ROOT / "Code" / "client" / "Services" / "DownedStateService.cpp").read_text(encoding="utf-8")
    coord_save_cpp = (ROOT / "Code" / "client" / "Services" / "CoordinatedSaveService.cpp").read_text(encoding="utf-8")
    snap_cpp = (ROOT / "Code" / "server" / "Services" / "SaveSnapshotService.cpp").read_text(encoding="utf-8")
    srv_char_cpp = (ROOT / "Code" / "server" / "Services" / "CharacterService.cpp").read_text(encoding="utf-8")
    assert "RestoreLivingCombatTargetability" in downed_cpp
    assert "NormalizePostReviveActorState" in downed_cpp
    assert "SchedulePostReviveHeadAndCameraSettle" in coord_save_cpp
    assert "NotifyInSessionRollbackApplied" in coord_save_cpp
    assert "kNpcSpatialExtMagic" in snap_cpp
    assert "ForceResyncObserverAfterRollback" in snap_cpp
    assert "ForceResyncObserverAfterRollback" in srv_char_cpp

    # 8. v2.0.1 Pillar 3: Multi-threaded engine hook isolation (zero raw EnTT views on worker threads)
    combat_ctrl_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Combat" / "CombatController.cpp").read_text(encoding="utf-8")
    magic_target_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Magic" / "MagicTarget.cpp").read_text(encoding="utf-8")
    refr_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "TESObjectREFR.cpp").read_text(encoding="utf-8")
    spell_evt_h = (ROOT / "Code" / "client" / "Events" / "SpellCastEvent.h").read_text(encoding="utf-8")
    assert "HasCombatTargetLock()" in combat_ctrl_cpp
    assert "GetCombatService()" not in combat_ctrl_cpp
    assert "CollectPartyPeerActorFormIds" in magic_target_cpp
    assert "world.view" not in magic_target_cpp
    assert "IsFactionCrimeContainer" in refr_cpp
    assert "ActorMagicCaster* pCaster" not in spell_evt_h and "CasterFormId" in spell_evt_h

    # 9. v2.0.1 Pillars 4-8: Dynamic crime factions, 0x3000802 FaceGen flags, crash unwinder, CEF overlay
    player_char_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "PlayerCharacter.cpp").read_text(encoding="utf-8")
    cli_char_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterService.cpp").read_text(encoding="utf-8")
    cli_char_move_assign_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterMovementAndAssignmentSubservice.cpp").read_text(encoding="utf-8")
    cli_char_xp_diag_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterXpAndDialogueSubservice.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "Code" / "client" / "main.cpp").read_text(encoding="utf-8")
    overlay_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "OverlayService.cpp").read_text(encoding="utf-8")
    assert "pModManager->factions" in player_char_cpp
    assert "0x3000802u" in (cli_char_cpp + cli_char_move_assign_cpp)
    assert "CleanupUnassignedActorEntity" in cli_char_cpp
    assert "RtlVirtualUnwind" in main_cpp
    assert "RFAB_Online_Crash.log" in main_cpp
    assert "CoopOverlayApp" in overlay_cpp

    # 10. Stages 0-3 SkyMP-grade Root-Cause Stabilization & Cross-Version RFAB Compatibility
    backup_manifest = (ROOT / "docs" / "V2.0.1_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    actor_ext_h = (ROOT / "Code" / "client" / "Games" / "ActorExtension.h").read_text(encoding="utf-8")
    memory_cpp = (ROOT / "Code" / "client" / "Games" / "Memory.cpp").read_text(encoding="utf-8")
    ni_ptr_h = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "NetImmerse" / "NiPointer.h").read_text(encoding="utf-8")
    actor_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp").read_text(encoding="utf-8")
    rfab_compat_h = (ROOT / "Code" / "common" / "RfabModpackCompat.h").read_text(encoding="utf-8")
    rfab_adapter_cpp = (ROOT / "Code" / "client" / "Services" / "RfabAdapterService.cpp").read_text(encoding="utf-8")
    rfab_resolver_cpp = (ROOT / "Code" / "client" / "Services" / "Rfab" / "RfabFormResolver.cpp").read_text(encoding="utf-8")
    assert '"v2.0.1-backup"' in backup_manifest
    assert "std::is_trivially_destructible_v<ActorExtension>" in actor_ext_h
    assert "kActorValueListByteSize = 0x528" in memory_cpp
    assert "TP_HOOK_IAT(_recalloc, cHeapModule)" in memory_cpp
    assert "~NiPointer() noexcept" in ni_ptr_h
    assert "TriggerNativeBleedout" in actor_cpp and "LatchNativeBleedoutEntered" in actor_cpp
    assert "65536.0f" not in actor_cpp and "ScopedLethalPlayerDamageIntercept" not in actor_cpp
    assert "pPlayer->DispelHostileEffects();" in coord_save_cpp
    assert "pPlayer->DispelAllSpells();" not in coord_save_cpp
    assert "MoveToWorldOrCell" in coord_save_cpp
    assert "ownerComponent.SetOwner(nullptr, nowMs)" in srv_char_cpp
    cli_char_h = (ROOT / "Code" / "client" / "Services" / "CharacterService.h").read_text(encoding="utf-8")
    assert "m_entityIndex" in cli_char_h and "ReconcilePlacedActorBaseFormInPlace" in cli_char_h
    assert "m_suppressedPlacedActors" not in cli_char_h
    assert "class EspEdidIndex" in rfab_compat_h
    assert "ParseActivePluginList" in rfab_compat_h
    assert "ResolveRfabConfigPath" in rfab_compat_h
    assert "m_espEdidIndex.IndexRfabPluginsInDataDir" in (rfab_adapter_cpp + rfab_resolver_cpp)

    # 11. v2.0.2 SkyMP-Grade Server Authority, Anti-Dupe Ledger, Zero-Spoof Security & O(1) Indexing
    loot_ledger_h = (ROOT / "Code" / "common" / "CoopLootLedger.h").read_text(encoding="utf-8")
    srv_inv_cpp = (ROOT / "Code" / "server" / "Services" / "InventoryService.cpp").read_text(encoding="utf-8")
    srv_av_cpp = (ROOT / "Code" / "server" / "Services" / "ActorValueService.cpp").read_text(encoding="utf-8")
    srv_magic_cpp = (ROOT / "Code" / "server" / "Services" / "MagicService.cpp").read_text(encoding="utf-8")
    srv_mods_cpp = (ROOT / "Code" / "server" / "Components" / "ModsComponent.cpp").read_text(encoding="utf-8")
    srv_game_cpp = (ROOT / "Code" / "server" / "GameServer.cpp").read_text(encoding="utf-8")
    srv_party_cpp = (ROOT / "Code" / "server" / "Services" / "PartyService.cpp").read_text(encoding="utf-8")
    srv_obj_cpp = (ROOT / "Code" / "server" / "Services" / "ObjectService.cpp").read_text(encoding="utf-8")
    cli_utils_h = (ROOT / "Code" / "client" / "Utils.h").read_text(encoding="utf-8")
    assert "class ContainerLootLedger" in loot_ledger_h
    assert "m_lootLedger.EvaluateWithdrawal" in srv_inv_cpp and "OverdrawnRollback" in srv_inv_cpp
    assert "kExternalDamageReconcileWindowMs" in srv_av_cpp and "IsBossGhost(message.Id)" in srv_av_cpp
    assert "senderAuthorized" in srv_magic_cpp
    assert "NormalizeModFilenameKey" in srv_mods_cpp
    assert "Duplicate AuthenticationRequest" in srv_game_cpp
    assert "m_world.view<PartyComponent>()" not in srv_party_cpp
    assert "m_objectEntityByKey" in srv_obj_cpp
    assert "FindEntityByServerId(acServerId)" in cli_utils_h

    # 12. v2.0.3 Architectural Stabilization (WorldView Two-Phase Ownership, Multi-Hop EngineFixes, RFAB Combat & Checkpoint Identity)
    v202_backup_manifest = (ROOT / "docs" / "V2.0.2_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    dmg_pipe_h = (ROOT / "Code" / "common" / "CoopDamagePipeline.h").read_text(encoding="utf-8")
    save_snap_cpp = (ROOT / "Code" / "server" / "Services" / "SaveSnapshotService.cpp").read_text(encoding="utf-8")
    cli_char_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterService.cpp").read_text(encoding="utf-8")
    assert '"v2.0.2-backup"' in v202_backup_manifest and "424319337" in v202_backup_manifest
    assert "DecodeHookJumpTargetLive" in dmg_pipe_h
    assert "ScriptDamageAuthLedger" in dmg_pipe_h and "ConcentrationSyncLimiter" in dmg_pipe_h
    assert "kV203IdentityAndLedgerExtMagic" in save_snap_cpp and "HydratePersistedWorldState" in save_snap_cpp
    assert "PendingOwnershipEpoch" in cli_char_cpp and "SyncRemoteCachedInventory" in cli_char_cpp
    assert "WakeDormantCharactersInActiveRanges" in srv_char_cpp and "NotifyDrawWeapon" in srv_inv_cpp

    # 13. v2.1.0 Root-Cause Architectural Stabilization (RemoteEntityModel SSOT, Zero Linear Scans, Bidirectional InterestRegistry, ServerCombatAuthorityLedger, PartySessionLedger, TLV PersistentActorsByKey)
    v203_backup_manifest = (ROOT / "docs" / "V2.0.3_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    world_view_h = (ROOT / "Code" / "common" / "CoopWorldView.h").read_text(encoding="utf-8")
    checkpoint_h = (ROOT / "Code" / "common" / "CoopCheckpointStore.h").read_text(encoding="utf-8")
    cli_magic_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "MagicService.cpp").read_text(encoding="utf-8")
    cli_combat_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CombatService.cpp").read_text(encoding="utf-8")
    cli_object_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "ObjectService.cpp").read_text(encoding="utf-8")
    assert '"v2.0.3-backup"' in v203_backup_manifest and "424566518" in v203_backup_manifest
    assert '"v2.0.3-backup"' in build_txt and "Ensure-V203BackupRelease" in build_txt
    assert "ResolveAuthoritativeSpawnState" in cli_char_h and "HasAuthoritativeSpawnPacket" in cli_char_h
    assert "std::find_if" not in cli_char_cpp
    assert "std::find_if" not in cli_magic_cpp
    assert "std::find_if" not in cli_combat_cpp
    assert "std::find_if" not in cli_object_cpp
    assert "BindEntityCell" in world_view_h and "ObserversOfEntity" in world_view_h and "PartySessionLedger" in world_view_h
    assert "ServerCombatAuthorityLedger" in dmg_pipe_h and "ValidateNonOwnerHealthDelta" in srv_av_cpp
    assert "m_sessionLedger" in srv_party_cpp and "FoundWPartyId" in (ROOT / "Code" / "server" / "Services" / "QuestService.cpp").read_text(encoding="utf-8")
    assert "TlvContainerBuilder" in checkpoint_h and "ParseTlvContainer" in checkpoint_h
    assert "PersistentActorsByKey" in save_snap_cpp and "EnsureDormantPersistentCharacter" in srv_char_cpp

    # 14. v2.1.0 Playtest Root-Cause Fixes (3D-Readiness Invariant, Thread-Safe BehaviorVar/DescriptorManager, Party-Wipe Rollback Anchoring & Control Lock, Leveled NPC Template Sync & Voice Gender Guard)
    desc_mgr_h = (ROOT / "Code" / "encoding" / "Structs" / "AnimationGraphDescriptorManager.h").read_text(encoding="utf-8")
    desc_mgr_cpp = (ROOT / "Code" / "encoding" / "Structs" / "AnimationGraphDescriptorManager.cpp").read_text(encoding="utf-8")
    behavior_var_cpp = (ROOT / "Code" / "client" / "ModCompat" / "BehaviorVar.cpp").read_text(encoding="utf-8")
    tes_npc_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Forms" / "TESNPC.cpp").read_text(encoding="utf-8")
    actor_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp").read_text(encoding="utf-8")
    downed_cpp = (ROOT / "Code" / "client" / "Services" / "DownedStateService.cpp").read_text(encoding="utf-8")
    coop_anim_h = (ROOT / "Code" / "common" / "CoopAnimationState.h").read_text(encoding="utf-8")
    assert "std::shared_mutex m_mutex" in desc_mgr_h and "std::unordered_map<uint64_t, AnimationGraphDescriptor>" in desc_mgr_h
    assert "m_descriptors.try_emplace" in desc_mgr_cpp
    assert "std::recursive_mutex mutex_lock" in behavior_var_cpp
    assert "HookSetLeveledNpc" in tes_npc_cpp and "14257" in tes_npc_cpp
    assert "Actor::GetLeveledTemplateBase" in actor_cpp and "ExtraDataType::LeveledCreature" in actor_cpp
    assert "ShouldAnchorPartyMemberToLeaderOnRollback" in checkpoint_h and "ShouldAnchorPartyMemberToLeaderOnRollback" in save_snap_cpp
    assert "pControls->pJumpHandler->isEnabled = true" in downed_cpp and "pControls->togglePOVHandler->isEnabled = true" in downed_cpp
    assert "m_postReviveBleedoutExitFrames" in downed_cpp
    assert "HasVoicePathGenderMismatch" in coop_anim_h and "HasVoicePathGenderMismatch" in (cli_char_cpp + cli_char_xp_diag_cpp)
    assert "if (pOwningPlayer != nullptr && pOwningPlayer == acMessage.pPlayer)" in srv_char_cpp

    # 15. v3.0 Phase 1 Clean Slate & Thread-Safe Core (ScopedScratchFrame, Direct World Service Pointers & Cached PVP, Simulation Authority != Combat Target, Zero Forced Combat Exit / Boss Heal Ping-Pong)
    packet_safety_h = (ROOT / "Code" / "common" / "CoopPacketSafety.h").read_text(encoding="utf-8")
    cli_transport_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "TransportService.cpp").read_text(encoding="utf-8")
    srv_gameserver_cpp = (ROOT / "Code" / "server" / "GameServer.cpp").read_text(encoding="utf-8")
    adm_transport_cpp = (ROOT / "Code" / "admin" / "Transport.cpp").read_text(encoding="utf-8")
    world_h = (ROOT / "Code" / "client" / "World.h").read_text(encoding="utf-8")
    world_cpp = (ROOT / "Code" / "client" / "World.cpp").read_text(encoding="utf-8")
    combat_ctrl_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Combat" / "CombatController.cpp").read_text(encoding="utf-8")
    magic_target_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Magic" / "MagicTarget.cpp").read_text(encoding="utf-8")
    rfab_compat_h = (ROOT / "Code" / "common" / "RfabModpackCompat.h").read_text(encoding="utf-8")
    assert "class ScopedScratchFrame" in packet_safety_h
    assert "CoopPacketSafety::ScopedScratchFrame" in cli_transport_cpp and "m_allocator.Reset()" not in cli_transport_cpp
    assert "CoopPacketSafety::ScopedScratchFrame" in srv_gameserver_cpp and "m_allocator.Reset()" not in srv_gameserver_cpp
    assert "CoopPacketSafety::ScopedScratchFrame" in adm_transport_cpp and "m_allocator.Reset()" not in adm_transport_cpp
    assert "IsPvpEnabledCached()" in world_h and "m_pRfabAdapterService" in world_h and "m_pDownedStateService" in world_h
    assert "m_pRfabAdapterService = &ctx().emplace<RfabAdapterService>" in world_cpp
    assert "ScopedRemoteAddTargetState" in magic_target_cpp and "World::IsPvpEnabledCached()" in actor_cpp
    assert "pDet->level = -1000" not in actor_cpp
    assert "apThis->SetTarget(nullptr)" not in combat_ctrl_cpp and "if (spReplacement)" in combat_ctrl_cpp
    assert "ClaimCombatActorsFromDownedPeer" not in cli_char_h and "ClaimCombatActorsFromDownedPeer" not in cli_char_cpp and "ClaimCombatActorsFromDownedPeer" not in downed_cpp
    assert "ShouldBlockFalseBossResetHeal" not in srv_av_cpp and "ShouldBlockFalseBossResetHeal" not in rfab_compat_h

    # 16. v3.0 Phase 2 Native 0.0f HP Bleedout/Revive, Ally Healing-Spell Revive, Sticky Host Cell Authority & Auto-Party
    player_svc_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "PlayerService.cpp").read_text(encoding="utf-8")
    downed_h = (ROOT / "Code" / "client" / "Services" / "DownedStateService.h").read_text(encoding="utf-8")
    revive_req_h = (ROOT / "Code" / "encoding" / "Messages" / "ReviveActionRequest.h").read_text(encoding="utf-8")
    enc_h = (ROOT / "Code" / "server" / "Services" / "EncounterService.h").read_text(encoding="utf-8")
    enc_cpp = (ROOT / "Code" / "server" / "Services" / "EncounterService.cpp").read_text(encoding="utf-8")
    cli_magic_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "MagicService.cpp").read_text(encoding="utf-8")
    ownership_h = (ROOT / "Code" / "common" / "CoopOwnership.h").read_text(encoding="utf-8")
    assert "keepNativeBleedoutMode" in player_svc_cpp and "SetPlayerRespawnMode(false)" in player_svc_cpp
    assert "LatchNativeBleedoutEntered" in downed_h and "FinalizeLethalHitCushion" not in downed_h
    assert "kDownedFloorHealth" not in downed_cpp and "kNativeBleedoutFloorHealth = 0.0f" in downed_cpp
    assert "HealingSpellRevive = 2" in revive_req_h and "TryInstantSpellRevive" in enc_h and "HealingSpellRevive" in enc_cpp
    assert "RequestAllySpellReviveOnActor" in cli_magic_cpp and "NotifyAllyHealingSpellReceivedFromRemote" in cli_magic_cpp
    assert "aChallengerIsCalmPartyLeader" in ownership_h and "Sticky Host Cell Authority" in srv_char_cpp
    assert "primaryPartyId" in srv_party_cpp and "m_parties.size() == 1" not in srv_party_cpp

    # 17. v3.0 Phase 3 WorldReloadBarrier (Live-Socket .ess Checkpoint Reload, Zero SetActorInventory, 0x14 Reuse, Host-First Gate)
    discovery_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "DiscoveryService.cpp").read_text(encoding="utf-8")
    save_snap_h = (ROOT / "Code" / "server" / "Services" / "SaveSnapshotService.h").read_text(encoding="utf-8")
    assert "kReloadBarrierEpochBit" in checkpoint_h and "CanLoadPreJoinSlotInCurrentSession" in checkpoint_h
    assert "m_liveSessionWorldReloadPending" in coord_save_cpp and "CompleteLiveWorldReloadAfterEngineReady" in coord_save_cpp
    assert "SetActorInventory(" not in coord_save_cpp
    assert "IsHoldingWorldDiscoveryForHostBarrier()" in discovery_cpp
    assert "RestoreMultiplayerGlobalsAfterWorldReload()" in player_svc_cpp
    assert "k_ESteamNetworkingConfig_TimeoutConnected" in cli_transport_cpp and "k_ESteamNetworkingConfig_TimeoutConnected" in srv_gameserver_cpp
    assert "struct WorldReloadBarrier" in save_snap_h and "ShouldHoldNonHostWorldClaims" in save_snap_h
    assert "BeginWorldReloadBarrier(" in save_snap_cpp and "SyncPostReloadStateToPlayer(" in save_snap_cpp
    assert "ResetWorldEntitiesForCheckpointReload" in srv_char_cpp and "[RFAB WorldReloadBarrier] Reused player" in srv_char_cpp

    # 18. v3.0 Phase 4 Clean Damage, Movement & Animation Pipeline (Native MaxHP 1/M Scaling, Seamless Exterior Cell-Border Motion & BehaviorVar Blending)
    cli_av_h = (ROOT / "Code" / "client" / "Services" / "ActorValueService.h").read_text(encoding="utf-8")
    cli_av_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "ActorValueService.cpp").read_text(encoding="utf-8")
    interp_cpp = (ROOT / "Code" / "client" / "Systems" / "InterpolationSystem.cpp").read_text(encoding="utf-8")
    refr_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "TESObjectREFR.cpp").read_text(encoding="utf-8")
    ag_desc_h = (ROOT / "Code" / "encoding" / "Structs" / "AnimationGraphDescriptor.h").read_text(encoding="utf-8")
    master_desc_cpp = (ROOT / "Code" / "encoding" / "Structs" / "Skyrim" / "AnimationGraphDescriptor_Master_Behavior.cpp").read_text(encoding="utf-8")
    assert "m_unscaledMaxHealth" not in cli_av_h and "ApplyHealthMultiplier" not in cli_av_cpp and "SetScaledHealthSafely" not in cli_av_cpp
    assert "SeedUnscaledMaxHealthFromRemote" not in cli_char_cpp and "CaptureNativeBaseMaxHealth" not in cli_char_cpp
    assert "GetEnemyHealthMultiplierCached" in world_h and "ScaleIncomingEnemyDamageDelta" in actor_cpp and "ApplyAuthoritativeServerHealthDelta" in srv_av_cpp
    assert "EvaluateExteriorCellMotion" in interp_cpp and "MoveToCellSeamless" in interp_cpp and "!apActor->GetNiNode()" not in interp_cpp
    assert "void TESObjectREFR::MoveToCellSeamless" in refr_cpp
    assert "AnimationFloatBlendMode" in ag_desc_h and "kCircularDegrees360" in master_desc_cpp and "kDiscreteStep" in master_desc_cpp
    assert "ClassifyBehaviorFloatBlendKind" in behavior_var_cpp and "BlendAnimationFloatValue" in interp_cpp

    # 19. v3.0 Phase 5 Modular Decomposition of CharacterService & RfabAdapterService + CoopExperienceLedger & O(1) Mount Retry
    xp_ledger_h = (ROOT / "Code" / "common" / "CoopExperienceLedger.h").read_text(encoding="utf-8")
    rfab_adapter_h = (ROOT / "Code" / "client" / "Services" / "RfabAdapterService.h").read_text(encoding="utf-8")
    rfab_xp_book_cpp = (ROOT / "Code" / "client" / "Services" / "Rfab" / "RfabXpAndBookSync.cpp").read_text(encoding="utf-8")
    rfab_combat_cpp = (ROOT / "Code" / "client" / "Services" / "Rfab" / "RfabCombatRules.cpp").read_text(encoding="utf-8")
    rfab_hud_cpp = (ROOT / "Code" / "client" / "Services" / "Rfab" / "RfabHudAndPings.cpp").read_text(encoding="utf-8")
    cli_char_idx_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterEntityIndexSubservice.cpp").read_text(encoding="utf-8")
    cli_char_mount_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterMountAndPackageSubservice.cpp").read_text(encoding="utf-8")
    srv_char_social_cpp = (ROOT / "Code" / "server" / "Services" / "CharacterSocialAndMountSubservice.cpp").read_text(encoding="utf-8")

    assert "class ClientRfabXpTracker" in xp_ledger_h and "NormalizeSenderBaseXp" in xp_ledger_h
    assert len(rfab_adapter_cpp.splitlines()) < 600
    assert len(cli_char_cpp.splitlines()) < 2900
    assert len(srv_char_cpp.splitlines()) < 1950
    assert "RfabFormResolver m_formResolver" in rfab_adapter_h and "RfabHudAndPings m_hudAndPings" in rfab_adapter_h
    assert "ApplyDualPoolExperienceDelta" in rfab_xp_book_cpp and "EvaluateSnapshotAction" in rfab_combat_cpp and "BuildHudSnapshot" in rfab_hud_cpp
    assert "CharacterService::FindEntityByServerId" in cli_char_idx_cpp
    assert "CharacterService::RetryPendingMounts" in cli_char_mount_cpp and "mountActorView" not in cli_char_mount_cpp
    assert "m_rfabXpTracker.EvaluateLocalWalletPoll" in cli_char_xp_diag_cpp
    assert "CoopExperienceLedger::kMaxRfabXpDelta" in srv_char_social_cpp
    v210_backup_manifest = (ROOT / "docs" / "V2.1.0_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v2.1.0-backup"' in v210_backup_manifest and "424756541" in v210_backup_manifest
    v300_backup_manifest = (ROOT / "docs" / "V3.0.0_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v3.0.0-backup"' in v300_backup_manifest and "425670346" in v300_backup_manifest
    v310_backup_manifest = (ROOT / "docs" / "V3.1.0_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v3.1.0-backup"' in v310_backup_manifest and "425952399" in v310_backup_manifest
    v311_backup_manifest = (ROOT / "docs" / "V3.1.1_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v3.1.1-backup"' in v311_backup_manifest and "426193785" in v311_backup_manifest
    v312_backup_manifest = (ROOT / "docs" / "V3.1.2_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v3.1.2-backup"' in v312_backup_manifest and "427038318" in v312_backup_manifest
    v313_backup_manifest = (ROOT / "docs" / "V3.1.3_RELEASE_BACKUP_MANIFEST.json").read_text(encoding="utf-8")
    assert '"v3.1.3-backup"' in v313_backup_manifest and "427269248" in v313_backup_manifest
    assert '"v3.1.0-backup"' in build_txt and "Ensure-V310BackupRelease" in build_txt
    assert '"v3.1.1-backup"' in build_txt and "Ensure-V311BackupRelease" in build_txt
    assert '"v3.1.2-backup"' in build_txt and "Ensure-V312BackupRelease" in build_txt
    assert '"v3.1.3-backup"' in build_txt and "Ensure-V313BackupRelease" in build_txt and '[string]$VersionTag = "v3.1.6"' in build_txt
    assert '"v3.1.4"' in build_txt and '"v3.1.5"' in build_txt

    # 20. v3.1 Hybrid Combat, 1/M Regen Parity, 0x14 Perk-Exact Magic, Concentration Beam Sync, Hit Impact & Host-Canonical BGSScene
    cli_quest_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "QuestService.cpp").read_text(encoding="utf-8")
    srv_quest_cpp = (ROOT / "Code" / "server" / "Services" / "QuestService.cpp").read_text(encoding="utf-8")
    anim_cpp = (ROOT / "Code" / "client" / "Games" / "Animation.cpp").read_text(encoding="utf-8")
    assert "ScaleIncomingEnemyHealDelta" in dmg_pipe_h and "ScaleIncomingEnemyHealDelta" in actor_cpp
    assert "ComputeCasterOutgoingPerkMultiplier" in dmg_pipe_h and "ScopedLocalCasterPerkCapture" in magic_target_cpp
    assert "allowLocalPlayerPvpBeam" in magic_target_cpp and "resyncEvt.HasPreAdjustedPerks = true;" in cli_magic_cpp
    assert "s_inLocalCasterConcentrationEffect" in actor_cpp and "m_concentrationClaimKeepaliveMs" in cli_magic_cpp
    assert "ShouldReconcileRemoteActorHealthSnapshot" in dmg_pipe_h and "ShouldReconcileRemoteActorHealthSnapshot" in cli_av_cpp
    assert "std::max(maxHealth, currentHealth)" in srv_av_cpp and "pActor->InterruptCast(false);" in cli_av_cpp
    assert "RemoteHitImpactGate" in dmg_pipe_h and "m_remoteHitImpactGate.TryConsumeStagger" in cli_av_cpp
    assert "TryAcceptRemoteHitReactionAction" in actor_cpp and "Actor::TryAcceptRemoteHitReactionAction" in anim_cpp
    assert "ScopedRemoteQuestUpdateGuard remoteSceneGuard" in cli_quest_cpp and "ignoring non-leader scene" in srv_quest_cpp
    assert "update.Status = RequestQuestUpdate::StageUpdateForced;" in cli_quest_cpp

    # 21. v3.1.3 Carriage Fast-Travel & Main-Thread Shader Stability
    skyrim_vm_cpp = (ROOT / "Code" / "client" / "SkyrimVM64.cpp").read_text(encoding="utf-8")
    world_cpp = (ROOT / "Code" / "client" / "World.cpp").read_text(encoding="utf-8")
    facegen_cpp = (ROOT / "Code" / "client" / "Systems" / "FaceGenSystem.cpp").read_text(encoding="utf-8")
    cli_assign_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "CharacterMovementAndAssignmentSubservice.cpp").read_text(encoding="utf-8")
    tp_guard_h = (ROOT / "Code" / "common" / "CoopTeleportGuard.h").read_text(encoding="utf-8")
    tes_refr_h = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "TESObjectREFR.h").read_text(encoding="utf-8")
    tes_refr_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "TESObjectREFR.cpp").read_text(encoding="utf-8")

    assert "World::Get().OnMainThreadUpdate();" in skyrim_vm_cpp
    assert "void World::OnMainThreadUpdate() noexcept" in world_cpp and "void World::QueueMainThreadTask(" in world_cpp
    world_update_body = world_cpp.split("void World::Update() noexcept", 1)[1].split("RunnerService& World::GetRunner()", 1)[0]
    assert "s_mainThreadId.store" not in world_update_body
    assert "IsSafeMainThreadSceneGraphWindow()" in facegen_cpp and "ExecuteMainThreadPass(" in facegen_cpp
    assert "Fader Menu" in facegen_cpp and "RestoreLocalHairTintDriftRecursive(" in facegen_cpp
    assert "ScheduleMainThreadFaceGenPass()" in cli_char_cpp and "FaceGenSystem::Update(" not in cli_char_cpp
    poll_body = cli_assign_cpp.split("void CharacterService::PollLocalPlayerAppearanceSync() noexcept", 1)[1].split("void CharacterService::ScheduleMainThreadFaceGenPass()", 1)[0]
    assert "UpdateLocalPlayerHairTintIsolation" not in poll_body
    assert "GetNiNode3rdPerson(" in tes_refr_h and "GetNiNode3rdPerson(false)" in facegen_cpp
    assert "NoteLocalDirectPlayerTravelInteraction" in tp_guard_h and "NoteLocalDirectPlayerTravelInteraction" in tes_refr_cpp

    # 22. v3.1.4 Single-Thread WinMain Frame Architecture, Cell-Seam Stabilization & Worker-Hook Thread Safety
    tes_cell_h = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Forms" / "TESObjectCELL.h").read_text(encoding="utf-8")
    stabilizer_h = (ROOT / "Code" / "common" / "CoopWorldStabilizer.h").read_text(encoding="utf-8")
    main_cpp = (ROOT / "Code" / "client" / "main.cpp").read_text(encoding="utf-8")
    launcher_cpp = (ROOT / "Code" / "immersive_launcher" / "Launcher.cpp").read_text(encoding="utf-8")
    downed_cpp = (ROOT / "Code" / "client" / "Services" / "DownedStateService.cpp").read_text(encoding="utf-8")
    party_h = (ROOT / "Code" / "client" / "Services" / "PartyService.h").read_text(encoding="utf-8")
    inv_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "InventoryService.cpp").read_text(encoding="utf-8")

    vm_hook_body = skyrim_vm_cpp.split("int TP_MAKE_THISCALL(HookVMUpdate", 1)[1].split("short TP_MAKE_THISCALL(HookMainLoop", 1)[0]
    main_hook_body = skyrim_vm_cpp.split("short TP_MAKE_THISCALL(HookMainLoop", 1)[1].split("uintptr_t TP_MAKE_THISCALL(HookVMDestructor", 1)[0]
    assert "g_appInstance->Update()" not in vm_hook_body
    assert "s_vmActiveSinceLastMainLoop.store(true" in vm_hook_body
    assert "g_appInstance->Update()" in main_hook_body and "!apThis->freezeTime" in main_hook_body
    assert "IsAttachedAndLoaded()" in tes_cell_h
    assert "CanTouchActorGraphDuringDetach" in stabilizer_h and "RelinquishClaimCooldownTracker" in stabilizer_h
    assert "IsActorLiveInAttachedCellWith3D" in cli_char_cpp and "m_relinquishClaimCooldowns" in cli_assign_cpp
    assert "AtomicOrActorFlags1" in magic_target_cpp and "s_scopedTeammateRefCounts" in magic_target_cpp
    assert "GetServerSettings().PvpEnabled" not in magic_target_cpp
    assert "std::atomic<bool> m_isLeader" in party_h
    assert "if (!World::IsMainThread())" in downed_cpp and "if (!World::IsMainThread())" in rfab_adapter_cpp
    assert "FlushPendingNpcAmmoDeltas" in inv_cpp
    assert "TiltedTrampoline" in main_cpp and "s_crashDedupMutex" in main_cpp
    assert "mo2FromExe" in launcher_cpp

    # 23. v3.1.5 Long-Session Paused-Menu Pump, Scalar ActivateEvent & Hook Hardening
    activate_evt_h = (ROOT / "Code" / "client" / "Events" / "ActivateEvent.h").read_text(encoding="utf-8")
    transport_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "TransportService.cpp").read_text(encoding="utf-8")
    overlay_client_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "OverlayClient.cpp").read_text(encoding="utf-8")
    coord_save_cpp = (ROOT / "Code" / "client" / "Services" / "CoordinatedSaveService.cpp").read_text(encoding="utf-8")
    invis_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Effects" / "InvisibilityEffect.cpp").read_text(encoding="utf-8")
    refs_cpp = (ROOT / "Code" / "client" / "Games" / "References.cpp").read_text(encoding="utf-8")

    assert "SelectMainLoopUpdateMode" in stabilizer_h and "ClassifyPausedInboundPacket" in stabilizer_h
    assert "UpdatePausedPump()" in main_hook_body and "void World::UpdatePausedPump() noexcept" in world_cpp
    assert "PollWhileSimulationPaused()" in transport_cpp and "FlushPausedInboundPackets()" in transport_cpp
    assert "opcode >= static_cast<uint32_t>(kServerOpcodeMax) || !m_messageHandlers[opcode]" in transport_cpp
    assert "QueueMainThreadTask" in overlay_client_cpp
    assert 'BSFixedString("StatsMenu")' in coord_save_cpp and 'BSFixedString("Console")' in coord_save_cpp
    assert "void CoordinatedSaveService::TickWhilePaused(" in coord_save_cpp
    assert "TESObjectREFR* pObject" not in activate_evt_h and "uint32_t ObjectId{0};" in activate_evt_h
    assert "IsAttachedAndLoaded()" in invis_cpp
    assert "s_cachedRfabActive" in refs_cpp and "ResolveRfabPluginMod" not in refs_cpp

    # 24. v3.1.6 Adversarial Audit: Causal-FIFO Paused Queue, High-Hz MainLoop Mode, NonActorMagicCaster & Reload Polish
    world_h = (ROOT / "Code" / "client" / "World.h").read_text(encoding="utf-8")
    player_char_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "PlayerCharacter.cpp").read_text(encoding="utf-8")
    assert "if (!m_connected)" not in transport_cpp and "if (!IsConnected())" in transport_cpp
    assert "m_lastPausedPumpTime" in world_h and "cNow - m_lastPausedPumpTime" in world_cpp
    assert "IsPausedPumpActive()" in world_h and "GetInventoryService()" in world_h
    assert "static_cast<const ActorMagicCaster*>(apThis)" not in magic_target_cpp
    assert "apThis->GetCasterAsActor()" in magic_target_cpp
    pickup_slice = player_char_cpp.split("HookPickUpObject", 1)[1][:420]
    assert "World::IsCreated()" in pickup_slice and "World::IsConnectedCached()" in pickup_slice
    assert "!ScopedInventoryOverride::IsOverriden()" in pickup_slice
    assert "Cast<TESObjectREFR>(TESForm::GetById(objectFormId)) : apThis" not in tes_refr_cpp
    assert "checkUnlockedAndEmit" in tes_refr_cpp
    assert "void InventoryService::ResetForWorldReload() noexcept" in inv_cpp
    assert "m_world.GetInventoryService().ResetForWorldReload();" in coord_save_cpp
    assert "statsOpen || m_world.IsPausedPumpActive()" in coord_save_cpp
    assert "m_deferredLoadMenuCloseFrames" in coord_save_cpp

    print("[test_v120_hotfix_regressions] OK: All v1.2.0-hotfix.1 + v2.0.1 + v2.0.2 + v2.0.3 + v2.1.0 + v3.0 + v3.1 + v3.1.3 + v3.1.4 + v3.1.5 + v3.1.6 architectural regression checks passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
