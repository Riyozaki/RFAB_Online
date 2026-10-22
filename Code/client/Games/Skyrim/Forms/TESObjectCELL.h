#pragma once

#include <Forms/TESForm.h>
#include <ExtraData/ExtraDataList.h>

struct TESObjectREFR;
struct TESWorldSpace;

// Skyrim SE 1.5.97 (pre-1.6.629) layout.
//
// Provenance of every offset below: the upstream pre-AE header
// (`Code/client/Games/Skyrim/Forms/TESObjectCELL.h` at commit 5c5837f6, the
// last commit before the "AE sigma grindset" migration), which was written
// against the real 1.5.97 binary:
//   cellFlags 0x40, refData 0x88, worldspace 0x120,
//   ReferenceData is 0x28 bytes (uint32 first member, NOT uint64).
//
// The 1.6.629 update inserted an 8-byte member in ReferenceData and a vtable in
// ExtraDataList, which is why AE-era code has 0x30 / 0xB0 / 0x140-size layouts.
// Do not "fix" this file towards AE: the client targets 1.5.97 only.
struct TESObjectCELL : TESForm
{
    Vector<TESObjectREFR*> GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept;
    void GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept;

    // 1.5.97 stores the cell state byte right after the two flag words; the
    // pre-AE upstream code addressed it as cellFlags[4].
    bool IsValid() const { return cellFlags[4] == 7; }

    struct ReferenceData
    {
        struct Reference
        {
            TESObjectREFR* ref;
            void* unk08;

            TESObjectREFR* Get() { return unk08 != nullptr ? ref : nullptr; }
        };

        uint32_t unk0;
        uint32_t capacity;
        uint32_t available;
        uint32_t unk0C;
        void* unk10;
        void* unk18;
        Reference* refArray;

        uint32_t Count() { return capacity - available; }
    };
    static_assert(sizeof(ReferenceData) == 0x28);

    uint8_t pad20[0x40 - 0x20];
    // byte 0..1 = cell flags, byte 2..3 = game flags, byte 4 = cell state.
    uint8_t cellFlags[5];
    // 0x45..0x88 is extra data / loaded state / water level. Upstream treated
    // this region as opaque; we keep it opaque instead of inventing names.
    uint8_t pad45[0x88 - 0x45];

    ReferenceData refData; // 0x88

    // 0x88 + 0x28 = 0xB0. The cell reference lists (objectList, ...) live in
    // 0xB0..0x118; no code in this repository reads them, so they stay opaque.
    uint8_t unkB0[0x118 - 0xB0];

    BSRecursiveLock lock;     // 0x118
    TESWorldSpace* worldspace; // 0x120
    uint64_t unk128;
    void* pLightingTemplate;
    uint64_t unk138;
};

static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);
static_assert(offsetof(TESObjectCELL, refData) == 0x88);
static_assert(offsetof(TESObjectCELL, lock) == 0x118);
static_assert(offsetof(TESObjectCELL, worldspace) == 0x120);
static_assert(offsetof(TESObjectCELL, pLightingTemplate) == 0x130);
static_assert(sizeof(TESObjectCELL) == 0x140);
