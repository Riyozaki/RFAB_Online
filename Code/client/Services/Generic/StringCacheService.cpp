
#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Services/StringCacheService.h>
#include "StringCache.h"

StringCacheService::StringCacheService(entt::dispatcher& aDispatcher) noexcept
{
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&StringCacheService::HandleConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&StringCacheService::HandleDisconnected>(this);
    m_stringCacheUpdateConnection = aDispatcher.sink<StringCacheUpdate>().connect<&StringCacheService::HandleStringCacheUpdate>(this);
}

void StringCacheService::HandleConnected(const ConnectedEvent& acEvent) noexcept
{
    // Can never be too careful
    StringCache::Get().Clear();
}

void StringCacheService::HandleDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    StringCache::Get().Clear();
}

void StringCacheService::HandleStringCacheUpdate(const StringCacheUpdate& acMessage) noexcept
{
    if (!StringCache::Get().Deserialize(acMessage))
    {
        // Applying a non-contiguous or mismatching update would shift every id and silently
        // corrupt every CachedString for the rest of the session, so it is dropped instead.
        spdlog::warn("Rejected StringCacheUpdate (StartId {}, {} values) against local table of {} entries; "
                     "total rejected: {}, unresolved ids: {}",
                     acMessage.StartId, acMessage.Values.size(), StringCache::Get().Size(),
                     StringCache::Get().GetRejectedUpdateCount(), StringCache::Get().GetUnknownIdCount());
    }
}
