#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <common/CoopWorldStabilizer.h>
#include <common/RfabModpackCompat.h>
#include <Games/IFormFactory.h>
#include <Games/Memory.h>
#include <Games/TES.h>
#include <Services/CharacterService.h>
#include <Services/CoordinatedSaveService.h>
#include <Utils.h>
#include <World.h>

#include <atomic>
#include <climits>
#include <cstring>
#include <filesystem>
#include <string>

namespace
{
TP_THIS_FUNCTION(TSaveImpl, bool, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName);
TP_THIS_FUNCTION(TSaveGame2, bool, BGSSaveLoadManager, uint64_t aArg1, uint64_t aArg2, uint64_t aArg3);
TP_THIS_FUNCTION(TLoadImpl, bool, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods);
TP_THIS_FUNCTION(TLoadMostRecent, bool, BGSSaveLoadManager);
TP_THIS_FUNCTION(TQueueSaveLoadTask, void, BGSSaveLoadManager, uint32_t aTaskMask);
TP_THIS_FUNCTION(TProcessEvents, void, BGSSaveLoadManager);
TP_THIS_FUNCTION(TLoadEntry, bool, BGSSaveLoadManager, void* apFileEntry, uint64_t aArg2, uint64_t aArg3, uint64_t aArg4, uint64_t aArg5);
TP_THIS_FUNCTION(TLoadGameCallSite, bool, BGSSaveLoadManager, uint64_t* apUnk0, uint32_t aUnk1, uint32_t aUnk2, void* apUnk3);

TSaveImpl* RealSaveImpl = nullptr;
TSaveGame2* RealSaveGame2 = nullptr;
TLoadImpl* RealLoadImpl = nullptr;
TLoadMostRecent* RealLoadMostRecent = nullptr;
TQueueSaveLoadTask* RealQueueSaveLoadTask = nullptr;
TProcessEvents* RealProcessEvents = nullptr;
TLoadEntry* RealLoadEntry = nullptr;
TLoadGameCallSite* RealLoadCallSite = nullptr;

std::atomic<bool> s_universalHooksInstalled{false};
std::atomic<bool> s_quarantinedOnce{false};
thread_local bool s_inDirectProcessEventsSave = false;

// Exact Skyrim SE 1.5.97.0.8 function & call-site offsets verified verbatim against ianpatt/skse64 tag v2.0.20
// (skse64/Hooks_SaveLoad.cpp line 129: LoadGame_HookTarget_Enter = 0x0058AE30 + 0x26C).
// CRITICAL:
// 1. SaveGame_HookTarget_Enter (0x00586DE0 + 0x18B) and SaveGame2_HookTarget_Enter (0x005875F0 + 0x138)
//    MUST NEVER be aborted mid-function because BGSSaveLoadManager has already created/truncated the .ess file
//    on disk before reaching those inner call sites!
// 2. Likewise, LoadGame_HookTarget_Enter (0x0058AE30 + 0x26C) MUST NEVER return a synthetic `false` to block
//    co-op loads, because 0x0058AE30 interprets `false` from +0x26C (`BGSSaveLoadGame::LoadGame`) as `.ess`
//    corruption and displays HUD `sSaveGameCorrupt` ("Файл сохранения поврежден и не может быть загружен.")!
//    All load gating happens at the outer function prologues (`34859`, `34819`, `34856`, and `0x0058AE30`)
//    BEFORE entering 0x0058AE30's body, while +0x26C strictly runs `PrepareEngineForWorldLoad` on authorized loads.
constexpr uintptr_t kSe1597LoadEntryFuncRva = 0x0058AE30;
constexpr uintptr_t kSe1597LoadCallSiteRva = 0x0058AE30 + 0x26C;

bool IsReadableMemoryRange(const void* apAddr, size_t aBytes) noexcept
{
#ifdef _WIN32
    if (!apAddr || aBytes == 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(apAddr, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(apAddr);
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start <= regionEnd && (regionEnd - start) >= aBytes;
#else
    (void)apAddr;
    (void)aBytes;
    return false;
#endif
}

const char* ExtractSaveNameFromFileEntry(const void* apFileEntry) noexcept
{
    // RE::BGSSaveLoadFileEntry stores `BSFixedString fileName` at offset +0x00.
    if (!IsReadableMemoryRange(apFileEntry, sizeof(const char*)))
        return "";
    const char* candidate = *reinterpret_cast<const char* const*>(apFileEntry);
    if (!IsReadableMemoryRange(candidate, 16))
        return "";
    const auto firstByte = static_cast<unsigned char>(candidate[0]);
    if (firstByte < 0x20 || firstByte == 0x7F)
        return "";
    for (size_t i = 0; i < 260; ++i)
    {
        if (!IsReadableMemoryRange(candidate + i, 1))
            return "";
        if (candidate[i] == '\0')
            return candidate;
    }
    return "";
}

const char* ExtractSaveNameFromContext(uint64_t* apUnk0) noexcept
{
    if (!IsReadableMemoryRange(apUnk0, 0xBB0 + sizeof(const char*)))
        return "";
    const char* name = reinterpret_cast<const char*>(apUnk0[0xBB0 / 8]);
    return IsReadableMemoryRange(name, 1) ? name : "";
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

bool TP_MAKE_THISCALL(HookLoadEntry, BGSSaveLoadManager, void* apFileEntry, uint64_t aArg2, uint64_t aArg3, uint64_t aArg4, uint64_t aArg5)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return TiltedPhoques::ThisCall(RealLoadEntry, apThis, apFileEntry, aArg2, aArg3, aArg4, aArg5);

    const char* saveName = ExtractSaveNameFromFileEntry(apFileEntry);
    auto& coord = World::Get().GetCoordinatedSaveService();
    if (!coord.HandleEngineLoadAttempt(saveName))
        return false;

    return TiltedPhoques::ThisCall(RealLoadEntry, apThis, apFileEntry, aArg2, aArg3, aArg4, aArg5);
}

bool TP_MAKE_THISCALL(HookLoadCallSite, BGSSaveLoadManager, uint64_t* apUnk0, uint32_t aUnk1, uint32_t aUnk2, void* apUnk3)
{
    const char* saveName = ExtractSaveNameFromContext(apUnk0);
    auto& coord = World::Get().GetCoordinatedSaveService();

    // Detach live network actors & close transport BEFORE kMessage_PreLoadGame and BGSSaveLoadGame::LoadGame
    // without queueing BSTaskPool Disable/Delete tasks (preventing +0x6EDFC2 ~MovementControllerNPC CTD!).
    // Never return a synthetic `false` here: 0x0058AE30 treats `false` from +0x26C as `sSaveGameCorrupt`.
    coord.PrepareEngineForWorldLoad(saveName);
    const bool ok = TiltedPhoques::ThisCall(RealLoadCallSite, apThis, apUnk0, aUnk1, aUnk2, apUnk3);
    coord.NotifyEngineLoadCompleted(saveName, ok);
    return ok;
}

bool TP_MAKE_THISCALL(HookSaveImpl, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);

    if (s_inDirectProcessEventsSave)
        return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);

    const char* effectiveName = (acpFileName && acpFileName[0] != '\0') ? acpFileName : "ManualSave";
    auto& coord = World::Get().GetCoordinatedSaveService();
    if (!coord.HandleEngineSaveAttempt(effectiveName))
        return false;

    auto& charSvc = World::Get().GetCharacterService();
    charSvc.PreSavePurgeDynamicActors();
    const bool ok = TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);
    charSvc.SchedulePostSaveRemoteRestore();
    coord.NotifyEngineSaveCompleted(effectiveName);
    return ok;
}

