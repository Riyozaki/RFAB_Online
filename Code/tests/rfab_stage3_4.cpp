#include <catch2/catch.hpp>

#include <common/RfabModpackCompat.h>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Structs/ServerSettings.h>

#include <entt/entt.hpp>
namespace TiltedPhoques
{
using ConnectionId_t = uint64_t;
}

#include <Services/EncounterService.h>
#include <Services/RfabProgressionService.h>
#include <Services/SaveSnapshotService.h>

#include <filesystem>

using namespace TiltedPhoques;

namespace
{
template <class TMsg>
void AssertClientPacketRoundtrip(const TMsg& acOutgoing)
{
    Buffer rawBuffer(4096);
    Buffer::Writer writer(&rawBuffer);
    acOutgoing.Serialize(writer);

    Buffer::Reader reader(&rawBuffer);
    ClientMessageFactory factory;
    auto extracted = factory.Extract(reader);
    REQUIRE(extracted != nullptr);
    REQUIRE(extracted->GetOpcode() == TMsg::Opcode);

    const auto* pTyped = dynamic_cast<const TMsg*>(extracted.get());
    REQUIRE(pTyped != nullptr);
    REQUIRE(*pTyped == acOutgoing);
}

template <class TMsg>
void AssertServerPacketRoundtrip(const TMsg& acOutgoing)
{
    Buffer rawBuffer(4096);
    Buffer::Writer writer(&rawBuffer);
    acOutgoing.Serialize(writer);

    Buffer::Reader reader(&rawBuffer);
    ServerMessageFactory factory;
    auto extracted = factory.Extract(reader);
    REQUIRE(extracted != nullptr);
    REQUIRE(extracted->GetOpcode() == TMsg::Opcode);

    const auto* pTyped = dynamic_cast<const TMsg*>(extracted.get());
    REQUIRE(pTyped != nullptr);
    REQUIRE(*pTyped == acOutgoing);
}
} // namespace

