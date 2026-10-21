#pragma once

#include <VersionDb.h>

#include <spdlog/spdlog.h>

// Guard for byte level patches inside game functions.
//
// Several hooks in the client do not hook a function by its Address Library id
// (which is version independent) but patch a fixed offset *inside* it:
//
//     TiltedPhoques::Put(mem::pointer(id.GetPtr()) + 0x174 + 1, WS_OVERLAPPEDWINDOW);
//
// Those offsets were reversed on SkyrimSE 1.6.x. Function bodies are different
// on 1.5.97, so applying the same write there either does nothing useful or
// overwrites an unrelated instruction and corrupts code. A wrong write cannot
// be detected at runtime without the expected original bytes, which are not
// available for 1.5.97, so the patch is skipped and reported instead.
//
// Hooks that replace a whole function (TiltedPhoques::Jump on the resolved
// address, TP_HOOK_IMMEDIATE, vtable swaps) are version independent and are not
// affected by this guard.
//
// The list of sites and what has to be re-reversed is in
// docs/phase1/BYTEPATCHES_RU.md.
inline bool CanApplyInsideFunctionPatch(const char* acSite) noexcept
{
    if (!VersionDb::Get().IsPreAnniversary())
        return true;

    spdlog::warn("{}: skipped, the byte offset was reversed on 1.6.x. The feature stays disabled on 1.5.97 "
                 "until the patch is re-reversed (docs/phase1/BYTEPATCHES_RU.md)",
                 acSite);
    return false;
}