bool TP_MAKE_THISCALL(HookSaveGame2, BGSSaveLoadManager, uint64_t aArg1, uint64_t aArg2, uint64_t aArg3)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return TiltedPhoques::ThisCall(RealSaveGame2, apThis, aArg1, aArg2, aArg3);

    auto& coord = World::Get().GetCoordinatedSaveService();
    if (!coord.HandleEngineSaveAttempt("Autosave"))
        return false;

    auto& charSvc = World::Get().GetCharacterService();
    charSvc.PreSavePurgeDynamicActors();
    const bool ok = TiltedPhoques::ThisCall(RealSaveGame2, apThis, aArg1, aArg2, aArg3);
    charSvc.SchedulePostSaveRemoteRestore();
    return ok;
}

bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return TiltedPhoques::ThisCall(RealLoadImpl, apThis, acpFileName, aDeviceId, aOutputStats, aCheckForMods);

    if (!World::Get().GetCoordinatedSaveService().HandleEngineLoadAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealLoadImpl, apThis, acpFileName, aDeviceId, aOutputStats, aCheckForMods);
}

bool TP_MAKE_THISCALL(HookLoadMostRecent, BGSSaveLoadManager)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return TiltedPhoques::ThisCall(RealLoadMostRecent, apThis);

    if (!World::Get().GetCoordinatedSaveService().HandleEngineLoadAttempt(""))
        return false;

    return TiltedPhoques::ThisCall(RealLoadMostRecent, apThis);
}

