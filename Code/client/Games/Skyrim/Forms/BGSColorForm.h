#pragma once

#include <cstddef>
#include <Forms/TESForm.h>
#include <Components/TESFullName.h>

struct BGSColorForm : TESForm
{
    static constexpr FormType Type = FormType::ColorForm;

    TESFullName fullName;

    union
    {
        struct Color
        {
            uint8_t red, green, blue, alpha;
        } color;
        uint32_t abgr;
    };
    uint32_t unk20;
};

static_assert(offsetof(BGSColorForm, color) == 0x30);
static_assert(sizeof(BGSColorForm) == 0x38);
