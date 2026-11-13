#include <TiltedOnlinePCH.h>

#include <Games/Skyrim/Interface/IMenu.h>
#include <Games/Skyrim/Interface/Menus/SkillsMenu.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Misc/BSFixedString.h>
#include "immersive_launcher/stubs/ConfigOverlayRedirect.h"
#include "immersive_launcher/stubs/DllBlocklist.h"
#include <common/Compat/MenuPausePolicy.h>

#include <World.h>

#include <Windows.h>
#include <cstring>
#include <string_view>
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

// The one place that decides who (un)pauses menus. Decided once on the main thread in the first
// UI::Register call (after SKSE loaded its plugins), read by the menu creators on the main thread.
Compat::MenuPausePolicy s_menuPolicy{};
bool s_menuPolicyDecided = false;

void DecideMenuPolicyPostSkse() noexcept
{
    Compat::MenuEnvironment environment{};
    environment.SkyrimSoulsLoaded = stubs::g_IsSoulsREActive || GetModuleHandleW(L"SkyrimSoulsRE.dll") != nullptr;
    environment.PerkTreeModsPresent = HasPerkTreeModsPostSkse();
    s_menuPolicy = Compat::DecideMenuPausePolicy(environment);
    s_menuPolicyDecided = true;

    if (s_menuPolicy.PatchFavoritesInput)
    {
        VersionDbPtr<uint8_t> FavoritesCanProcess(50644);
        TiltedPhoques::Put<uint16_t>(FavoritesCanProcess.Get() + 0x15, 0x9090);
    }
    if (s_menuPolicy.PatchStatsMenuInput)
        ApplyStatsMenuInputPatches();

    spdlog::info("[RFAB Menus] {} (Skyrim Souls RE {}, perk-tree mods {})", s_menuPolicy.Describe(),
                 environment.SkyrimSoulsLoaded ? "loaded" : "absent", environment.PerkTreeModsPresent ? "present" : "absent");

    const std::string& coopConfig = stubs::GetCoopConfigReport();
    if (!coopConfig.empty())
        spdlog::info("[RFAB Compat] {}", coopConfig);
}

template <size_t Index>
IMenu* WrappedMenuCreator(UIMessage* apMessage)
{
    const std::string_view menuName = kAllowList[Index];
    // Cached after the first successful resolve; retried here (connected or not) in case RFAB.dll
    // was not mapped yet at the first UI::Register.
    if (menuName == "StatsMenu")
        ApplyRfabDllPerkPlacementBoundsFixPostSkse();

    IMenu* pMenu = s_realCreators[Index](apMessage);
    if (!pMenu || !World::IsConnectedCached() || !s_menuPolicyDecided)
        return pMenu;

    if (menuName == "ContainerMenu")
        World::SetContainerMenuOpenHint(true);

    // RFAB's plugins can only be seen once the plugin list is loaded; that evidence keeps
    // StatsMenu paused from then on (CustomSkills skydome / RFAB PerkPlacementMenuOpenCloseManager).
    // The input patches may already be in place then (they are not reverted; upstream Skyrim Together
    // applies them unconditionally). With RFAB this path is not taken: CustomSkills.dll / RFAB.dll
    // are loaded before the first UI::Register, so the policy is final from the start.
    if (menuName == "StatsMenu" && s_menuPolicy.TogetherUnpauses(menuName) && HasPerkTreePluginsLoaded())
    {
        s_menuPolicy.NoteLatePerkTreeEvidence();
        spdlog::info("[RFAB Menus] RFAB plugins loaded: {}", s_menuPolicy.Describe());
    }

    // With Skyrim Souls RE loaded, Souls' CreateMenu wrapper reads PausesGame() after us to keep
    // UI::numPausesGame / MenuFlagEx::kUnpaused consistent: stripping kPausesGame here breaks
    // TweenMenu (Tab) and crashes StatsMenu. The policy then never lets Together unpause.
    if (s_menuPolicy.TogetherUnpauses(menuName))
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

        // Hook_UI_Register runs inside Main::Init AFTER StartSKSE() has loaded all SKSE plugins,
        // so module presence is final here.
        ApplyRfabDllPerkPlacementBoundsFixPostSkse();
        DecideMenuPolicyPostSkse();
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
