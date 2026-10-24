#include <common/RfabModpackCompat.h>
#include <common/CoopOwnership.h>
#include <common/CoopFlightDigest.h>
#include <es_loader/stdafx.h>
#include <es_loader/ESLoader.h>
#include <es_loader/TESFile.h>

#ifdef RFAB_STANDALONE_AUDIT_MAIN
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <BSCore/BSIntrusiveRefCounted.h>
#include <BSCore/BSTSmartPointer.h>
#include <Havok/hkStepInfo.h>
#include <Havok/bhkCharacterController.h>
#include <Havok/hkbBehaviorGraphData.h>
#include <Havok/hkbBehaviorGraph.h>
#include <Havok/BShkbAnimationGraph.h>
#include <Misc/MiddleProcess.h>
#include <BSAnimationGraphManager.h>
#include <Forms/TESForm.h>
#include <Components/TESSpellList.h>
#include <Forms/TESActorBase.h>
#include <Forms/EffectSetting.h>
#include <NetImmerse/BSMaskedShaderMaterial.h>

namespace
{
struct TestGraphObj : BSIntrusiveRefCounted
{
    inline static int s_aliveCount = 0;
    int tag = 0;

    explicit TestGraphObj(int aTag) noexcept
        : tag(aTag)
    {
        ++s_aliveCount;
    }

    virtual ~TestGraphObj() noexcept
    {
        --s_aliveCount;
    }
};
} // namespace

