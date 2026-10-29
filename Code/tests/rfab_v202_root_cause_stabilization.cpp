#include <catch2/catch.hpp>

#include <common/CoopLootLedger.h>
#include <common/RfabModpackCompat.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
std::filesystem::path LocateRepoRootForV202Audit()
{
    std::error_code ec;
    auto searchUpwards = [&](std::filesystem::path probe) -> std::filesystem::path
    {
        for (int depth = 0; depth < 8 && !probe.empty(); ++depth)
        {
            if (std::filesystem::exists(probe / "xmake.lua", ec) &&
                std::filesystem::exists(probe / "Code" / "client" / "Games" / "Skyrim" / "Actor.cpp", ec))
            {
                return probe;
            }
            const auto parent = probe.parent_path();
            if (parent == probe)
                break;
            probe = parent;
        }
        return {};
    };

    if (const auto fromCwd = searchUpwards(std::filesystem::current_path(ec)); !fromCwd.empty())
        return fromCwd;

    return searchUpwards(std::filesystem::path(__FILE__).parent_path());
}

std::string LoadRepoFileText(const std::filesystem::path& acRoot, const char* acRelPath)
{
    std::string out;
    RfabCompat::Detail::ReadTextFile(acRoot / acRelPath, out);
    return out;
}

void AppendSubrecord(std::vector<uint8_t>& aOut, const char aType[4], const void* apPayload, uint16_t aLen)
{
    const size_t start = aOut.size();
    aOut.resize(start + 6ull + aLen);
    std::memcpy(aOut.data() + start, aType, 4);
    std::memcpy(aOut.data() + start + 4, &aLen, 2);
    if (aLen > 0 && apPayload)
        std::memcpy(aOut.data() + start + 6, apPayload, aLen);
}

void AppendRecord(
    std::vector<uint8_t>& aOut,
    const char aRecType[4],
    uint32_t aFlags,
    uint32_t aFormId,
    const std::vector<uint8_t>& acSubrecords)
{
    const uint32_t dataSize = static_cast<uint32_t>(acSubrecords.size());
    const size_t start = aOut.size();
    aOut.resize(start + 24ull + dataSize, 0);
    std::memcpy(aOut.data() + start, aRecType, 4);
    std::memcpy(aOut.data() + start + 4, &dataSize, 4);
    std::memcpy(aOut.data() + start + 8, &aFlags, 4);
    std::memcpy(aOut.data() + start + 12, &aFormId, 4);
    if (dataSize > 0)
        std::memcpy(aOut.data() + start + 24, acSubrecords.data(), dataSize);
}

void AppendTopLevelGroup(
    std::vector<uint8_t>& aOut,
    const char aLabel[4],
    const std::vector<uint8_t>& acInnerRecords)
{
    const uint32_t groupSize = static_cast<uint32_t>(24ull + acInnerRecords.size());
    const uint32_t groupType = 0; // Top-level GRUP
    const size_t start = aOut.size();
    aOut.resize(start + groupSize, 0);
    std::memcpy(aOut.data() + start, "GRUP", 4);
    std::memcpy(aOut.data() + start + 4, &groupSize, 4);
    std::memcpy(aOut.data() + start + 8, aLabel, 4);
    std::memcpy(aOut.data() + start + 12, &groupType, 4);
    if (!acInnerRecords.empty())
        std::memcpy(aOut.data() + start + 24, acInnerRecords.data(), acInnerRecords.size());
}
} // namespace

