#pragma once

#include "TESLeveledList.h"

struct TESLevItem : TESBoundObject, TESLeveledList
{
};

#ifdef _MSC_VER
static_assert(sizeof(TESLevItem) == 0x58);
#else
static_assert(sizeof(TESLevItem) == 0x50);
#endif
