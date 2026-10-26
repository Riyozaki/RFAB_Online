// The game calls it statsmenu.
#include <TiltedOnlinePCH.h>

#include <Interface/Menus/SkillsMenu.h>
#include <Games/TES.h>
#include <common/RfabModpackCompat.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <Windows.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>

namespace
{
uint8_t* AllocateExecutableTrampolineNear(uintptr_t aAnchorAddr, size_t aBytes) noexcept
{
#ifdef _WIN32
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    const uintptr_t gran = sysInfo.dwAllocationGranularity ? sysInfo.dwAllocationGranularity : 0x10000u;
    constexpr uintptr_t kMaxRel32Radius = 0x70000000ULL; // 1.75 GB (< 2 GB rel32 limit)

    const uintptr_t minAddr = (aAnchorAddr > kMaxRel32Radius)
        ? ((aAnchorAddr - kMaxRel32Radius) & ~(gran - 1u))
        : reinterpret_cast<uintptr_t>(sysInfo.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = (std::min)(
        aAnchorAddr + kMaxRel32Radius,
        reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress));

    // Search upward from anchor first (right past RFAB.dll's image), then downward.
    for (uintptr_t probe = (aAnchorAddr & ~(gran - 1u)) + gran; probe < maxAddr;)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(probe), &mbi, sizeof(mbi)))
            break;

        if (mbi.State == MEM_FREE)
        {
            const uintptr_t candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (gran - 1u)) & ~(gran - 1u);
            if (candidate >= probe && candidate + aBytes <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize &&
                candidate - aAnchorAddr <= kMaxRel32Radius)
            {
                if (void* pMem = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), aBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return static_cast<uint8_t*>(pMem);
            }
        }

        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        probe = (next > probe) ? ((next + (gran - 1u)) & ~(gran - 1u)) : (probe + gran);
    }

    for (uintptr_t probe = (aAnchorAddr & ~(gran - 1u)); probe >= minAddr + gran;)
    {
        probe -= gran;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(probe), &mbi, sizeof(mbi)))
            break;

        if (mbi.State == MEM_FREE)
        {
            const uintptr_t candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (gran - 1u)) & ~(gran - 1u);
            if (candidate + aBytes <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize &&
                aAnchorAddr >= candidate && (aAnchorAddr - candidate) <= kMaxRel32Radius)
            {
                if (void* pMem = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), aBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                    return static_cast<uint8_t*>(pMem);
            }
        }
    }
#else
    (void)aAnchorAddr;
    (void)aBytes;
#endif
    return nullptr;
}
} // namespace

