
#include "Services/InputService.h"
#include "Systems/RenderSystemD3D11.h"

#include "World.h"

#include "BSGraphics/BSGraphicsRenderer.h"
#include "BSRandom/BSRandom.h"

#include <cwchar>

// shared resource by launcher
extern HICON g_SharedWindowIcon;

namespace BSGraphics
{
namespace
{

static RenderSystemD3D11* g_sRs = nullptr;
static WNDPROC RealWndProc = nullptr;
static RendererWindow* g_RenderWindow = nullptr;

static constexpr char kTogetherWindowName[]{"Skyrim Together"};

} // namespace
RendererWindow* GetMainWindow()
{
    return g_RenderWindow;
}

bool RendererWindow::IsForeground()
{
    return GetForegroundWindow() == hWnd;
}

void (*Renderer_Init)(Renderer*, BSGraphics::RendererInitOSData*, const BSGraphics::ApplicationWindowProperties*, BSGraphics::RendererInitReturn*) = nullptr;

// WNDPROC seems to be part of the renderer
LRESULT CALLBACK Hook_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (InputService::WndProc(hwnd, uMsg, wParam, lParam) != 0)
        return 0;

    return RealWndProc(hwnd, uMsg, wParam, lParam);
}

void Hook_Renderer_Init(Renderer* self, BSGraphics::RendererInitOSData* aOSData, const BSGraphics::ApplicationWindowProperties* aFBData, BSGraphics::RendererInitReturn* aOut)
{
    // we feed this a shared icon as the resource directory of our former launcher data is already overwritten with the
    // game.
    aOSData->hIcon = g_SharedWindowIcon;
    // Append our window name.
    aOSData->pClassName = kTogetherWindowName;

    RealWndProc = aOSData->pWndProc;
    aOSData->pWndProc = Hook_WndProc;

    Renderer_Init(self, aOSData, aFBData, aOut);

    g_sRs = &World::Get().ctx().at<RenderSystemD3D11>();
    // This how the game does it too
    g_RenderWindow = &self->Data.RenderWindowA[0];

    const BSGraphics::RendererData& renderer = self->Data;

    g_sRs->OnDeviceCreation(renderer.RenderWindowA[0].pSwapChain, renderer.pForwarder, renderer.pContext);
}

static void (*Renderer_End)(void*, uint32_t) = nullptr;

// Draw the D3D11 overlay right before BSGraphics::Renderer::End presents the frame
static void Hook_Renderer_End(void* apSelf, uint32_t aTimer)
{
    if (g_sRs)
        g_sRs->OnRender();

    Renderer_End(apSelf, aTimer);
}

static TiltedPhoques::Initializer s_viewportHooks(
    []()
    {
        // Note: in Skyrim SE 1.5.97, BSGraphics::InitWindows (SE 75591 @ 0xD71F00) and
        // BSGraphics::Renderer::Init (SE 75595 @ 0xD72810) are separate functions.
        // Only override dwStyle when explicitly requested via +force_windowed so we do not
        // break SSEDisplayTweaks.dll Borderless Fullscreen window sizing & cursor hitboxes in RFAB.
        const wchar_t* pCmdLine = GetCommandLineW();
        if (pCmdLine && std::wcsstr(pCmdLine, L"+force_windowed"))
        {
            const VersionDbPtr<void> initWindowLoc(75591);
            TiltedPhoques::Put(mem::pointer(initWindowLoc.GetPtr()) + 0xA9 + 1, WS_OVERLAPPEDWINDOW);
        }

        // BSWin32KeyboardDevice::Initialize (SE 67471 @ 0xC1A060):
        // don't let the game steal media keys in windowed mode
        const VersionDbPtr<void> windowLoc(67471);
        TiltedPhoques::Put(
            mem::pointer(windowLoc.GetPtr()) + 0x55 + 2,
            /*strip DISCL_EXCLUSIVE bits and append DISCL_NONEXCLUSIVE*/ 3);

        // BSGraphics::Renderer::End (SE 75461 @ 0xD6A2B0) and BSGraphics::Renderer::Init (SE 75595 @ 0xD72810)
        const VersionDbPtr<void> renderEnd(75461);
        const VersionDbPtr<void> renderInit(75595);

        Renderer_End = static_cast<decltype(Renderer_End)>(renderEnd.GetPtr());
        Renderer_Init = static_cast<decltype(Renderer_Init)>(renderInit.GetPtr());

        TP_HOOK_IMMEDIATE(&Renderer_End, &Hook_Renderer_End);
        TP_HOOK_IMMEDIATE(&Renderer_Init, &Hook_Renderer_Init);
    });
} // namespace BSGraphics
