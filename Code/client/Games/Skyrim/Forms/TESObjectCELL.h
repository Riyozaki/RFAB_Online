#pragma once

#include <Forms/TESForm.h>
#include <BSCore/BSAtomic.h>

struct TESObjectREFR;
struct TESWorldSpace;
struct BGSEncounterZone;
struct LoadedCellData;

struct TESObjectCELL : TESForm
{
    Vector<TESObjectREFR*> GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept;
    void GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept;

    enum class CellState : uint8_t
    {
        Attached = 7
    };

    bool IsAttached() const { return cellState == CellState::Attached; }

    struct ReferenceData
    {
        struct Reference
        {
            TESObjectREFR* ref;
            void* unk08;

            TESObjectREFR* Get() { return unk08 != nullptr ? ref : nullptr; }
        };

        uint64_t unk0;
        uint32_t unk8;
        uint32_t capacity;
        uint32_t available;
        uint32_t unkC;
        void* unk10;
        void* unk18;
        Reference* refArray;

        uint32_t Count() { return capacity - available; }
    };
    static_assert(sizeof(ReferenceData) == 0x30);

    struct LoadedCellData
    {
        uint8_t pad0[0x160];
        BGSEncounterZone* encounterZone;
    };
    static_assert(offsetof(LoadedCellData, encounterZone) == 0x160);

    uint8_t pad20[0x40 - 0x20];
    uint16_t cellFlags;
    uint16_t cellGameFlags;
    CellState cellState;
    bool autoWaterLoaded;
    bool cellDetached;
    uint8_t pad47;
    ExtraDataList extraData;
    uint64_t cellData;
    void* pCellLand;
    float waterHeight;
    void* pNavMeshes;
    ReferenceData refData;
    void* pUnkB0;
    GameArray<TESObjectREFR*> objectList;
    GameArray<void*> unkD0;
    GameArray<void*> unkE8;
    GameArray<void*> unk100;
    BSSpinLock lock;
    TESWorldSpace* worldspace;
    LoadedCellData* loadedCellData;
    void* pLightingTemplate;
    uint64_t unk138;
};

// Skyrim SE 1.5.97 (pre-1.6.629) offsets: embedded ExtraDataList has no vtable.
static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);
static_assert(offsetof(TESObjectCELL, cellGameFlags) == 0x42);
static_assert(offsetof(TESObjectCELL, cellState) == 0x44);
static_assert(offsetof(TESObjectCELL, refData) == 0x80);
static_assert(offsetof(TESObjectCELL, lock) == 0x118);
static_assert(offsetof(TESObjectCELL, worldspace) == 0x120);
static_assert(offsetof(TESObjectCELL, loadedCellData) == 0x128);
static_assert(sizeof(TESObjectCELL) == 0x140);
