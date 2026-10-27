#pragma once

#include <Forms/TESActorBase.h>

#include <Components/TESRaceForm.h>
#include <Components/BGSOverridePackCollection.h>

struct BGSColorForm;
struct BGSTextureSet;
struct TESClass;
struct TESCombatStyle;
struct TESObjectARMO;
struct BGSOutfit;
struct BGSHeadPart;
struct BGSRelationship;

struct TESNPC : TESActorBase
{
    static constexpr FormType Type = FormType::Npc;

    static TESNPC* Create(const String& acBuffer, uint32_t aChangeFlags) noexcept;
    static TESNPC* GetLeveledSourceNpc(const TESNPC* apNpc) noexcept;

    TESNPC* GetTemplateBase() const noexcept
    {
        TESNPC* pTemplate = npcTemplate;
        uint32_t depth = 0;

        while (pTemplate && pTemplate->IsTemporary() && depth++ < 16u)
            pTemplate = pTemplate->npcTemplate;

        return (pTemplate && !pTemplate->IsTemporary()) ? pTemplate : nullptr;
    }

    struct FaceMorphs
    {
        float option[19];
        uint32_t presets[4];

        void CopyFrom(const FaceMorphs& acRhs)
        {
            std::copy(std::begin(acRhs.option), std::end(acRhs.option), std::begin(option));
            std::copy(std::begin(acRhs.presets), std::end(acRhs.presets), std::begin(presets));
        }
    };

    struct HeadData
    {
        BGSColorForm* hairColor;
        BGSTextureSet* headTexture;
    };

    TESRaceForm raceForm;
    BGSOverridePackCollection overridePacks;
    void* unkDC;
    uint8_t unk0E0[0x24];
    uint8_t pad1B4[0x6];
    uint16_t unk10A;
    TESClass* npcClass;

    HeadData* headData;
    uintptr_t unk114;
    TESCombatStyle* combatStyle;
    size_t unk11C;
    TESRace* overlayRace;
    TESNPC* npcTemplate;
    float height;
    float weight;
    uintptr_t unk130;
    BSFixedString shortName;
    TESObjectARMO* farArmo;
    BGSOutfit* outfits[2];
    uintptr_t unk144;
    TESFaction* faction;

    BGSHeadPart** headparts;
    uint8_t headpartsCount;

#if TP_PLATFORM_64
    uint8_t pad241[5];
#else
    uint8_t pad151[3];
#endif

    struct Color
    {
        uint8_t red, green, blue;
    } color;

    GameArray<BGSRelationship*>* relationships;
    FaceMorphs* faceMorphs;
    uintptr_t unk160;

    BGSHeadPart* GetHeadPart(uint32_t aType);
    void Serialize(String* apSaveBuffer) const noexcept;
    void Deserialize(const String& acBuffer, uint32_t aChangeFlags) noexcept;
    void Initialize() noexcept;
};

#ifdef _MSC_VER
static_assert(offsetof(TESNPC, npcClass) == 0x1C0);
static_assert(offsetof(TESNPC, color) == 0x246);
static_assert(offsetof(TESNPC, relationships) == 0x250);
#else
static_assert(offsetof(TESNPC, npcClass) == 0x1B8);
static_assert(offsetof(TESNPC, color) == 0x23E);
static_assert(offsetof(TESNPC, relationships) == 0x248);
#endif
