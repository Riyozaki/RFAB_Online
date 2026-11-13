
#include <TiltedOnlinePCH.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>

#include <Services/StringCacheService.h>
#include <World.h>
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
    auto& cache = StringCache::Get();
    const size_t localSizeBefore = cache.Size();
    if (cache.Deserialize(acMessage))
        return;

    // Applying a non-contiguous or mismatching update would shift every id and silently corrupt
    // every CachedString for the rest of the session, so it is dropped and the table is rebuilt
    // from id 0 by the server (common/Session/Resync.h, Scope::kStringCache).
    // - Gap (StartId past our end): our prefix is still correct, the full resend appends to it.
    // - Mismatch: our table disagrees with the server's on ids already handed out; keeping it would
    //   make the full resend fail the same prefix check, so it is cleared first.
    const bool isGap = acMessage.StartId > localSizeBefore;
    spdlog::warn("Rejected StringCacheUpdate (StartId {}, {} values) against local table of {} entries ({}); "
                 "total rejected: {}, unresolved ids: {} - requesting a full resend",
                 acMessage.StartId, acMessage.Values.size(), localSizeBefore, isGap ? "gap" : "mismatch",
                 cache.GetRejectedUpdateCount(), cache.GetUnknownIdCount());
    if (!isGap)
        cache.Clear();

    if (World::IsCreated())
        World::Get().GetSessionResyncService().NoteGap(
            Session::Resync::Scope::kStringCache, Session::Resync::kNoServerId, Session::Resync::Reason::kStringCacheRejected);
}
