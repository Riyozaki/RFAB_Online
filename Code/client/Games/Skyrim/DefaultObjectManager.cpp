#include <TiltedOnlinePCH.h>

#include <DefaultObjectManager.h>

DefaultObjectManager& DefaultObjectManager::Get()
{
    using TGetDefaultObjectManager = DefaultObjectManager&();

    POINTER_SKYRIMSE(TGetDefaultObjectManager, GetDefaultObjectManager, 10878);

    return GetDefaultObjectManager();
}
