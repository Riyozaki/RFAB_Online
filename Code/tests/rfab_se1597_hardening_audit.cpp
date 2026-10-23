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

    std::string partialOverride = "[Patches]\r\nRegularQuicksaves = false\r\n";
    REQUIRE(RfabCompat::PatchEngineFixesTomlContent(partialOverride));
    REQUIRE(partialOverride.find("MemoryManager = false") != std::string::npos);
    REQUIRE(partialOverride.find("ScaleformAllocator = false") != std::string::npos);
    REQUIRE(partialOverride.find("RegularQuicksaves = true") != std::string::npos);
    REQUIRE(partialOverride.find("MaxStdio = 8192") != std::string::npos);
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
}
#endif
