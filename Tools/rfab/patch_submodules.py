#!/usr/bin/env python3
"""
Applies RFAB Online Skyrim SE 1.5.97 fixes onto upstream submodules (TiltedReverse & TiltedHooks)
immediately after `git submodule update --init --force --recursive`:

  1. TiltedReverse FunctionHook.cpp (`GetImportedFunction`):
     - Skyrim SE 1.5.97 imports `DirectInput8Create` from `DINPUT8.dll` by ORDINAL (#1),
       not by ASCII name, causing `GetImportedFunction` to return `nullptr`!
     - Also guards `OriginalFirstThunk == 0` (falls back to `FirstThunk`).
     - Resolves `GetProcAddress(hLib, acpMethod)` and matches both by name AND by resolved
       IAT slot address when `IMAGE_SNAP_BY_ORDINAL(pOrdinal)` is set.

  2. TiltedHooks DInputHook.cpp (`DInputHook::Install`):
     - If `RealDirectInput8Create` is still null (e.g. proxy DLL redirection), falls back
       cleanly to `TP_HOOK_IMMEDIATE(&RealDirectInput8Create, HookDirectInput8Create)`.
     - Null-guards `GetOriginalFunctionPrologue()` before `std::memcpy(..., 16)` so it can
       NEVER dereference `0x0` (`0xC0000005`) at startup!
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]

fh_path = ROOT / "Libraries" / "TiltedReverse" / "Code" / "reverse" / "src" / "FunctionHook.cpp"
fh_txt = fh_path.read_text(encoding="utf-8")

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

di_path = ROOT / "Libraries" / "TiltedHooks" / "Code" / "hooks" / "src" / "DInputHook.cpp"
di_txt = di_path.read_text(encoding="utf-8")

old_di = """    void DInputHook::Install() noexcept
    {
        TP_HOOK_IAT(DirectInput8Create, "dinput8.dll");
        // Store original prologue to restore later
        std::memcpy(s_realDirectInput8CreatePrologue.data(), GetOriginalFunctionPrologue(), kDirectInput8CreatePrologueSize);
    }"""

new_di = """    void DInputHook::Install() noexcept
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

if "LoadLibraryA(\"dinput8.dll\")" not in di_txt:
    assert old_di in di_txt, "Failed to match DInputHook::Install in DInputHook.cpp!"
    di_path.write_text(di_txt.replace(old_di, new_di, 1), encoding="utf-8")
    print("[patch_submodules] Patched TiltedHooks DInputHook.cpp (DirectInput8Create fallback + null guard).")
