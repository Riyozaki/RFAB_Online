#include "StringCache.h"

#include <GameServer.h>
#include <Services/StringCacheService.h>
#include <Events/UpdateEvent.h>
#include <Game/Player.h>

StringCacheService::StringCacheService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&StringCacheService::HandleUpdate>(this))
{
}

void StringCacheService::HandleUpdate(const UpdateEvent&) const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    auto& stringCache = StringCache::Get();

    // Promote strings that were received as literals into the shared table. This may legitimately
    // report "nothing new" while a player is still missing entries (a late joiner starts at id 0
    // against an already warm table), so the send loop below is driven by per-player progress and
    // NOT by this flag - otherwise such a player never receives the table and every CachedString
    // the server relays to it by id resolves to nothing for the rest of the session.
    stringCache.ProcessDirty();

    const auto cCacheSize = stringCache.Size();

    for (const auto pPlayer : m_world.GetPlayerManager())
    {
        auto startId = pPlayer->GetStringCacheId();
        if (startId >= cCacheSize)
            continue;

        auto update = stringCache.Serialize(startId);
        pPlayer->SetStringCacheId(startId);

        pPlayer->Send(update);
    }
}

uint32_t StringCacheService::ResendAllTo(Player* apPlayer) const noexcept
{
    if (!apPlayer)
        return 0;

    auto& stringCache = StringCache::Get();
    uint32_t startId = 0;
    auto update = stringCache.Serialize(startId);
    // The client applies a resend from 0 as "skip the common prefix, append the rest", so the
    // per-player cursor simply moves to the end of the table as for a normal delta.
    apPlayer->SetStringCacheId(startId);
    apPlayer->Send(update);
    return static_cast<uint32_t>(update.Values.size());
}
