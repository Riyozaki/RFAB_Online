#include <common/RfabModpackCompat.h>
#include <common/CoopOwnership.h>
#include <common/CoopFlightDigest.h>
#ifndef RFAB_STANDALONE_NO_ESLOADER
#include <es_loader/stdafx.h>
#include <es_loader/ESLoader.h>
#include <es_loader/TESFile.h>
#endif

#ifdef RFAB_STANDALONE_AUDIT_MAIN
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
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
#endif

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
            "RegularQuicksaves = false\r\n"
            "[Experimental]\r\n"
            "S = a\r\n"
            "t = r\r\n"
            "SaveGameMaxSize = true\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(v5Toml));
        assert(v5Toml.find("MemoryManager = false") != std::string::npos);
        assert(v5Toml.find("ScaleformAllocator = false") != std::string::npos);
        assert(v5Toml.find("FormCaching = false") != std::string::npos);
        assert(v5Toml.find("TreeLODReferenceCaching = false") != std::string::npos);
        assert(v5Toml.find("RegularQuicksaves = true") != std::string::npos);
        assert(v5Toml.find("MaxStdio = 8192") != std::string::npos);
        assert(v5Toml.find("SaveGameMaxSize = false") != std::string::npos);
        assert(v5Toml.find("S = a") == std::string::npos);
        assert(!RfabCompat::PatchEngineFixesTomlContent(v5Toml));

        std::string partialOverride = "[Patches]\r\nRegularQuicksaves = false\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(partialOverride));
        assert(partialOverride.find("MemoryManager = false") != std::string::npos);
        assert(partialOverride.find("ScaleformAllocator = false") != std::string::npos);
        assert(partialOverride.find("FormCaching = false") != std::string::npos);
        assert(partialOverride.find("TreeLODReferenceCaching = false") != std::string::npos);
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
        assert(v7Toml.find("FormCaching = false") != std::string::npos);
        assert(v7Toml.find("TreeLODReferenceCaching = false") != std::string::npos);
        assert(v7Toml.find("RegularQuicksaves = true") != std::string::npos);
        assert(!RfabCompat::PatchEngineFixesTomlContent(v7Toml));

        std::string v7BPefixToml =
            "[Patches]\r\n"
            "bVerboseLogging = false\r\n"
            "bFormCaching = true\r\n"
            "bTreeLodReferenceCaching = true\r\n"
            "bRegularQuicksaves = false\r\n";
        assert(RfabCompat::PatchEngineFixesTomlContent(v7BPefixToml));
        assert(v7BPefixToml.find("bFormCaching = false") != std::string::npos);
        assert(v7BPefixToml.find("bTreeLodReferenceCaching = false") != std::string::npos);
        assert(v7BPefixToml.find("bRegularQuicksaves = true") != std::string::npos);
        assert(!RfabCompat::PatchEngineFixesTomlContent(v7BPefixToml));
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

#ifdef _WIN32
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

#ifndef RFAB_STANDALONE_NO_ESLOADER
        TiltedPhoques::Map<TiltedPhoques::String, uint8_t> masters;
        ESLoader::TESFile missingPlugin(masters);
        assert(!missingPlugin.LoadFile("/tmp/definitely_missing_rfab_plugin_99999.esp"));
#endif
    }
