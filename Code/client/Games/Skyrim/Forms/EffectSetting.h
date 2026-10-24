#pragma once

#include <Forms/TESForm.h>
#include <Components/TESFullName.h>
#include <Components/BGSKeywordForm.h>
#include <Components/BGSMenuDisplayObject.h>
#include <Games/Magic/MagicSystem.h>

struct EffectSetting : TESForm
{
    enum EffectFlag : uint32_t
    {
        kNone = 0,
        kHostile = 1u << 0,
        kRecover = 1u << 1,
        kDetrimental = 1u << 2,
    };

    TESFullName fullName;
    BGSMenuDisplayObject menuDisplayObject;
    BGSKeywordForm keywordForm;
    uintptr_t unk30;
    uintptr_t unk34;
    uint32_t flags;
    uint32_t unk3C;
    void* unk40;
    uint32_t unk44;
    uint32_t unk48;
    uint16_t unk4C;
    uint8_t pad4E[2];
    void* unk50;
    uint32_t unk54;
    void* unk58;
    void* unk5C;
    uint8_t unk60[0x18];
    EffectArchetypes::ArchetypeID eArchetype;
    int32_t primaryAV;
    void* unk80[2];
    uint32_t castType;
    uint32_t deliveryType;
    // more stuff
};

static_assert(offsetof(EffectSetting, flags) == 0x68);
static_assert(offsetof(EffectSetting, eArchetype) == 0xC0);
static_assert(offsetof(EffectSetting, primaryAV) == 0xC4);
static_assert(offsetof(EffectSetting, castType) == 0xD8);
static_assert(offsetof(EffectSetting, deliveryType) == 0xDC);