TEST_CASE("Stage 0: Immutable v2.0.1-backup manifest and CI release protection", "[rfab][v202][stage0]")
{
    const auto root = LocateRepoRootForV202Audit();
    REQUIRE_FALSE(root.empty());

    const std::string manifest = LoadRepoFileText(root, "docs/V2.0.1_RELEASE_BACKUP_MANIFEST.json");
    REQUIRE(manifest.find("\"v2.0.1-backup\"") != std::string::npos);
    REQUIRE(manifest.find("bfb633d6b751e8fd9f5a149f935bc149bf79a428276931b0a277a358577cb5bf") != std::string::npos);
    REQUIRE(manifest.find("f9e95eec07cdb63bb54bf8bbf39084636bb3ae6818efd65003d994d3c93b6581") != std::string::npos);
    REQUIRE(manifest.find("f48a9cf055d87c46437c0b0ad22514a27ac21068d783301661aa845434161da7") != std::string::npos);
    REQUIRE(manifest.find("71a0ff68a41b2dc2ff12065bcd4619c6b5734b2b588f5a327b95fa9b2fd485a1") != std::string::npos);

    const std::string ps1 = LoadRepoFileText(root, "Tools/rfab/Build-CloudRelease.ps1");
    REQUIRE(ps1.size() > 3);
    // Preserve UTF-8 BOM (EF BB BF) for Windows PowerShell 5.1 compatibility on windows-2022 runners
    REQUIRE(static_cast<unsigned char>(ps1[0]) == 0xEF);
    REQUIRE(static_cast<unsigned char>(ps1[1]) == 0xBB);
    REQUIRE(static_cast<unsigned char>(ps1[2]) == 0xBF);
    REQUIRE(ps1.find("\"v2.0.1-backup\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.0.1\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.0.2-backup\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.0.2\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.0.3-backup\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.0.3\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.1.0-backup\"") != std::string::npos);
    REQUIRE(ps1.find("\"v2.1.0\"") != std::string::npos);
    REQUIRE(ps1.find("\"v3.0.0-backup\"") != std::string::npos);
    REQUIRE(ps1.find("\"v3.0.0\"") != std::string::npos);
    REQUIRE(ps1.find("[string]$VersionTag = \"v3.1.5\"") != std::string::npos);
    REQUIRE(ps1.find("[switch]$SkipPublish") != std::string::npos);
}

