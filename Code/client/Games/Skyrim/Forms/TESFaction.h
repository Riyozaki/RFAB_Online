#pragma once

#include <cstddef>
#include <cstdint>
#include <Forms/TESForm.h>
#include <Components/TESFullName.h>

struct TESObjectREFR;
struct BGSListForm;

struct FactionCrimeData
{
    TESObjectREFR* factionJailMarker;                // 00
    TESObjectREFR* factionWaitMarker;                // 08
    TESObjectREFR* factionStolenContainer;           // 10 - STOL (evidence chest)
    TESObjectREFR* factionPlayerInventoryContainer;  // 18 - PLCN (prisoner belongings chest)
    BGSListForm* crimeGroup;                         // 20
    TESObjectREFR* jailOutfit;                       // 28
    uint8_t arrest;                                  // 30
    uint8_t attackOnSight;                           // 31
    uint16_t murder;                                 // 32
    uint16_t assault;                                // 34
    uint16_t trespass;                               // 36
    uint16_t pickpocket;                             // 38
    uint16_t pad3A;                                  // 3A
    float stealMult;                                 // 3C
    uint16_t escape;                                 // 40
    uint16_t werewolf;                               // 42
    uint32_t pad44;                                  // 44
};
static_assert(sizeof(FactionCrimeData) == 0x48);

struct TESFaction : TESForm
{
    TESFullName fullname;            // 020 (size 0x10)
    uint8_t reactionFormPad[0x20];   // 030 (TESReactionForm: size 0x20)
    void* crimeGoldMap;              // 050
    uint32_t factionFlags;           // 058
    uint32_t pad5C;                  // 05C
    FactionCrimeData crimeData;      // 060
    uint8_t vendorAndRankPad[0x100 - 0xA8];
};

static_assert(offsetof(TESFaction, crimeData) == 0x60);
static_assert(offsetof(TESFaction, crimeData.factionStolenContainer) == 0x70);
static_assert(offsetof(TESFaction, crimeData.factionPlayerInventoryContainer) == 0x78);
static_assert(sizeof(TESFaction) == 0x100);
