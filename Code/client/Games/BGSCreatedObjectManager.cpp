#include "BGSCreatedObjectManager.h"

BGSCreatedObjectManager* BGSCreatedObjectManager::Get() noexcept
{
    POINTER_SKYRIMSE(BGSCreatedObjectManager*, pObjManager, 514172);
    return *pObjManager.Get();
}