TEST_CASE("Stage 3-4: All 12 new network messages & ServerSettings round-trip cleanly via Factories", "[rfab][stage3_4][encoding]")
{
    SECTION("ServerSettings Stage 3-4 fields & configurable HP scaling curve")
    {
        ServerSettings sendSettings{};
        sendSettings.Difficulty = 4;
        sendSettings.GreetingsEnabled = false;
        sendSettings.PvpEnabled = false;
        sendSettings.SyncPlayerHomes = true;
        sendSettings.DeathSystemEnabled = true;
        sendSettings.SyncPlayerCalendar = true;
        sendSettings.AutoPartyJoin = true;
        sendSettings.HealthMultiplier = 3.0f;
        sendSettings.HealthExtraBase = 1.0f;
        sendSettings.DownedSystemEnabled = true;
        sendSettings.ReviveDurationSec = 4.0f;
        sendSettings.ReviveHealthFraction = 0.30f;
        sendSettings.PostReviveGraceSec = 3.0f;
        sendSettings.CombatLeashRadiusMeters = 120.0f;
        sendSettings.CoordinatedSavesEnabled = true;
        sendSettings.PostCombatAutosaveIntervalSec = 180;

        Buffer buf(1024);
        Buffer::Writer writer(&buf);
        sendSettings.Serialize(writer);

        Buffer::Reader reader(&buf);
        ServerSettings recvSettings{};
        recvSettings.Deserialize(reader);
        REQUIRE(recvSettings == sendSettings);

        // Default curve (ExtraBase = 1.0): 1p -> 1x, 2p -> 3x, 3p -> 4x, 4p -> 5x
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.0f, true, 1.0f, 1) == Approx(1.0f));
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.0f, true, 1.0f, 2) == Approx(3.0f));
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.0f, true, 1.0f, 3) == Approx(4.0f));
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.0f, true, 1.0f, 4) == Approx(5.0f));

        // Custom curve (ExtraBase = 0.0 -> linear Nx; Base = 1.5 -> scaled hardcore mode)
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.0f, true, 0.0f, 2) == Approx(2.0f));
        REQUIRE(ServerSettings::ComputeEffectiveHealthMultiplier(1.5f, true, 1.0f, 2) == Approx(4.5f));
    }

    SECTION("Client <-> Server Stage 3-4 factory serialization & opcode dispatch")
    {
        RequestCoordinatedSaveLoad saveReq{};
        saveReq.Action = RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot;
        saveReq.SlotName = "RFAB_MP_S1234_Manual_1";
        AssertClientPacketRoundtrip(saveReq);

        CoordinatedSaveAck saveAck{};
        saveAck.EpochId = 7;
        saveAck.SlotName = "RFAB_MP_S1234_Auto_2";
        saveAck.Success = true;
        saveAck.Reason = "";
        AssertClientPacketRoundtrip(saveAck);

        SyncPlayerDownedState downedSync{};
        downedSync.IsDowned = true;
        downedSync.PositionX = 1420.5f;
        downedSync.PositionY = -880.25f;
        downedSync.PositionZ = 310.0f;
        downedSync.NearbyHostileCount = 4;
        AssertClientPacketRoundtrip(downedSync);

        ReviveActionRequest revReq{};
        revReq.Action = ReviveActionRequest::ActionType::StartChannel;
        revReq.TargetPlayerId = 2;
        AssertClientPacketRoundtrip(revReq);

        CombatEncounterStateReport combatRep{};
        combatRep.InCombat = true;
        combatRep.NearbyHostileCount = 3;
        combatRep.NearestHostileDistanceMeters = 14.5f;
        combatRep.BossDefeatedInEncounter = true;
        combatRep.PositionX = 100.f;
        combatRep.PositionY = 200.f;
        combatRep.PositionZ = 50.f;
        AssertClientPacketRoundtrip(combatRep);

        SyncRfabProgressionEvent progReq{};
        progReq.Kind = SyncRfabProgressionEvent::EventKind::BookRead;
        progReq.SourceKey = "book:00000005:001A2B3C";
        progReq.EditorId = "XPBooksReaded";
        progReq.BookId = GameId(5, 0x001A2B3C);
        progReq.XpDelta = 750;
        progReq.GlobalValue = 12.0f;
        AssertClientPacketRoundtrip(progReq);

        NotifyPrepareSave prepSave{};
        prepSave.EpochId = 9;
        prepSave.SlotName = "RFAB_MP_S1234_Auto_0";
        prepSave.IsAutoCheckpoint = true;
        AssertServerPacketRoundtrip(prepSave);

        NotifyCommitSave commitSave{};
        commitSave.EpochId = 9;
        commitSave.SlotName = "RFAB_MP_S1234_Auto_0";
        commitSave.Committed = true;
        commitSave.Message = "RFAB Online: Чекпоинт сохранён";
        AssertServerPacketRoundtrip(commitSave);

        NotifyExecuteLoad execLoad{};
        execLoad.EpochId = 9;
        execLoad.SlotName = "RFAB_MP_S1234_Auto_0";
        execLoad.IsPartyWipe = true;
        execLoad.AllowPreJoinFallback = true;
        execLoad.Message = "RFAB Online: Отряд пал";
        AssertServerPacketRoundtrip(execLoad);

        NotifyPlayerDownedState notifyDowned{};
        notifyDowned.PlayerId = 2;
        notifyDowned.CharacterServerId = 1042;
        notifyDowned.IsDowned = true;
        notifyDowned.PositionX = 1420.5f;
        notifyDowned.PositionY = -880.25f;
        notifyDowned.PositionZ = 310.0f;
        AssertServerPacketRoundtrip(notifyDowned);

        NotifyReviveState notifyRevive{};
        notifyRevive.Status = NotifyReviveState::ReviveStatus::RevivedByAlly;
        notifyRevive.TargetPlayerId = 2;
        notifyRevive.ReviverPlayerId = 1;
        notifyRevive.DurationSec = 4.0f;
        notifyRevive.HealthFraction = 0.30f;
        notifyRevive.GraceSec = 3.0f;
        AssertServerPacketRoundtrip(notifyRevive);

        NotifyRfabProgressionEvent notifyProg{};
        notifyProg.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpRebate;
        notifyProg.EpochId = 9;
        notifyProg.SourceKey = "quest:00000000:000D91E5:100";
        notifyProg.XpDelta = -1500;
        notifyProg.AuthoritativeTotalXp = 14250;
        AssertServerPacketRoundtrip(notifyProg);
    }
}

