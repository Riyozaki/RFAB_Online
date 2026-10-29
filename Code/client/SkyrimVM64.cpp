#include <TiltedOnlinePCH.h>
#include "TiltedOnlineApp.h"

extern std::unique_ptr<TiltedOnlineApp> g_appInstance;

#include <GameVM.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Misc/BSFixedString.h>
#include <PlayerCharacter.h>
#include <World.h>
#include <common/CoopWorldStabilizer.h>

#include <atomic>
#include <cstddef>

struct Main
{
    uint8_t pad00[0x10];
    bool quitGame;        // 0x10 (RE::Main::quitGame)
    bool resetGame;       // 0x11
    bool fullReset;       // 0x12
    bool gameActive;      // 0x13
    bool onIdle;          // 0x14
    bool reloadContent;   // 0x15
    bool freezeTime;      // 0x16 (RE::Main::freezeTime)
    bool freezeNextFrame; // 0x17
};
static_assert(offsetof(Main, quitGame) == 0x10);
static_assert(offsetof(Main, freezeTime) == 0x16);

struct VMContext
{
    char pad[0x680];
    uint8_t inactive; // 0x680
};

namespace
{
// Lock-free handoff from Skyrim's BSJobs VM job (`SE 53115`) to `WinMain` (`SE 35565`).
// `HookVMUpdate` NEVER touches `World`, `entt::registry`, `Actor`, `NiNode`, or `DefaultHeap`
// on worker threads; all client ECS/network/world updates execute strictly on `WinMain`
// at the top of `HookMainLoop` BEFORE `RealMainLoop` dispatches BSJobs or `PurgeExteriorCells`.
std::atomic<bool> s_vmActiveSinceLastMainLoop{false};

bool IsMainOrLoadingMenuOpen() noexcept
{
    auto* const pUi = UI::Get();
    if (!pUi)
        return true;

    static const BSFixedString* s_pMainMenu = new BSFixedString("Main Menu");
    static const BSFixedString* s_pLoadingMenu = new BSFixedString("Loading Menu");
    return pUi->GetMenuOpen(*s_pMainMenu) || pUi->GetMenuOpen(*s_pLoadingMenu);
}
} // namespace

TP_THIS_FUNCTION(TVMUpdate, int, VMContext, float);
TP_THIS_FUNCTION(TMainLoop, short, Main);
TP_THIS_FUNCTION(TVMDestructor, uintptr_t, void);

static TVMUpdate* VMUpdate = nullptr;
static TMainLoop* MainLoop = nullptr;
static TVMDestructor* VMDestructor = nullptr;

int TP_MAKE_THISCALL(HookVMUpdate, VMContext, float a2)
{
    if (apThis && apThis->inactive == 0)
        s_vmActiveSinceLastMainLoop.store(true, std::memory_order_release);

    return TiltedPhoques::ThisCall(VMUpdate, apThis, a2);
}

short TP_MAKE_THISCALL(HookMainLoop, Main)
{
    const bool vmWasActive = s_vmActiveSinceLastMainLoop.exchange(false, std::memory_order_acq_rel);

    if (apThis && !apThis->quitGame && World::IsCreated())
    {
        World::Get().OnMainThreadUpdate();

        if (g_appInstance)
        {
            PlayerCharacter* const pPlayer = PlayerCharacter::Get();
            const bool hasLoadedPlayerWorld =
                pPlayer && (pPlayer->parentCell != nullptr || pPlayer->GetWorldSpace() != nullptr);
            const auto updateMode = CoopWorldStabilizer::SelectMainLoopUpdateMode(
                apThis->quitGame,
                apThis->resetGame,
                apThis->fullReset,
                apThis->reloadContent,
                IsMainOrLoadingMenuOpen(),
                !apThis->freezeTime,
                vmWasActive,
                hasLoadedPlayerWorld);

            if (updateMode == CoopWorldStabilizer::MainLoopUpdateMode::kFullSimulationUpdate)
            {
                g_appInstance->Update();
                World::Get().OnMainThreadUpdate();
            }
            else if (updateMode == CoopWorldStabilizer::MainLoopUpdateMode::kPausedNetworkAndSavePump)
            {
                World::Get().UpdatePausedPump();
                World::Get().OnMainThreadUpdate();
            }
        }
    }

    return TiltedPhoques::ThisCall(MainLoop, apThis);
}

uintptr_t TP_MAKE_THISCALL(HookVMDestructor, void)
{
    TP_EMPTY_HOOK_PLACEHOLDER

    return TiltedPhoques::ThisCall(VMDestructor, apThis);
}

static TiltedPhoques::Initializer s_mainHooks(
    []()
    {
        POINTER_SKYRIMSE(TMainLoop, cMainLoop, 35565);
        POINTER_SKYRIMSE(TVMUpdate, cVMUpdate, 53115);
        POINTER_SKYRIMSE(TVMDestructor, cVMDestructor, 39341);

        VMUpdate = cVMUpdate.Get();
        MainLoop = cMainLoop.Get();
        VMDestructor = cVMDestructor.Get();

        TP_HOOK(&VMUpdate, HookVMUpdate);
        TP_HOOK(&MainLoop, HookMainLoop);
        TP_HOOK(&VMDestructor, HookVMDestructor);
    });

