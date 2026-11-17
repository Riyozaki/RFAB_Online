#include <Havok/bhkCharacterController.h>

bool bhkCharacterController::UpdateStepTiming(float aMovementDeltaTime) noexcept
{
    // Same physics timestep used by the native rigid-body controller movement path
    POINTER_SKYRIMSE(float, s_physicsDeltaTime, 512261);
    return stepInfo.UpdateDeltaTime(*s_physicsDeltaTime.Get(), aMovementDeltaTime);
}
