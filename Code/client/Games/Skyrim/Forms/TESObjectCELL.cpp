#include <TiltedOnlinePCH.h>

#include <Forms/TESObjectCELL.h>
#include <Games/Memory.h>
#include <TESObjectREFR.h>

#include <climits>
#include <cstring>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
TESWorldSpace* SafeGetWorldSpaceOfExteriorCell(const TESObjectCELL* apCell) noexcept
{
    if (!apCell)
        return nullptr;

    return (apCell->cellFlags[0] & 1u) != 0 ? nullptr : apCell->worldspace;
}

TiltedPhoques::Initializer s_cellWorldSpaceNullGuard(
    []()
    {
#ifdef _WIN32
        // Skyrim SE 1.5.97 ID 17859 (RVA 0x23D000): TESObjectCELL::GetWorldSpaceOfExteriorCell.
        // Background QueuedPromoteLargeReferencesTask (+0x23CD30..+0x23D070) can pass a null TESObjectCELL*
        // right after abrupt cross-hold teleports, crashing at RIP=0x14023D000. Verify exact SE 1.5.97 bytes
        // (`test byte ptr [rcx+0x40], 1; jnz +8; mov rax, [rcx+0x120]; ret`) and redirect cleanly via RipAllocateN
        // without relocating the short `jnz +8` instruction.
        using TGetWorldSpaceFn = TESWorldSpace*(__fastcall*)(const TESObjectCELL*);
        POINTER_SKYRIMSE(TGetWorldSpaceFn, s_getWorldSpaceOfExteriorCell, 17859);

        auto* pFn = reinterpret_cast<uint8_t*>(s_getWorldSpaceOfExteriorCell.Get());
        constexpr uint8_t kExpectedBytes[14] = {
            0xF6, 0x41, 0x40, 0x01,                         // test byte ptr [rcx+0x40], 1
            0x75, 0x08,                                     // jnz +8
            0x48, 0x8B, 0x81, 0x20, 0x01, 0x00, 0x00,       // mov rax, [rcx+0x120]
            0xC3                                            // ret
        };

        if (!pFn || std::memcmp(pFn, kExpectedBytes, sizeof(kExpectedBytes)) != 0)
            return;

        auto* pThunk = static_cast<uint8_t*>(::RipAllocateN(32));
        if (!pThunk)
            return;

        const uintptr_t nextRip = reinterpret_cast<uintptr_t>(pFn + 5);
        const intptr_t relDisp = reinterpret_cast<intptr_t>(pThunk) - static_cast<intptr_t>(nextRip);
        if (relDisp < INT32_MIN || relDisp > INT32_MAX)
            return;

        pThunk[0] = 0xFF;
        pThunk[1] = 0x25;
        pThunk[2] = 0x00;
        pThunk[3] = 0x00;
        pThunk[4] = 0x00;
        pThunk[5] = 0x00;
        const uint64_t detourAddr = reinterpret_cast<uint64_t>(&SafeGetWorldSpaceOfExteriorCell);
        std::memcpy(pThunk + 6, &detourAddr, sizeof(detourAddr));
        FlushInstructionCache(GetCurrentProcess(), pThunk, 14);

        DWORD oldProt = 0;
        if (!VirtualProtect(pFn, 6, PAGE_EXECUTE_READWRITE, &oldProt))
            return;

        pFn[0] = 0xE9;
        const auto disp32 = static_cast<int32_t>(relDisp);
        std::memcpy(pFn + 1, &disp32, sizeof(disp32));
        pFn[5] = 0x90;
        VirtualProtect(pFn, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), pFn, 6);
#endif
    });
} // namespace

Vector<TESObjectREFR*> TESObjectCELL::GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept
{
    Vector<TESObjectREFR*> references{};

    BSScopedLock<BSRecursiveLock> cellLock(lock);
    if (!refData.refArray)
        return references;

    for (uint32_t i = 0; i < refData.capacity; i++)
    {
        TESObjectREFR* pRef = refData.refArray[i].Get();
        if (!pRef || !pRef->baseForm)
            continue;

        for (FormType formType : aFormTypes)
        {
            if (pRef->baseForm->formType == formType)
                references.push_back(pRef);
        }
    }

    return references;
}

void TESObjectCELL::GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept
{
    TP_THIS_FUNCTION(TGetCOCPlacementInfo, void, TESObjectCELL, NiPoint3*, NiPoint3*, bool);
    POINTER_SKYRIMSE(TGetCOCPlacementInfo, s_getCOCPlacementInfo, 18606);
    TiltedPhoques::ThisCall(s_getCOCPlacementInfo, this, aOutPos, aOutRot, aAllowCellLoad);
}
