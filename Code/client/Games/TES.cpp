#include <TiltedOnlinePCH.h>

#include <Games/TES.h>

TES* TES::Get() noexcept
{
    POINTER_SKYRIMSE(TES*, tes, 514281);

    return *tes.Get();
}

ProcessLists* ProcessLists::Get() noexcept
{
    POINTER_SKYRIMSE(ProcessLists*, processLists, 514167);

    return *processLists.Get();
}
