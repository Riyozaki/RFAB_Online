#include <TiltedOnlinePCH.h>

#include <SaveLoad.h>
#include <Services/CoordinatedSaveService.h>
#include <Utils.h>
#include <World.h>

namespace
{
TP_THIS_FUNCTION(TSaveImpl, bool, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName);
TP_THIS_FUNCTION(TLoadImpl, bool, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods);

TSaveImpl* RealSaveImpl = nullptr;
TLoadImpl* RealLoadImpl = nullptr;

bool TP_MAKE_THISCALL(HookSaveImpl, BGSSaveLoadManager, int32_t aDeviceId, uint32_t aOutputStats, const char* acpFileName)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineSaveAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealSaveImpl, apThis, aDeviceId, aOutputStats, acpFileName);
}

bool TP_MAKE_THISCALL(HookLoadImpl, BGSSaveLoadManager, const char* acpFileName, int32_t aDeviceId, uint32_t aOutputStats, bool aCheckForMods)
{
    if (!World::Get().GetCoordinatedSaveService().HandleEngineLoadAttempt(acpFileName))
        return false;

    return TiltedPhoques::ThisCall(RealLoadImpl, apThis, acpFileName, aDeviceId, aOutputStats, aCheckForMods);
}

TiltedPhoques::Initializer s_saveLoadHooks(
    []()
    {
        POINTER_SKYRIMSE(TSaveImpl, s_saveImpl, 34818);
        POINTER_SKYRIMSE(TLoadImpl, s_loadImpl, 34819);

        RealSaveImpl = s_saveImpl.Get();
        RealLoadImpl = s_loadImpl.Get();

        TP_HOOK(&RealSaveImpl, HookSaveImpl);
        TP_HOOK(&RealLoadImpl, HookLoadImpl);
    });
} // namespace

BGSSaveLoadManager* BGSSaveLoadManager::Get() noexcept
{
    POINTER_SKYRIMSE(BGSSaveLoadManager*, s_singleton, 516860);
    return *s_singleton.Get();
}

void BGSSaveLoadManager::Save(SaveData* apData)
{
    apData->flags |= 4;

    const char* cSaveName = "";
    if (apData->saveName)
        cSaveName = apData->saveName;
}

bool BGSSaveLoadManager::SaveNamed(const char* acpFileName) noexcept
{
    if (!RealSaveImpl || !acpFileName)
        return false;

    return TiltedPhoques::ThisCall(RealSaveImpl, this, 2, 0U, acpFileName);
}

bool BGSSaveLoadManager::LoadNamed(const char* acpFileName, bool aCheckForMods) noexcept
{
    if (!RealLoadImpl || !acpFileName)
        return false;

    return TiltedPhoques::ThisCall(RealLoadImpl, this, acpFileName, -1, 0U, aCheckForMods);
}
