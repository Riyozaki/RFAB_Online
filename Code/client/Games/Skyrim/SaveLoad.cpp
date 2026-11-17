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
    // No companion means no callback will ever release a pin. Shared effects cannot start in
    // that mode; leave ordinary OFFLINE saves usable, without certifying a co-op co-save.
    if (!SaveStateBridge::Installed())
    {
        if (World::IsOnlineCached()) return false;
        return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDevice, aStats, aName);
    }
    const auto ticket = SaveStateBridge::BeginSave();
    if (!ticket)
    {
        spdlog::warn("[RFAB] Save request refused while a native receipt/load/save is unresolved; existing saves untouched");
        return false;
    }
    bool result{};
    struct Pin
    {
        uint64_t Ticket;
        const bool& Accepted;
        ~Pin() { SaveStateBridge::EndSave(Ticket, Accepted); }
    } pin{*ticket, result};
    result = TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDevice, aStats, aName);
    return result;
}
bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* aName, int32_t aDevice, uint32_t aStats, bool aCheckMods)
{
    if (World::IsCreated() && !RFAB_EXPECT_MAIN_THREAD()) return false;
    const bool outer = !s_preparingLoad;
    ScopedTlsValue guard(s_preparingLoad, true);
    const auto epoch = outer ? SaveStateBridge::BeginLoadRequest() : 0;
    if (outer && !epoch) return false;
    if (outer && World::IsCreated()) World::Get().PrepareForWorldLoad();
    const bool result = TiltedPhoques::ThisCall(RealLoadImpl, apThis, aName, aDevice, aStats, aCheckMods);
    if (outer && !result) SaveStateBridge::RejectLoadRequest(epoch);
    return result;
}
bool TP_MAKE_THISCALL(HookLoadMostRecent, BGSSaveLoadManager)
{
    if (World::IsCreated() && !RFAB_EXPECT_MAIN_THREAD()) return false;
    const bool outer = !s_preparingLoad;
    ScopedTlsValue guard(s_preparingLoad, true);
    const auto epoch = outer ? SaveStateBridge::BeginLoadRequest() : 0;
    if (outer && !epoch) return false;
    if (outer && World::IsCreated()) World::Get().PrepareForWorldLoad();
    const bool result = TiltedPhoques::ThisCall(RealLoadMostRecent, apThis);
    if (outer && !result) SaveStateBridge::RejectLoadRequest(epoch);
    return result;
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
