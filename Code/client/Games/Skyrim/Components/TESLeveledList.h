#pragma once

#include <Components/BaseFormComponent.h>

struct TESForm;
struct TESGlobal;

struct TESLeveledList : BaseFormComponent
{
    struct Entry
    {
        TESForm* form;
        uint16_t count;
        uint16_t level;
        uint32_t pad0C;
        uint64_t unk10;
    };

    Entry* entries;
    uint8_t chanceValue;
    uint8_t flags;
    uint8_t length;
    uint8_t unk0B;
    uint32_t pad14;
    void* unk18;
    TESGlobal* chanceGlobal;
};

static_assert(sizeof(TESLeveledList::Entry) == 0x18);
static_assert(offsetof(TESLeveledList, entries) == 0x8);
static_assert(offsetof(TESLeveledList, chanceValue) == 0x10);
static_assert(offsetof(TESLeveledList, flags) == 0x11);
static_assert(offsetof(TESLeveledList, length) == 0x12);
static_assert(offsetof(TESLeveledList, unk0B) == 0x13);
static_assert(offsetof(TESLeveledList, unk18) == 0x18);
static_assert(offsetof(TESLeveledList, chanceGlobal) == 0x20);
static_assert(sizeof(TESLeveledList) == 0x28);
