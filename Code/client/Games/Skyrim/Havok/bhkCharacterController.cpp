#include <TiltedOnlinePCH.h>

#include <Havok/bhkCharacterController.h>

bool bhkCharacterController::UpdateStepTiming(float aMovementDeltaTime) noexcept
{
    // Verified against SkyrimSE.exe 1.5.97.0 (TrueHUD / SKSE64):
    //   523660 = g_deltaTime (0x2F6B948), 523661 = g_deltaTimeRealTime (0x2F6B94C)
    POINTER_SKYRIMSE(float, s_deltaTime, 523660);
    POINTER_SKYRIMSE(float, s_deltaTimeRealTime, 523661);

    const float dtMain = s_deltaTime.Get() ? *s_deltaTime.Get() : 0.f;
    const float dtReal = s_deltaTimeRealTime.Get() ? *s_deltaTimeRealTime.Get() : 0.f;
    const float frameDt = (dtMain > 0.0001f) ? dtMain : dtReal;
    return stepInfo.UpdateDeltaTime(frameDt, aMovementDeltaTime);
}
