#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/Interface/Menus/SkillsMenu.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Misc/BSFixedString.h>
#include "immersive_launcher/stubs/DllBlocklist.h"

#include <World.h>

#include <Windows.h>
#include <cstring>
#include <utility>

static bool g_RequestUnpauseAll{false};

UI* UI::Get()
{
    POINTER_SKYRIMSE(UI*, s_instance, 514178);
    return *s_instance.Get();
}

bool UI::GetMenuOpen(const BSFixedString& acName) const
{
    if (acName.data == nullptr)
        return false;

    TP_THIS_FUNCTION(TMenuSystem_IsOpen, bool, const UI, const BSFixedString&);
    POINTER_SKYRIMSE(TMenuSystem_IsOpen, s_isMenuOpen, 79937);

    return TiltedPhoques::ThisCall(s_isMenuOpen.Get(), this, acName);
}

void UI::CloseAllMenus()
{
    TP_THIS_FUNCTION(TUI_CloseAll, void, const UI);
    POINTER_SKYRIMSE(TUI_CloseAll, s_CloseAll, 79951);

    TiltedPhoques::ThisCall(s_CloseAll.Get(), this);
}

BSFixedString* UI::LookupMenuNameByInstance(IMenu* apMenu)
{
    for (auto& it : menuMap)
    {
        if (it.value.spMenu == apMenu)
            return &it.key;
    }
    return nullptr;
}

IMenu* UI::FindMenuByName(const BSFixedString& acName)
{
    for (const auto& it : menuMap)
    {
        if (it.key == acName)
            return it.value.spMenu;
    }
    return nullptr;
}

void UI::DebugLogAllMenus()
{
    for (auto& e : menuStack)
    {
        spdlog::info("Menu {}", e->uiMenuFlags);
    }
}

static void UnfreezeMenu(IMenu* apEntry)
{
    if (apEntry->PausesGame())
        apEntry->ClearFlag(IMenu::kPausesGame);

    if (apEntry->FreezesBackground())
        apEntry->ClearFlag(IMenu::kFreezeFrameBackground);

    if (apEntry->FreezesFramePause())
        apEntry->ClearFlag(IMenu::kFreezeFramePause);
}

// Includes "CustomMenu" so RFAB_Menu (stats/perks menu in RFAB) also works smoothly while connected.
static constexpr const char* kAllowList[] = {
    "TweenMenu",     "MagicMenu",     "StatsMenu",     "InventoryMenu", "MessageBoxMenu",
    "ContainerMenu", "FavoritesMenu", "Tutorial Menu", "Console",       "CustomMenu"
};

namespace
{
constexpr size_t kAllowListCount = sizeof(kAllowList) / sizeof(kAllowList[0]);
static UI::TCreate* s_realCreators[kAllowListCount]{};
static bool s_postSkseUiInitDone = false;

bool IsSkyrimSoulsRuntimeActive() noexcept
{
    if (stubs::g_IsSoulsREActive)
        return true;

    static int s_cachedModuleCheck = -1;
    if (s_cachedModuleCheck < 0)
        s_cachedModuleCheck = (GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr) ? 1 : 0;

    return s_cachedModuleCheck == 1;
}

bool ShouldKeepStatsMenuPaused() noexcept
{
    return ShouldKeepStatsMenuPausedRuntime();
}

template <size_t Index>
IMenu* WrappedMenuCreator(UIMessage* apMessage)
{
    IMenu* pMenu = s_realCreators[Index](apMessage);
    if (!pMenu || !World::IsConnectedCached())
        return pMenu;

    if constexpr (Index < kAllowListCount)
    {
        if (std::strcmp(kAllowList[Index], "ContainerMenu") == 0)
            World::SetContainerMenuOpenHint(true);
    }

    // CRITICAL FOR RFAB + SKYRIMSOULS RE (SE 1.5.97):
    // When SkyrimSoulsRE.dll is loaded, SkyrimSoulsRE::CreateMenu wraps our menu creator and checks
    // `if (menu->PausesGame())` to track MenuFlagEx::kUnpaused & UI::numPausesGame, while deliberately
    // keeping StatsMenu paused (`bStatsMenu = false` in RFAB's SkyrimSoulsRE.ini) for CustomSkills.dll / RFAB.dll.
    // Stripping kPausesGame here before SkyrimSoulsRE::CreateMenu sees the menu breaks TweenMenu (`Tab`)
    // unpause accounting and crashes StatsMenu (`Skills`)!
    if (IsSkyrimSoulsRuntimeActive())
        return pMenu;

    if (std::strcmp(kAllowList[Index], "StatsMenu") == 0 && ShouldKeepStatsMenuPaused())
        return pMenu;

    UnfreezeMenu(pMenu);
    return pMenu;
}

template <size_t... Indices>
UI::TCreate* WrapCreatorImpl(const char* apName, UI::TCreate* apCreate, std::index_sequence<Indices...>)
{
    UI::TCreate* result = apCreate;
    (
        [&]()
        {
            if (std::strcmp(apName, kAllowList[Indices]) == 0)
            {
                s_realCreators[Indices] = apCreate;
                result = &WrappedMenuCreator<Indices>;
            }
        }(),
        ...);
    return result;
}

using TUI_Register = void(UI*, const char*, UI::TCreate*);
static TUI_Register* Real_UI_Register = nullptr;

static void Hook_UI_Register(UI* apSelf, const char* apMenuName, UI::TCreate* apCreate)
{
    if (!s_postSkseUiInitDone)
    {
        s_postSkseUiInitDone = true;
        if (auto* pIni = INISettingCollection::Get())
        {
            if (auto* pIntro = pIni->GetSetting("sIntroSequence:General"))
                pIntro->data = reinterpret_cast<uint64_t>("");
        }

        // Hook_UI_Register runs inside Main::Init AFTER StartSKSE() has loaded all SKSE plugins.
        // Only apply vanilla fallback patches when SkyrimSoulsRE.dll is genuinely absent.
        if (!IsSkyrimSoulsRuntimeActive())
        {
            VersionDbPtr<uint8_t> FavoritesCanProcess(50644);
            TiltedPhoques::Put<uint16_t>(FavoritesCanProcess.Get() + 0x15, 0x9090);
        }

        ApplyFallbackStatsMenuPatchesPostSkse();
    }

    if (apMenuName && apCreate)
    {
        apCreate = WrapCreatorImpl(apMenuName, apCreate, std::make_index_sequence<kAllowListCount>{});
    }

    Real_UI_Register(apSelf, apMenuName, apCreate);
}
} // namespace

static TiltedPhoques::Initializer s_s(
    []()
    {
        // Hook UI::Register (SE 79949 @ 0xEBF9C0) at function entry (+0x0).
        // All SKSE plugin presence checks (SkyrimSoulsRE.dll, CustomSkills.dll, RFAB.dll) are deferred
        // to Hook_UI_Register / WrappedMenuCreator so they execute AFTER StartSKSE() at [BOOT 4/4].
        VersionDbPtr<TUI_Register> registerMenu(79949);
        Real_UI_Register = registerMenu.Get();
        TP_HOOK(&Real_UI_Register, Hook_UI_Register);
    });
