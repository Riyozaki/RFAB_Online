#pragma once

#include <Forms/TESForm.h>
#include <ExtraData/ExtraDataList.h>

struct TESObjectREFR;
struct TESWorldSpace;

struct TESObjectCELL : TESForm
{
    Vector<TESObjectREFR*> GetRefsByFormTypes(const Vector<FormType>& aFormTypes) const noexcept;
    void GetCOCPlacementInfo(NiPoint3* aOutPos, NiPoint3* aOutRot, bool aAllowCellLoad) noexcept;

    bool IsValid() const { return cellState == 7; }

    struct ReferenceData
    {
        struct Reference
        {
            TESObjectREFR* ref;
            void* unk08;

            TESObjectREFR* Get() { return unk08 != nullptr ? ref : nullptr; }
        };

        uint32_t unk00;
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
    uint16_t cellFlags;
    uint16_t cellGameFlags;
    uint8_t cellState;
    bool autoWaterLoaded;
    bool cellDetached;
    uint8_t pad47;
    ExtraDataList extraData;
    void* unk060;
    void* unk068;
    float waterLevel;
    void* unk078;
    void* unk080;
    ReferenceData refData;
    void* unk0B0;
    GameArray<TESObjectREFR*> objectList;
    GameArray<void*> unk0D0;
    GameArray<void*> unk0E8;
    GameArray<void*> unk100;
    uint64_t unk118;
    TESWorldSpace* worldspace;
    uint64_t unk128;
    void* pLightingTemplate;
    uint64_t unk138;
};

static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);
static_assert(offsetof(TESObjectCELL, cellGameFlags) == 0x42);
static_assert(offsetof(TESObjectCELL, cellState) == 0x44);
static_assert(offsetof(TESObjectCELL, extraData) == 0x48);
static_assert(offsetof(TESObjectCELL, refData) == 0x88);
static_assert(offsetof(TESObjectCELL, objectList) == 0xB8);
static_assert(offsetof(TESObjectCELL, worldspace) == 0x120);
static_assert(offsetof(TESObjectCELL, pLightingTemplate) == 0x130);
static_assert(sizeof(TESObjectCELL) == 0x140);