bool ApplyRfabDllPerkPlacementBoundsFixPostSkse() noexcept
{
#ifdef _WIN32
    static bool s_resolved = false;
    static bool s_result = false;
    if (s_resolved)
        return s_result;

    HMODULE hRfab = GetModuleHandleW(L"RFAB.dll");
    if (!hRfab)
        return false; // Don't lock s_resolved yet if called before RFAB.dll loaded

    s_resolved = true;

    const auto* pBase = reinterpret_cast<const uint8_t*>(hRfab);
    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pBase);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(pBase + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
    for (WORD idx = 0; idx < pNt->FileHeader.NumberOfSections; ++idx, ++pSec)
    {
        if ((pSec->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;

        const size_t secSize = pSec->Misc.VirtualSize ? pSec->Misc.VirtualSize : pSec->SizeOfRawData;
        const uint8_t* pSecBytes = pBase + pSec->VirtualAddress;

        const auto scan = RfabCompat::LocateRfabPerkPlacementLoop(pSecBytes, secSize);
        if (scan.State == RfabCompat::RfabPerkLoopScanResult::Status::NotFound)
            continue;

        const uintptr_t loopHeadAddr = reinterpret_cast<uintptr_t>(pSecBytes + scan.LoopHeadOffset);
        const uintptr_t loopHeadRva = pSec->VirtualAddress + scan.LoopHeadOffset;

        if (scan.State == RfabCompat::RfabPerkLoopScanResult::Status::AlreadyPatched)
        {
            spdlog::info(
                "[RFAB SkillsMenu] RFAB.dll PerkPlacement ActorValueList bounds hook already present at RVA +0x{:X}; leaving existing patch intact.",
                loopHeadRva);
            s_result = true;
            return true;
        }

        uint8_t* pCave = AllocateExecutableTrampolineNear(loopHeadAddr, 64);
        if (!pCave)
        {
            spdlog::error("[RFAB SkillsMenu] Failed to allocate near trampoline for RFAB.dll +0x{:X}", loopHeadRva);
            return false;
        }

        const uintptr_t caveAddr = reinterpret_cast<uintptr_t>(pCave);
        const uintptr_t resumeAddr = reinterpret_cast<uintptr_t>(pSecBytes + scan.ResumeOffset);
        const uintptr_t skipAddr = reinterpret_cast<uintptr_t>(pSecBytes + scan.SkipOffset);
        const uintptr_t exitAddr = reinterpret_cast<uintptr_t>(pSecBytes + scan.ExitOffset);

        uint8_t caveCode[RfabCompat::kRfabPerkTrampolineSize]{};
        if (!RfabCompat::BuildRfabPerkPlacementTrampoline(caveAddr, resumeAddr, skipAddr, exitAddr, caveCode))
        {
            VirtualFree(pCave, 0, MEM_RELEASE);
            spdlog::error("[RFAB SkillsMenu] Trampoline rel32 out of range for RFAB.dll +0x{:X}", loopHeadRva);
            return false;
        }

        const intptr_t entryDisp = static_cast<intptr_t>(caveAddr) - static_cast<intptr_t>(loopHeadAddr + 5);
        if (entryDisp < INT32_MIN || entryDisp > INT32_MAX)
        {
            VirtualFree(pCave, 0, MEM_RELEASE);
            return false;
        }

        std::memcpy(pCave, caveCode, sizeof(caveCode));
        DWORD dummyProt = 0;
        VirtualProtect(pCave, 64, PAGE_EXECUTE_READ, &dummyProt);
        FlushInstructionCache(GetCurrentProcess(), pCave, sizeof(caveCode));

        auto* pMutableHead = reinterpret_cast<uint8_t*>(loopHeadAddr);
        DWORD oldProt = 0;
        if (!VirtualProtect(pMutableHead, 7, PAGE_EXECUTE_READWRITE, &oldProt))
        {
            spdlog::error("[RFAB SkillsMenu] VirtualProtect failed for RFAB.dll +0x{:X}", loopHeadRva);
            return false;
        }

        pMutableHead[0] = 0xE9;
        const auto disp32 = static_cast<int32_t>(entryDisp);
        std::memcpy(pMutableHead + 1, &disp32, sizeof(disp32));
        pMutableHead[5] = 0x90;
        pMutableHead[6] = 0x90;
        VirtualProtect(pMutableHead, 7, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), pMutableHead, 7);

        spdlog::info(
            "[RFAB SkillsMenu] Installed runtime ActorValueList bounds & null-data guard on RFAB.dll +0x{:X} -> trampoline 0x{:X} (update-resilient).",
            loopHeadRva,
            caveAddr);
        s_result = true;
        return true;
    }

    spdlog::info(
        "[RFAB SkillsMenu] RFAB.dll PerkPlacement loop signature not matched (likely updated RFAB.dll); relying on HookFormAllocate ActorValueList zero-sentinel tail.");
#endif
    return false;
}

bool ShouldKeepStatsMenuPausedRuntime() noexcept
{
    ApplyRfabDllPerkPlacementBoundsFixPostSkse();

    static int s_cachedDecision = -1;
    if (s_cachedDecision == 1)
        return true;
    if (s_cachedDecision == 0)
        return false;

    // 1. Direct SKSE module handles (CustomSkills v1/v2/v3 or RFAB plugin)
    if (GetModuleHandleW(L"CustomSkills.dll") != nullptr || GetModuleHandleW(L"RFAB.dll") != nullptr)
    {
        s_cachedDecision = 1;
        return true;
    }

    // 2. Version-agnostic VFS check under Data/ (handles future RFAB / CustomSkills v2/v3 / Extended UI renames)
    if (RfabCompat::HasCustomSkillsOrRfabFilesUnderData(std::filesystem::path(L"Data")))
    {
        s_cachedDecision = 1;
        return true;
    }

    // 3. Loaded ESP/ESM plugins in ModManager (only lock in `false` once ModManager has actually populated mods)
    if (auto* pModManager = ModManager::Get())
    {
        if (pModManager->GetByName("RFAB.esp") != nullptr ||
            pModManager->GetByName("Extended UI.esp") != nullptr)
        {
            s_cachedDecision = 1;
            return true;
        }

        bool hasAnyModsLoaded = false;
        for (auto* pMod : pModManager->mods)
        {
            if (!pMod || !pMod->IsLoaded())
                continue;
            hasAnyModsLoaded = true;
            const std::string lowerMod = RfabCompat::Detail::ToLowerAscii(pMod->filename);
            if (lowerMod.starts_with("[rfab]") || lowerMod.starts_with("rfab"))
            {
                s_cachedDecision = 1;
                return true;
            }
        }

        if (hasAnyModsLoaded)
            s_cachedDecision = 0;
    }

    return false;
}

void ApplyFallbackStatsMenuPatchesPostSkse() noexcept
{
    ApplyRfabDllPerkPlacementBoundsFixPostSkse();

    static bool s_checkedPostSkse = false;
    if (s_checkedPostSkse)
        return;
    s_checkedPostSkse = true;

    // Must run AFTER StartSKSE() loads SKSE plugins (never inside TiltedPhoques::Initializer at [BOOT 3.1/4]).
    // When SkyrimSoulsRE, CustomSkills, Extended UI, or RFAB (any version) is active, never patch StatsMenu::ProcessMessage
    // or StatsMenu::CanProcess: StatsMenu must stay paused so skydome switching and PerkPlacementMenuOpenCloseManager stay safe.
    const bool hasSoulsRE = stubs::g_IsSoulsREActive || GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr;
    if (hasSoulsRE || ShouldKeepStatsMenuPausedRuntime())
        return;

    // Vanilla fallback (only when neither SkyrimSoulsRE nor CustomSkills/RFAB is present):
    // StatsMenu::ProcessMessage = SE 51638 (0x8BF360)
    VersionDbPtr<uint8_t> ProcessMessage(51638);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0x929, 6);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0xAEC, 4);
    TiltedPhoques::Nop(ProcessMessage.Get() + 0xFBC, 2);

    // StatsMenu::CanProcess = SE 51645 (0x8C0B80)
    VersionDbPtr<uint8_t> controlPatch(51645);
    TiltedPhoques::Nop(controlPatch.Get() + 0x46, 4);
    TiltedPhoques::Nop(controlPatch.Get() + 0x4A, 2);
}
