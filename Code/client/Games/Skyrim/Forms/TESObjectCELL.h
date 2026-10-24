#pragma once

#include <Forms/TESForm.h>

struct TESObjectREFR;
struct TESWorldSpace;
struct BGSEncounterZone;
struct LoadedCellData;

struct TESObjectCELL : TESForm
{
    Vector<TESObjectREFR*> GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept;
    void GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept;

    bool IsValid() const { return cellFlags[4] == 7; }

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
    uint8_t cellFlags[5];
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
    mutable BSRecursiveLock lock;
    TESWorldSpace* worldspace;
    LoadedCellData* loadedCellData;
    void* pLightingTemplate;
    uint64_t unk138;
};

// Skyrim SE 1.5.97 (pre-1.6.629) offsets: the embedded ExtraDataList has no
// vtable yet, so everything from cellData on sits 8 bytes lower than on
// 629+ (cellData 0x60, references 0x80, spinLock 0x118, worldSpace 0x120, size 0x140).
// Cross-checked with CommonLibSSE TESObjectCELL for SE 1.5.97.
#ifdef _MSC_VER
static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);
#else
static_assert(offsetof(TESObjectCELL, cellFlags) == 0x3C);
#endif
static_assert(offsetof(TESObjectCELL, refData) == 0x80);
static_assert(offsetof(TESObjectCELL, lock) == 0x118);
static_assert(offsetof(TESObjectCELL, worldspace) == 0x120);
static_assert(offsetof(TESObjectCELL, loadedCellData) == 0x128);
static_assert(sizeof(TESObjectCELL) == 0x140);
