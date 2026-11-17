#pragma once
#include <ExtraData/ExtraData.h>

// Layout references: CommonLibSSE-NG b93280e832f263dbef44e44cbe2936622a02f91a.
// Engine-owned instances are read on the main thread and re-resolved for each native operation.
struct ExtraOwnership : BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::Ownership;
    TESForm* owner;
};
struct ExtraUniqueID : BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::UniqueID;
    uint32_t baseID;
    uint16_t uniqueID, pad16;
};
static_assert(sizeof(ExtraOwnership) == 0x18);
static_assert(sizeof(ExtraUniqueID) == 0x18);