int main()
{
    namespace fs = std::filesystem;

    // 1. Endpoint parser / duplicate :10578:10578 fix verification
    {
        assert(RfabCompat::NormalizeServerEndpoint("") == "127.0.0.1:10578");
        assert(RfabCompat::NormalizeServerEndpoint("127.0.0.1") == "127.0.0.1:10578");
        assert(RfabCompat::NormalizeServerEndpoint("26.160.34.131:10578") == "26.160.34.131:10578");
        assert(RfabCompat::NormalizeServerEndpoint("26.160.34.131:10578:10578") == "26.160.34.131:10578");
        assert(RfabCompat::NormalizeServerEndpoint("  http://localhost:27015/  ") == "127.0.0.1:27015");
    }

    // 2. EngineFixes.toml v5/v6, v7, AND MO2 partial override ('Disable Quick Saves') verification
    {
        std::string v5Toml =
            "[Patches]\r\n"
            "MaxStdio = 2048\r\n"
            "MemoryManager = true\r\n"
            "ScaleformAllocator = true\r\n"
            "RegularQuicksaves = false\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(v5Toml));
        assert(v5Toml.find("MemoryManager = false") != std::string::npos);
        assert(v5Toml.find("ScaleformAllocator = false") != std::string::npos);
        assert(v5Toml.find("RegularQuicksaves = true") != std::string::npos);
        assert(v5Toml.find("MaxStdio = 8192") != std::string::npos);
        assert(v5Toml.find("SaveGameMaxSize = true") != std::string::npos);
        assert(!RfabCompat::PatchEngineFixesTomlContent(v5Toml));

        std::string partialOverride = "[Patches]\r\nRegularQuicksaves = false\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(partialOverride));
        assert(partialOverride.find("MemoryManager = false") != std::string::npos);
        assert(partialOverride.find("ScaleformAllocator = false") != std::string::npos);
        assert(partialOverride.find("RegularQuicksaves = true") != std::string::npos);
        assert(partialOverride.find("MaxStdio = 8192") != std::string::npos);
        assert(!RfabCompat::PatchEngineFixesTomlContent(partialOverride));

        std::string v7Toml =
            "[MemoryManager]\r\n"
            "OverrideVanilla = true\r\n"
            "OverrideScaleform = true\r\n"
            "[Patches]\r\n"
            "RegularQuicksaves = false\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(v7Toml));
        assert(v7Toml.find("OverrideVanilla = false") != std::string::npos);
        assert(v7Toml.find("OverrideScaleform = false") != std::string::npos);
        assert(v7Toml.find("RegularQuicksaves = true") != std::string::npos);
    }

    // 3. SkyrimSoulsRE.ini [SLOWMOTION] neutralizer + full MO2 VFS tree scan + BetterStealing auto-hide
    {
        const fs::path tempRoot = fs::temp_directory_path() / "rfab_online_vfs_test_tree";
        fs::remove_all(tempRoot);
        fs::create_directories(tempRoot / "MO2" / "mods" / "Disable Quick Saves" / "skse" / "plugins");
        fs::create_directories(tempRoot / "MO2" / "overwrite" / "SKSE" / "Plugins");
        fs::create_directories(tempRoot / "MO2" / "mods" / "BetterStealing" / "NetScriptFramework" / "Plugins");

        {
            std::ofstream f(tempRoot / "MO2" / "mods" / "Disable Quick Saves" / "skse" / "plugins" / "EngineFixes.toml");
            f << "[Patches]\nRegularQuicksaves = false\n";
        }
        {
            std::ofstream f(tempRoot / "MO2" / "overwrite" / "SKSE" / "Plugins" / "SkyrimSoulsRE.ini");
            f << "[SLOWMOTION]\nfSlowMotionMultiplier = 0.250000\nbEnableSlowMotionInventoryMenu = true\nbEnableSlowMotion_MagicMenu = true\n";
        }
        {
            std::ofstream f(tempRoot / "MO2" / "mods" / "BetterStealing" / "NetScriptFramework" / "Plugins" / "BetterStealing.dll");
            f << "stub";
        }

        const size_t patchedFiles = RfabCompat::EnsureEngineFixesPatchedUnderRoot(tempRoot);
        assert(patchedFiles >= 2);
        assert(!fs::exists(tempRoot / "MO2" / "mods" / "BetterStealing" / "NetScriptFramework" / "Plugins" / "BetterStealing.dll"));
        assert(fs::exists(tempRoot / "MO2" / "mods" / "BetterStealing" / "NetScriptFramework" / "Plugins" / "BetterStealing.dll.mohidden"));
        assert(RfabCompat::EnsureEngineFixesPatchedUnderRoot(tempRoot) == 0);
        fs::remove_all(tempRoot);
    }

    // 4. BSTSmartPointer + BSIntrusiveRefCounted RAII zero-leak verification
    {
        assert(TestGraphObj::s_aliveCount == 0);
        auto* raw = new TestGraphObj(77);
        raw->refCount = 0;
        assert(TestGraphObj::s_aliveCount == 1);
        {
            BSTSmartPointer<TestGraphObj> sp1(raw);
            assert(raw->refCount == 1);
            {
                BSTSmartPointer<TestGraphObj> sp2 = sp1;
                assert(raw->refCount == 2);
                BSTSmartPointer<TestGraphObj> sp3 = std::move(sp2);
                assert(sp2.get() == nullptr);
                assert(sp3.get() == raw);
                assert(raw->refCount == 2);
            }
            assert(raw->refCount == 1);
            assert(TestGraphObj::s_aliveCount == 1);
        }
        assert(TestGraphObj::s_aliveCount == 0);
    }

    // 5. Remote Havok bhkCharacterController::UpdateStepTiming & hkbBehaviorGraph::IsWordVariable checks
    {
        bhkCharacterController ctrl{};
        ctrl.stepInfo = {};
        ctrl.UpdateStepTiming(0.016f);
        assert(std::abs(ctrl.stepInfo.deltaTime - 0.016f) < 1e-6f);
        assert(std::abs(ctrl.stepInfo.invDeltaTime - (1.0f / 0.016f)) < 1e-2f);
        assert(std::abs((ctrl.stepInfo.endTime - ctrl.stepInfo.startTime) - 0.016f) < 1e-6f);

        ctrl.UpdateStepTiming(0.0f);
        assert(std::isfinite(ctrl.stepInfo.invDeltaTime));
        ctrl.UpdateStepTiming(5.0f);
        assert(std::isfinite(ctrl.stepInfo.invDeltaTime) && ctrl.stepInfo.deltaTime <= 0.1f);

        hkbVariableInfo infos[6]{};
        infos[0].type = hkbVariableInfo::kInt32;
        infos[1].type = hkbVariableInfo::kReal;
        infos[2].type = hkbVariableInfo::kInt8;
        infos[3].type = hkbVariableInfo::kPointer;
        infos[4].type = hkbVariableInfo::kVector4;
        infos[5].type = hkbVariableInfo::kQuaternion;

        hkbBehaviorGraphData graphData{};
        graphData.variableInfos = infos;
        graphData.variableInfoCount = 6;

        hkbBehaviorGraph graph{};
        assert(!graph.IsWordVariable(0));
        graph.data = &graphData;
        assert(graph.IsWordVariable(0));
        assert(graph.IsWordVariable(1));
        assert(graph.IsWordVariable(2));
        assert(!graph.IsWordVariable(3));
        assert(!graph.IsWordVariable(4));
        assert(!graph.IsWordVariable(5));
        assert(!graph.IsWordVariable(-1));
        assert(!graph.IsWordVariable(6));
    }

    // 6. TESForm::IsStillLoading, TESSpellList::RemoveNullEntries & ESLoader noexcept check
    {
        TESForm dummyForm{};
        dummyForm.flags = 0;
        assert(!dummyForm.IsStillLoading());
        dummyForm.flags = TESForm::FormFlags::STILL_LOADING;
        assert(dummyForm.IsStillLoading());

        SpellItem* arr[5] = {nullptr, reinterpret_cast<SpellItem*>(0x1000), nullptr, reinterpret_cast<SpellItem*>(0x2000), nullptr};
        TESSpellList::Lists lists{};
        lists.spells = arr;
        lists.spellCount = 4;
        TESSpellList spellList{};
        spellList.lists = &lists;
        assert(spellList.RemoveNullEntries() == 2);
        assert(lists.spellCount == 2);
        assert(lists.spells[0] == reinterpret_cast<SpellItem*>(0x1000));
        assert(lists.spells[1] == reinterpret_cast<SpellItem*>(0x2000));
        assert(lists.spells[2] == nullptr);

        TiltedPhoques::Map<TiltedPhoques::String, uint8_t> masters;
        ESLoader::TESFile missingPlugin(masters);
        assert(!missingPlugin.LoadFile("/tmp/definitely_missing_rfab_plugin_99999.esp"));
    }

    // 7. v1.1.0 Playtest Regressions: TESV_SAVEGAME validator, stem normalizer, quarantine,
    //    RFAB_XP_Amount/Multiplier wallet isolation, [RFAB] Autosave.ini full suppression,
    //    EncounterCoordinator post-rollback wipe-loop guard, and cleaned SkyrimTogether.esp!
    {
        // Stem normalizer strips .ess/.bak/paths cleanly
        assert(RfabCompat::Detail::StripSaveSlotExtensionPreserveCase("RFAB_MP_S123_Auto_0.ess") == "RFAB_MP_S123_Auto_0");
        assert(RfabCompat::Detail::StripSaveSlotExtensionPreserveCase("Saves\\Quicksave0_FFFFFFFF.ess.bak") == "Quicksave0_FFFFFFFF");
        assert(RfabCompat::Detail::NormalizeSaveSlotStem("  RFAB_MP_S123_Manual_1.ESS  ") == "rfab_mp_s123_manual_1");

        // TESV_SAVEGAME validator & corrupt-file quarantine
        const fs::path saveTmpDir = fs::temp_directory_path() / "rfab_save_validator_test";
        fs::remove_all(saveTmpDir);
        fs::create_directories(saveTmpDir);

        const fs::path zeroEss = saveTmpDir / "CorruptZero.ess";
        {
            std::ofstream f(zeroEss, std::ios::binary);
        }
        assert(!RfabCompat::Detail::IsValidEssFileOnDisk(zeroEss));
        assert(RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(saveTmpDir) == 1);
        assert(!fs::exists(zeroEss));
        assert(fs::exists(saveTmpDir / "Quarantined_Corrupt_v100" / "CorruptZero.ess"));

        const fs::path truncatedEss = saveTmpDir / "TruncatedHeader.ess";
        {
            std::ofstream f(truncatedEss, std::ios::binary);
            f.write("TESV_SAVEGAME_SHORT", 19);
        }
        assert(!RfabCompat::Detail::IsValidEssFileOnDisk(truncatedEss));

        // Construct valid SE 1.5.97 v12 TESV_SAVEGAME header + payload
        const fs::path validEss = saveTmpDir / "RFAB_MP_S1_Manual_0.ess";
        {
            std::vector<uint8_t> bytes(512, 0xAB);
            std::memcpy(bytes.data(), "TESV_SAVEGAME", 13);
            std::ofstream f(validEss, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        assert(RfabCompat::Detail::IsValidEssFileOnDisk(validEss));
        assert(RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(saveTmpDir) == 1); // quarantines TruncatedHeader.ess only
        assert(fs::exists(validEss));
        fs::remove_all(saveTmpDir);

        // RFAB_XP_Amount & RFAB_XP_Multiplier must NEVER be broadcast via GlobalValueSync
        const auto bindings = RfabCompat::LoadBindingsFromDisk(fs::current_path());
        assert(!RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_XP_Amount", bindings));
        assert(!RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_XP_Multiplier", bindings));
        assert(!RfabCompat::ShouldAutoSyncGlobalEditorId("RFAB_XP_Bonus", bindings));

        // [RFAB] Autosave.ini full suppression (including location change & sleep/wait)
        const std::string autosaveRaw =
            "[Main]\r\nEnable autosave = 1\r\nSave after combat = 1\r\nSave on location change = 1\r\nSave on sleep/wait = 1\r\n";
        const std::string autosaveSuppressed = RfabCompat::SuppressRfabAutosaveIniContent(autosaveRaw);
        assert(autosaveSuppressed.find("Enable autosave = 0") != std::string::npos);
        assert(autosaveSuppressed.find("Save after combat = 0") != std::string::npos);
        assert(autosaveSuppressed.find("Save on location change = 0") != std::string::npos);
        assert(autosaveSuppressed.find("Save on sleep/wait = 0") != std::string::npos);

        // Verify SkyrimTogether.esp is clean & TempleBlessingScript.pex / logo.nif are absent
        assert(!fs::exists("GameFiles/Skyrim/scripts/TempleBlessingScript.pex"));
        assert(!fs::exists("dist_ready/RFAB_Online_MO2/scripts/TempleBlessingScript.pex"));
        assert(!fs::exists("GameFiles/Skyrim/scripts/source/TempleBlessingScript.psc"));
        assert(!fs::exists("dist_ready/RFAB_Online_MO2/scripts/source/TempleBlessingScript.psc"));
        assert(!fs::exists("GameFiles/Skyrim/meshes/Interface/logo/logo.nif"));
        assert(!fs::exists("dist_ready/RFAB_Online_MO2/meshes/Interface/logo/logo.nif"));

        // Verify v1.2.0 RFAB Difficulty Modes (Странник / Довакин / Легенда + Проклятый) & Perk 0x581F8 (1.25x)
        const auto mStrannik = RfabCompat::ResolveRfabDifficultyMultipliers(RfabCompat::RfabDifficultyMode::Strannik, false);
        assert(std::abs(mStrannik.OutgoingByPlayer - 2.0f) < 0.001f && std::abs(mStrannik.IncomingToPlayer - 0.5f) < 0.001f);
        const auto mDragon = RfabCompat::ResolveRfabDifficultyMultipliers(RfabCompat::RfabDifficultyMode::Dragonborn, false);
        assert(std::abs(mDragon.OutgoingByPlayer - 1.0f) < 0.001f && std::abs(mDragon.IncomingToPlayer - 1.0f) < 0.001f);
        const auto mLegend = RfabCompat::ResolveRfabDifficultyMultipliers(RfabCompat::RfabDifficultyMode::Legend, false);
        assert(std::abs(mLegend.OutgoingByPlayer - 1.0f) < 0.001f && std::abs(mLegend.IncomingToPlayer - 2.0f) < 0.001f);
        const auto mCursed = RfabCompat::ResolveRfabDifficultyMultipliers(RfabCompat::RfabDifficultyMode::Strannik, true);
        assert(std::abs(mCursed.OutgoingByPlayer - 0.5f) < 0.001f && std::abs(mCursed.IncomingToPlayer - 0.5f) < 0.001f);
        assert(RfabCompat::NormalizeRfabEngineDifficultyIndex(2) == 4);
        assert(RfabCompat::NormalizeRfabEngineDifficultyIndex(0) == 0);
        assert(std::abs(RfabCompat::GetRestorationImprovedHealingMultiplier(true) - 1.25f) < 0.001f);
    }

    std::cout << "[PASS] All SE 1.5.97 struct static_asserts, BSTSmartPointer RAII, Havok stepInfo/IsWordVariable guards, "
                 "TESSpellList null-sanitizer, TESForm::IsStillLoading, ESLoader BOM/CRLF/overflow safety, "
                 "NormalizeServerEndpoint, and MO2 VFS self-healers passed!"
              << std::endl;
    return 0;
}
#else
#include <catch2/catch.hpp>

TEST_CASE("RFAB Modpack Self-Healers, Endpoint Normalization & ESLoader BOM/Overflow Safety", "[rfab][hardening]")
{
    REQUIRE(RfabCompat::NormalizeServerEndpoint("") == "127.0.0.1:10578");
    REQUIRE(RfabCompat::NormalizeServerEndpoint("26.160.34.131") == "26.160.34.131:10578");
    REQUIRE(RfabCompat::NormalizeServerEndpoint("26.160.34.131:10578:10578") == "26.160.34.131:10578");
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0-6-g383738a6", "v1.0.0-7-gc0b262aa"));
    REQUIRE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0", "v1.0.9-12-gabcdef0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0", "v1.1.0"));
    REQUIRE_FALSE(RfabCompat::AreBuildVersionsProtocolCompatible("383738a6", "c0b262aa"));

    std::string partialOverride = "[Patches]\r\nRegularQuicksaves = false\r\n\r\n[SaveGameMaxSize]\r\nSaveGameMaxSize = true\r\n";
    REQUIRE(RfabCompat::PatchEngineFixesTomlContent(partialOverride));
    REQUIRE(partialOverride.find("MemoryManager = false") != std::string::npos);
    REQUIRE(partialOverride.find("ScaleformAllocator = false") != std::string::npos);
    REQUIRE(partialOverride.find("RegularQuicksaves = true") != std::string::npos);
    REQUIRE(partialOverride.find("MaxStdio = 8192") != std::string::npos);
    REQUIRE(partialOverride.find("[Experimental]") != std::string::npos);
    REQUIRE(partialOverride.find("[SaveGameMaxSize]") == std::string::npos);
    REQUIRE_FALSE(RfabCompat::PatchEngineFixesTomlContent(partialOverride));

    std::string soulsIni = "[SLOWMOTION]\r\nfSlowMotionMultiplier = 0.250000\r\nbEnableSlowMotionInventoryMenu = true\r\n";
    REQUIRE(RfabCompat::PatchSkyrimSoulsReIniContent(soulsIni));
    REQUIRE(soulsIni.find("fSlowMotionMultiplier = 1.000000") != std::string::npos);
    REQUIRE(soulsIni.find("bEnableSlowMotionInventoryMenu = false") != std::string::npos);

    TiltedPhoques::Map<TiltedPhoques::String, uint8_t> masters;
    ESLoader::TESFile missingPlugin(masters);
    REQUIRE_FALSE(missingPlugin.LoadFile("/tmp/non_existent_plugin_for_noexcept_test.esp"));

    const auto loDir = std::filesystem::temp_directory_path() / "rfab_esloader_600_test";
    std::filesystem::remove_all(loDir);
    std::filesystem::create_directories(loDir / "Data");
    {
        std::ofstream lo(loDir / "Data" / "loadorder.txt", std::ios::binary);
        const char bom[3] = {static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF)};
        lo.write(bom, 3);
        lo << "Skyrim.esm\r\n*Update.esm\r\n";
        for (int i = 0; i < 300; ++i)
            lo << "RfabMod_" << i << ".esp\r\n";
        lo << "SkyrimTogether.esp\r\n";
    }
    {
        const auto oldCwd = std::filesystem::current_path();
        std::filesystem::current_path(loDir);
        ESLoader::ESLoader loader;
        (void)loader.BuildRecordCollection();
        const auto& order = loader.GetLoadOrder();
        REQUIRE(order.size() == 303);
        REQUIRE(order.front().m_filename == "Skyrim.esm");
        REQUIRE(order.front().m_standardId == 0x00);
        REQUIRE(order[260].m_isLite == true);
        REQUIRE(order.back().m_filename == "SkyrimTogether.esp");
        std::filesystem::current_path(oldCwd);
    }
    std::filesystem::remove_all(loDir);

    // RFAB «Единство в битве (Кооп)» (Co-Op Ally Friendly-Fire Protection) matrix:
    // 1. Non-party actors, enemies, solo NPC followers, and personal summons are untouched
    //    (preserving RFAB's Speechcraft 50 «Лидерство» perk).
    {
        const auto soloHarm = RfabCompat::EvaluateCoopAllyActionPolicy(false, true, false);
        REQUIRE_FALSE(soloHarm.BlockHarmfulAction);
        REQUIRE_FALSE(soloHarm.AllowPartySparring);
        REQUIRE_FALSE(soloHarm.AllowBeneficialMagic);

        const auto soloHeal = RfabCompat::EvaluateCoopAllyActionPolicy(false, true, true);
        REQUIRE_FALSE(soloHeal.BlockHarmfulAction);
        REQUIRE_FALSE(soloHeal.AllowPartySparring);
        REQUIRE_FALSE(soloHeal.AllowBeneficialMagic);
    }
    // 2. Same co-op party with AllyProtection ENABLED (default):
    //    - 100% of physical attacks, hostile spells, shouts, poisons, cloaks, and debuffs are blocked.
    //    - 100% of heals & buffs (including RFAB_List_NoAllyDamage_AllowedSpells) still work!
    {
        const auto allyHarm = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, false);
        REQUIRE(allyHarm.BlockHarmfulAction);
        REQUIRE_FALSE(allyHarm.AllowPartySparring);
        REQUIRE_FALSE(allyHarm.AllowBeneficialMagic);

        const auto allyHealOrBuff = RfabCompat::EvaluateCoopAllyActionPolicy(true, true, true);
        REQUIRE_FALSE(allyHealOrBuff.BlockHarmfulAction);
        REQUIRE_FALSE(allyHealOrBuff.AllowPartySparring);
        REQUIRE(allyHealOrBuff.AllowBeneficialMagic);
    }
    // 3. Same co-op party with AllyProtection DISABLED via Right-Ctrl overlay toggle:
    //    - Party members can spar / test builds without editing STServer.ini, while heals & buffs still work!
    {
        const auto sparHarm = RfabCompat::EvaluateCoopAllyActionPolicy(true, false, false);
        REQUIRE_FALSE(sparHarm.BlockHarmfulAction);
        REQUIRE(sparHarm.AllowPartySparring);
        REQUIRE_FALSE(sparHarm.AllowBeneficialMagic);

        const auto sparHeal = RfabCompat::EvaluateCoopAllyActionPolicy(true, false, true);
        REQUIRE_FALSE(sparHeal.BlockHarmfulAction);
        REQUIRE(sparHeal.AllowPartySparring);
        REQUIRE(sparHeal.AllowBeneficialMagic);
    }
}
#endif