#endif

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

        auto buildMockSe12Ess = [](uint32_t ssW, uint32_t ssH, uint16_t compType, uint32_t uncompLen, uint32_t compLen, size_t actualBodyBytes) -> std::vector<uint8_t>
        {
            auto appendU16 = [](std::vector<uint8_t>& v, uint16_t x) {
                v.push_back(static_cast<uint8_t>(x & 0xFFu));
                v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
            };
            auto appendU32 = [](std::vector<uint8_t>& v, uint32_t x) {
                v.push_back(static_cast<uint8_t>(x & 0xFFu));
                v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
                v.push_back(static_cast<uint8_t>((x >> 16) & 0xFFu));
                v.push_back(static_cast<uint8_t>((x >> 24) & 0xFFu));
            };
            auto appendStr16 = [&](std::vector<uint8_t>& v, std::string_view s) {
                appendU16(v, static_cast<uint16_t>(s.size()));
                v.insert(v.end(), s.begin(), s.end());
            };

            std::vector<uint8_t> hdr;
            appendU32(hdr, 12u); // Skyrim SE version 12
            appendU32(hdr, 1u);  // saveNumber
            appendStr16(hdr, "Dovakin");
            appendU32(hdr, 1u);  // playerLevel
            appendStr16(hdr, "Winterhold");
            appendStr16(hdr, "000.01.00");
            appendStr16(hdr, "NordRace");
            appendU16(hdr, 0u);  // playerSex
            appendU32(hdr, 0u);  // curExp
            appendU32(hdr, 0u);  // lvlExp
            appendU32(hdr, 0u);  // filetime low
            appendU32(hdr, 0u);  // filetime high
            appendU32(hdr, ssW);
            appendU32(hdr, ssH);
            appendU16(hdr, compType);

            std::vector<uint8_t> out;
            out.reserve(4096 + actualBodyBytes);
            out.insert(out.end(), {'T','E','S','V','_','S','A','V','E','G','A','M','E'});
            appendU32(out, static_cast<uint32_t>(hdr.size()));
            out.insert(out.end(), hdr.begin(), hdr.end());
            out.insert(out.end(), static_cast<size_t>(4u) * ssW * ssH, 0x33u); // RGBA screenshot
            if (compType != 0)
            {
                appendU32(out, uncompLen);
                appendU32(out, compLen);
            }
            out.insert(out.end(), actualBodyBytes, 0x5Au);
            return out;
        };

        // Regression test for EngineFixes v5/v6 SaveGameMaxSize=true + MemoryManager=false ScrapHeap failure:
        // Header + RGBA screenshot written (>256 bytes, starts with TESV_SAVEGAME), 0 compressed body bytes!
        const fs::path scrapHeapAbortedEss = saveTmpDir / "ScrapHeapAborted.ess";
        {
            auto abortedBytes = buildMockSe12Ess(16, 16, 2, 0, 0, 0);
            abortedBytes.resize(abortedBytes.size() - 8); // stopped right after RGBA screenshot!
            std::ofstream f(scrapHeapAbortedEss, std::ios::binary);
            f.write(reinterpret_cast<const char*>(abortedBytes.data()), static_cast<std::streamsize>(abortedBytes.size()));
        }
        assert(!RfabCompat::Detail::IsValidEssFileOnDisk(scrapHeapAbortedEss));

        // Construct valid SE 1.5.97 v12 LZ4 TESV_SAVEGAME header + RGBA screenshot + compressed payload
        const fs::path validEss = saveTmpDir / "RFAB_MP_S1_Manual_0.ess";
        {
            const auto bytes = buildMockSe12Ess(8, 8, 2, 4096, 256, 256);
            std::ofstream f(validEss, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        assert(RfabCompat::Detail::IsValidEssFileOnDisk(validEss));
        assert(RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(saveTmpDir) == 2); // quarantines TruncatedHeader.ess + ScrapHeapAborted.ess
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

        // Regression audit for v1.1.0 peer location/cell transition CTD root causes:
        {
            auto readTextFile = [](const fs::path& p) -> std::string {
                std::ifstream f(p, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            };

            const std::string refrCpp = readTextFile("Code/client/Games/Skyrim/TESObjectREFR.cpp");
            assert(refrCpp.find("PAPYRUS_FUNCTION(void, ObjectReference, Disable") == std::string::npos);
            assert(refrCpp.find("PAPYRUS_FUNCTION(void, ObjectReference, Enable") == std::string::npos);
            assert(refrCpp.find("DisableImpl()") != std::string::npos);
            assert(refrCpp.find("if (!apCell)") != std::string::npos);

            const std::string faceGenCpp = readTextFile("Code/client/Systems/FaceGenSystem.cpp");
            assert(faceGenCpp.find("pLightingShader->material != pClonedMaterial") != std::string::npos);
            assert(faceGenCpp.find("Memory::Free(pClonedMaterial)") == std::string::npos);

            const std::string charSvcCpp = readTextFile("Code/client/Services/Generic/CharacterService.cpp");
            assert(charSvcCpp.find("m_world.remove<FormIdComponent, EarlyAnimationBufferComponent, PlayerComponent>(aEntity)") != std::string::npos);
            assert(charSvcCpp.find("apActor->Disable();") == std::string::npos);

            // Regression audit for all 6 thread-safety / UAF / re-entrancy / null-safety hardening fixes:
            // (1) RfabAdapterService::EvaluateCoopAction uses mutex-guarded CoopCombatSnapshot (no cross-thread entt view or PartyService vector read)
            const std::string rfabSvcH = readTextFile("Code/client/Services/RfabAdapterService.h");
            const std::string rfabSvcCpp = readTextFile("Code/client/Services/RfabAdapterService.cpp");
            assert(rfabSvcH.find("struct CoopCombatSnapshot") != std::string::npos);
            assert(rfabSvcH.find("std::atomic<BGSListForm*> m_pNoAllyDamageAllowedSpellsList") != std::string::npos);
            assert(rfabSvcCpp.find("void RfabAdapterService::RefreshCoopCombatSnapshot() noexcept") != std::string::npos);
            {
                const auto evalPos = rfabSvcCpp.find("RfabAdapterService::EvaluateCoopAction(");
                const auto onConnPos = rfabSvcCpp.find("void RfabAdapterService::OnConnected(");
                assert(evalPos != std::string::npos && onConnPos != std::string::npos && evalPos < onConnPos);
                const std::string evalBody = rfabSvcCpp.substr(evalPos, onConnPos - evalPos);
                assert(evalBody.find("m_world.view<") == std::string::npos);
                assert(evalBody.find("GetPartyService()") == std::string::npos);
                assert(evalBody.find("std::lock_guard<std::mutex> lock(m_coopSnapshotMutex)") != std::string::npos);
            }

            // (2) DownedStateService defers camera/Papyrus/aggro/network side effects out of FinalizeLethalHitCushion into OnUpdate
            const std::string downedH = readTextFile("Code/client/Services/DownedStateService.h");
            const std::string downedCpp = readTextFile("Code/client/Services/DownedStateService.cpp");
            assert(downedH.find("std::atomic<bool> m_isLocalDowned") != std::string::npos);
            assert(downedH.find("std::atomic<bool> m_pendingEnterDownedTransition") != std::string::npos);
            assert(downedCpp.find("CompleteEnterLocalDownedState()") != std::string::npos);
            {
                const auto cushionPos = downedCpp.find("void DownedStateService::FinalizeLethalHitCushion(");
                const auto posturePos = downedCpp.find("void DownedStateService::ApplyRemoteActorDownedPosture(");
                assert(cushionPos != std::string::npos && posturePos != std::string::npos && cushionPos < posturePos);
                const std::string cushionBody = downedCpp.substr(cushionPos, posturePos - cushionPos);
                assert(cushionBody.find("EnterLocalDownedState()") == std::string::npos);
                assert(cushionBody.find("m_pendingEnterDownedTransition.store(true") != std::string::npos);
            }

            // (3) AnimationGraphDescriptorManager uses std::unordered_map + std::shared_mutex; BehaviorVar::Add has zero heap leaks
            const std::string agdmH = readTextFile("Code/encoding/Structs/AnimationGraphDescriptorManager.h");
            const std::string agdmCpp = readTextFile("Code/encoding/Structs/AnimationGraphDescriptorManager.cpp");
            const std::string behaviorVarCpp = readTextFile("Code/client/ModCompat/BehaviorVar.cpp");
            const std::string animCpp = readTextFile("Code/client/Games/Animation.cpp");
            assert(agdmH.find("mutable std::shared_mutex m_mutex") != std::string::npos);
            assert(agdmH.find("std::unordered_map<uint64_t, AnimationGraphDescriptor> m_descriptors") != std::string::npos);
            assert(agdmCpp.find("std::shared_lock<std::shared_mutex>") != std::string::npos);
            assert(agdmCpp.find("std::unique_lock<std::shared_mutex>") != std::string::npos);
            assert(behaviorVarCpp.find("new AnimationGraphDescriptor()") == std::string::npos);
            assert(behaviorVarCpp.find("new AnimationGraphDescriptorManager::Builder") == std::string::npos);
            assert(animCpp.find("if (!apAction || !apAction->actor)") != std::string::npos);
            assert(animCpp.find("apAction->action ? apAction->action->formID : 0") != std::string::npos);

            // (4) TESObjectCELL::lock (0x118) guards refData.refArray iteration + baseForm null check
            const std::string cellH = readTextFile("Code/client/Games/Skyrim/Forms/TESObjectCELL.h");
            const std::string cellCpp = readTextFile("Code/client/Games/Skyrim/Forms/TESObjectCELL.cpp");
            const std::string actorCpp = readTextFile("Code/client/Games/Skyrim/Actor.cpp");
            assert(cellH.find("mutable BSRecursiveLock lock;") != std::string::npos);
            assert(cellH.find("static_assert(offsetof(TESObjectCELL, lock) == 0x118);") != std::string::npos);
            assert(cellCpp.find("BSScopedLock<BSRecursiveLock> cellLock(lock);") != std::string::npos);
            assert(cellCpp.find("if (!pRef || !pRef->baseForm)") != std::string::npos);
            assert(actorCpp.find("BSScopedLock<BSRecursiveLock> cellLock(apCell->lock);") != std::string::npos);

            // (6) Stage 1 (v1.2.0): Boss & Arena Encounter Sync
            const std::string extraDataH = readTextFile("Code/client/Games/Skyrim/ExtraData/ExtraData.h");
            const std::string actorBaseDataH = readTextFile("Code/client/Games/Skyrim/Components/TESActorBaseData.h");
            const std::string magicTargetCpp = readTextFile("Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
            const std::string serverAvCpp = readTextFile("Code/server/Services/ActorValueService.cpp");
            const std::string serverObjCpp = readTextFile("Code/server/Services/ObjectService.cpp");
            const std::string clientObjCpp = readTextFile("Code/client/Services/Generic/ObjectService.cpp");
            const std::string serverProgH = readTextFile("Code/server/Services/RfabProgressionService.h");
            const std::string serverProgCpp = readTextFile("Code/server/Services/RfabProgressionService.cpp");

            assert(extraDataH.find("Ghost = 0x7A") != std::string::npos);
            assert(extraDataH.find("struct ExtraGhost : BSExtraData") != std::string::npos);
            assert(actorBaseDataH.find("IS_GHOST = 1u << 29") != std::string::npos);
            assert(actorBaseDataH.find("IS_INVULNERABLE = 1u << 31") != std::string::npos);
            assert(actorCpp.find("bool Actor::IsGhost() const noexcept") != std::string::npos);
            assert(actorCpp.find("void Actor::SetGhost(bool aIsGhost) noexcept") != std::string::npos);
            assert(actorCpp.find("if (apThis && !apThis->GetExtension()->IsPlayer() && apThis->IsGhost())") != std::string::npos);
            assert(magicTargetCpp.find("if (!isFriendlySupport && pTargetActor->IsGhost() && arData.pCaster != pTargetActor)") != std::string::npos);
            assert(rfabSvcCpp.find("bool RfabAdapterService::IsBossActor(Actor* apActor) const noexcept") != std::string::npos);
            assert(rfabSvcCpp.find("void RfabAdapterService::PollBossEncounterState() noexcept") != std::string::npos);
            assert(rfabSvcCpp.find("void RfabAdapterService::BroadcastArenaBarrierUnlock(const GameId& acObjectId) const noexcept") != std::string::npos);
            assert(charSvcCpp.find("void CharacterService::ClaimCombatActorsFromDownedPeer(uint32_t aDownedPeerPlayerId) noexcept") != std::string::npos);
            assert(downedCpp.find("m_world.GetCharacterService().ClaimCombatActorsFromDownedPeer(acMessage.PlayerId);") != std::string::npos);
            assert(serverAvCpp.find("RfabCompat::ShouldBlockFalseBossResetHeal(") != std::string::npos);
            assert(serverProgH.find("bool UpdateBossGhostState(uint32_t aServerEntityId, bool aIsGhost)") != std::string::npos);
            assert(serverProgCpp.find("case SyncRfabProgressionEvent::EventKind::BossEncounterSync:") != std::string::npos);
            assert(refrCpp.find("#define OBJECT_ANIM_SYNC 1") != std::string::npos);
            assert(refrCpp.find("ShouldAutoUnlockCoopArenaBarrier(apThis, pActivator)") != std::string::npos);
            assert(clientObjCpp.find("RfabCompat::AreDoorStatesCompatibleForSync(remotePreActivationState, localState)") != std::string::npos);
            assert(serverObjCpp.find("if (!pPlayer || pPlayer == acMessage.pPlayer)") != std::string::npos);

            // Unit tests for Stage 1 helpers in RfabModpackCompat.h:
            {
                RfabCompat::RfabLiveXpTable bossTable{};
                RfabCompat::ParseBossListJson(
                    R"({"formList":{"bosses":["__formData|Skyrim.esm|0x000F8A48","RFAB_Boss_CustomLich"]}})",
                    bossTable);
                assert(bossTable.ContainsBossBaseFormId(0x040F8A48u));
                assert(!bossTable.ContainsBossBaseFormId(0x00012345u));
                assert(RfabCompat::IsRfabBossEditorId("RFAB_Boss_CustomLich", &bossTable));
                assert(RfabCompat::IsRfabBossEditorId("DLC1LD_ForgemasterBoss", &bossTable));
                assert(RfabCompat::IsRfabBossEditorId("EncDragonPriest09Morokei", &bossTable));
                assert(!RfabCompat::IsRfabBossEditorId("EncBandit01Melee", &bossTable));

                // False boss out-of-combat reset heal blocked while living allies are fighting:
                assert(RfabCompat::ShouldBlockFalseBossResetHeal(
                    /*aEncounterActiveWithLivingAllies=*/true,
                    /*aIsBossOrHighHpActor=*/true,
                    /*aServerCurrentHp=*/1200.0f,
                    /*aServerMaxHp=*/5000.0f,
                    /*aProposedHp=*/5000.0f));

                // Normal RFAB in-combat regeneration ticks (+150 HP per tick) are NOT blocked:
                assert(!RfabCompat::ShouldBlockFalseBossResetHeal(
                    /*aEncounterActiveWithLivingAllies=*/true,
                    /*aIsBossOrHighHpActor=*/true,
                    /*aServerCurrentHp=*/1200.0f,
                    /*aServerMaxHp=*/5000.0f,
                    /*aProposedHp=*/1350.0f));

                // Legitimate out-of-combat reset when encounter has ended (team wiped / left leash) is NOT blocked:
                assert(!RfabCompat::ShouldBlockFalseBossResetHeal(
                    /*aEncounterActiveWithLivingAllies=*/false,
                    /*aIsBossOrHighHpActor=*/true,
                    /*aServerCurrentHp=*/1200.0f,
                    /*aServerMaxHp=*/5000.0f,
                    /*aProposedHp=*/5000.0f));

                // Transitional door open states normalize cleanly so doors never permanently desync:
                assert(RfabCompat::AreDoorStatesCompatibleForSync(1u, 1u)); // kOpen == kOpen
                assert(RfabCompat::AreDoorStatesCompatibleForSync(2u, 1u)); // kOpening ~ kOpen
                assert(RfabCompat::AreDoorStatesCompatibleForSync(3u, 4u)); // kClosed ~ kClosing
                assert(!RfabCompat::AreDoorStatesCompatibleForSync(1u, 3u)); // kOpen != kClosed
            }

            // (7) Stage 2 (v1.2.0): Quest, Puzzle & Dungeon Trigger Sync
            const std::string clientQuestCpp = readTextFile("Code/client/Services/Generic/QuestService.cpp");
            const std::string serverQuestH = readTextFile("Code/server/Services/QuestService.h");
            const std::string serverQuestCpp = readTextFile("Code/server/Services/QuestService.cpp");
            const std::string serverPartyCpp = readTextFile("Code/server/Services/PartyService.cpp");

            assert(clientQuestCpp.find("ScopedRemoteQuestUpdateGuard remoteQuestGuard;") != std::string::npos);
            assert(clientQuestCpp.find("RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId(edid, apQuest->formID)") != std::string::npos);
            assert(clientQuestCpp.find("RfabCompat::IsDungeonOrStoryQuestEditorId(edid)") != std::string::npos);
            assert(clientQuestCpp.find("std::strcmp(pQuest->idName.AsAscii(), name.c_str()) == 0") != std::string::npos);
            assert(clientQuestCpp.find("pQuest->EnsureQuestStarted(started, false);") != std::string::npos);
            assert(serverQuestH.find("void SyncPartyQuestsForPlayer(Player* apPlayer) noexcept;") != std::string::npos);
            assert(serverQuestCpp.find("Console::Setting bEnableMiscQuestSync{\"Gameplay:bEnableMiscQuestSync\", \"Syncs dungeon, bounty, and miscellaneous quests across the party\", true};") != std::string::npos);
            assert(serverQuestCpp.find("void QuestService::SyncLeaderQuestLogToMember(Player* apLeader, Player* apMember) noexcept") != std::string::npos);
            assert(serverQuestCpp.find("dedupHistory.ResetSceneMaster();") != std::string::npos);
            assert(serverPartyCpp.find("m_world.GetQuestService().SyncPartyQuestsForPlayer(aPlayer);") != std::string::npos);
            assert(refrCpp.find("WasObjectDirectlyActivatedRecently(apSelf->formID)") != std::string::npos);
            assert(refrCpp.find("RecordDirectObjectActivation(apThis->formID);") != std::string::npos);

            // Unit tests for Stage 2 quest classification helpers in RfabModpackCompat.h:
            {
                // System / MCM / UI / transformation quests are always blocked across any load order:
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("SKI_ConfigManagerInstance", 0x05000800u));
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("REQ_Atmosphere", 0x09123456u));
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("RFAB_MCM_Quest", 0x07001234u));
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("RFAB_XP_MainQuest", 0x07005678u));
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("", 0x020071D0u)); // DLC1PlayerVampireQuest at slot 02
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("", 0x0A0071D0u)); // DLC1PlayerVampireQuest at slot 0A
                assert(RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("", 0x0002BA16u)); // PlayerWerewolfQuest

                // Real dungeon, puzzle, claw, Dragon Priest, bounty, and story quests are recognized and synced:
                assert(!RfabCompat::IsQuestNonSyncableByEditorIdOrBaseId("dunBleakFallsBarrowQST", 0x00035354u));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("dunBleakFallsBarrowQST"));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("dunForelhostQST"));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("dunValthumeQST"));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("dunLabyrinthianMazeControl"));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("BQ01"));
                assert(RfabCompat::IsDungeonOrStoryQuestEditorId("RFAB_Quest_BossTrial"));
                assert(!RfabCompat::IsDungeonOrStoryQuestEditorId("SKI_WidgetManager"));
                assert(!RfabCompat::IsDungeonOrStoryQuestEditorId("RFAB_MCM"));
            }

            // (8) Stage 3 (v1.2.0): RFAB Combat Stances, Custom Buffs & Aura Sync
            const std::string magicItemCpp = readTextFile("Code/client/Games/Skyrim/Forms/MagicItem.cpp");
            const std::string magicSvcH = readTextFile("Code/client/Services/MagicService.h");
            const std::string magicSvcCpp = readTextFile("Code/client/Services/Generic/MagicService.cpp");

            assert(magicItemCpp.find("RfabCompat::IsFriendlyScriptBuffEffectEditorId(") != std::string::npos);
            assert(magicItemCpp.find("case EffectArchetypes::ArchetypeID::kCureParalysis:") != std::string::npos);
            assert(magicTargetCpp.find("struct ScopedCoopPartyTeammateFlags") != std::string::npos);
            assert(magicTargetCpp.find("ScopedCoopPartyTeammateFlags teammateGuard(enablePartyTeammateAuras);") != std::string::npos);
            assert(magicTargetCpp.find("ScopedCoopPartyTeammateFlags teammateGuard(isFriendlySupport);") != std::string::npos);
            assert(actorCpp.find("PAPYRUS_FUNCTION(bool, Actor, DispelSpell, MagicItem*);") != std::string::npos);
            assert(actorCpp.find("pEffect->uiFlags |= kActiveEffectDispelledFlag;") != std::string::npos);
            assert(magicSvcH.find("void PollLocalPlayerActiveBuffs(double aDeltaSec) noexcept;") != std::string::npos);
            assert(magicSvcH.find("void ResyncLocalPlayerActiveBuffs() noexcept;") != std::string::npos);
            assert(magicSvcCpp.find("bool TryRefreshOrDeduplicateActiveEffect(Actor* apActor, MagicItem* apSpell, EffectItem* apEffect, float aMagnitude) noexcept") != std::string::npos);

            {
                assert(RfabCompat::IsFriendlyScriptBuffEffectEditorId("RFAB_Aura_Priesthood_Buff"));
                assert(RfabCompat::IsFriendlyScriptBuffEffectEditorId("REQ_Illusion_LionheartProc"));
                assert(RfabCompat::IsFriendlyScriptBuffEffectEditorId("RFAB_Stance_TwoHanded_Empower"));
                assert(RfabCompat::IsFriendlyScriptBuffEffectEditorId("AuraSummoning_UndeadFortify"));
                assert(!RfabCompat::IsFriendlyScriptBuffEffectEditorId("RFAB_Script_SummonSpirit"));
                assert(!RfabCompat::IsFriendlyScriptBuffEffectEditorId("REQ_KnockdownStaggerPush"));
                assert(!RfabCompat::IsFriendlyScriptBuffEffectEditorId("RFAB_PoisonDecayCurse"));
            }

            // (5) OverlayClient marshals all CEF UI actions onto World::Get().GetRunner().Queue(...) + PartyService null-guards GetOverlayApp()
            const std::string overlayCpp = readTextFile("Code/client/Services/Generic/OverlayClient.cpp");
            const std::string partyCpp = readTextFile("Code/client/Services/Generic/PartyService.cpp");
            assert(overlayCpp.find("World::Get().GetRunner().Queue([]() { World::Get().GetPartyService().CreateParty(); });") != std::string::npos);
            assert(overlayCpp.find("World::Get().GetRunner().Queue([]() { World::Get().GetPartyService().LeaveParty(); });") != std::string::npos);
            assert(overlayCpp.find("World::Get().GetRunner().Queue([]() { World::Get().GetMagicService().StartRevealingOtherPlayers(); });") != std::string::npos);
            assert(partyCpp.find("GetOverlayApp()->ExecuteAsync") == std::string::npos);

            // (6) ObjectService & PlayerService null-pointer guards
            const std::string objSvcCpp = readTextFile("Code/client/Services/Generic/ObjectService.cpp");
            const std::string playerSvcCpp = readTextFile("Code/client/Services/Generic/PlayerService.cpp");
            assert(objSvcCpp.find("if (!pObject || !pObject->baseForm)") != std::string::npos);
            assert(objSvcCpp.find("pObject->parentCell->formID") == std::string::npos);
            assert(playerSvcCpp.find("PlayerCharacter::Get()->") == std::string::npos);

            // Regression audit for the 6 active co-op playtest fixes (2026-10-24):
            // [Issue #1] Save-load +0x6EDFC2 CTD: never call ->DisableImpl() before ->Delete() or during world-load detach!
            {
                const auto purgePos = charSvcCpp.find("void PurgeDynamicActorCleanly(");
                const auto sanitizePos = charSvcCpp.find("void SanitizeActorCombatStateForLocalOwnership(");
                assert(purgePos != std::string::npos && sanitizePos != std::string::npos && purgePos < sanitizePos);
                const std::string purgeBody = charSvcCpp.substr(purgePos, sanitizePos - purgePos);
                assert(purgeBody.find("->DisableImpl()") == std::string::npos);

                const auto detachPos = charSvcCpp.find("void CharacterService::DetachAllActorsForEngineLoad() noexcept");
                const auto liveDiscPos = charSvcCpp.find("void CharacterService::PurgeNetworkActorsOnLiveDisconnect() noexcept");
                assert(detachPos != std::string::npos && liveDiscPos != std::string::npos && detachPos < liveDiscPos);
                const std::string detachBody = charSvcCpp.substr(detachPos, liveDiscPos - detachPos);
                assert(detachBody.find("->DisableImpl()") == std::string::npos);
                assert(detachBody.find("->Delete()") == std::string::npos);
                assert(detachBody.find("PurgeDynamicActorCleanly(") == std::string::npos);

                const std::string coordSaveCpp = readTextFile("Code/client/Services/CoordinatedSaveService.cpp");
                assert(coordSaveCpp.find("IsLocalPlayerAllyProtectionHost()") != std::string::npos);
                assert(coordSaveCpp.find("BeginEngineWorldLoad()") != std::string::npos);
                assert(coordSaveCpp.find("EndEngineWorldLoad()") != std::string::npos);
            }

            // [Issue #2] StatsMenu CTD & Tab (TweenMenu) lockout: post-SKSE SoulsRE/CustomSkills/RFAB guards + ControlMap self-heal
            {
                const std::string skillsCpp = readTextFile("Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp");
                const std::string uiCpp = readTextFile("Code/client/Games/Skyrim/Interface/UI.cpp");
                const std::string playerControlsH = readTextFile("Code/client/Games/Skyrim/AI/Movement/PlayerControls.h");
                assert(skillsCpp.find("s_skillsMenuInit") == std::string::npos);
                assert(skillsCpp.find("ApplyFallbackStatsMenuPatchesPostSkse()") != std::string::npos);
                assert(skillsCpp.find("CustomSkills.dll") != std::string::npos);
                assert(uiCpp.find("if (IsSkyrimSoulsRuntimeActive())") != std::string::npos);
                assert(uiCpp.find("ApplyFallbackStatsMenuPatchesPostSkse();") != std::string::npos);
                assert(playerControlsH.find("GetEnabledControlsMask()") != std::string::npos);
                assert(rfabSvcCpp.find("SelfHealFreeRoamMenuControls()") != std::string::npos);
            }

            // [Issues #3 & #4] Equip slot/count, speed AV protection, Havok descriptors, & combat-state sanitization on ownership handoff
            {
                const std::string weapH = readTextFile("Code/client/Games/Skyrim/Forms/TESObjectWEAP.h");
                const std::string avSvcCpp = readTextFile("Code/client/Services/Generic/ActorValueService.cpp");
                const std::string masterDescCpp = readTextFile("Code/encoding/Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.cpp");
                const std::string draugrDescCpp = readTextFile("Code/encoding/Structs/Skyrim/AnimationGraphDescriptor_DraugrBehavior.cpp");
                const std::string falmerDescCpp = readTextFile("Code/encoding/Structs/Skyrim/AnimationGraphDescriptor_Falmer_Master_Behavior.cpp");
                assert(weapH.find("UsesSingleHandEquipSlot") != std::string::npos);
                assert(refrCpp.find("pWeapon->UsesSingleHandEquipSlot(dom)") != std::string::npos);
                assert(actorCpp.find("ActorValueInfo::kWeaponSpeedMult") != std::string::npos);
                assert(avSvcCpp.find("ActorValueInfo::kWeaponSpeedMult") != std::string::npos);
                assert(avSvcCpp.find("ActorValueInfo::kLeftWeaponSpeedMult") != std::string::npos);
                assert(masterDescCpp.find("kweaponSpeedMult, k2HMweaponSpeedMult, kleftWeaponSpeedMult") != std::string::npos);
                assert(draugrDescCpp.find("kIsStaggering, kIsBlocking, kIsPowerAttacking") != std::string::npos);
                assert(falmerDescCpp.find("kiRightHandType") != std::string::npos);
                assert(charSvcCpp.find("SanitizeActorCombatStateForLocalOwnership(pActor);") != std::string::npos);
            }

            // [Issues #5 & #6] Network-created vs native cell 0xFFxxxxxx tracking, FormIdComponent on deferred spawns,
            // non-leader ownership handoff from downed owner, and lock-free AI downed detection/target switching
            {
                const std::string actorExtH = readTextFile("Code/client/Games/ActorExtension.h");
                const std::string srvCharCpp = readTextFile("Code/server/Services/CharacterService.cpp");
                const std::string animSysCpp = readTextFile("Code/client/Systems/AnimationSystem.cpp");
                const std::string combatCtrlCpp = readTextFile("Code/client/Games/Skyrim/Combat/CombatController.cpp");
                assert(actorExtH.find("std::atomic<uint32_t> onlineFlags{0};") != std::string::npos);
                assert(actorExtH.find("IsCreatedByNetwork()") != std::string::npos);
                assert(actorExtH.find("IsDownedPlayer()") != std::string::npos);
                assert(charSvcCpp.find("pActor->GetExtension()->SetCreatedByNetwork(true);") != std::string::npos);
                assert(charSvcCpp.find("m_world.emplace_or_replace<FormIdComponent>(aEntity, pActor->formID);") != std::string::npos);
                assert(srvCharCpp.find("partyService.IsPlayerLeader(acMessage.pPlayer) || ownerDowned || !ownerValidInRange") != std::string::npos);
                assert(animSysCpp.find("kLifeStateBleedout") != std::string::npos);
                assert(actorCpp.find("pTargetActor->GetExtension()->IsDownedPlayer()") != std::string::npos);
                assert(combatCtrlCpp.find("#if 0") == std::string::npos);
                assert(combatCtrlCpp.find("FindBestStandingPlayerTargetFor(pAttacker)") != std::string::npos);
                assert(downedCpp.find("->StartCombatEx(") == std::string::npos);
                assert(downedCpp.find("pNpc->SetCombatTargetEx(pNearestLivingAlly);") != std::string::npos);

                const auto findPos = rfabSvcCpp.find("Actor* RfabAdapterService::FindBestStandingPlayerTargetFor(");
                const auto setAllyPos = rfabSvcCpp.find("void RfabAdapterService::SetLocalAllyProtectionEnabled(");
                assert(findPos != std::string::npos && setAllyPos != std::string::npos && findPos < setAllyPos);
                const std::string findBody = rfabSvcCpp.substr(findPos, setAllyPos - findPos);
                assert(findBody.find("m_world.view<") == std::string::npos);
                assert(findBody.find("GetPartyService()") == std::string::npos);
                assert(findBody.find("std::lock_guard<std::mutex> lock(m_coopSnapshotMutex)") != std::string::npos);
            }

            // Regression audit for v1.2.0 deep codebase review & cross-version RFAB resilience fixes:
            {
                assert(fs::exists("docs/V1.0.0_RELEASE_BACKUP_MANIFEST.json"));
                assert(fs::exists("docs/V1.1.0_RELEASE_BACKUP_MANIFEST.json"));
                assert(std::string(RfabCompat::kRfabOnlineVersion) == "1.2.0");
                assert(RfabCompat::AreBuildVersionsProtocolCompatible("v1.2.0", "v1.2.1-3-gabcdef0"));
                assert(RfabCompat::AreBuildVersionsProtocolCompatible("v1.1.0", "v1.2.0"));
                assert(!RfabCompat::AreBuildVersionsProtocolCompatible("v1.0.0", "v1.2.0"));

                const std::string actorExtH = readTextFile("Code/client/Games/ActorExtension.h");
                const std::string avSvcCpp = readTextFile("Code/client/Services/Generic/ActorValueService.cpp");
                const std::string srvAvSvcCpp = readTextFile("Code/server/Services/ActorValueService.cpp");
                const std::string srvPlayerSvcCpp = readTextFile("Code/server/Services/PlayerService.cpp");
                const std::string srvCharCpp = readTextFile("Code/server/Services/CharacterService.cpp");
                const std::string srvProgCpp = readTextFile("Code/server/Services/RfabProgressionService.cpp");
                const std::string magicTargetCpp = readTextFile("Code/client/Games/Skyrim/Magic/MagicTarget.cpp");
                const std::string interpCpp = readTextFile("Code/client/Systems/InterpolationSystem.cpp");
                const std::string discCpp = readTextFile("Code/client/Services/Generic/DiscoveryService.cpp");

                // (a) kInfamy (AV 37) XP-suppression marker is never leaked across the network or left on surviving NPCs
                assert(actorCpp.find("aAvId == ActorValueInfo::kInfamy") != std::string::npos);
                assert(actorCpp.find("apThis->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, std::max(0.0f, prevInfamy));") != std::string::npos);
                assert(avSvcCpp.find("i == ActorValueInfo::kInfamy") != std::string::npos);
                assert(avSvcCpp.find("key == ActorValueInfo::kInfamy") != std::string::npos);
                assert(avSvcCpp.find("pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);") != std::string::npos);
                assert(srvAvSvcCpp.find("kInfamyActorValueIndex = 37") != std::string::npos);

                // (b) Recursive TESLevItem traversal in ShouldWearBodyPiece for Requiem/RFAB nested outfits
                assert(actorCpp.find("CollectArmorFromOutfitForm(") != std::string::npos);

                // (c) RFAB Spirit Summon & remote summon authority (kPlayerSummon flag + IsPlayerSummon across Actor/MagicTarget/CharacterService)
                assert(actorExtH.find("kPlayerSummon = 1u << 4") != std::string::npos);
                assert(actorExtH.find("IsPlayerSummonFlag()") != std::string::npos);
                assert(magicTargetCpp.find("arData.pCaster->IsPlayerSummon()") != std::string::npos);
                assert(magicTargetCpp.find("isFromRemotePlayerOrSummon") != std::string::npos);

                // (d) Server grid shift wilderness/Dragon range check, cell update ordering, ownership claim reconciliation, and host promotion
                assert(srvPlayerSvcCpp.find("newlyEnteredWorldRange") != std::string::npos);
                assert(srvCharCpp.find("reconcileNotify.NewActorData = sanitizedData;") != std::string::npos);
                assert(srvProgCpp.find("BroadcastAllyProtectionState(false);") != std::string::npos);

                // (e) Cross-version RFAB dynamic form resolution & null/teleport safety
                assert(rfabSvcH.find("ResolveRfabPluginMod()") != std::string::npos);
                assert(rfabSvcH.find("ResolveFormByEditorIdOrBaseId(") != std::string::npos);
                assert(interpCpp.find("kTeleportSnapThresholdSq") != std::string::npos);
                assert(discCpp.find("if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID") != std::string::npos);
            }
        }

        // Multi-threaded pointer-stability stress test mirroring AnimationGraphDescriptorManager (shared_mutex + unordered_map):
        {
            struct DummyDescriptor
            {
                uint64_t Hash{0};
                std::vector<uint32_t> Vars;
            };
            std::shared_mutex mtx;
            std::unordered_map<uint64_t, DummyDescriptor> registry;
            registry.emplace(1001ull, DummyDescriptor{1001ull, {1u, 2u, 3u, 4u}});
            const DummyDescriptor* pinnedPtr = &registry.at(1001ull);

            std::atomic<bool> stop{false};
            std::vector<std::thread> readers;
            for (int r = 0; r < 4; ++r)
            {
                readers.emplace_back([&]() {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        const DummyDescriptor* lookedUp = nullptr;
                        {
                            std::shared_lock<std::shared_mutex> lk(mtx);
                            auto it = registry.find(1001ull);
                            assert(it != registry.end());
                            lookedUp = &it->second;
                        }
                        // Node pointer and vector payload MUST stay valid even while writers force bucket rehashes!
                        assert(lookedUp == pinnedPtr);
                        assert(lookedUp->Hash == 1001ull && lookedUp->Vars.size() == 4 && lookedUp->Vars[3] == 4u);
                        std::this_thread::yield();
                    }
                });
            }

            std::vector<std::thread> writers;
            for (int w = 0; w < 2; ++w)
            {
                writers.emplace_back([&, w]() {
                    for (uint64_t i = 0; i < 1500; ++i)
                    {
                        const uint64_t key = 100000ull + static_cast<uint64_t>(w) * 10000ull + i;
                        std::unique_lock<std::shared_mutex> lk(mtx);
                        registry.try_emplace(key, DummyDescriptor{key, {static_cast<uint32_t>(i), 42u}});
                    }
                });
            }

            for (auto& t : writers)
                t.join();
            stop.store(true, std::memory_order_release);
            for (auto& t : readers)
                t.join();
            assert(registry.size() == 3001);
            assert(&registry.at(1001ull) == pinnedPtr);
        }

        // Stage 4 (v1.2.0): Co-Op Party Tactical HUD & 3D Ping System audit
        {
            RfabCompat::TacticalPingPayload src{};
            src.Kind = RfabCompat::TacticalPingKind::EnemyBoss;
            src.SenderPlayerId = 7u;
            src.TargetServerId = 4201u;
            src.WorldSpaceBaseId = 0x3Cu;
            src.CellBaseId = 0x1042u;
            src.PosX = -14250.5f;
            src.PosY = 8920.25f;
            src.PosZ = 1340.75f;
            src.SenderName = "Dragonborn|Injected\nName";
            src.TargetLabel = "Драконий жрец|Морваскар";

            const std::string encoded = RfabCompat::SerializeTacticalPingPayload(src);
            RfabCompat::TacticalPingPayload decoded{};
            assert(RfabCompat::DeserializeTacticalPingPayload(encoded, decoded));
            assert(decoded.Kind == RfabCompat::TacticalPingKind::EnemyBoss);
            assert(decoded.SenderPlayerId == 7u);
            assert(decoded.TargetServerId == 4201u);
            assert(decoded.WorldSpaceBaseId == 0x3Cu);
            assert(decoded.CellBaseId == 0x1042u);
            assert(std::fabs(decoded.PosX - src.PosX) < 0.05f);
            assert(std::fabs(decoded.PosY - src.PosY) < 0.05f);
            assert(std::fabs(decoded.PosZ - src.PosZ) < 0.05f);
            assert(decoded.SenderName == "Dragonborn Injected Name");
            assert(decoded.TargetLabel == "Драконий жрец Морваскар");

            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(true, false, false, true, true, false, 62) ==
                   RfabCompat::TacticalPingKind::Danger);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, true, true, false, false, false, 62) ==
                   RfabCompat::TacticalPingKind::AssistAlly);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, true, true, false, 62) ==
                   RfabCompat::TacticalPingKind::EnemyBoss);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, true, false, 62) ==
                   RfabCompat::TacticalPingKind::Enemy);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, true, 62) ==
                   RfabCompat::TacticalPingKind::Loot);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 28) ==
                   RfabCompat::TacticalPingKind::Loot);
            assert(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 24) ==
                   RfabCompat::TacticalPingKind::Objective);

            const auto onScreen = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.5f, 0.5f, 0.85f, 1920.0f, 1080.0f, 48.0f);
            assert(onScreen.IsOnScreen && !onScreen.IsClampedToEdge);
            assert(std::fabs(onScreen.ScreenX - 960.0f) < 0.5f);
            assert(std::fabs(onScreen.ScreenY - 540.0f) < 0.5f);

            const auto behindRight = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.2f, 0.5f, -0.5f, 1920.0f, 1080.0f, 48.0f);
            assert(!behindRight.IsOnScreen && behindRight.IsClampedToEdge);
            assert(behindRight.DirX > 0.9f);
            assert(std::fabs(behindRight.ScreenX - (1920.0f - 48.0f)) < 1.0f);

            RfabCompat::TacticalChatCommand cmd{};
            assert(RfabCompat::ParseTacticalChatCommand("/ping boss Драконий жрец", cmd));
            assert(cmd.Kind == RfabCompat::TacticalChatCommand::Action::TriggerExplicitPing);
            assert(cmd.PingKind == RfabCompat::TacticalPingKind::EnemyBoss);
            assert(cmd.CustomLabel == "Драконий жрец");

            assert(RfabCompat::ParseTacticalChatCommand("/ping clear", cmd));
            assert(cmd.Kind == RfabCompat::TacticalChatCommand::Action::ClearPings);

            assert(RfabCompat::ParseTacticalChatCommand("/hud compact", cmd));
            assert(cmd.Kind == RfabCompat::TacticalChatCommand::Action::SetHudMode);
            assert(cmd.HudMode == RfabCompat::TacticalHudMode::Compact);

            std::string rfabAdapterCpp;
            std::string rfabProgServerCpp;
            std::string overlayClientCpp;
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/server/Services/RfabProgressionService.cpp", rfabProgServerCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayClient.cpp", overlayClientCpp));
            assert(rfabAdapterCpp.find("OnDrawTacticalHud") != std::string::npos);
            assert(rfabAdapterCpp.find("TacticalPingSync") != std::string::npos);
            assert(rfabAdapterCpp.find("ComputeTacticalMarkerScreenPlacement") != std::string::npos);
            assert(rfabProgServerCpp.find("TacticalPingSync") != std::string::npos);
            assert(overlayClientCpp.find("ParseTacticalChatCommand") != std::string::npos);
        }

        // Stage 5: World Containers, Locks, Shared Keys/Claws, Projectiles/Aggro & Snapshot Rollback
        {
            assert(RfabCompat::IsSharedCoopKeyOrClaw("BleakFallsBarrowKey", 45, 0x000A9636, false));
            assert(!RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, false));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, true));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("MS13GoldenClaw", 32, 0x000399E7, false));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("dunFolgunthurIvoryDragonClaw", 32, 0x000AB7BB, false));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("MQ103Dragonstone", 32, 0x000DF202, false));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("DA04AttunementSphere", 32, 0x0003532C, false));
            assert(RfabCompat::IsSharedCoopKeyOrClaw("MQ205ElderScrollDragon", 27, 0x0002D513, false));
            assert(RfabCompat::IsExcludedFromQuestItemReplication("Gold001", 32, 0x0000000F));
            assert(RfabCompat::IsExcludedFromQuestItemReplication("Lockpick", 32, 0x0000000A));
            assert(RfabCompat::IsExcludedFromQuestItemReplication("RFAB_XPBook_Small", 27, 0x07123456));
            assert(!RfabCompat::IsSharedCoopKeyOrClaw("RFAB_XPBook_Small", 27, 0x07123456, false));

            RfabCompat::CoopAggroDecisionInput aggro{};
            aggro.IsHitteeValidNpc = true;
            aggro.IsHitteeRemote = false;
            aggro.IsHitteeDead = false;
            aggro.IsHitterPlayer = true;
            aggro.IsHitterDowned = false;
            aggro.HasActiveTargetLock = false;
            aggro.IsHitteeInCombat = true;
            aggro.HasValidCurrentTarget = true;
            aggro.IsCurrentTargetDeadOrDowned = false;
            aggro.DistToCurrentTarget = 950.0f;
            aggro.DistToAttacker = 180.0f;
            assert(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));
            aggro.HasActiveTargetLock = true;
            assert(!RfabCompat::ShouldSwitchCoopCombatTarget(aggro));
            aggro.IsCurrentTargetDeadOrDowned = true;
            assert(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));
            aggro.IsHitterDowned = true;
            assert(!RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

            assert(RfabCompat::AreProjectileLaunchParamsSafe(1024.0f, -2048.0f, 512.0f, 0.1f, 0.0f, 1.57f, 1.0f, 1.0f));
            assert(!RfabCompat::AreProjectileLaunchParamsSafe(
                std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f));
        }

        // Stage 6: World Systems, Teleportation, Calendar/Weather, Beast Forms & Reconnect Polish
        {
            for (int h = 0; h < 24; ++h)
            {
                for (int m = 0; m < 60; ++m)
                {
                    const float gameHour = RfabCompat::EncodeSkyrimGameHour(h, m);
                    assert(gameHour >= 0.0f && gameHour < 24.0f);
                    int decH = -1, decM = -1;
                    RfabCompat::DecodeSkyrimGameHour(gameHour, decH, decM);
                    assert(decH == h && decM == m);
                }
            }

            assert(RfabCompat::IsBeastFormRace("WerewolfBeastRace", 0x000CDD84u));
            assert(RfabCompat::IsBeastFormRace("DLC1VampireBeastRace", 0x0400283Au));
            assert(RfabCompat::IsBeastFormRace("DLC2WerebearBeastRace", 0x03023FE9u));
            assert(!RfabCompat::IsBeastFormRace("NordRace", 0x00013746u));

            assert(RfabCompat::CanResolveTeleportDestination(true, false, false, 100.0f, 200.0f, 50.0f, false, false));
            assert(RfabCompat::CanResolveTeleportDestination(true, true, true, 12000.0f, -8000.0f, 350.0f, false, false));
            assert(!RfabCompat::CanResolveTeleportDestination(true, true, false, 12000.0f, -8000.0f, 350.0f, false, false));
            assert(!RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, true, false));
            assert(!RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, false, true));

            std::string serverCalendarCpp;
            std::string serverCommandCpp;
            std::string clientOverlayCpp;
            std::string clientCommandCpp;
            std::string clientPlayerCpp;
            std::string clientWeatherCpp;
            std::string clientMapCpp;
            std::string coordSaveCpp;
            assert(RfabCompat::Detail::ReadTextFile("Code/server/Services/CalendarService.cpp", serverCalendarCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/server/Services/CommandService.cpp", serverCommandCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayService.cpp", clientOverlayCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CommandService.cpp", clientCommandCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/PlayerService.cpp", clientPlayerCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/WeatherService.cpp", clientWeatherCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/MapService.cpp", clientMapCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/CoordinatedSaveService.cpp", coordSaveCpp));

            assert(serverCalendarCpp.find("EncodeSkyrimGameHour") != std::string::npos);
            assert(serverCalendarCpp.find("DecodeSkyrimGameHour") != std::string::npos);
            assert(serverCommandCpp.find("IsPlayerAuthorizedHost") != std::string::npos);
            assert(clientOverlayCpp.find("CanResolveTeleportDestination") != std::string::npos);
            assert(clientCommandCpp.find("CanResolveTeleportDestination") != std::string::npos);
            assert(clientPlayerCpp.find("IsBeastFormRace") != std::string::npos);
            assert(clientWeatherCpp.find("if (Sky* pSky = Sky::Get())") != std::string::npos);
            assert(clientWeatherCpp.find("CanMutateOutdoorSkyWeather") != std::string::npos);
            assert(clientWeatherCpp.find("BSFixedString(\"StatsMenu\")") != std::string::npos);
            assert(clientWeatherCpp.find("BSFixedString(\"CustomMenu\")") != std::string::npos);
            assert(clientMapCpp.find("if (!pWorldSpace)") != std::string::npos);
            assert(coordSaveCpp.find("!pPlayer->IsStillLoading()") != std::string::npos);
        }

        // v1.2.0-hotfix.1 Skills-CTD, MO2 gamePath, Render-Thread Snapshot & Cross-Version RFAB tests:
        {
            assert(fs::exists("docs/V1.2.0_RELEASE_BACKUP_MANIFEST.json"));

            // 1. SkyrimSoulsRE.ini bStatsMenu = false self-healer
            std::string soulsStats = "[UNPAUSED_MENUS]\r\nbStatsMenu = true\r\nbCustomMenu = true\r\n[SLOWMOTION]\r\nfSlowMotionMultiplier = 0.5\r\n";
            assert(RfabCompat::PatchSkyrimSoulsReIniContent(soulsStats));
            assert(soulsStats.find("bStatsMenu = false") != std::string::npos);
            assert(soulsStats.find("bCustomMenu = true") != std::string::npos);
            assert(soulsStats.find("fSlowMotionMultiplier = 1.000000") != std::string::npos);
            assert(!RfabCompat::PatchSkyrimSoulsReIniContent(soulsStats));

            // 2. MO2 ModOrganizer.ini gamePath= decoder (portable ..\\, clean Skyrim override over stale p4, Cyrillic @ByteArray & \xHHHH, %BASE_DIR%)
            assert(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(..\\\\)") == "..\\");
            assert(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(D:\\\\\\xd0\\x98\\xd0\\xb3\\xd1\\x80\\xd1\\x8b\\\\RFAB)") == "D:\\Игры\\RFAB");
            assert(RfabCompat::DecodeMo2IniValueUtf8("D:/\\x0418\\x0433\\x0440\\x044b/RFAB") == "D:/Игры/RFAB");

            const fs::path tmpMo2Root = fs::temp_directory_path() / "rfab_mo2_gamepath_unit_test";
            std::error_code ec;
            fs::remove_all(tmpMo2Root, ec);
            const fs::path stalePackRoot = tmpMo2Root / "OldRfabPack";
            const fs::path cleanSkyrimRoot = tmpMo2Root / RfabCompat::Detail::PathFromUtf8Safe("Чистый_Skyrim_1597");
            const fs::path mo2Dir = stalePackRoot / "MO2";
            const fs::path launcherSelfDir = mo2Dir / "mods" / "RFAB_Online" / "SkyrimTogetherReborn";
            fs::create_directories(stalePackRoot / "Data", ec);
            fs::create_directories(cleanSkyrimRoot / "Data", ec);
            fs::create_directories(launcherSelfDir, ec);
            RfabCompat::Detail::WriteTextFile(stalePackRoot / "SkyrimSE.exe", "MZ");
            RfabCompat::Detail::WriteTextFile(cleanSkyrimRoot / "SkyrimSE.exe", "MZ");

            const std::string cleanUtf8 = RfabCompat::Detail::PathToUtf8Safe(cleanSkyrimRoot);
            std::string hexEscaped;
            for (unsigned char ch : cleanUtf8)
            {
                if (ch == '\\')
                    hexEscaped += "\\\\";
                else if (ch >= 0x80u)
                {
                    char b[8]{};
                    std::snprintf(b, sizeof(b), "\\x%02x", static_cast<unsigned>(ch));
                    hexEscaped += b;
                }
                else
                    hexEscaped.push_back(static_cast<char>(ch));
            }
            RfabCompat::Detail::WriteTextFile(
                mo2Dir / "ModOrganizer.ini",
                "[General]\r\ngameName=Skyrim Special Edition\r\ngamePath=@ByteArray(" + hexEscaped + ")\r\n");

            const fs::path resolvedClean = RfabCompat::ResolveMo2ConfiguredGamePath(launcherSelfDir, launcherSelfDir, "SkyrimSE.exe");
            assert(!resolvedClean.empty());
            assert(fs::equivalent(resolvedClean, cleanSkyrimRoot, ec));

            // Portable template `gamePath=@ByteArray(..\\)` resolves to stalePackRoot
            RfabCompat::Detail::WriteTextFile(
                mo2Dir / "ModOrganizer.ini",
                "[General]\r\ngamePath=@ByteArray(..\\\\)\r\n");
            const fs::path resolvedPortable = RfabCompat::ResolveMo2ConfiguredGamePath(launcherSelfDir, launcherSelfDir, "SkyrimSE.exe");
            assert(!resolvedPortable.empty());
            assert(fs::equivalent(resolvedPortable, stalePackRoot, ec));

            // Version-agnostic CustomSkills / RFAB VFS detection (v1 NSF, v2/v3 SKSE CustomSkills, PerksPlacement, Extended UI)
            assert(!RfabCompat::HasCustomSkillsOrRfabFilesUnderData(cleanSkyrimRoot / "Data"));
            fs::create_directories(cleanSkyrimRoot / "Data" / "SKSE" / "Plugins" / "CustomSkills", ec);
            assert(RfabCompat::HasCustomSkillsOrRfabFilesUnderData(cleanSkyrimRoot / "Data"));
            fs::remove_all(tmpMo2Root, ec);

            // 3. Render-thread isolation check on RfabAdapterService::OnDrawTacticalHud()
            std::string rfabAdapterCpp;
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
            const size_t drawPos = rfabAdapterCpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept");
            assert(drawPos != std::string::npos);
            const std::string drawBody = rfabAdapterCpp.substr(drawPos);
            assert(drawBody.find("m_tacticalHudSnapshotMutex") != std::string::npos);
            assert(drawBody.find("BSFixedString") == std::string::npos);
            assert(drawBody.find("GetMenuOpen") == std::string::npos);
            assert(drawBody.find("m_world.view") == std::string::npos);
            assert(drawBody.find("TESForm::GetById") == std::string::npos);
            assert(drawBody.find("WorldPtToScreenPt3") == std::string::npos);

            // 4. UNICODE_STRING byte-length safety in FileMapping.cpp
            std::string fileMappingCpp;
            assert(RfabCompat::Detail::ReadTextFile("Code/immersive_launcher/stubs/FileMapping.cpp", fileMappingCpp));
            assert(fileMappingCpp.find("DllName->Length / sizeof(wchar_t)") != std::string::npos);

            // 5. Live-test Fixes #1, #2, #3 regression guards
            std::string discoveryCpp;
            std::string coordSaveCpp2;
            std::string clientCharCpp;
            std::string serverCharCpp;
            std::string cellCpp;
            std::string skillsMenuCpp;
            std::string patchSubmodulesPy;
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/DiscoveryService.cpp", discoveryCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/CoordinatedSaveService.cpp", coordSaveCpp2));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CharacterService.cpp", clientCharCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/server/Services/CharacterService.cpp", serverCharCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Forms/TESObjectCELL.cpp", cellCpp));
            assert(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp", skillsMenuCpp));
            assert(RfabCompat::Detail::ReadTextFile("Tools/rfab/patch_submodules.py", patchSubmodulesPy));

            assert(discoveryCpp.find("void DiscoveryService::ResetLoadedForms() noexcept") != std::string::npos);
            assert(discoveryCpp.find("void DiscoveryService::ReconcileMissingWorldEntities() noexcept") != std::string::npos);
            assert(coordSaveCpp2.find("m_world.GetDiscoveryService().ResetLoadedForms()") != std::string::npos);
            assert(coordSaveCpp2.find("pPlayer->parentCell != nullptr") != std::string::npos);
            assert(coordSaveCpp2.find("m_autoReconnectAttemptsLeft") != std::string::npos);
            assert(clientCharCpp.find("void CharacterService::EnsureLocalPlayerRegistered() noexcept") != std::string::npos);
            assert(serverCharCpp.find("otherCell.IsInRange(cellIdComponent, false)") != std::string::npos);
            assert(patchSubmodulesPy.find("apInfo->m_hConn != m_connection") != std::string::npos);

            assert(clientCharCpp.find("pWorldSpace->LoadCell") == std::string::npos);
            assert(clientCharCpp.find("GetCellFromCoordinates(coordinates.X, coordinates.Y, pWorldSpace, false)") != std::string::npos);
            assert(serverCharCpp.find("SendToPlayersInRange(notify, cEntity, acMessage.pPlayer)") != std::string::npos);
            assert(cellCpp.find("SafeGetWorldSpaceOfExteriorCell") != std::string::npos);
            assert(cellCpp.find("17859") != std::string::npos);

            assert(skillsMenuCpp.find("ApplyRfabDllPerkPlacementBoundsFixPostSkse") != std::string::npos);
        }
    }

    std::cout << "[PASS] All SE 1.5.97 struct static_asserts, BSTSmartPointer RAII, Havok stepInfo/IsWordVariable guards, "
                 "TESSpellList null-sanitizer, TESForm::IsStillLoading, ESLoader BOM/CRLF/overflow safety, "
                 "NormalizeServerEndpoint, and MO2 VFS self-healers passed!"
              << std::endl;
    return 0;
}
#else
#include <catch2/catch.hpp>
#include <common/CoopOwnership.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_DraugrBehavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Falmer_Master_Behavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_Master_Behavior.h>
#include <atomic>
#include <thread>
#include <vector>

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
    REQUIRE(partialOverride.find("SaveGameMaxSize = false") != std::string::npos);
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

    // 4. Exact RFAB Respect Edition 30-plugin loadorder.txt verification (RFAB.esp at 0x07, SkyrimTogether.esp at 0x10,
    //    SkyrimTogetherQuestPatches.esp at FE:003, SkyrimOutfitSystem.esp at 0x17):
    {
        const auto respectDir = std::filesystem::temp_directory_path() / "rfab_respect_edition_lo_test";
        std::filesystem::remove_all(respectDir);
        std::filesystem::create_directories(respectDir / "Data");

        auto writeMinPlugin = [&](const std::string& fileName, uint32_t tes4Flags)
        {
            std::ofstream out(respectDir / "Data" / fileName, std::ios::binary);
            std::vector<uint8_t> rec(24 + 18, 0);
            std::memcpy(rec.data(), "TES4", 4);
            const uint32_t dataSize = 18;
            std::memcpy(rec.data() + 4, &dataSize, 4);
            std::memcpy(rec.data() + 8, &tes4Flags, 4);
            std::memcpy(rec.data() + 24, "HEDR", 4);
            const uint16_t hedrSize = 12;
            std::memcpy(rec.data() + 28, &hedrSize, 2);
            const float ver = 1.70f;
            std::memcpy(rec.data() + 30, &ver, 4);
            out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
        };

        const std::vector<std::pair<std::string, uint32_t>> respectPlugins = {
            {"Skyrim.esm", 0x001u},
            {"Update.esm", 0x001u},
            {"Dawnguard.esm", 0x001u},
            {"HearthFires.esm", 0x001u},
            {"Dragonborn.esm", 0x001u},
            {"Unofficial Skyrim Special Edition Patch.esp", 0x001u},
            {"Skyrim Project Optimization - Full Version.esm", 0x001u},
            {"RFAB.esp", 0x001u}, // ESM-flagged at 0x07 in RFAB Respect Edition!
            {"SkyUI_SE.esp", 0x000u},
            {"SkyHUD.esp", 0x000u},
            {"UIExtensions.esp", 0x000u},
            {"Extended UI.esp", 0x000u},
            {"RaceMenu.esp", 0x000u},
            {"RaceMenuPlugin.esp", 0x000u},
            {"Skyrim Mixed Enhanced Physics - Force and Projectiles.esp", 0x000u},
            {"AHZmoreHUDInventory.esp", 0x200u},
            {"BetterContainerControls.esp", 0x000u},
            {"MCMHelper.esp", 0x200u},
            {"TrueHUD.esl", 0x201u},
            {"SkyrimTogether.esp", 0x000u},
            {"SkyrimTogetherQuestPatches.esp", 0x200u},
            {"UntarnishedUI_Blur.esp", 0x200u},
            {"RFAB WheelMenu.esp", 0x000u},
            {"RFABLoadScreens.esp", 0x000u},
            {"QuickLootIE.esp", 0x200u},
            {"AddItemMenuSE.esp", 0x000u},
            {"RFAB_PerksMenu.esp", 0x000u},
            {"RFAB HF for Gold.esp", 0x000u},
            {"icepenguinworldmapclassic.esp", 0x000u},
            {"SkyrimOutfitSystem.esp", 0x000u},
        };

        {
            std::ofstream lo(respectDir / "Data" / "loadorder.txt", std::ios::binary);
            for (const auto& [name, flags] : respectPlugins)
            {
                writeMinPlugin(name, flags);
                lo << name << "\r\n";
            }
        }

        {
            const auto oldCwd = std::filesystem::current_path();
            std::filesystem::current_path(respectDir);
            ESLoader::ESLoader loader;
            (void)loader.BuildRecordCollection();
            const auto& order = loader.GetLoadOrder();
            REQUIRE(order.size() == 30);
            REQUIRE(order[7].m_filename == "RFAB.esp");
            REQUIRE(order[7].m_standardId == 0x07);
            REQUIRE_FALSE(order[7].m_isLite);
            REQUIRE(order[19].m_filename == "SkyrimTogether.esp");
            REQUIRE(order[19].m_standardId == 0x10);
            REQUIRE_FALSE(order[19].m_isLite);
            REQUIRE(order[20].m_filename == "SkyrimTogetherQuestPatches.esp");
            REQUIRE(order[20].m_isLite);
            REQUIRE(order[20].m_liteId == 0x003);
            REQUIRE(order[29].m_filename == "SkyrimOutfitSystem.esp");
            REQUIRE(order[29].m_standardId == 0x17);
            std::filesystem::current_path(oldCwd);
        }
        std::filesystem::remove_all(respectDir);
    }

    // Live multi-threaded stress test on AnimationGraphDescriptorManager:
    // Verifies that concurrent BehaviorVar::Patch registrations on worker threads never race or
    // invalidate raw const AnimationGraphDescriptor* pointers across hash-table bucket rehashes.
    {
        auto& mgr = AnimationGraphDescriptorManager::Get();
        constexpr uint64_t kAnchorKey = 0x52464142'00000001ull;
        {
            const uint32_t bools[] = {3u, 7u, 11u};
            const uint32_t floats[] = {19u, 23u};
            const uint32_t ints[] = {29u};
            AnimationGraphDescriptor anchor(bools, floats, ints);
            AnimationGraphDescriptorManager::Builder(mgr, kAnchorKey, std::move(anchor));
        }

        const AnimationGraphDescriptor* const pPinnedAnchor = mgr.GetDescriptor(kAnchorKey);
        REQUIRE(pPinnedAnchor != nullptr);
        REQUIRE(pPinnedAnchor->BooleanLookUpTable.size() == 3);
        REQUIRE(pPinnedAnchor->FloatLookupTable.size() == 2);
        REQUIRE(pPinnedAnchor->IntegerLookupTable.size() == 1);

        std::atomic<bool> stopReaders{false};
        std::atomic<bool> pointerMismatch{false};
        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r)
        {
            readers.emplace_back([&]() {
                while (!stopReaders.load(std::memory_order_acquire))
                {
                    const AnimationGraphDescriptor* pFound = mgr.GetDescriptor(kAnchorKey);
                    if (pFound != pPinnedAnchor ||
                        pFound->BooleanLookUpTable.size() != 3 ||
                        pFound->BooleanLookUpTable[2] != 11u ||
                        pFound->FloatLookupTable[1] != 23u ||
                        pFound->IntegerLookupTable[0] != 29u)
                    {
                        pointerMismatch.store(true, std::memory_order_release);
                        break;
                    }
                }
            });
        }

        std::vector<std::thread> writers;
        for (int w = 0; w < 2; ++w)
        {
            writers.emplace_back([&, w]() {
                for (uint64_t i = 0; i < 1000; ++i)
                {
                    const uint64_t dynKey = 0x52464142'10000000ull + (static_cast<uint64_t>(w) << 20) + i;
                    const uint32_t bools[] = {static_cast<uint32_t>(i), 42u};
                    const uint32_t floats[] = {100u};
                    const uint32_t ints[] = {200u};
                    AnimationGraphDescriptor desc(bools, floats, ints);
                    AnimationGraphDescriptorManager::Builder(mgr, dynKey, std::move(desc));
                }
            });
        }

        for (auto& t : writers)
            t.join();
        stopReaders.store(true, std::memory_order_release);
        for (auto& t : readers)
            t.join();

        REQUIRE_FALSE(pointerMismatch.load(std::memory_order_acquire));
        REQUIRE(mgr.GetDescriptor(kAnchorKey) == pPinnedAnchor);
        REQUIRE(mgr.GetDescriptors().count(kAnchorKey) == 1);
    }

    // Playtest Combat & Downed Ownership Regression Checks:
    // 1. Verify Master_Behavior, DraugrBehavior, and Falmer_Master_Behavior register synced weapon-speed and combat state variables
    //    within the 64-variable per-type wire limit.
    {
        auto& mgr = AnimationGraphDescriptorManager::Get();
        AnimationGraphDescriptor_Master_Behavior initMaster(mgr);
        AnimationGraphDescriptor_DraugrBehavior initDraugr(mgr);
        AnimationGraphDescriptor_Falmer_Master_Behavior initFalmer(mgr);

        const auto* pMaster = mgr.GetDescriptor(AnimationGraphDescriptor_Master_Behavior::m_key);
        REQUIRE(pMaster != nullptr);
        REQUIRE(pMaster->FloatLookupTable.size() == 16);
        REQUIRE(pMaster->FloatLookupTable.size() <= 64);

        const auto* pDraugr = mgr.GetDescriptor(1556693752012287718ull);
        REQUIRE(pDraugr != nullptr);
        REQUIRE(pDraugr->BooleanLookUpTable.size() == 15);
        REQUIRE(pDraugr->BooleanLookUpTable.size() <= 64);

        const auto* pFalmer = mgr.GetDescriptor(3009402738794250552ull);
        REQUIRE(pFalmer != nullptr);
        REQUIRE(pFalmer->BooleanLookUpTable.size() == 12);
        REQUIRE(pFalmer->IntegerLookupTable.size() == 8);
        REQUIRE(pFalmer->IntegerLookupTable.size() <= 64);
    }

    // 2. Verify CoopOwnership::EvaluateVoluntaryClaim accepts a standing non-leader party member claiming an enemy
    //    from a downed leader immediately (even within the 1500ms anti-flap window) while rejecting claims FROM a downed player.
    {
        const auto acceptFromDownedLeader = CoopOwnership::EvaluateVoluntaryClaim(
            /*aCurrentOwnerId=*/1u,
            /*aChallengerId=*/2u,
            /*aLastChangedAtMs=*/1000ull,
            /*aNowMs=*/1200ull,
            /*aIsPlayerActor=*/false,
            /*aIsPlayerSummon=*/false,
            /*aOwnerValidInRange=*/true,
            /*aOwnerDowned=*/true,
            /*aChallengerDowned=*/false);
        REQUIRE(acceptFromDownedLeader == CoopOwnership::ClaimDecision::kAccept);

        const auto rejectDownedChallenger = CoopOwnership::EvaluateVoluntaryClaim(
            /*aCurrentOwnerId=*/2u,
            /*aChallengerId=*/1u,
            /*aLastChangedAtMs=*/1000ull,
            /*aNowMs=*/5000ull,
            /*aIsPlayerActor=*/false,
            /*aIsPlayerSummon=*/false,
            /*aOwnerValidInRange=*/true,
            /*aOwnerDowned=*/false,
            /*aChallengerDowned=*/true);
        REQUIRE(rejectDownedChallenger == CoopOwnership::ClaimDecision::kRejectChallengerDowned);
    }

    // 3. Stage 4 (v1.2.0): Co-Op Party Tactical HUD & 3D Ping System audit
    {
        // Round-trip serialization and delimiter injection sanitization
        RfabCompat::TacticalPingPayload src{};
        src.Kind = RfabCompat::TacticalPingKind::EnemyBoss;
        src.SenderPlayerId = 7u;
        src.TargetServerId = 4201u;
        src.WorldSpaceBaseId = 0x3Cu;
        src.CellBaseId = 0x1042u;
        src.PosX = -14250.5f;
        src.PosY = 8920.25f;
        src.PosZ = 1340.75f;
        src.SenderName = "Dragonborn|Injected\nName";
        src.TargetLabel = "Драконий жрец|Морваскар";

        const std::string encoded = RfabCompat::SerializeTacticalPingPayload(src);
        RfabCompat::TacticalPingPayload decoded{};
        REQUIRE(RfabCompat::DeserializeTacticalPingPayload(encoded, decoded));
        REQUIRE(decoded.Kind == RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(decoded.SenderPlayerId == 7u);
        REQUIRE(decoded.TargetServerId == 4201u);
        REQUIRE(decoded.WorldSpaceBaseId == 0x3Cu);
        REQUIRE(decoded.CellBaseId == 0x1042u);
        REQUIRE(std::fabs(decoded.PosX - src.PosX) < 0.05f);
        REQUIRE(std::fabs(decoded.PosY - src.PosY) < 0.05f);
        REQUIRE(std::fabs(decoded.PosZ - src.PosZ) < 0.05f);
        REQUIRE(decoded.SenderName == "Dragonborn Injected Name");
        REQUIRE(decoded.TargetLabel == "Драконий жрец Морваскар");

        // Contextual crosshair ping classification
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(true, false, false, true, true, false, 62) ==
                RfabCompat::TacticalPingKind::Danger);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, true, true, false, false, false, 62) ==
                RfabCompat::TacticalPingKind::AssistAlly);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, true, true, false, 62) ==
                RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, true, false, 62) ==
                RfabCompat::TacticalPingKind::Enemy);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, true, 62) ==
                RfabCompat::TacticalPingKind::Loot);
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 28) ==
                RfabCompat::TacticalPingKind::Loot); // Container
        REQUIRE(RfabCompat::ClassifyCrosshairTacticalPingKind(false, false, false, false, false, false, 24) ==
                RfabCompat::TacticalPingKind::Objective); // Activator / Lever

        // 3D-to-2D screen placement & behind-camera (aDepthZ < 0) inversion guard
        const auto onScreen = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.5f, 0.5f, 0.85f, 1920.0f, 1080.0f, 48.0f);
        REQUIRE(onScreen.IsOnScreen);
        REQUIRE_FALSE(onScreen.IsClampedToEdge);
        REQUIRE(std::fabs(onScreen.ScreenX - 960.0f) < 0.5f);
        REQUIRE(std::fabs(onScreen.ScreenY - 540.0f) < 0.5f);

        // Target behind camera on the right (perspective divide by -W mirrored normX to < 0.5; ComputeTacticalMarkerScreenPlacement must flip back to right edge!)
        const auto behindRight = RfabCompat::ComputeTacticalMarkerScreenPlacement(0.2f, 0.5f, -0.5f, 1920.0f, 1080.0f, 48.0f);
        REQUIRE_FALSE(behindRight.IsOnScreen);
        REQUIRE(behindRight.IsClampedToEdge);
        REQUIRE(behindRight.DirX > 0.9f);
        REQUIRE(std::fabs(behindRight.ScreenX - (1920.0f - 48.0f)) < 1.0f);

        // Chat command parser & HUD mode cycling
        RfabCompat::TacticalChatCommand cmd{};
        REQUIRE(RfabCompat::ParseTacticalChatCommand("/ping boss Драконий жрец", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::TriggerExplicitPing);
        REQUIRE(cmd.PingKind == RfabCompat::TacticalPingKind::EnemyBoss);
        REQUIRE(cmd.CustomLabel == "Драконий жрец");

        REQUIRE(RfabCompat::ParseTacticalChatCommand("/ping clear", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::ClearPings);

        REQUIRE(RfabCompat::ParseTacticalChatCommand("/hud compact", cmd));
        REQUIRE(cmd.Kind == RfabCompat::TacticalChatCommand::Action::SetHudMode);
        REQUIRE(cmd.HudMode == RfabCompat::TacticalHudMode::Compact);

        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Full) == RfabCompat::TacticalHudMode::Compact);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Compact) == RfabCompat::TacticalHudMode::MarkersOnly);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::MarkersOnly) == RfabCompat::TacticalHudMode::Hidden);
        REQUIRE(RfabCompat::CycleTacticalHudMode(RfabCompat::TacticalHudMode::Hidden) == RfabCompat::TacticalHudMode::Full);

        std::string rfabAdapterCpp;
        std::string rfabProgServerCpp;
        std::string overlayClientCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/RfabProgressionService.cpp", rfabProgServerCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayClient.cpp", overlayClientCpp));
        REQUIRE(rfabAdapterCpp.find("OnDrawTacticalHud") != std::string::npos);
        REQUIRE(rfabAdapterCpp.find("TacticalPingSync") != std::string::npos);
        REQUIRE(rfabAdapterCpp.find("ComputeTacticalMarkerScreenPlacement") != std::string::npos);
        REQUIRE(rfabProgServerCpp.find("TacticalPingSync") != std::string::npos);
        REQUIRE(overlayClientCpp.find("ParseTacticalChatCommand") != std::string::npos);
    }

    // 19. Stage 5: World Containers, Locks, Shared Keys/Claws, Projectiles/Aggro & Snapshot Rollback
    {
        // Shared dungeon keys & Dragon Claws vs excluded items
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("BleakFallsBarrowKey", 45, 0x000A9636, false));
        REQUIRE_FALSE(RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("WhiterunBreezehomeKey", 45, 0x00093B08, true));

        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MS13GoldenClaw", 32, 0x000399E7, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("dunFolgunthurIvoryDragonClaw", 32, 0x000AB7BB, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MQ103Dragonstone", 32, 0x000DF202, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("DA04AttunementSphere", 32, 0x0003532C, false));
        REQUIRE(RfabCompat::IsSharedCoopKeyOrClaw("MQ205ElderScrollDragon", 27, 0x0002D513, false));

        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("Gold001", 32, 0x0000000F));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("Lockpick", 32, 0x0000000A));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("RFAB_XPBook_Small", 27, 0x07123456));
        REQUIRE(RfabCompat::IsExcludedFromQuestItemReplication("PotionRestoreHealth01", 46, 0x0003EADD));
        REQUIRE_FALSE(RfabCompat::IsSharedCoopKeyOrClaw("RFAB_XPBook_Small", 27, 0x07123456, false));

        // Co-Op Smart Threat / Aggro Switching policy
        RfabCompat::CoopAggroDecisionInput aggro{};
        aggro.IsHitteeValidNpc = true;
        aggro.IsHitteeRemote = false;
        aggro.IsHitteeDead = false;
        aggro.IsHitterPlayer = true;
        aggro.IsHitterDowned = false;
        aggro.HasActiveTargetLock = false;
        aggro.IsHitteeInCombat = true;
        aggro.HasValidCurrentTarget = true;
        aggro.IsCurrentTargetDeadOrDowned = false;
        aggro.DistToCurrentTarget = 950.0f; // Host Archer/Mage at range
        aggro.DistToAttacker = 180.0f;      // Co-Op Tank in melee range
        REQUIRE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Respect active target lock when current target is alive and standing
        aggro.HasActiveTargetLock = true;
        REQUIRE_FALSE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Break target lock immediately if current target goes downed
        aggro.IsCurrentTargetDeadOrDowned = true;
        REQUIRE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Never switch aggro onto a downed attacker
        aggro.IsHitterDowned = true;
        REQUIRE_FALSE(RfabCompat::ShouldSwitchCoopCombatTarget(aggro));

        // Projectile parameter safety
        REQUIRE(RfabCompat::AreProjectileLaunchParamsSafe(1024.0f, -2048.0f, 512.0f, 0.1f, 0.0f, 1.57f, 1.0f, 1.0f));
        REQUIRE_FALSE(RfabCompat::AreProjectileLaunchParamsSafe(
            std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f));
        REQUIRE_FALSE(RfabCompat::AreProjectileLaunchParamsSafe(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f));

        // Static source audit for Stage 5 fixes
        std::string invStructCpp;
        std::string refrCpp;
        std::string clientObjCpp;
        std::string serverObjCpp;
        std::string clientCombatCpp;
        std::string saveSnapCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/encoding/Structs/Inventory.cpp", invStructCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/TESObjectREFR.cpp", refrCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/ObjectService.cpp", clientObjCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/ObjectService.cpp", serverObjCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CombatService.cpp", clientCombatCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/SaveSnapshotService.cpp", saveSnapCpp));

        REQUIRE(invStructCpp.find("remainingToRemove") != std::string::npos);
        REQUIRE(refrCpp.find("RemoveItem(pObject, -arEntry.Count, ITEM_REMOVE_REASON::kRemove, nullptr, nullptr)") != std::string::npos);
        REQUIRE(refrCpp.find("IsSharedCoopKeyOrClaw") != std::string::npos);
        REQUIRE(refrCpp.find("wasLockedBefore") != std::string::npos);
        REQUIRE(clientObjCpp.find("if (pLock->IsLocked())") != std::string::npos);
        REQUIRE(clientObjCpp.find("if (pLock->flags & 0xFF)") == std::string::npos);
        REQUIRE(serverObjCpp.find("IsInRange(acMessage.pPlayer->GetCellComponent(), false)") != std::string::npos);
        REQUIRE(clientCombatCpp.find("ShouldSwitchCoopCombatTarget") != std::string::npos);
        REQUIRE(saveSnapCpp.find("TrackedObjects") != std::string::npos);
    }

    // 20. Stage 6: World Systems, Teleportation, Calendar/Weather, Beast Forms & Reconnect Polish
    {
        // Lossless Skyrim GameHour encode/decode across all 24 * 60 minute combinations (including 23:59!)
        for (int h = 0; h < 24; ++h)
        {
            for (int m = 0; m < 60; ++m)
            {
                const float gameHour = RfabCompat::EncodeSkyrimGameHour(h, m);
                REQUIRE(gameHour >= 0.0f);
                REQUIRE(gameHour < 24.0f);

                int decH = -1;
                int decM = -1;
                RfabCompat::DecodeSkyrimGameHour(gameHour, decH, decM);
                REQUIRE(decH == h);
                REQUIRE(decM == m);
            }
        }

        // Load-order independent Beast Form race recognition
        REQUIRE(RfabCompat::IsBeastFormRace("WerewolfBeastRace", 0x000CDD84u));
        REQUIRE(RfabCompat::IsBeastFormRace("DLC1VampireBeastRace", 0x0200283Au));
        REQUIRE(RfabCompat::IsBeastFormRace("DLC1VampireBeastRace", 0x0400283Au)); // Dawnguard at load index 04
        REQUIRE(RfabCompat::IsBeastFormRace("DLC2WerebearBeastRace", 0x03023FE9u));
        REQUIRE(RfabCompat::IsBeastFormRace("RFAB_CustomLichRace", 0x07123456u));
        REQUIRE_FALSE(RfabCompat::IsBeastFormRace("NordRace", 0x00013746u));
        REQUIRE_FALSE(RfabCompat::IsBeastFormRace("NordRaceVampire", 0x00088794u));

        // Safe Teleport Destination validation (blocks downed/loading, NaN/Inf, and temp 0xFFxxxxxx cells without WorldSpace)
        REQUIRE(RfabCompat::CanResolveTeleportDestination(true, false, false, 100.0f, 200.0f, 50.0f, false, false));
        REQUIRE(RfabCompat::CanResolveTeleportDestination(true, true, true, 12000.0f, -8000.0f, 350.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, true, false, 12000.0f, -8000.0f, 350.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(false, false, false, 0.0f, 0.0f, 0.0f, false, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, true, false));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(true, false, true, 100.0f, 200.0f, 50.0f, false, true));
        REQUIRE_FALSE(RfabCompat::CanResolveTeleportDestination(
            true, false, true, std::numeric_limits<float>::quiet_NaN(), 200.0f, 50.0f, false, false));

        // Static source audit for Stage 6 fixes
        std::string serverCalendarCpp;
        std::string serverCommandCpp;
        std::string clientOverlayCpp;
        std::string clientCommandCpp;
        std::string clientPlayerCpp;
        std::string clientWeatherCpp;
        std::string clientMapCpp;
        std::string coordSaveCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CalendarService.cpp", serverCalendarCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CommandService.cpp", serverCommandCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/OverlayService.cpp", clientOverlayCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CommandService.cpp", clientCommandCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/PlayerService.cpp", clientPlayerCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/WeatherService.cpp", clientWeatherCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/MapService.cpp", clientMapCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/CoordinatedSaveService.cpp", coordSaveCpp));

        REQUIRE(serverCalendarCpp.find("EncodeSkyrimGameHour") != std::string::npos);
        REQUIRE(serverCalendarCpp.find("DecodeSkyrimGameHour") != std::string::npos);
        REQUIRE(serverCommandCpp.find("IsPlayerAuthorizedHost") != std::string::npos);
        REQUIRE(clientOverlayCpp.find("CanResolveTeleportDestination") != std::string::npos);
        REQUIRE(clientCommandCpp.find("CanResolveTeleportDestination") != std::string::npos);
        REQUIRE(clientPlayerCpp.find("IsBeastFormRace") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("if (Sky* pSky = Sky::Get())") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("CanMutateOutdoorSkyWeather") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("BSFixedString(\"StatsMenu\")") != std::string::npos);
        REQUIRE(clientWeatherCpp.find("BSFixedString(\"CustomMenu\")") != std::string::npos);
        REQUIRE(clientMapCpp.find("if (!pWorldSpace)") != std::string::npos);
        REQUIRE(coordSaveCpp.find("!pPlayer->IsStillLoading()") != std::string::npos);

        // v1.2.0-hotfix.1 Skills-CTD, MO2 gamePath, Render-Thread Snapshot & Cross-Version RFAB tests:
        REQUIRE(std::filesystem::exists("docs/V1.2.0_RELEASE_BACKUP_MANIFEST.json"));

        std::string soulsStats = "[UNPAUSED_MENUS]\r\nbStatsMenu = true\r\n[SLOWMOTION]\r\nfSlowMotionMultiplier = 0.5\r\n";
        REQUIRE(RfabCompat::PatchSkyrimSoulsReIniContent(soulsStats));
        REQUIRE(soulsStats.find("bStatsMenu = false") != std::string::npos);
        REQUIRE(soulsStats.find("fSlowMotionMultiplier = 1.000000") != std::string::npos);

        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(..\\\\)") == "..\\");
        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("@ByteArray(D:\\\\\\xd0\\x98\\xd0\\xb3\\xd1\\x80\\xd1\\x8b\\\\RFAB)") == "D:\\Игры\\RFAB");
        REQUIRE(RfabCompat::DecodeMo2IniValueUtf8("D:/\\x0418\\x0433\\x0440\\x044b/RFAB") == "D:/Игры/RFAB");

        std::string rfabAdapterCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/RfabAdapterService.cpp", rfabAdapterCpp));
        const size_t drawPos = rfabAdapterCpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept");
        REQUIRE(drawPos != std::string::npos);
        const std::string drawBody = rfabAdapterCpp.substr(drawPos);
        REQUIRE(drawBody.find("m_tacticalHudSnapshotMutex") != std::string::npos);
        REQUIRE(drawBody.find("BSFixedString") == std::string::npos);
        REQUIRE(drawBody.find("GetMenuOpen") == std::string::npos);
        REQUIRE(drawBody.find("m_world.view") == std::string::npos);
        REQUIRE(drawBody.find("TESForm::GetById") == std::string::npos);
        REQUIRE(drawBody.find("WorldPtToScreenPt3") == std::string::npos);

        std::string fileMappingCpp;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/immersive_launcher/stubs/FileMapping.cpp", fileMappingCpp));
        REQUIRE(fileMappingCpp.find("DllName->Length / sizeof(wchar_t)") != std::string::npos);

        // Live-test Fixes #1, #2, #3 regression guards:
        // 1) Save+Load mutual player visibility, DiscoveryService reset & GNS lingering-handle reconnect guard
        std::string discoveryCpp;
        std::string clientCharCpp;
        std::string serverCharCpp;
        std::string cellCpp;
        std::string skillsMenuCpp;
        std::string patchSubmodulesPy;
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/DiscoveryService.cpp", discoveryCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Services/Generic/CharacterService.cpp", clientCharCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/server/Services/CharacterService.cpp", serverCharCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Forms/TESObjectCELL.cpp", cellCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Code/client/Games/Skyrim/Interface/Menus/SkillsMenu.cpp", skillsMenuCpp));
        REQUIRE(RfabCompat::Detail::ReadTextFile("Tools/rfab/patch_submodules.py", patchSubmodulesPy));

        REQUIRE(discoveryCpp.find("void DiscoveryService::ResetLoadedForms() noexcept") != std::string::npos);
        REQUIRE(discoveryCpp.find("void DiscoveryService::ReconcileMissingWorldEntities() noexcept") != std::string::npos);
        REQUIRE(coordSaveCpp.find("m_world.GetDiscoveryService().ResetLoadedForms()") != std::string::npos);
        REQUIRE(coordSaveCpp.find("pPlayer->parentCell != nullptr") != std::string::npos);
        REQUIRE(coordSaveCpp.find("m_autoReconnectAttemptsLeft") != std::string::npos);
        REQUIRE(clientCharCpp.find("void CharacterService::EnsureLocalPlayerRegistered() noexcept") != std::string::npos);
        REQUIRE(serverCharCpp.find("ReconcileAllEntitiesForObserver(acMessage.pPlayer)") != std::string::npos);
        REQUIRE(serverCharCpp.find("m_interestRegistry.IsMarkedInRange(acMessage.pPlayer->GetId(), otherServerId)") != std::string::npos);
        REQUIRE(patchSubmodulesPy.find("apInfo->m_hConn != m_connection") != std::string::npos);

        // 2) SkyrimSE.exe+0x23D000 (SE_ID 17859) Teleport CTD guard: MoveActor never invokes TESWorldSpace::LoadCell
        REQUIRE(clientCharCpp.find("pWorldSpace->LoadCell") == std::string::npos);
        REQUIRE(clientCharCpp.find("GetCellFromCoordinates(coordinates.X, coordinates.Y, pWorldSpace, false)") != std::string::npos);
        REQUIRE(serverCharCpp.find("SendToPlayersInRange(notify, cEntity, acMessage.pPlayer)") != std::string::npos);
        REQUIRE(cellCpp.find("SafeGetWorldSpaceOfExteriorCell") != std::string::npos);
        REQUIRE(cellCpp.find("17859") != std::string::npos);

        // 3) Update-resilient RFAB.dll PerkPlacementMenuOpenCloseManager bounds scanner + trampoline builder
        REQUIRE(skillsMenuCpp.find("ApplyRfabDllPerkPlacementBoundsFixPostSkse") != std::string::npos);
        {
            // Exact byte slice of RFAB.dll PerkPlacement loop placed at an arbitrary shifted RVA (+0x1234):
            std::vector<uint8_t> fakeText(0x2000, 0xCC);
            const size_t baseOff = 0x1234;
            const uint8_t unpatchedLoop[] = {
                0x33, 0xDB,                                     // xor ebx, ebx
                0x48, 0x8B, 0x90, 0x18, 0x01, 0x00, 0x00,       // mov rdx, [rax+0x118]  (head = baseOff + 2)
                0x48, 0x85, 0xD2,                               // test rdx, rdx         (resume = head + 7)
                0x74, 0x1F,                                     // je skip (+0x1F)
                0x83, 0x7A, 0x20, 0x00,                         // cmp dword ptr [rdx+0x20], 0
                0x74, 0x19,                                     // je skip
                // 25 bytes of inner loop body (`lea rcx, [rdx+0x10]; call copy_ctor; ...`)
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90,
                0x48, 0xFF, 0xC3,                               // inc rbx               (skip = head + 0x2B)
                0x48, 0x8B, 0x44, 0xDE, 0x08,                   // mov rax, [rsi+rbx*8+8]
                0x48, 0x85, 0xC0,                               // test rax, rax
                0x75, 0xC8,                                     // jne head              (exit = skip + 13)
                0x48, 0x8B, 0x4F, 0x08                          // mov rcx, [rdi+8]
            };
            std::memcpy(fakeText.data() + baseOff, unpatchedLoop, sizeof(unpatchedLoop));

            const auto scan = RfabCompat::LocateRfabPerkPlacementLoop(fakeText.data(), fakeText.size());
            REQUIRE(scan.State == RfabCompat::RfabPerkLoopScanResult::Status::FoundUnpatched);
            REQUIRE(scan.LoopHeadOffset == baseOff + 2);
            REQUIRE(scan.ResumeOffset == baseOff + 9);
            REQUIRE(scan.SkipOffset == baseOff + 2 + 0x2B);
            REQUIRE(scan.ExitOffset == baseOff + 2 + 0x2B + 13);

            uint8_t cave[RfabCompat::kRfabPerkTrampolineSize]{};
            const uintptr_t moduleBase = 0x180000000ULL;
            const uintptr_t caveAddr = 0x180250000ULL;
            REQUIRE(RfabCompat::BuildRfabPerkPlacementTrampoline(
                caveAddr,
                moduleBase + scan.ResumeOffset,
                moduleBase + scan.SkipOffset,
                moduleBase + scan.ExitOffset,
                cave));

            auto readRel32Target = [&](size_t dispOffset, size_t nextIpOffset) -> uintptr_t
            {
                int32_t d32 = 0;
                std::memcpy(&d32, cave + dispOffset, sizeof(d32));
                return static_cast<uintptr_t>(static_cast<intptr_t>(caveAddr + nextIpOffset) + static_cast<intptr_t>(d32));
            };
            REQUIRE(readRel32Target(9,  13) == moduleBase + scan.ExitOffset);
            REQUIRE(readRel32Target(19, 23) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(35, 39) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(46, 50) == moduleBase + scan.SkipOffset);
            REQUIRE(readRel32Target(51, 55) == moduleBase + scan.ResumeOffset);

            // Simulate user's disk-patched RFAB.dll (`E9 <rel32> 90 90` at loopHead) -> must detect AlreadyPatched
            fakeText[scan.LoopHeadOffset] = 0xE9;
            fakeText[scan.LoopHeadOffset + 5] = 0x90;
            fakeText[scan.LoopHeadOffset + 6] = 0x90;
            const auto scanDiskPatched = RfabCompat::LocateRfabPerkPlacementLoop(fakeText.data(), fakeText.size());
            REQUIRE(scanDiskPatched.State == RfabCompat::RfabPerkLoopScanResult::Status::AlreadyPatched);
        }
    }
}
#endif