TEST_CASE("Stage 1 & 2: Zero-CTD Core, Persistent ServerWorldModel, and O(1) ClientWorldView invariants", "[rfab][v202][stage1][stage2]")
{
    const auto root = LocateRepoRootForV202Audit();
    REQUIRE_FALSE(root.empty());

    SECTION("Stage 1: Trivially destructible ActorExtension, CRT mimalloc family, RAII NiPointer, and thread-safe World/ModSystem")
    {
        const std::string actorExtH = LoadRepoFileText(root, "Code/client/Games/ActorExtension.h");
        REQUIRE(actorExtH.find("std::is_trivially_destructible_v<ActorExtension>") != std::string::npos);
        REQUIRE(actorExtH.find("ActionEvent LatestAnimation") == std::string::npos);

        const std::string memoryCpp = LoadRepoFileText(root, "Code/client/Games/Memory.cpp");
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(calloc, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(_recalloc, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(_expand, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(_msize, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(_aligned_msize, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("TP_HOOK_IAT(_aligned_recalloc, cHeapModule)") != std::string::npos);
        REQUIRE(memoryCpp.find("kActorValueListByteSize = 0x528") != std::string::npos);

        const std::string niPtrH = LoadRepoFileText(root, "Code/client/Games/Skyrim/NetImmerse/NiPointer.h");
        REQUIRE(niPtrH.find("~NiPointer() noexcept") != std::string::npos);
        REQUIRE(niPtrH.find("std::add_lvalue_reference_t<T>") != std::string::npos);

        const std::string modSysH = LoadRepoFileText(root, "Code/client/Systems/ModSystem.h");
        REQUIRE(modSysH.find("mutable std::shared_mutex m_mutex") != std::string::npos);

        const std::string worldH = LoadRepoFileText(root, "Code/client/World.h");
        REQUIRE(worldH.find("IsCreated()") != std::string::npos);
        REQUIRE(worldH.find("IsConnectedCached()") != std::string::npos);
        REQUIRE(worldH.find("IsContainerMenuOpenCached()") != std::string::npos);
        REQUIRE(worldH.find("IsSyncPlayerHomesCached()") != std::string::npos);

        const std::string combatCtrlCpp = LoadRepoFileText(root, "Code/client/Games/Skyrim/Combat/CombatController.cpp");
        REQUIRE(combatCtrlCpp.find("pExt->IsRemote() || pExt->HasCombatTargetLock()") != std::string::npos);
        REQUIRE(combatCtrlCpp.find("Actor::LookupByHandle") != std::string::npos);
    }

    SECTION("Stage 2: Dormant persistent NPCs, O(1) client entity indices, 0xFF00xxxx template protection, and placed spawner restore")
    {
        const std::string serverCharCpp = LoadRepoFileText(root, "Code/server/Services/CharacterService.cpp");
        REQUIRE(serverCharCpp.find("ownerComponent.SetOwner(nullptr, nowMs)") != std::string::npos);
        REQUIRE(serverCharCpp.find("PruneExcessDormantCharacters") != std::string::npos);
        REQUIRE(serverCharCpp.find("m_interestRegistry.MarkInRange(acMessage.pPlayer->GetId(), existingServerId)") != std::string::npos);
        REQUIRE(serverCharCpp.find("m_interestRegistry.MarkInRange(pPreviousOwner->GetId(), acServerId)") != std::string::npos);

        const std::string gameServerCpp = LoadRepoFileText(root, "Code/server/GameServer.cpp");
        REQUIRE(gameServerCpp.find("pCharComp->IsPlayerSummon()") != std::string::npos);

        const std::string clientCharH = LoadRepoFileText(root, "Code/client/Services/CharacterService.h");
        REQUIRE(clientCharH.find("m_entityIndex") != std::string::npos);
        REQUIRE(clientCharH.find("ReconcilePlacedActorBaseFormInPlace") != std::string::npos);
        REQUIRE(clientCharH.find("m_suppressedPlacedActors") == std::string::npos);

        const std::string clientCharCpp = LoadRepoFileText(root, "Code/client/Services/Generic/CharacterService.cpp");
        REQUIRE(clientCharCpp.find("(pNpc->formID & 0xFF000000u) == 0xFF000000u") != std::string::npos);
        REQUIRE(clientCharCpp.find("CharacterService::ReconcilePlacedActorBaseFormInPlace") != std::string::npos);
        REQUIRE(clientCharCpp.find("m_suppressedPlacedActors") == std::string::npos);

        const std::string interpCpp = LoadRepoFileText(root, "Code/client/Systems/InterpolationSystem.cpp");
        REQUIRE(interpCpp.find("(apActor->formID & 0xFF000000u) == 0xFF000000u") != std::string::npos);

        const std::string discH = LoadRepoFileText(root, "Code/client/Services/DiscoveryService.h");
        REQUIRE(discH.find("m_niNodeFlickerGraceFrames") != std::string::npos);
    }
}

TEST_CASE("Stage 3.1 & 3.2: Exact RFAB lethal-damage intercept and buff-preserving cross-cell rollback", "[rfab][v202][stage3]")
{
    const auto root = LocateRepoRootForV202Audit();
    REQUIRE_FALSE(root.empty());

    const std::string actorCpp = LoadRepoFileText(root, "Code/client/Games/Skyrim/Actor.cpp");
    // +65536 temporaryModifier HP inflation and 1.0f HP clamp must be gone in favor of native 0.0f bleedout
    REQUIRE(actorCpp.find("65536.0f") == std::string::npos);
    REQUIRE(actorCpp.find("TriggerNativeBleedout") != std::string::npos);
    REQUIRE(actorCpp.find("LatchNativeBleedoutEntered") != std::string::npos);
    REQUIRE(actorCpp.find("void Actor::DispelHostileEffects() noexcept") != std::string::npos);

    const std::string coordSaveCpp = LoadRepoFileText(root, "Code/client/Services/CoordinatedSaveService.cpp");
    REQUIRE(coordSaveCpp.find("pPlayer->DispelHostileEffects();") != std::string::npos);
    REQUIRE(coordSaveCpp.find("pPlayer->DispelAllSpells();") == std::string::npos);
    REQUIRE(coordSaveCpp.find("pPlayer->MoveToWorldOrCell(pTargetCell, pTargetWorldSpace, targetPos, targetRot);") != std::string::npos);
}

TEST_CASE("Stage 3.3: Cross-version RFAB binary ESP/ESL EDID indexer, plugins.txt '*' parser, and recursive SKSE config discovery", "[rfab][v202][stage3][compat]")
{
    SECTION("EspEdidIndex resolves standard ESP, master overrides, XXXX extended records, and ESL light plugins")
    {
        // Build a synthetic `RFAB_NextGen.esp` with masters: [0] = Skyrim.esm, [1] = Requiem.esp
        std::vector<uint8_t> tes4Subs;
        const char mast0[] = "Skyrim.esm";
        const char mast1[] = "Requiem.esp";
        AppendSubrecord(tes4Subs, "MAST", mast0, sizeof(mast0));
        uint64_t zeroData = 0;
        AppendSubrecord(tes4Subs, "DATA", &zeroData, sizeof(zeroData));
        AppendSubrecord(tes4Subs, "MAST", mast1, sizeof(mast1));
        AppendSubrecord(tes4Subs, "DATA", &zeroData, sizeof(zeroData));

        std::vector<uint8_t> espBytes;
        AppendRecord(espBytes, "TES4", 0x00000000u, 0u, tes4Subs);

        // 1. FLST record owned by RFAB_NextGen.esp itself (`modIdx = 0x02 >= masters.size()`): `XPBooksReaded` at `0x02ABCDEF`
        std::vector<uint8_t> flstGroup;
        {
            std::vector<uint8_t> subs;
            const char edid[] = "XPBooksReaded";
            AppendSubrecord(subs, "EDID", edid, sizeof(edid));
            AppendRecord(flstGroup, "FLST", 0u, 0x02ABCDEFu, subs);
        }
        AppendTopLevelGroup(espBytes, "FLST", flstGroup);

        // 2. PERK override owned by master [1] `Requiem.esp` (`modIdx = 0x01 < masters.size()`) using `XXXX` extended size
        std::vector<uint8_t> perkGroup;
        {
            std::vector<uint8_t> subs;
            const char edid[] = "RFAB_Perk_Restoration_ImprovedHealing";
            const uint32_t extLen = sizeof(edid);
            AppendSubrecord(subs, "XXXX", &extLen, 4);
            // Following subrecord has 16-bit size == 0; actual payload length comes from XXXX!
            const size_t start = subs.size();
            subs.resize(start + 6ull + extLen);
            std::memcpy(subs.data() + start, "EDID", 4);
            uint16_t zero16 = 0;
            std::memcpy(subs.data() + start + 4, &zero16, 2);
            std::memcpy(subs.data() + start + 6, edid, extLen);
            AppendRecord(perkGroup, "PERK", 0u, 0x01045678u, subs);
        }
        AppendTopLevelGroup(espBytes, "PERK", perkGroup);

        // 3. Top-level WRLD spatial group (must be skipped cleanly in O(1) without breaking subsequent groups!)
        std::vector<uint8_t> dummyWorldPayload(256, 0xCC);
        AppendTopLevelGroup(espBytes, "WRLD", dummyWorldPayload);

        // 4. NPC_ record AFTER WRLD group: `RFAB_Boss_CustomDragonPriest` at `0x02099887`
        std::vector<uint8_t> npcGroup;
        {
            std::vector<uint8_t> subs;
            const char edid[] = "RFAB_Boss_CustomDragonPriest";
            AppendSubrecord(subs, "EDID", edid, sizeof(edid));
            AppendRecord(npcGroup, "NPC_", 0u, 0x02099887u, subs);
        }
        AppendTopLevelGroup(espBytes, "NPC_", npcGroup);

        // Build an ESL-flagged plugin `RFAB_Rings.esp` (`tes4Flags = 0x200`) with `RFAB_ExperienceRing` at `0x00F00D42` -> ESL local `0x000D42`
        std::vector<uint8_t> eslBytes;
        AppendRecord(eslBytes, "TES4", 0x00000200u, 0u, {});
        std::vector<uint8_t> armoGroup;
        {
            std::vector<uint8_t> subs;
            const char edid[] = "RFAB_ExperienceRing";
            AppendSubrecord(subs, "EDID", edid, sizeof(edid));
            AppendRecord(armoGroup, "ARMO", 0u, 0x00F00D42u, subs);
        }
        AppendTopLevelGroup(eslBytes, "ARMO", armoGroup);

        RfabCompat::EspEdidIndex index;
        REQUIRE(index.IndexPluginBuffer("RFAB_NextGen.esp", espBytes.data(), espBytes.size()));
        REQUIRE(index.IndexPluginBuffer("RFAB_Rings.esp", eslBytes.data(), eslBytes.size()));
        REQUIRE(index.IndexedPluginCount() == 2);
        REQUIRE(index.Size() == 4);

        const auto* pBooks = index.LookupByEditorId("xpbooksreaded");
        REQUIRE(pBooks != nullptr);
        REQUIRE(pBooks->PluginName == "RFAB_NextGen.esp");
        REQUIRE(pBooks->LocalFormId == 0x00ABCDEFu);
        REQUIRE_FALSE(pBooks->IsSelfLite);

        const auto* pPerk = index.LookupByEditorId("RFAB_Perk_Restoration_ImprovedHealing");
        REQUIRE(pPerk != nullptr);
        REQUIRE(pPerk->PluginName == "Requiem.esp");
        REQUIRE(pPerk->LocalFormId == 0x00045678u);

        const auto* pBoss = index.LookupByEditorId("RFAB_Boss_CustomDragonPriest");
        REQUIRE(pBoss != nullptr);
        REQUIRE(pBoss->PluginName == "RFAB_NextGen.esp");
        REQUIRE(pBoss->LocalFormId == 0x00099887u);
        REQUIRE(index.LookupEditorId("rfab_nextgen.esp", 0x00099887u) == "RFAB_Boss_CustomDragonPriest");

        const auto* pRing = index.LookupByEditorId("RFAB_ExperienceRing");
        REQUIRE(pRing != nullptr);
        REQUIRE(pRing->PluginName == "RFAB_Rings.esp");
        REQUIRE(pRing->LocalFormId == 0x00000D42u);
        REQUIRE(pRing->IsSelfLite);
    }

    SECTION("ParseActivePluginList ignores disabled lines without '*' in SSE plugins.txt while preserving pure loadorder.txt")
    {
        const std::string ssePluginsTxt =
            "\xEF\xBB\xBF# MO2 SSE plugins.txt\r\n"
            "Disabled_Optional_Patch.esp\r\n"
            "*Unofficial Skyrim Special Edition Patch.esp\r\n"
            "*Requiem.esp\r\n"
            "Unchecked_Hardcore_Addon.esp\r\n"
            "*RFAB_v10_Core.esp\r\n"
            "*SkyrimTogether.esp\r\n";

        const auto active = RfabCompat::ParseActivePluginList(ssePluginsTxt, true);
        REQUIRE(active.size() == 9); // 5 base masters + 4 starred plugins
        REQUIRE(active[0] == "Skyrim.esm");
        REQUIRE(active[5] == "Unofficial Skyrim Special Edition Patch.esp");
        REQUIRE(active[7] == "RFAB_v10_Core.esp");
        for (const auto& p : active)
        {
            REQUIRE(p != "Disabled_Optional_Patch.esp");
            REQUIRE(p != "Unchecked_Hardcore_Addon.esp");
        }

        const std::string pureLoadOrder =
            "Skyrim.esm\n"
            "*Update.esm\n"
            "RFAB.esp\n"
            "SkyrimTogether.esp\n";
        const auto lo = RfabCompat::ParseActivePluginList(pureLoadOrder, false);
        REQUIRE(lo.size() == 4);
        REQUIRE(lo[1] == "Update.esm");
        REQUIRE(lo[2] == "RFAB.esp");
        REQUIRE(lo[3] == "SkyrimTogether.esp");
    }

    SECTION("ResolveRfabConfigPath discovers relocated RFAB configs in nested Data/SKSE/Plugins subdirectories")
    {
        const auto tempRoot = std::filesystem::temp_directory_path() / "rfab_v202_cfg_discovery_test";
        std::error_code ec;
        std::filesystem::remove_all(tempRoot, ec);
        const auto nestedDir = tempRoot / "Data" / "SKSE" / "Plugins" / "RFAB_v10" / "Tables";
        std::filesystem::create_directories(nestedDir, ec);

        {
            std::ofstream out(nestedDir / "[RFAB] Experience.ini", std::ios::binary);
            out << "[Experience]\nLevel 2 = 3000\n";
        }

        const auto found = RfabCompat::ResolveRfabConfigPath(
            tempRoot,
            "Data/SKSE/Plugins/StorageUtilData/Experience.ini",
            {"Experience.ini", "[RFAB] Experience.ini"});
        REQUIRE_FALSE(found.empty());
        REQUIRE(found.filename().string() == "[RFAB] Experience.ini");

        std::filesystem::remove_all(tempRoot, ec);
    }

    SECTION("Plugin-scoped BossPluginFormKeys and non-syncable quest checks prevent cross-mod 24-bit collisions")
    {
        RfabCompat::RfabLiveXpTable table{};
        RfabCompat::ParseBossListJson(
            R"({"form":{"__formData|RFAB_Bosses.esp|0x012345":1,"__formData|Requiem.esp|0x0ABCDE":1}})",
            table);

        REQUIRE(table.ContainsBossForm("RFAB_Bosses.esp", 0x012345u));
        REQUIRE(table.ContainsBossForm("rfab_bosses.esp", 0x012345u));
        // Same 24-bit local ID in Skyrim.esm is NOT treated as a boss!
        REQUIRE_FALSE(table.ContainsBossForm("Skyrim.esm", 0x012345u));

        // Custom story quest whose 24-bit baseId happens to match 0x0071D0 must remain syncable when its EditorID is known!
        REQUIRE_FALSE(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("CustomMod_MainStoryQuest", 0x1B0071D0u));
        REQUIRE_FALSE(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("", 0xFE0071D0u));
        REQUIRE(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("", 0x020071D0u));
        REQUIRE(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("DLC1PlayerVampireQuest", 0x020071D0u));
    }
}

TEST_CASE("v2.0.2 SkyMP-Grade Server Container Anti-Dupe Ledger & Security Hardening", "[rfab][v202][skymp]")
{
    SECTION("ContainerLootLedger clamps concurrent peer boss-chest & corpse double-takes and emits exact rollback")
    {
        CoopLootLedger::ContainerLootLedger ledger;
        constexpr uint32_t kBossChestServerId = 501;
        constexpr uint32_t kModId = 7;
        constexpr uint32_t kEbonyBladeBaseId = 0x0004A38Fu;

        // Boss chest holds 1 unique weapon. Player 1 and Player 2 click Take simultaneously under 90ms latency.
        const auto p1Claim = ledger.EvaluateWithdrawal(kBossChestServerId, kModId, kEbonyBladeBaseId, /*claimantPlayerId=*/1, /*availableBefore=*/1, /*requestedTake=*/1, /*isQuestItem=*/false, /*nowMs=*/10000);
        REQUIRE(p1Claim.GrantedTake == 1);
        REQUIRE(p1Claim.OverdrawnRollback == 0);

        // Chest authoritative stock is now 0! Player 2's packet arrives 90ms later:
        const auto p2Claim = ledger.EvaluateWithdrawal(kBossChestServerId, kModId, kEbonyBladeBaseId, /*claimantPlayerId=*/2, /*availableBefore=*/0, /*requestedTake=*/1, /*isQuestItem=*/false, /*nowMs=*/10090);
        REQUIRE(p2Claim.GrantedTake == 0);
        REQUIRE(p2Claim.OverdrawnRollback == 1);

        // Stackable potions: chest has 3, Player 1 takes 2, Player 2 tries to take all 3 50ms later -> gets 1, rolls back 2!
        constexpr uint32_t kPotionBaseId = 0x0003EAE3u;
        const auto potP1 = ledger.EvaluateWithdrawal(kBossChestServerId, 0, kPotionBaseId, 1, 3, 2, false, 20000);
        REQUIRE(potP1.GrantedTake == 2);
        REQUIRE(potP1.OverdrawnRollback == 0);

        const auto potP2 = ledger.EvaluateWithdrawal(kBossChestServerId, 0, kPotionBaseId, 2, 1, 3, false, 20050);
        REQUIRE(potP2.GrantedTake == 1);
        REQUIRE(potP2.OverdrawnRollback == 2);

        // Player 1 puts 1 potion back into the chest -> clears depletion marker so Player 2 can legitimately take it!
        ledger.RecordDeposit(kBossChestServerId, 0, kPotionBaseId);
        const auto potAfterDeposit = ledger.EvaluateWithdrawal(kBossChestServerId, 0, kPotionBaseId, 2, 1, 1, false, 20100);
        REQUIRE(potAfterDeposit.GrantedTake == 1);
        REQUIRE(potAfterDeposit.OverdrawnRollback == 0);
    }

    SECTION("ContainerLootLedger allows single-looter dynamic RFAB OnDeath script loot while blocking second-peer dupe")
    {
        CoopLootLedger::ContainerLootLedger ledger;
        constexpr uint32_t kCorpseServerId = 888;
        constexpr uint32_t kRfabTokenBaseId = 0x00123456u;

        // Dynamic OnDeath script added 1 token locally on corpse (`availableBefore == 0`, no prior depletion).
        // First looter (Player 1) gets it cleanly without false rollback:
        const auto firstLoot = ledger.EvaluateWithdrawal(kCorpseServerId, 12, kRfabTokenBaseId, 1, 0, 1, false, 50000);
        REQUIRE(firstLoot.GrantedTake == 1);
        REQUIRE(firstLoot.OverdrawnRollback == 0);

        // Second looter (Player 2) clicking within conflict window gets rolled back!
        const auto secondLoot = ledger.EvaluateWithdrawal(kCorpseServerId, 12, kRfabTokenBaseId, 2, 0, 1, false, 50200);
        REQUIRE(secondLoot.GrantedTake == 0);
        REQUIRE(secondLoot.OverdrawnRollback == 1);

        // Shared quest items / dungeon claws (`isQuestItem == true`) are never rolled back:
        const auto clawP1 = ledger.EvaluateWithdrawal(kCorpseServerId, 0, 0x00039647u, 1, 0, 1, true, 50300);
        const auto clawP2 = ledger.EvaluateWithdrawal(kCorpseServerId, 0, 0x00039647u, 2, 0, 1, true, 50320);
        REQUIRE(clawP1.OverdrawnRollback == 0);
        REQUIRE(clawP2.OverdrawnRollback == 0);

        // Checkpoint rollback resets the ledger cleanly:
        ledger.Clear();
        REQUIRE(ledger.Size() == 0);
    }

    SECTION("Source invariants verify Server Combat Reconcile Window, Zero-Spoof Magic/Auth & O(1) Object Indexing")
    {
        const auto root = LocateRepoRootForV202Audit();
        REQUIRE_FALSE(root.empty());

        const std::string avService = LoadRepoFileText(root, "Code/server/Services/ActorValueService.cpp");
        REQUIRE(avService.find("kExternalDamageReconcileWindowMs") != std::string::npos);
        REQUIRE(avService.find("IsBossGhost(message.Id)") != std::string::npos);

        const std::string magicService = LoadRepoFileText(root, "Code/server/Services/MagicService.cpp");
        REQUIRE(magicService.find("senderAuthorized") != std::string::npos);

        const std::string modsComp = LoadRepoFileText(root, "Code/server/Components/ModsComponent.cpp");
        REQUIRE(modsComp.find("NormalizeModFilenameKey") != std::string::npos);

        const std::string objComp = LoadRepoFileText(root, "Code/server/Components/ObjectComponent.h");
        REQUIRE(objComp.find("pLastSender") == std::string::npos);

        const std::string objService = LoadRepoFileText(root, "Code/server/Services/ObjectService.cpp");
        REQUIRE(objService.find("m_objectEntityByKey") != std::string::npos);

        const std::string utilsH = LoadRepoFileText(root, "Code/client/Utils.h");
        REQUIRE(utilsH.find("FindEntityByServerId(acServerId)") != std::string::npos);
    }
}