TEST_CASE("Stage 3: External RFAB bindings, EngineFixes override patcher & Autosave INI suppression", "[rfab][stage3_4][modpack]")
{
    SECTION("Loads repository Data/SKSE/Plugins/RFAB_Online_Bindings.json & wildcard global filter")
    {
        const auto cfg = RfabCompat::LoadBindingsFromDisk(std::filesystem::current_path());
        REQUIRE(cfg.SchemaVersion == 1);
        REQUIRE(cfg.PluginName == "RFAB.esp");
        REQUIRE(cfg.XpAmountGlobalEditorId == "RFAB_XP_Amount");
        REQUIRE(cfg.XpBooksFormListEditorId == "XPBooksReaded");
        REQUIRE(cfg.NoDeathHandlingGlobalEditorId == "REQ_Combat_NoDeathHandling");

        // Auto-discovers counters & boss phase globals across future RFAB updates
        REQUIRE(RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_Counter_FalmersKilled", cfg));
        REQUIRE(RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_Global_FutureBossPhase_2027", cfg));
        REQUIRE(RfabCompat::ShouldAutoSyncGlobalEditorId("TFOTG_Kill_Counter", cfg));

        // Never syncs local player UI/MCM/hotkey preferences or raw XP pool global via global broadcast
        REQUIRE_FALSE(RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_MCM_HudOpacity", cfg));
        REQUIRE_FALSE(RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_Key_Dodge", cfg));
        REQUIRE_FALSE(RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_XP_Amount", cfg));
    }

    SECTION("Idempotently patches Disable Quick Saves/skse/plugins/EngineFixes.toml while keeping RegularQuicksaves = true")
    {
        std::string overrideToml =
            "[Patches]\r\n"
            "MemoryManager = true # allocates more memory\r\n"
            "ScaleformAllocator = true\r\n"
            "MaxStdio = 2048\r\n"
            "RegularQuicksaves = true # required by RFAB Disable Quick Saves\r\n";

        REQUIRE(RfabCompat::PatchEngineFixesTomlContent(overrideToml));
        REQUIRE(overrideToml.find("MemoryManager = false") != std::string::npos);
        REQUIRE(overrideToml.find("ScaleformAllocator = false") != std::string::npos);
        REQUIRE(overrideToml.find("MaxStdio = 8192") != std::string::npos);
        REQUIRE(overrideToml.find("RegularQuicksaves = true") != std::string::npos);

        // Second pass must be 100% idempotent (changed == false, byte-for-byte identical)
        const std::string afterFirstPass = overrideToml;
        REQUIRE_FALSE(RfabCompat::PatchEngineFixesTomlContent(overrideToml));
        REQUIRE(overrideToml == afterFirstPass);
    }

    SECTION("Suppresses [RFAB] Autosave.ini during multiplayer and parses real RFAB StorageUtil files if present")
    {
        const std::string originalIni =
            "[General]\n"
            "Enable autosave = 1\n"
            "Save after combat = 1\n"
            "Save interval = 15\n";

        const std::string suppressed = RfabCompat::SuppressRfabAutosaveIniContent(originalIni);
        REQUIRE(suppressed.find("Enable autosave = 0") != std::string::npos);
        REQUIRE(suppressed.find("Save after combat = 0") != std::string::npos);
        REQUIRE(suppressed.find("Save interval = 15") != std::string::npos);

        // Verify against real drake1ch/RFAB sparse checkout files when available in build environment
        const std::filesystem::path rfabModRoot = "/var/tmp/rfab_work/rfab_sparse/MO2/mods/RFAB";
        std::string expRaw;
        if (RfabCompat::Detail::ReadTextFile(rfabModRoot / "SKSE/Plugins/StorageUtilData/Experience.ini", expRaw))
        {
            RfabCompat::RfabLiveXpTable liveTable{};
            RfabCompat::ParseExperienceIni(expRaw, liveTable);
            REQUIRE(!liveTable.LevelThresholds.empty());
        }
    }
}

TEST_CASE("Stage 3: Two-Phase SaveSnapshotCoordinator ring buffers, mid-save disconnect, boss bypass & rollback", "[rfab][stage3_4][saves]")
{
    SaveSnapshotCoordinator coord("TEST", 3);

    // Separate ring buffers: 3 auto saves NEVER overwrite manual F5 slots!
    auto b1 = coord.BeginBarrier({1, 2}, false, 1000);
    REQUIRE(b1.has_value());
    REQUIRE(b1->EpochId == 1);
    REQUIRE(b1->SlotName == "RFAB_MP_TEST_Manual_0");

    REQUIRE(coord.RecordClientAck(1, 1, true, "") == SaveSnapshotCoordinator::AckResolution::WaitingForMorePlayers);
    REQUIRE(coord.RecordClientAck(2, 1, true, "") == SaveSnapshotCoordinator::AckResolution::ReadyToCommit);
    coord.ConsumeAndCommit(1100);
    REQUIRE(coord.GetLastCommittedEpoch() == 1);
    REQUIRE(coord.GetLastCommittedSlot() == "RFAB_MP_TEST_Manual_0");

    // First post-combat auto checkpoint starts at Auto_0 without touching Manual_0
    REQUIRE(coord.ShouldTriggerPostCombatAutosave(2000, 180, false));
    auto b2 = coord.BeginBarrier({1, 2}, true, 2000);
    REQUIRE(b2.has_value());
    REQUIRE(b2->SlotName == "RFAB_MP_TEST_Auto_0");
    REQUIRE(coord.RecordClientAck(1, 2, true, "") == SaveSnapshotCoordinator::AckResolution::WaitingForMorePlayers);

    // Mid-barrier disconnect of player 2 must immediately unblock & commit Epoch 2 for surviving player 1!
    REQUIRE(coord.HandlePlayerDisconnected(2) == SaveSnapshotCoordinator::AckResolution::ReadyToCommit);
    coord.ConsumeAndCommit(2100);
    REQUIRE(coord.GetLastCommittedEpoch() == 2);

    // Within 180s cooldown, trash fight skips autosave, EXCEPT boss victory ALWAYS checkpoints immediately!
    REQUIRE_FALSE(coord.ShouldTriggerPostCombatAutosave(50000, 180, false));
    REQUIRE(coord.ShouldTriggerPostCombatAutosave(50000, 180, true));

    // Client loading-screen rejection aborts cleanly without overwriting LastCommittedEpoch/LastCommittedSlot
    auto b3 = coord.BeginBarrier({1, 2}, true, 50000);
    REQUIRE(b3.has_value());
    REQUIRE(coord.RecordClientAck(2, 3, false, "Loading Menu active") == SaveSnapshotCoordinator::AckResolution::Aborted);
    coord.ConsumeAborted();
    REQUIRE(coord.GetLastCommittedEpoch() == 2);
    REQUIRE(coord.GetLastCommittedSlot() == "RFAB_MP_TEST_Auto_0");
}

TEST_CASE("Stage 3: EncounterCoordinator enforces 4.0s E-revive, Strict RFAB 120m Leash, 3-player isolation & Total Party Wipe", "[rfab][stage3_4][downed]")
{
    EncounterCoordinator::Config cfg{};
    cfg.ReviveDurationSec = 4.0f;
    cfg.ReviveMaxDistanceMeters = 4.5f;
    cfg.ReviveHealthFraction = 0.30f;
    cfg.PostReviveGraceSec = 3.0f;
    cfg.LeashRadiusMeters = 120.0f;
    cfg.CombatClearHysteresisSec = 4.5f;
    cfg.PartyWipeDelaySec = 3.0f;

    SECTION("Fleeing >120m away from fallen teammate NEVER triggers post-combat auto-revive cheese")
    {
        EncounterCoordinator enc(cfg);
        enc.UpdatePlayerCombatReport(1, true, 2, 5.0f, false, 0.f, 0.f, 0.f, 1000);
        enc.UpdatePlayerCombatReport(2, true, 2, 6.0f, false, 140.f, 0.f, 0.f, 1000);

        // Player 1 falls on knee at origin; Player 2 runs 160m away (160 * 70 = 11200 Skyrim units > 120m leash!)
        enc.SetPlayerDowned(1, true, 0.f, 0.f, 0.f, 2, 1500);
        enc.UpdatePlayerCombatReport(1, false, 0, 9999.f, false, 0.f, 0.f, 0.f, 2000);
        enc.UpdatePlayerCombatReport(2, false, 0, 9999.f, false, RfabCompat::MetersToSkyrimUnits(160.0f), 0.f, 0.f, 2000);

        // Even after 10 full seconds out of combat, Player 1 MUST stay downed because Player 2 fled outside 120m leash!
        const auto fledTick = enc.Tick(12000);
        REQUIRE(fledTick.PostCombatRevivedPlayers.empty());
        REQUIRE_FALSE(fledTick.EncounterVictoryResolved);
        REQUIRE(enc.IsPlayerDowned(1));

        // Once Player 2 returns within honest combat range (20m) and clears the encounter, Player 1 revives cleanly!
        enc.UpdatePlayerCombatReport(2, false, 0, 9999.f, true, RfabCompat::MetersToSkyrimUnits(20.0f), 0.f, 0.f, 13000);
        const auto honestVictory = enc.Tick(18000);
        REQUIRE(honestVictory.EncounterVictoryResolved);
        REQUIRE(honestVictory.BossDefeatedInResolvedEncounter);
        REQUIRE(honestVictory.PostCombatRevivedPlayers == std::vector<uint32_t>{1});
        REQUIRE_FALSE(enc.IsPlayerDowned(1));
    }

    SECTION("3-player mid-combat E-revive lifts ONLY target ally and triggers Total Party Wipe if all 3 fall")
    {
        EncounterCoordinator enc(cfg);
        enc.UpdatePlayerCombatReport(1, true, 3, 4.0f, false, 0.f, 0.f, 0.f, 1000);
        enc.UpdatePlayerCombatReport(2, true, 3, 4.0f, false, 100.f, 0.f, 0.f, 1000);
        enc.UpdatePlayerCombatReport(3, true, 3, 4.0f, false, 120.f, 0.f, 0.f, 1000);

        // Players 1 and 2 both go down; Player 3 channels E on Player 1 (distance = 100 units ~= 1.43m <= 4.5m)
        enc.SetPlayerDowned(1, true, 0.f, 0.f, 0.f, 3, 1200);
        enc.SetPlayerDowned(2, true, 100.f, 0.f, 0.f, 3, 1300);
        REQUIRE(enc.TryStartAllyRevive(3, 1, 2000));

        // At 5.9s (< 4.0s channel), neither is up yet
        REQUIRE(enc.Tick(5900).CompletedAllyRevives.empty());

        // At 6.0s, ONLY Player 1 stands up; Player 2 remains downed!
        const auto revDone = enc.Tick(6000);
        REQUIRE(revDone.CompletedAllyRevives.size() == 1);
        REQUIRE(revDone.CompletedAllyRevives[0].second == 1);
        REQUIRE_FALSE(enc.IsPlayerDowned(1));
        REQUIRE(enc.IsPlayerDowned(2));

        // Now all remaining living players (1 and 3) also fall -> Total Party Wipe!
        enc.SetPlayerDowned(1, true, 0.f, 0.f, 0.f, 3, 7000);
        enc.SetPlayerDowned(3, true, 120.f, 0.f, 0.f, 3, 7100);

        const auto wipeStart = enc.Tick(7200);
        REQUIRE(wipeStart.PartyWipeWarningStarted);
        REQUIRE_FALSE(wipeStart.PartyWipeTriggered);

        const auto wipeExec = enc.Tick(10300);
        REQUIRE(wipeExec.PartyWipeTriggered);
    }
}

TEST_CASE("Stage 4: RfabProgressionLedger deduplicates XP races, rebates duplicates, restores on wipe & matches 1-to-1 ambushes", "[rfab][stage3_4][progression]")
{
    RfabProgressionLedger ledger;

    // Epoch 1 baseline before boss pull
    REQUIRE(ledger.TryGrantXp("quest:MQ101:100", 1000).Accepted);
    REQUIRE(ledger.RecordBookRead(GameId(2, 0x0001AF01)));
    REQUIRE(ledger.UpdateGlobal("RFAB_Counter_FalmersKilled", 14.0f));
    const auto preBossCheckpoint = ledger.CaptureSnapshot();

    // During boss fight: both Client A and Client B simultaneously report killing Boss #77
    const auto firstGrant = ledger.TryGrantXp("kill:boss:77", 5000);
    REQUIRE(firstGrant.Accepted);
    REQUIRE(firstGrant.AppliedDelta == 5000);
    REQUIRE(firstGrant.DuplicateRebateDelta == 0);
    REQUIRE(ledger.GetTotalSharedXp() == 6000);

    const auto duplicateRace = ledger.TryGrantXp("kill:boss:77", 5000);
    REQUIRE_FALSE(duplicateRace.Accepted);
    REQUIRE(duplicateRace.DuplicateRebateDelta == -5000);
    REQUIRE(ledger.GetTotalSharedXp() == 6000);

    // Party wipes immediately on boss phase 2 explosion -> rollback to preBossCheckpoint!
    ledger.RestoreSnapshot(preBossCheckpoint);
    REQUIRE(ledger.GetTotalSharedXp() == 1000);
    REQUIRE(ledger.GetSyncedGlobals().at("RFAB_Counter_FalmersKilled") == Approx(14.0f));

    // Second attempt kills Boss #77 cleanly -> MUST grant full 5000 XP again because ledger rolled back with world!
    const auto retryBossGrant = ledger.TryGrantXp("kill:boss:77", 5000);
    REQUIRE(retryBossGrant.Accepted);
    REQUIRE(retryBossGrant.AppliedDelta == 5000);
    REQUIRE(ledger.GetTotalSharedXp() == 6000);

    // Multi-actor ambush deduplication: RFAB_AmbushScript calls PlaceActorAtMe(Draugr, 3) on Client 1 AND Client 2!
    const GameId cell(0, 0x00015200);
    const GameId draugrBase(0, 0x00023BC0);

    // Client 1 spawns all 3 intended ambush draugrs (ServerIds 501, 502, 503): NONE of Client 1's own 3 spawns merge!
    REQUIRE(ledger.FindMatchingRecentAmbush(1, cell, draugrBase, 100.f, 200.f, 0.f, 1000, 3000, 840.f) == 0);
    ledger.RegisterAmbushSpawn(501, 1, cell, draugrBase, 100.f, 200.f, 0.f, 1000, 3000);

    REQUIRE(ledger.FindMatchingRecentAmbush(1, cell, draugrBase, 105.f, 205.f, 0.f, 1010, 3000, 840.f) == 0);
    ledger.RegisterAmbushSpawn(502, 1, cell, draugrBase, 105.f, 205.f, 0.f, 1010, 3000);

    REQUIRE(ledger.FindMatchingRecentAmbush(1, cell, draugrBase, 110.f, 210.f, 0.f, 1020, 3000, 840.f) == 0);
    ledger.RegisterAmbushSpawn(503, 1, cell, draugrBase, 110.f, 210.f, 0.f, 1020, 3000);

    // Client 2 runs the same script 80ms later: its 3 local spawns bind 1-to-1 to 501, 502, 503 (exact 3 enemies, NOT 1 and NOT 6!)
    REQUIRE(ledger.FindMatchingRecentAmbush(2, cell, draugrBase, 102.f, 201.f, 0.f, 1100, 3000, 840.f) == 501);
    REQUIRE(ledger.FindMatchingRecentAmbush(2, cell, draugrBase, 106.f, 204.f, 0.f, 1110, 3000, 840.f) == 502);
    REQUIRE(ledger.FindMatchingRecentAmbush(2, cell, draugrBase, 111.f, 209.f, 0.f, 1120, 3000, 840.f) == 503);
}