void TP_MAKE_THISCALL(HookQueueSaveLoadTask, BGSSaveLoadManager, uint32_t aTaskMask)
{
    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
    {
        TiltedPhoques::ThisCall(RealQueueSaveLoadTask, apThis, aTaskMask);
        return;
    }

    auto& coord = World::Get().GetCoordinatedSaveService();

    // Note: 0xF0004000 is SkyrimSoulsRE's custom deferred save task; let it pass into ProcessEvents so
    // SkyrimSoulsRE::SaveHandler resets m_isSaving = false and calls 34818 (HookSaveImpl), which gates it cleanly!
    switch (aTaskMask)
    {
    case 0xF0000040u: // Engine Autosave
        if (!coord.HandleEngineSaveAttempt("Autosave"))
            return;
        break;
    case 0xF0000080u: // EngineFixes RegularQuicksaves F5 / Regular Save
    case 0xF0000100u: // Vanilla F5 Quicksave
    case 0xF0000800u: // Journal Menu Save (when SkyrimSoulsRE is inactive)
        if (!coord.HandleEngineSaveAttempt("ManualSave"))
            return;
        break;
    case 0xD0000100u: // EngineFixes RegularQuicksaves F9 LoadLastSave
    case 0xD0000200u: // Vanilla F9 Quickload
        if (!coord.HandleEngineLoadAttempt(""))
            return;
        break;
    default:
        if (CoopWorldStabilizer::IsQuickloadTaskMask(aTaskMask))
        {
            if (!coord.HandleEngineLoadAttempt(""))
                return;
        }
        break;
    }

    TiltedPhoques::ThisCall(RealQueueSaveLoadTask, apThis, aTaskMask);
}

void TP_MAKE_THISCALL(HookProcessEvents, BGSSaveLoadManager)
{
    TiltedPhoques::ThisCall(RealProcessEvents, apThis);

    if (!s_universalHooksInstalled.load(std::memory_order_acquire))
        return;

    World::Get().GetCoordinatedSaveService().ProcessReadyDeferredSaveOrLoad(apThis);
}

TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
#ifdef _WIN32
        // Raise CRT open-file handle cap to 8192 BEFORE Skyrim opens 600+ RFAB BSAs/ESPs!
        // Prevents EMFILE (512-file limit) from causing "Файл сохранения поврежден и не может быть загружен."
        // even if EngineFixes.toml failed to parse or EngineFixes.dll was not loaded.
        for (const wchar_t* crtModName : {L"ucrtbase.dll", L"api-ms-win-crt-stdio-l1-1-0.dll"})
        {
            if (HMODULE hCrt = GetModuleHandleW(crtModName))
            {
                using TSetMaxStdio = int(__cdecl*)(int);
                if (auto fnSetMaxStdio = reinterpret_cast<TSetMaxStdio>(GetProcAddress(hCrt, "_setmaxstdio")))
                {
                    fnSetMaxStdio(8192);
                    break;
                }
            }
        }
#endif
        POINTER_SKYRIMSE(TSaveImpl, s_saveImpl, 34818);
        POINTER_SKYRIMSE(TSaveGame2, s_saveGame2, 34823);
        POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 34819);
        POINTER_SKYRIMSE(TLoadMostRecent, s_loadMostRecent, 34856);
        POINTER_SKYRIMSE(TQueueSaveLoadTask, s_queueTask, 34859);
        POINTER_SKYRIMSE(TProcessEvents, s_processEvents, 34862);

        RealSaveImpl = s_saveImpl.Get();
        RealSaveGame2 = s_saveGame2.Get();
        RealLoadImpl = s_loadImpl.Get();
        RealLoadMostRecent = s_loadMostRecent.Get();
        RealQueueSaveLoadTask = s_queueTask.Get();
        RealProcessEvents = s_processEvents.Get();

        TP_HOOK(&RealSaveImpl, HookSaveImpl);
        TP_HOOK(&RealSaveGame2, HookSaveGame2);
        TP_HOOK(&RealLoadImpl, HookLoadImpl);
        TP_HOOK(&RealLoadMostRecent, HookLoadMostRecent);
        TP_HOOK(&RealQueueSaveLoadTask, HookQueueSaveLoadTask);
        TP_HOOK(&RealProcessEvents, HookProcessEvents);
    });
} // namespace

