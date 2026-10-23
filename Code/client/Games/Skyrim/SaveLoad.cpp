#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <Games/IFormFactory.h>
#include <Games/Memory.h>
#include <Services/CharacterService.h>
#include <Services/CoordinatedSaveService.h>
#include <Utils.h>
#include <World.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <string>

namespace
{
TP_THIS_FUNCTION(TSaveImpl, bool, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName);
TP_THIS_FUNCTION(TLoadImpl, bool, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods);
TP_THIS_FUNCTION(TSaveGameCallSite, void, BGSSaveLoadManager, uint64_t* apUnk0);
TP_THIS_FUNCTION(TLoadGameCallSite, bool, BGSSaveLoadManager, uint64_t* apUnk0, uint32_t aUnk1, uint32_t aUnk2, void* apUnk3);

TSaveImpl* RealSaveImpl = nullptr;
TLoadImpl* RealLoadImpl = nullptr;
TSaveGameCallSite* RealSaveCallSite1 = nullptr;
TSaveGameCallSite* RealSaveCallSite2 = nullptr;
TLoadGameCallSite* RealLoadCallSite = nullptr;
std::atomic<bool> s_universalHooksInstalled{false};

// Exact Skyrim SE 1.5.97.0.8 call-site offsets verified verbatim against ianpatt/skse64 tag v2.0.20
// (skse64/Hooks_SaveLoad.cpp lines 122-124):
// - SaveGame_HookTarget_Enter  = 0x00586DE0 + 0x18B (named / console save path)
// - SaveGame2_HookTarget_Enter = 0x005875F0 + 0x138 (Journal Menu Save, F5 Quicksave, RegularQuicksaves, Autosave)
// - LoadGame_HookTarget_Enter  = 0x0058AE30 + 0x26C (Journal Menu Load, F9 Quickload, Continue, named load)
// Wrapping the 5-byte E8 call instruction AFTER skse64_1_5_97.dll commits its own hooks preserves 100% of
// SKSE's .skse co-save serialization while intercepting blocked saves/loads BEFORE kMessage_PreLoadGame fires!
constexpr uintptr_t kSe1597SaveCallSite1Rva = 0x00586DE0 + 0x18B;
constexpr uintptr_t kSe1597SaveCallSite2Rva = 0x005875F0 + 0x138;
constexpr uintptr_t kSe1597LoadCallSiteRva = 0x0058AE30 + 0x26C;

const char* ExtractSaveNameFromContext(uint64_t* apUnk0) noexcept
{
    if (!apUnk0)
        return "";
    const char* name = reinterpret_cast<const char*>(apUnk0[0xBB0 / 8]);
    return name ? name : "";
}

template <typename TFn>
bool PatchRelativeCallSite(uintptr_t aCallSiteAddr, void* apDetourFn, TFn** appOutOriginal) noexcept
{
#ifdef _WIN32
    auto* pSite = reinterpret_cast<uint8_t*>(aCallSiteAddr);
    if (!pSite || pSite[0] != 0xE8)
    {
        spdlog::warn("[RFAB SaveLoad] Skipped call-site 0x{:X}: first byte is 0x{:02X} (expected 0xE8)", aCallSiteAddr, pSite ? pSite[0] : 0);
        return false;
    }

    const int32_t origDisp = *reinterpret_cast<const int32_t*>(pSite + 1);
    const uintptr_t nextRip = aCallSiteAddr + 5;
    const uintptr_t origTarget = static_cast<uintptr_t>(static_cast<int64_t>(nextRip) + static_cast<int64_t>(origDisp));

    // Emit a clean 14-byte indirect absolute jump (`FF 25 00 00 00 00 <64-bit target>`) inside the ±2 GB
    // RIP-near executable region (`RipAllocateN` guarantees PAGE_EXECUTE_READWRITE).
    auto* pThunk = static_cast<uint8_t*>(::RipAllocateN(32));
    if (!pThunk)
    {
        spdlog::error("[RFAB SaveLoad] RipAllocateN(32) failed for call-site 0x{:X}", aCallSiteAddr);
        return false;
    }

    const int64_t newDisp = reinterpret_cast<intptr_t>(pThunk) - static_cast<intptr_t>(nextRip);
    if (newDisp < INT32_MIN || newDisp > INT32_MAX)
    {
        spdlog::error("[RFAB SaveLoad] Thunk out of rel32 range for call-site 0x{:X}", aCallSiteAddr);
        return false;
    }

    pThunk[0] = 0xFF;
    pThunk[1] = 0x25;
    pThunk[2] = 0x00;
    pThunk[3] = 0x00;
    pThunk[4] = 0x00;
    pThunk[5] = 0x00;
    const uint64_t targetAddr = reinterpret_cast<uint64_t>(apDetourFn);
    std::memcpy(pThunk + 6, &targetAddr, sizeof(targetAddr));
    FlushInstructionCache(GetCurrentProcess(), pThunk, 14);

    *appOutOriginal = reinterpret_cast<TFn*>(origTarget);

    DWORD oldProt = 0;
    if (!VirtualProtect(pSite, 5, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;

    const auto disp32 = static_cast<int32_t>(newDisp);
    std::memcpy(pSite + 1, &disp32, sizeof(disp32));
    VirtualProtect(pSite, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), pSite, 5);
    return true;
#else
    (void)aCallSiteAddr;
    (void)apDetourFn;
    (void)appOutOriginal;
    return false;
#endif
}

void RunSaveCallSiteCommon(TSaveGameCallSite* apOriginal, BGSSaveLoadManager* apThis, uint64_t* apUnk0) noexcept
{
    const char* saveName = ExtractSaveNameFromContext(apUnk0);
    auto& coord = World::Get().GetCoordinatedSaveService();
    if (!coord.HandleEngineSaveAttempt(saveName))
        return;

    // Strip all dynamic 0xFFxxxxxx remote clones right before BGSSaveLoadGame serializes forms so
    // EVERY .ess file written to disk is 100% pure single-player-clean (restored automatically next tick).
    World::Get().GetCharacterService().PreSavePurgeDynamicActors();
    TiltedPhoques::ThisCall(apOriginal, apThis, apUnk0);
    coord.NotifyEngineSaveCompleted(saveName);
}

void TP_MAKE_THISCALL(HookSaveCallSite1, BGSSaveLoadManager, uint64_t* apUnk0)
{
    RunSaveCallSiteCommon(RealSaveCallSite1, apThis, apUnk0);
}

void TP_MAKE_THISCALL(HookSaveCallSite2, BGSSaveLoadManager, uint64_t* apUnk0)
{
    RunSaveCallSiteCommon(RealSaveCallSite2, apThis, apUnk0);
}

bool TP_MAKE_THISCALL(HookLoadCallSite, BGSSaveLoadManager, uint64_t* apUnk0, uint32_t aUnk1, uint32_t aUnk2, void* apUnk3)
{
    const char* saveName = ExtractSaveNameFromContext(apUnk0);
    auto& coord = World::Get().GetCoordinatedSaveService();
    if (!coord.HandleEngineLoadAttempt(saveName))
        return false;

    // Tear down live network session & purge EVERY dynamic 0xFFxxxxxx actor BEFORE kMessage_PreLoadGame
    // and BGSSaveLoadGame::Revert execute — eliminating the 0xC0000005 UAF crash at SkyrimSE.exe+0x26693B!
    coord.PrepareEngineForWorldLoad(saveName);
    const bool ok = TiltedPhoques::ThisCall(RealLoadCallSite, apThis, apUnk0, aUnk1, aUnk2, apUnk3);
    coord.NotifyEngineLoadCompleted(saveName, ok);
    return ok;
}

bool ExecuteConsoleSaveOrLoadCommand(const std::string& acCommand) noexcept
{
    if (acCommand.empty())
        return false;

    static TESForm* s_pConsoleScript = nullptr;
    if (!s_pConsoleScript)
    {
        auto* pFactory = IFormFactory::GetForType(FormType::Script);
        if (!pFactory)
            return false;
        s_pConsoleScript = pFactory->Create();
    }
    if (!s_pConsoleScript)
        return false;

    // Offset 0x38 inside RE::Script (sizeof == 0x80) is `char* text`
    char** ppText = reinterpret_cast<char**>(reinterpret_cast<uint8_t*>(s_pConsoleScript) + 0x38);
    if (*ppText)
    {
        Memory::Free(*ppText);
        *ppText = nullptr;
    }

    char* pBuf = static_cast<char*>(Memory::Allocate(acCommand.size() + 1));
    if (!pBuf)
        return false;

    std::memcpy(pBuf, acCommand.data(), acCommand.size() + 1);
    *ppText = pBuf;

    struct DummyScriptCompiler
    {
        uint8_t pad{0};
    } compiler{};

    TP_THIS_FUNCTION(TCompileAndRun, void, TESForm, DummyScriptCompiler*, uint32_t, void*);
    POINTER_SKYRIMSE(TCompileAndRun, s_compileAndRun, 21416);
    TiltedPhoques::ThisCall(s_compileAndRun, s_pConsoleScript, &compiler, 1U /*kSystemWindowCompiler*/, nullptr);
    return true;
}

bool TP_MAKE_THISCALL(HookSaveImpl, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineSaveAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);
}

bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineLoadAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealLoadImpl, apThis, acpFileName, aDeviceId, aOutputStats, aCheckForMods);
}

TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TSaveImpl, s_saveImpl, 34818);
        POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 34819);

        RealSaveImpl = s_saveImpl.Get();
        RealLoadImpl = s_loadImpl.Get();

        TP_HOOK(&RealSaveImpl, HookSaveImpl);
        TP_HOOK(&RealLoadImpl, HookLoadImpl);
    });
} // namespace

BGSSaveLoadManager* BGSSaveLoadManager::Get() noexcept
{
    POINTER_SKYRIMSE(BGSSaveLoadManager*, s_singleton, 516860);
    return *s_singleton.Get();
}

void BGSSaveLoadManager::InstallUniversalPostSkseHooks() noexcept
{
    bool expected = false;
    if (!s_universalHooksInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

#ifdef _WIN32
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
        return;

    const bool okSave1 = PatchRelativeCallSite(base + kSe1597SaveCallSite1Rva, reinterpret_cast<void*>(&HookSaveCallSite1), &RealSaveCallSite1);
    const bool okSave2 = PatchRelativeCallSite(base + kSe1597SaveCallSite2Rva, reinterpret_cast<void*>(&HookSaveCallSite2), &RealSaveCallSite2);
    const bool okLoad = PatchRelativeCallSite(base + kSe1597LoadCallSiteRva, reinterpret_cast<void*>(&HookLoadCallSite), &RealLoadCallSite);
    spdlog::info(
        "[RFAB SaveLoad] Installed post-SKSE universal hooks (Save1={}, Save2={}, Load={})",
        okSave1,
        okSave2,
        okLoad);
#endif
}

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}

bool BGSSaveLoadManager::SaveNamed(const char* acpFileName) noexcept
{
    if (!acpFileName || acpFileName[0] == '\0')
        return false;

    // Enqueue via Skyrim's native console command runner (`save "<slot>"`) instead of calling
    // Save_Impl directly inside Main::Update, avoiding renderer screenshot capture deadlocks.
    std::string cmd = "save \"";
    cmd += acpFileName;
    cmd += "\"";
    return ExecuteConsoleSaveOrLoadCommand(cmd);
}

bool BGSSaveLoadManager::LoadNamed(const char* acpFileName, bool aCheckForMods) noexcept
{
    TP_UNUSED(aCheckForMods);
    if (!acpFileName || acpFileName[0] == '\0')
        return false;

    std::string cmd = "load \"";
    cmd += acpFileName;
    cmd += "\"";
    return ExecuteConsoleSaveOrLoadCommand(cmd);
}
