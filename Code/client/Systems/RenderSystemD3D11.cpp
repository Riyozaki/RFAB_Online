#include <TiltedOnlinePCH.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/DebugService.h>
#include <Services/OverlayService.h>
#include <Services/ImguiService.h>

#include <d3d11.h>
#include <dxgi.h>
#include <Windows.h>

namespace
{
using TSwapChainResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain* apSwapChain,
    UINT aBufferCount,
    UINT aWidth,
    UINT aHeight,
    DXGI_FORMAT aNewFormat,
    UINT aSwapChainFlags);

RenderSystemD3D11* s_pActiveRenderSystem = nullptr;
TSwapChainResizeBuffers s_realSwapChainResizeBuffers = nullptr;

HRESULT STDMETHODCALLTYPE Hook_SwapChain_ResizeBuffers(
    IDXGISwapChain* apSwapChain,
    UINT aBufferCount,
    UINT aWidth,
    UINT aHeight,
    DXGI_FORMAT aNewFormat,
    UINT aSwapChainFlags)
{
    // Release CEF & ImGui D3D11 device/render-target objects BEFORE IDXGISwapChain::ResizeBuffers runs
    // so resolution changes, Alt-Tab, and display-adapter reconfigurations never fail with DXGI_ERROR_INVALID_CALL.
    if (s_pActiveRenderSystem && apSwapChain && apSwapChain == s_pActiveRenderSystem->GetSwapChain())
        s_pActiveRenderSystem->OnReset(apSwapChain);

    return s_realSwapChainResizeBuffers(apSwapChain, aBufferCount, aWidth, aHeight, aNewFormat, aSwapChainFlags);
}

void InstallSwapChainResizeHook(IDXGISwapChain* apSwapChain) noexcept
{
    if (!apSwapChain)
        return;

    void** pVTable = *reinterpret_cast<void***>(apSwapChain);
    if (!pVTable)
        return;

    // IDXGISwapChain::ResizeBuffers is VTable slot 13.
    constexpr size_t kResizeBuffersSlot = 13;
    if (pVTable[kResizeBuffersSlot] == reinterpret_cast<void*>(&Hook_SwapChain_ResizeBuffers))
        return;

    DWORD oldProtect = 0;
    if (VirtualProtect(&pVTable[kResizeBuffersSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        s_realSwapChainResizeBuffers = reinterpret_cast<TSwapChainResizeBuffers>(pVTable[kResizeBuffersSlot]);
        pVTable[kResizeBuffersSlot] = reinterpret_cast<void*>(&Hook_SwapChain_ResizeBuffers);
        DWORD ignored = 0;
        VirtualProtect(&pVTable[kResizeBuffersSlot], sizeof(void*), oldProtect, &ignored);
    }
}
} // namespace

RenderSystemD3D11::RenderSystemD3D11(OverlayService& aOverlay, ImguiService& aImguiService)
    : m_pSwapChain(nullptr)
    , m_pDevice(nullptr)
    , m_pDeviceContext(nullptr)
    , m_overlay(aOverlay)
    , m_imguiService(aImguiService)
{
}

RenderSystemD3D11::~RenderSystemD3D11()
{
    if (s_pActiveRenderSystem == this)
        s_pActiveRenderSystem = nullptr;
}

HWND RenderSystemD3D11::GetWindow() const
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.OutputWindow = nullptr;

    if (m_pSwapChain)
        m_pSwapChain->GetDesc(&desc);

    return desc.OutputWindow;
}

void RenderSystemD3D11::OnDeviceCreation(IDXGISwapChain* apSwapChain, ID3D11Device* apDevice, ID3D11DeviceContext* apContext)
{
    m_pSwapChain = apSwapChain;
    m_pDevice = apDevice;
    m_pDeviceContext = apContext;
    s_pActiveRenderSystem = this;

    InstallSwapChainResizeHook(apSwapChain);

    m_imguiService.Create(this, GetWindow());
    m_overlay.Create(this);
    DebugService::ArrangeGameWindows(GetWindow());
}

void RenderSystemD3D11::OnRender()
{
    m_imguiService.Render();
    m_overlay.Render();
}

void RenderSystemD3D11::OnReset(IDXGISwapChain* apSwapChain)
{
    m_pSwapChain = apSwapChain;

    m_overlay.Reset();
    m_imguiService.Reset();
}
