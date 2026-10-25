// The game calls it statsmenu.
#include <TiltedOnlinePCH.h>

#include <Interface/Menus/SkillsMenu.h>
#include <Games/TES.h>
#include <common/RfabModpackCompat.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <Windows.h>
#include <filesystem>

bool ShouldKeepStatsMenuPausedRuntime() noexcept
{
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
