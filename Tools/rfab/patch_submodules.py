#!/usr/bin/env python3
"""
Applies RFAB Online Skyrim SE 1.5.97 & stability fixes onto upstream submodules
(TiltedReverse, TiltedHooks, and TiltedUI) immediately after
`git submodule update --init --force --recursive`:

  1. TiltedReverse FunctionHook.cpp (`GetImportedFunction`):
     - Skyrim SE 1.5.97 imports `DirectInput8Create` from `DINPUT8.dll` by ORDINAL (#1),
       not by ASCII name, causing `GetImportedFunction` to return `nullptr`!
     - Guards `OriginalFirstThunk == 0` (falls back to `FirstThunk`).
     - Resolves `GetProcAddress(hLib, acpMethod)` and matches both by name AND by resolved
       IAT slot address when `IMAGE_SNAP_BY_ORDINAL(pOrdinal)` is set.

  2. TiltedHooks DInputHook.cpp:
     - Null-guards `outData` / `outDataLen` in `GetDeviceState` and `GetDeviceData`.
     - Guards `pDirectInput != nullptr` and `out != nullptr` in `HookDirectInput8Create`.
     - Falls back cleanly to `TP_HOOK_IMMEDIATE` when proxy `dinput8.dll` redirects IAT.

  3. TiltedHooks D3D11Hook.cpp & WindowsHook.cpp:
     - Checks `SUCCEEDED(result) && ppSwapChain && *ppSwapChain` before `HookVTable(*ppSwapChain, ...)`.
     - Guards `RealWndProc` against null/duplicate overwrite by secondary window classes.

  4. TiltedUI OverlayApp.cpp, OverlayClient.cpp & OverlayRenderHandlerD3D11.cpp:
     - Uses `RFAB_TILTED_CLIENT_DIR` for real CEF UI & cursor paths.
     - Checks `pSrcBuffer != nullptr` and `desc.Width/Height > 0` in `GetRenderTargetSize()`.
     - Checks `m_pTexture != nullptr` before `m_pContext->Map(...)` and copies row-by-row
       respecting `mappedResource.RowPitch` (fixing non-64px-aligned resolutions like 1366x768 / 1600x900).
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]

# 1. TiltedReverse FunctionHook.cpp
fh_path = ROOT / "Libraries" / "TiltedReverse" / "Code" / "reverse" / "src" / "FunctionHook.cpp"
fh_txt = fh_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_func = """    void** GetImportedFunction(const wchar_t *acpModuleName, const char* acpLibraryName, const char* acpMethod) noexcept
    {
        const auto pBase = GetModuleHandleW(acpModuleName);

        const auto pImageDosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(pBase);
        auto pImageNtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(RtlOffsetToPointer(pBase, pImageDosHeader->e_lfanew));

        const auto pVirtualAddress = pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;

        for (auto pImageImportDescriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(RtlOffsetToPointer(pBase, pVirtualAddress)); pImageImportDescriptor->Name; ++pImageImportDescriptor)
        {
            const auto pLibraryName = reinterpret_cast<const char*>RtlOffsetToPointer(pBase, pImageImportDescriptor->Name);

            if (!_stricmp(pLibraryName, acpLibraryName))
            {
                auto pImportAddressTable = reinterpret_cast<uintptr_t*>(RtlOffsetToPointer(pBase, pImageImportDescriptor->FirstThunk));
                auto pImageThunkData = reinterpret_cast<IMAGE_THUNK_DATA*>(RtlOffsetToPointer(pBase, pImageImportDescriptor->OriginalFirstThunk));

                while (const auto pOrdinal = pImageThunkData->u1.Ordinal)
                {
                    const auto pFunctionName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(RtlOffsetToPointer(pBase, pImageThunkData->u1.AddressOfData))->Name;

                    if (IMAGE_SNAP_BY_ORDINAL(pOrdinal))
                    {
                        // Skip ordinal functions, we just want named functions
                    }
                    else if (!_stricmp(pFunctionName, acpMethod))
                    {
                        return reinterpret_cast<void**>(pImportAddressTable);
                    }

                    ++pImageThunkData;
                    ++pImportAddressTable;
                }

                return nullptr;
            }
        }

        return nullptr;
    }"""

new_func = """    void** GetImportedFunction(const wchar_t *acpModuleName, const char* acpLibraryName, const char* acpMethod) noexcept
    {
        const auto pBase = GetModuleHandleW(acpModuleName);
        if (!pBase || !acpLibraryName || !acpMethod)
            return nullptr;

        const auto pImageDosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(pBase);
        auto pImageNtHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(RtlOffsetToPointer(pBase, pImageDosHeader->e_lfanew));

        const auto pVirtualAddress = pImageNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!pVirtualAddress)
            return nullptr;

        void* pTargetProc = nullptr;
        if (HMODULE hLib = GetModuleHandleA(acpLibraryName))
            pTargetProc = reinterpret_cast<void*>(GetProcAddress(hLib, acpMethod));

        for (auto pImageImportDescriptor = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(RtlOffsetToPointer(pBase, pVirtualAddress)); pImageImportDescriptor->Name; ++pImageImportDescriptor)
        {
            const auto pLibraryName = reinterpret_cast<const char*>(RtlOffsetToPointer(pBase, pImageImportDescriptor->Name));

            if (!_stricmp(pLibraryName, acpLibraryName))
            {
                const auto thunkRva = pImageImportDescriptor->OriginalFirstThunk
                                          ? pImageImportDescriptor->OriginalFirstThunk
                                          : pImageImportDescriptor->FirstThunk;
                auto pImportAddressTable = reinterpret_cast<uintptr_t*>(RtlOffsetToPointer(pBase, pImageImportDescriptor->FirstThunk));
                auto pImageThunkData = reinterpret_cast<IMAGE_THUNK_DATA*>(RtlOffsetToPointer(pBase, thunkRva));

                while (const auto pOrdinal = pImageThunkData->u1.Ordinal)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(pOrdinal))
                    {
                        // Skyrim SE 1.5.97 imports DirectInput8Create from DINPUT8.dll by ordinal (#1)!
                        if (pTargetProc != nullptr && reinterpret_cast<void*>(*pImportAddressTable) == pTargetProc)
                        {
                            return reinterpret_cast<void**>(pImportAddressTable);
                        }
                    }
                    else
                    {
                        const auto pFunctionName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(RtlOffsetToPointer(pBase, pImageThunkData->u1.AddressOfData))->Name;
                        if (!_stricmp(pFunctionName, acpMethod))
                        {
                            return reinterpret_cast<void**>(pImportAddressTable);
                        }
                    }

                    ++pImageThunkData;
                    ++pImportAddressTable;
                }

                return nullptr;
            }
        }

        return nullptr;
    }"""

if "pTargetProc" not in fh_txt:
    assert old_func in fh_txt, "Failed to match GetImportedFunction in FunctionHook.cpp!"
    fh_path.write_text(fh_txt.replace(old_func, new_func, 1), encoding="utf-8")
    print("[patch_submodules] Patched TiltedReverse FunctionHook.cpp (ordinal IAT support + null guards).")

# 2. TiltedHooks DInputHook.cpp
di_path = ROOT / "Libraries" / "TiltedHooks" / "Code" / "hooks" / "src" / "DInputHook.cpp"
di_txt = di_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_di_methods = """    HRESULT _stdcall StubIDirectInputDevice8A::GetDeviceState(DWORD outDataLen, LPVOID outData)
    {
        if (DInputHook::Get().IsEnabled())
        {
            std::memset(outData, 0, outDataLen);
            return 0;
        }

        return IDirectInputDevice8_GetDeviceState(m_pDevice, outDataLen, outData);
    }

    HRESULT _stdcall StubIDirectInputDevice8A::GetDeviceData(DWORD dataSize, LPDIDEVICEOBJECTDATA outData, LPDWORD outDataLen, DWORD flags)
    {
        auto& input = DInputHook::Get();

        const auto result = IDirectInputDevice8_GetDeviceData(m_pDevice, dataSize, outData, outDataLen, flags);

        DIDEVICEINSTANCEA instanceInfo;
        instanceInfo.dwSize = sizeof(instanceInfo);
        if (IDirectInputDevice8_GetDeviceInfo(m_pDevice, &instanceInfo) != DI_OK)
        {
            return result;
        }

        if (input.IsEnabled())
        {
            *outDataLen = 0;

            return result;
        }

        if (instanceInfo.guidInstance == GUID_SysKeyboard)
        {
            for (DWORD i = 0; i < *outDataLen; ++i)"""

new_di_methods = """    HRESULT _stdcall StubIDirectInputDevice8A::GetDeviceState(DWORD outDataLen, LPVOID outData)
    {
        if (DInputHook::Get().IsEnabled())
        {
            if (outData && outDataLen > 0)
                std::memset(outData, 0, outDataLen);
            return 0;
        }

        return IDirectInputDevice8_GetDeviceState(m_pDevice, outDataLen, outData);
    }

    HRESULT _stdcall StubIDirectInputDevice8A::GetDeviceData(DWORD dataSize, LPDIDEVICEOBJECTDATA outData, LPDWORD outDataLen, DWORD flags)
    {
        auto& input = DInputHook::Get();

        const auto result = IDirectInputDevice8_GetDeviceData(m_pDevice, dataSize, outData, outDataLen, flags);
        if (!outDataLen)
            return result;

        DIDEVICEINSTANCEA instanceInfo;
        instanceInfo.dwSize = sizeof(instanceInfo);
        if (IDirectInputDevice8_GetDeviceInfo(m_pDevice, &instanceInfo) != DI_OK)
        {
            return result;
        }

        if (input.IsEnabled())
        {
            *outDataLen = 0;

            return result;
        }

        if (SUCCEEDED(result) && outData != nullptr && instanceInfo.guidInstance == GUID_SysKeyboard)
        {
            for (DWORD i = 0; i < *outDataLen; ++i)"""

if "SUCCEEDED(result) && outData != nullptr" not in di_txt:
    assert old_di_methods in di_txt, "Failed to match GetDeviceState/GetDeviceData in DInputHook.cpp!"
    di_txt = di_txt.replace(old_di_methods, new_di_methods, 1)

old_di_create = """    static HRESULT _stdcall HookDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
    {
        EnforceOriginalFunctionPrologue();

        IDirectInput8A* pDirectInput = nullptr;
        const auto result = RealDirectInput8Create(instance, version, iid, reinterpret_cast<LPVOID*>(&pDirectInput), outer);

        *out = static_cast<LPVOID>(pDirectInput);

        if (result == DI_OK && RealIDirectInputA_CreateDevice == nullptr)
        {
            RealIDirectInputA_CreateDevice = pDirectInput->lpVtbl->CreateDevice;
            TP_HOOK_IMMEDIATE(&RealIDirectInputA_CreateDevice, HookIDirectInputA_CreateDevice);
        }

        return result;
    }"""

new_di_create = """    static HRESULT _stdcall HookDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
    {
        EnforceOriginalFunctionPrologue();

        IDirectInput8A* pDirectInput = nullptr;
        const auto result = RealDirectInput8Create(instance, version, iid, reinterpret_cast<LPVOID*>(&pDirectInput), outer);

        if (out)
            *out = static_cast<LPVOID>(pDirectInput);

        if (result == DI_OK && pDirectInput != nullptr && pDirectInput->lpVtbl != nullptr && RealIDirectInputA_CreateDevice == nullptr)
        {
            RealIDirectInputA_CreateDevice = pDirectInput->lpVtbl->CreateDevice;
            TP_HOOK_IMMEDIATE(&RealIDirectInputA_CreateDevice, HookIDirectInputA_CreateDevice);
        }

        return result;
    }"""

if "pDirectInput != nullptr && pDirectInput->lpVtbl != nullptr" not in di_txt:
    assert old_di_create in di_txt, "Failed to match HookDirectInput8Create in DInputHook.cpp!"
    di_txt = di_txt.replace(old_di_create, new_di_create, 1)

old_di_install = """    void DInputHook::Install() noexcept
    {
        TP_HOOK_IAT(DirectInput8Create, "dinput8.dll");
        // Store original prologue to restore later
        std::memcpy(s_realDirectInput8CreatePrologue.data(), GetOriginalFunctionPrologue(), kDirectInput8CreatePrologueSize);
    }"""

new_di_install = """    void DInputHook::Install() noexcept
    {
        TP_HOOK_IAT(DirectInput8Create, "dinput8.dll");
        if (RealDirectInput8Create != nullptr)
        {
            // Store original prologue to restore later when IAT hook was used
            std::memcpy(s_realDirectInput8CreatePrologue.data(), GetOriginalFunctionPrologue(), kDirectInput8CreatePrologueSize);
        }
        else if (HMODULE hDi = LoadLibraryA("dinput8.dll"))
        {
            RealDirectInput8Create = reinterpret_cast<TDirectInput8Create>(GetProcAddress(hDi, "DirectInput8Create"));
            if (RealDirectInput8Create)
                TP_HOOK_IMMEDIATE(&RealDirectInput8Create, HookDirectInput8Create);
        }
    }"""

if 'LoadLibraryA("dinput8.dll")' not in di_txt:
    assert old_di_install in di_txt, "Failed to match DInputHook::Install in DInputHook.cpp!"
    di_txt = di_txt.replace(old_di_install, new_di_install, 1)

di_path.write_text(di_txt, encoding="utf-8")
print("[patch_submodules] Patched TiltedHooks DInputHook.cpp (DirectInput8Create fallback + buffer/pointer guards).")

# 3. TiltedHooks D3D11Hook.cpp
d3d_path = ROOT / "Libraries" / "TiltedHooks" / "Code" / "hooks" / "src" / "D3D11Hook.cpp"
d3d_txt = d3d_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_d3d = """        if (RealDXGISwapChainPresent == nullptr && ppSwapChain)
            RealDXGISwapChainPresent = HookVTable(*ppSwapChain, 8, &HookDXGISwapChainPresent);"""

new_d3d = """        if (SUCCEEDED(result) && RealDXGISwapChainPresent == nullptr && ppSwapChain && *ppSwapChain)
            RealDXGISwapChainPresent = HookVTable(*ppSwapChain, 8, &HookDXGISwapChainPresent);"""

if "SUCCEEDED(result) && RealDXGISwapChainPresent == nullptr" not in d3d_txt:
    assert old_d3d in d3d_txt, "Failed to match HookD3D11CreateDeviceAndSwapChain in D3D11Hook.cpp!"
    d3d_path.write_text(d3d_txt.replace(old_d3d, new_d3d, 1), encoding="utf-8")
    print("[patch_submodules] Patched TiltedHooks D3D11Hook.cpp (SUCCEEDED && *ppSwapChain null guard).")

# 4. TiltedHooks WindowsHook.cpp
win_path = ROOT / "Libraries" / "TiltedHooks" / "Code" / "hooks" / "src" / "WindowsHook.cpp"
win_txt = win_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_win = """    ATOM __stdcall HookRegisterClassA(WNDCLASSA* lpWndClass)
    {
        RealWndProc = lpWndClass->lpfnWndProc;
        lpWndClass->lpfnWndProc = HookWndProc;

        return RealRegisterClassA(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassW(WNDCLASSW* lpWndClass)
    {
        RealWndProc = lpWndClass->lpfnWndProc;
        lpWndClass->lpfnWndProc = HookWndProc;

        return RealRegisterClassW(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassExA(WNDCLASSEXA* lpWndClass)
    {
        RealWndProc = lpWndClass->lpfnWndProc;
        lpWndClass->lpfnWndProc = HookWndProc;

        return RealRegisterClassExA(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassExW(WNDCLASSEXW* lpWndClass)
    {
        RealWndProc = lpWndClass->lpfnWndProc;
        lpWndClass->lpfnWndProc = HookWndProc;

        return RealRegisterClassExW(lpWndClass);
    }"""

new_win = """    ATOM __stdcall HookRegisterClassA(WNDCLASSA* lpWndClass)
    {
        if (lpWndClass && RealWndProc == nullptr && lpWndClass->lpfnWndProc && lpWndClass->lpfnWndProc != &HookWndProc)
        {
            RealWndProc = lpWndClass->lpfnWndProc;
            lpWndClass->lpfnWndProc = HookWndProc;
        }

        return RealRegisterClassA(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassW(WNDCLASSW* lpWndClass)
    {
        if (lpWndClass && RealWndProc == nullptr && lpWndClass->lpfnWndProc && lpWndClass->lpfnWndProc != &HookWndProc)
        {
            RealWndProc = lpWndClass->lpfnWndProc;
            lpWndClass->lpfnWndProc = HookWndProc;
        }

        return RealRegisterClassW(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassExA(WNDCLASSEXA* lpWndClass)
    {
        if (lpWndClass && RealWndProc == nullptr && lpWndClass->lpfnWndProc && lpWndClass->lpfnWndProc != &HookWndProc)
        {
            RealWndProc = lpWndClass->lpfnWndProc;
            lpWndClass->lpfnWndProc = HookWndProc;
        }

        return RealRegisterClassExA(lpWndClass);
    }

    ATOM __stdcall HookRegisterClassExW(WNDCLASSEXW* lpWndClass)
    {
        if (lpWndClass && RealWndProc == nullptr && lpWndClass->lpfnWndProc && lpWndClass->lpfnWndProc != &HookWndProc)
        {
            RealWndProc = lpWndClass->lpfnWndProc;
            lpWndClass->lpfnWndProc = HookWndProc;
        }

        return RealRegisterClassExW(lpWndClass);
    }"""

if "lpWndClass->lpfnWndProc != &HookWndProc" not in win_txt:
    assert old_win in win_txt, "Failed to match HookRegisterClass* in WindowsHook.cpp!"
    win_path.write_text(win_txt.replace(old_win, new_win, 1), encoding="utf-8")
    print("[patch_submodules] Patched TiltedHooks WindowsHook.cpp (RealWndProc overwrite guard).")

# 5. TiltedUI OverlayApp.cpp & OverlayClient.cpp
ui_win_inc = """#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
"""

ui_replacement = """        wchar_t clientDirEnv[4096]{};
        const DWORD envLen = GetEnvironmentVariableW(L"RFAB_TILTED_CLIENT_DIR", clientDirEnv, 4096);
        const auto currentPath = (envLen > 0 && envLen < 4096)
                                     ? std::filesystem::path(clientDirEnv)
                                     : TiltedPhoques::GetPath();"""

for ui_rel in (
    ("Libraries", "TiltedUI", "Code", "ui", "src", "OverlayApp.cpp"),
    ("Libraries", "TiltedUI", "Code", "ui", "src", "OverlayClient.cpp"),
):
    ui_file = ROOT.joinpath(*ui_rel)
    raw_ui = ui_file.read_text(encoding="utf-8").replace("\r\n", "\n")
    needle_ui = "        const auto currentPath = TiltedPhoques::GetPath();"
    if "RFAB_TILTED_CLIENT_DIR" not in raw_ui:
        assert needle_ui in raw_ui, f"Failed to match GetPath() in {ui_file}!"
        patched_ui = ui_win_inc + raw_ui.replace(needle_ui, ui_replacement, 1)
        ui_file.write_text(patched_ui, encoding="utf-8")
        print(f"[patch_submodules] Patched {ui_file.name} (RFAB_TILTED_CLIENT_DIR real path for CEF UI & cursors).")

# 6. TiltedUI OverlayRenderHandlerD3D11.cpp
orh_path = ROOT / "Libraries" / "TiltedUI" / "Code" / "ui",
orh_path = ROOT / "Libraries" / "TiltedUI" / "Code" / "ui" / "src" / "OverlayRenderHandlerD3D11.cpp"
orh_txt = orh_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_orh = """    void OverlayRenderHandlerD3D11::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirtyRects, const void* buffer, int width, int height)
    {
        if (type == PET_VIEW && m_width == width && m_height == height)
        {
            std::unique_lock _(m_textureLock);

            if (!m_pTexture)
                CreateRenderTexture();

            // OnPaint may be called before context initialization
            if (!m_pContext)
                return;

            D3D11_MAPPED_SUBRESOURCE mappedResource;
            const auto result = m_pContext->Map(m_pTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);

            if(SUCCEEDED(result))
            {
                const auto pDest = static_cast<uint8_t*>(mappedResource.pData);
                std::memcpy(pDest, buffer, width * height * 4);
                m_pContext->Unmap(m_pTexture.Get(), 0);
            }
            else
            {
                // We got no mapping, let's drop the context and reset the texture so that we attempt to create a new one during the next frame
                m_pContext.Reset();
                m_pTexture.Reset();
                m_width = m_height = 0;
            }
        }
    }

    void OverlayRenderHandlerD3D11::GetRenderTargetSize()
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pRenderTargetView;

        m_pImmediateContext->OMGetRenderTargets(1, pRenderTargetView.ReleaseAndGetAddressOf(), nullptr);
        if (pRenderTargetView)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> pSrcResource;
            pRenderTargetView->GetResource(pSrcResource.ReleaseAndGetAddressOf());

            if (pSrcResource)
            {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pSrcBuffer;
                pSrcResource.As(&pSrcBuffer);

                D3D11_TEXTURE2D_DESC desc;
                pSrcBuffer->GetDesc(&desc);

                if ((m_width != desc.Width || m_height != desc.Height) && m_pParent)"""

new_orh = """    void OverlayRenderHandlerD3D11::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirtyRects, const void* buffer, int width, int height)
    {
        if (type == PET_VIEW && buffer != nullptr && width > 0 && height > 0 && m_width == static_cast<uint32_t>(width) && m_height == static_cast<uint32_t>(height))
        {
            std::unique_lock _(m_textureLock);

            if (!m_pTexture)
                CreateRenderTexture();

            // OnPaint may be called before context initialization or when CreateRenderTexture fails
            if (!m_pContext || !m_pTexture)
                return;

            D3D11_MAPPED_SUBRESOURCE mappedResource{};
            const auto result = m_pContext->Map(m_pTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);

            if (SUCCEEDED(result) && mappedResource.pData != nullptr)
            {
                auto* pDest = static_cast<uint8_t*>(mappedResource.pData);
                const auto* pSrc = static_cast<const uint8_t*>(buffer);
                const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
                if (mappedResource.RowPitch == srcRowBytes)
                {
                    std::memcpy(pDest, pSrc, srcRowBytes * static_cast<size_t>(height));
                }
                else if (mappedResource.RowPitch >= srcRowBytes)
                {
                    for (int row = 0; row < height; ++row)
                        std::memcpy(pDest + static_cast<size_t>(row) * mappedResource.RowPitch, pSrc + static_cast<size_t>(row) * srcRowBytes, srcRowBytes);
                }
                m_pContext->Unmap(m_pTexture.Get(), 0);
            }
            else
            {
                // We got no mapping, let's drop the context and reset the texture so that we attempt to create a new one during the next frame
                m_pContext.Reset();
                m_pTexture.Reset();
                m_width = m_height = 0;
            }
        }
    }

    void OverlayRenderHandlerD3D11::GetRenderTargetSize()
    {
        if (!m_pImmediateContext)
            return;

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pRenderTargetView;

        m_pImmediateContext->OMGetRenderTargets(1, pRenderTargetView.ReleaseAndGetAddressOf(), nullptr);
        if (pRenderTargetView)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> pSrcResource;
            pRenderTargetView->GetResource(pSrcResource.ReleaseAndGetAddressOf());

            if (pSrcResource)
            {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pSrcBuffer;
                if (FAILED(pSrcResource.As(&pSrcBuffer)) || !pSrcBuffer)
                    return;

                D3D11_TEXTURE2D_DESC desc{};
                pSrcBuffer->GetDesc(&desc);
                if (desc.Width == 0 || desc.Height == 0)
                    return;

                if ((m_width != desc.Width || m_height != desc.Height) && m_pParent)"""

old_orh_create = """    void OverlayRenderHandlerD3D11::CreateRenderTexture()
    {
        D3D11_TEXTURE2D_DESC textDesc;"""

new_orh_create = """    void OverlayRenderHandlerD3D11::CreateRenderTexture()
    {
        if (!m_pDevice || m_width == 0 || m_height == 0)
            return;

        D3D11_TEXTURE2D_DESC textDesc{};"""

if "mappedResource.RowPitch == srcRowBytes" not in orh_txt:
    assert old_orh in orh_txt, "Failed to match OnPaint/GetRenderTargetSize in OverlayRenderHandlerD3D11.cpp!"
    assert old_orh_create in orh_txt, "Failed to match CreateRenderTexture in OverlayRenderHandlerD3D11.cpp!"
    orh_txt = orh_txt.replace(old_orh, new_orh, 1).replace(old_orh_create, new_orh_create, 1)
    orh_path.write_text(orh_txt, encoding="utf-8")
    print("[patch_submodules] Patched TiltedUI OverlayRenderHandlerD3D11.cpp (RowPitch copy + pSrcBuffer/m_pTexture null guards).")

# 7. TiltedReverse JitAssembly.hpp
jit_path = ROOT / "Libraries" / "TiltedReverse" / "Code" / "reverse" / "include" / "JitAssembly.hpp"
jit_txt = jit_path.read_text(encoding="utf-8")
if "kDefaultAllocStubsize = 64;" in jit_txt:
    jit_path.write_text(
        jit_txt.replace("kDefaultAllocStubsize = 64;", "kDefaultAllocStubsize = 512;", 1),
        encoding="utf-8",
    )
    print("[patch_submodules] Patched TiltedReverse JitAssembly.hpp (kDefaultAllocStubsize 64 -> 512 bytes).")

# 8. TiltedConnect Client.cpp (prevent stale lingering GNS callback from killing post-LoadGame reconnect!)
tc_path = ROOT / "Libraries" / "TiltedConnect" / "Code" / "connect" / "src" / "Client.cpp"
tc_txt = tc_path.read_text(encoding="utf-8").replace("\r\n", "\n")

old_tc_connect = """    bool Client::Connect(const SteamNetworkingIPAddr& acEndpoint) noexcept
    {
        SteamNetworkingConfigValue_t opt = {};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        m_connection = m_pInterface->ConnectByIPAddress(acEndpoint, 1, &opt);

        return m_connection != k_HSteamNetConnection_Invalid;
    }"""

new_tc_connect = """    bool Client::Connect(const SteamNetworkingIPAddr& acEndpoint) noexcept
    {
        if (m_connection != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;
        }

        SteamNetworkingConfigValue_t opt = {};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        m_connection = m_pInterface->ConnectByIPAddress(acEndpoint, 1, &opt);

        return m_connection != k_HSteamNetConnection_Invalid;
    }"""

old_tc_status = """        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;"""

new_tc_status = """        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            if (apInfo->m_hConn != m_connection)
            {
                m_pInterface->CloseConnection(apInfo->m_hConn, 0, nullptr, false);
                break;
            }

            m_pInterface->CloseConnection(m_connection, 0, nullptr, false);
            m_connection = k_HSteamNetConnection_Invalid;"""

if "apInfo->m_hConn != m_connection" not in tc_txt:
    assert old_tc_connect in tc_txt, "Failed to match Client::Connect in TiltedConnect Client.cpp!"
    assert old_tc_status in tc_txt, "Failed to match OnSteamNetConnectionStatusChanged in TiltedConnect Client.cpp!"
    tc_txt = tc_txt.replace(old_tc_connect, new_tc_connect, 1).replace(old_tc_status, new_tc_status, 1)
    tc_path.write_text(tc_txt, encoding="utf-8")
    print("[patch_submodules] Patched TiltedConnect Client.cpp (guarded stale m_hConn callback across reconnects).")

