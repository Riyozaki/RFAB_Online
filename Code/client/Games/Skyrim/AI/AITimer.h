#pragma once

struct AITimer
{
    static float GetAITime()
    {
        POINTER_SKYRIMSE(float, s_value, 517597);
        return *s_value;
    }

    float fTargetTime;
    float fStartTime;
};