BGSSaveLoadManager* BGSSaveLoadManager::Get() noexcept
{
    POINTER_SKYRIMSE(BGSSaveLoadManager*, s_singleton, 516860);
    return *s_singleton.Get();
}

std::filesystem::path BGSSaveLoadManager::ResolveLocalSavesDirectory() noexcept
{
#ifdef _WIN32
    wchar_t docsBuf[MAX_PATH]{};
    bool haveDocs = false;

    if (HMODULE hShell32 = GetModuleHandleW(L"shell32.dll"))
    {
        using TSHGetFolderPathW = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
        auto fnGetFolderPath = reinterpret_cast<TSHGetFolderPathW>(GetProcAddress(hShell32, "SHGetFolderPathW"));
        if (fnGetFolderPath && SUCCEEDED(fnGetFolderPath(nullptr, 0x0005 /*CSIDL_PERSONAL*/, nullptr, 0, docsBuf)) && docsBuf[0] != L'\0')
            haveDocs = true;
    }

    std::filesystem::path rootPath;
    if (haveDocs)
    {
        rootPath = std::filesystem::path(docsBuf);
    }
    else
    {
        wchar_t profileBuf[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profileBuf, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
            rootPath = std::filesystem::path(profileBuf) / L"Documents";
    }

    if (rootPath.empty())
        return {};

    rootPath /= L"My Games";
    rootPath /= L"Skyrim Special Edition";

    std::string localSubPath = "Saves\\";
    if (auto* pIni = INISettingCollection::Get())
    {
        if (Setting* pLocal = pIni->GetSetting("sLocalSavePath:General"))
        {
            if (pLocal->name && pLocal->name[0] == 's' && pLocal->data != 0)
            {
                const char* cStr = reinterpret_cast<const char*>(pLocal->data);
                if (cStr && cStr[0] != '\0')
                    localSubPath = cStr;
            }
        }
    }

    return rootPath / std::filesystem::path(localSubPath);
#else
    return {};
#endif
}

bool BGSSaveLoadManager::VerifySavedSlotOnDisk(const char* acpFileName) noexcept
{
    if (!acpFileName || acpFileName[0] == '\0')
        return false;

    const std::string stem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(acpFileName);
    if (stem.empty())
        return false;

    try
    {
        const std::filesystem::path savesDir = ResolveLocalSavesDirectory();
        if (savesDir.empty())
            return true;

        std::error_code ec;
        if (!std::filesystem::exists(savesDir, ec) || ec)
            return true;

        const std::string targetNorm = RfabCompat::Detail::NormalizeSaveSlotStem(stem);
        const std::wstring wStem(stem.begin(), stem.end());
        const std::filesystem::path directCandidate = savesDir / (wStem + L".ess");
        if (std::filesystem::exists(directCandidate, ec) && !ec)
            return RfabCompat::Detail::IsValidEssFileOnDisk(directCandidate);

        for (const auto& entry : std::filesystem::directory_iterator(savesDir, ec))
        {
            if (ec)
                break;

            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && !entryEc)
            {
                const auto u8Name = entry.path().filename().u8string();
                const std::string fn(reinterpret_cast<const char*>(u8Name.data()), u8Name.size());
                if (RfabCompat::Detail::EndsWithIgnoreCase(fn, ".ess") &&
                    RfabCompat::Detail::NormalizeSaveSlotStem(fn) == targetNorm)
                {
                    return RfabCompat::Detail::IsValidEssFileOnDisk(entry.path());
                }
            }
            else if (entry.is_directory(entryEc) && !entryEc)
            {
                const std::filesystem::path subCandidate = entry.path() / (wStem + L".ess");
                if (std::filesystem::exists(subCandidate, entryEc) && !entryEc)
                    return RfabCompat::Detail::IsValidEssFileOnDisk(subCandidate);
            }
        }
    }
    catch (...)
    {
        return false;
    }

    return false;
}

