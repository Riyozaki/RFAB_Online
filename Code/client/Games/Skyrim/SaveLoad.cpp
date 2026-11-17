#include <TiltedOnlinePCH.h>
#include <SaveLoad.h>
#include <World.h>
#include <Games/Overrides.h>
#include <Services/SaveStateService.h>

namespace
{
// Narrow pre-load wrappers from CommonLibSSE-NG b93280e832f263dbef44e44cbe2936622a02f91a:
// src/RE/B/BGSSaveLoadManager.cpp + include/RE/Offsets.h. Do not abort a mid-load call-site or
// infer success from .ess existence; the original engine return value remains authoritative.
TP_THIS_FUNCTION(TLoadImpl, bool, BGSSaveLoadManager, const char*, int32_t, uint32_t, bool);
TP_THIS_FUNCTION(TLoadMostRecent, bool, BGSSaveLoadManager);
TP_THIS_FUNCTION(TSaveImpl, bool, BGSSaveLoadManager, int32_t, uint32_t, const char*);
TSaveImpl* RealSaveImpl{};
TLoadImpl* RealLoadImpl{};
TLoadMostRecent* RealLoadMostRecent{};
thread_local bool s_preparingLoad{};

bool TP_MAKE_THISCALL(HookSaveImpl, BGSSaveLoadManager, int32_t aDevice, uint32_t aStats, const char* aName)
{
    // This is the OUTER bool request, not SKSE's inner save callback. Refuse before partial ESS
    // writing rather than pretending a save made during Prepared/Applied contains the receipt.
    if (!SaveStateBridge::SaveAllowed())
    {
        spdlog::warn("[RFAB] Save request refused while native receipt/load is unresolved; existing saves untouched");
        return false;
    }
    return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDevice, aStats, aName);
}
bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* aName, int32_t aDevice, uint32_t aStats, bool aCheckMods)
{
    if (World::IsCreated())
    {
        if (!RFAB_EXPECT_MAIN_THREAD()) return false; // outer request, never an inner 'save corrupt' return
        if (!s_preparingLoad)
            World::Get().PrepareForWorldLoad();
    }
    ScopedTlsValue guard(s_preparingLoad, true);
    return TiltedPhoques::ThisCall(RealLoadImpl, apThis, aName, aDevice, aStats, aCheckMods);
}
bool TP_MAKE_THISCALL(HookLoadMostRecent, BGSSaveLoadManager)
{
    if (World::IsCreated())
    {
        if (!RFAB_EXPECT_MAIN_THREAD()) return false;
        if (!s_preparingLoad)
            World::Get().PrepareForWorldLoad();
    }
    ScopedTlsValue guard(s_preparingLoad, true);
    return TiltedPhoques::ThisCall(RealLoadMostRecent, apThis);
}
TiltedPhoques::Initializer s_loadBoundary([] {
    POINTER_SKYRIMSE(TSaveImpl, save, 34818);
    RealSaveImpl = save.Get();
    TP_HOOK(&RealSaveImpl, HookSaveImpl);
    POINTER_SKYRIMSE(TLoadImpl, load, 34819);
    POINTER_SKYRIMSE(TLoadMostRecent, recent, 34856);
    RealLoadImpl = load.Get();
    RealLoadMostRecent = recent.Get();
    TP_HOOK(&RealLoadImpl, HookLoadImpl);
    TP_HOOK(&RealLoadMostRecent, HookLoadMostRecent);
});
} // namespace

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;
}
