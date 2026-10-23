#pragma once

#include "SteamCrypto.h"
#include <mem/mem.h>

namespace steam
{
struct CEGLocationInfo
{
    uint8_t* start;
    mem::region textSeg;
};

bool HasSteamStubV31(const uint8_t* apEntryPtr) noexcept;
uintptr_t CrackCEGInPlace(const CEGLocationInfo&);
} // namespace steam