size_t BGSSaveLoadManager::QuarantineCorruptSavesOnce() noexcept
{
    bool expected = false;
    if (!s_quarantinedOnce.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return 0;

    try
    {
        const std::filesystem::path savesDir = ResolveLocalSavesDirectory();
        if (savesDir.empty())
            return 0;

        size_t quarantined = RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(savesDir);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(savesDir, ec))
        {
            if (ec)
                break;

            std::error_code subEc;
            if (!entry.is_directory(subEc) || subEc)
                continue;

            const auto u8Dir = entry.path().filename().u8string();
            const std::string dirName = RfabCompat::Detail::ToLowerAscii(
                std::string_view(reinterpret_cast<const char*>(u8Dir.data()), u8Dir.size()));
            if (dirName == "quarantined_corrupt_v100" || dirName == "_rfab_corruptquarantine")
                continue;

            quarantined += RfabCompat::Detail::QuarantineCorruptEssFilesInDirectory(entry.path());
        }

        if (quarantined > 0)
            spdlog::warn("[RFAB SaveLoad] Quarantined {} corrupt/truncated .ess save file(s) into _RFAB_CorruptQuarantine", quarantined);

        return quarantined;
    }
    catch (...)
    {
        return 0;
    }
}

void BGSSaveLoadManager::InstallUniversalPostSkseHooks() noexcept
{
    bool expected = false;
    if (!s_universalHooksInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

#ifdef _WIN32
    // Defense-in-depth for large modpacks (>512 BSAs/plugins): raise CRT stdio handle cap to 8192
    // even if SSE Engine Fixes was bypassed or misconfigured.
    for (const wchar_t* crtModName : {L"ucrtbase.dll", L"api-ms-win-crt-stdio-l1-1-0.dll"})
    {
        if (HMODULE hCrt = GetModuleHandleW(crtModName))
        {
            using TSetMaxStdio = int(__cdecl*)(int);
            if (auto fnSetMaxStdio = reinterpret_cast<TSetMaxStdio>(GetProcAddress(hCrt, "_setmaxstdio")))
            {
                fnSetMaxStdio(8192);
                break;
            }
        }
    }
#endif

    QuarantineCorruptSavesOnce();

#ifdef _WIN32
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base)
        return;

    RealLoadEntry = reinterpret_cast<TLoadEntry*>(base + kSe1597LoadEntryFuncRva);
    TP_HOOK(&RealLoadEntry, HookLoadEntry);

    const bool okLoad = PatchRelativeCallSite(base + kSe1597LoadCallSiteRva, reinterpret_cast<void*>(&HookLoadCallSite), &RealLoadCallSite);
    spdlog::info("[RFAB SaveLoad] Installed post-SKSE LoadEntry + LoadCallSite hooks (LoadCallSite={})", okLoad);
#endif
}

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;
}

bool BGSSaveLoadManager::ExecuteDirectSaveFromProcessEvents(const char* acpFileName) noexcept
{
    if (!acpFileName || acpFileName[0] == '\0' || !RealSaveImpl)
        return false;

    const std::string cleanStem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(acpFileName);
    if (cleanStem.empty())
        return false;

    s_inDirectProcessEventsSave = true;
    const bool rawOk = TiltedPhoques::ThisCall(RealSaveImpl, this, 2, 0u, cleanStem.c_str());
    s_inDirectProcessEventsSave = false;

    if (!rawOk)
        return false;

    return VerifySavedSlotOnDisk(cleanStem.c_str());
}

bool BGSSaveLoadManager::ExecuteDirectLoadFromProcessEvents(const char* acpFileName, bool aCheckForMods) noexcept
{
    if (!acpFileName || acpFileName[0] == '\0' || !RealLoadImpl)
        return false;

    const std::string cleanStem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(acpFileName);
    if (cleanStem.empty())
        return false;

    if (!VerifySavedSlotOnDisk(cleanStem.c_str()))
    {
        spdlog::error("[RFAB SaveLoad] Refusing to load missing or corrupt .ess slot '{}'", cleanStem);
        return false;
    }

    return TiltedPhoques::ThisCall(RealLoadImpl, this, cleanStem.c_str(), -1, 0u, aCheckForMods);
}

bool BGSSaveLoadManager::SaveNamed(const char* acpFileName) noexcept
{
    return ExecuteDirectSaveFromProcessEvents(acpFileName);
}

bool BGSSaveLoadManager::LoadNamed(const char* acpFileName, bool aCheckForMods) noexcept
{
    return ExecuteDirectLoadFromProcessEvents(acpFileName, aCheckForMods);
}
