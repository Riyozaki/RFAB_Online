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
    main_cpp = (ROOT / "Code" / "client" / "main.cpp").read_text(encoding="utf-8")
    overlay_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "OverlayService.cpp").read_text(encoding="utf-8")
    assert "pModManager->factions" in player_char_cpp
    assert "0x3000802u" in cli_char_cpp
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
    assert '"v2.0.1-backup"' in backup_manifest
    assert "std::is_trivially_destructible_v<ActorExtension>" in actor_ext_h
    assert "kActorValueListByteSize = 0x528" in memory_cpp
    assert "TP_HOOK_IAT(_recalloc, cHeapModule)" in memory_cpp
    assert "~NiPointer() noexcept" in ni_ptr_h
    assert "ScopedLethalPlayerDamageIntercept" in actor_cpp
    assert "65536.0f" not in actor_cpp
    assert "pPlayer->DispelHostileEffects();" in coord_save_cpp
    assert "pPlayer->DispelAllSpells();" not in coord_save_cpp
    assert "MoveToWorldOrCell" in coord_save_cpp
    assert "ownerComponent.SetOwner(nullptr, nowMs)" in srv_char_cpp
    assert "m_actorEntityByFormId" in (ROOT / "Code" / "client" / "Services" / "CharacterService.h").read_text(encoding="utf-8")
    assert "class EspEdidIndex" in rfab_compat_h
    assert "ParseActivePluginList" in rfab_compat_h
    assert "ResolveRfabConfigPath" in rfab_compat_h
    assert "m_espEdidIndex.IndexRfabPluginsInDataDir" in rfab_adapter_cpp

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

    print("[test_v120_hotfix_regressions] OK: All v1.2.0-hotfix.1 + v2.0.1 + v2.0.2 SkyMP root-cause regression checks passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
