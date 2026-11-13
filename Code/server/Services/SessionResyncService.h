#pragma once

#include <Events/PacketEvent.h>
#include <common/Session/Resync.h>

#include <cstdint>
#include <unordered_map>

struct World;
struct PlayerLeaveEvent;
struct RequestResync;

/**
 * Server half of the self-healing contract (architecture step 1, common/Session/Resync.h).
 *
 * Answers RequestResync with authoritative snapshots built by the services that own the state
 * (CharacterService for characters, StringCacheService for the string table) and terminates
 * every answer with NotifyResyncComplete on the same reliable ordered channel. Every request is
 * admitted through a per-player ServerBudget, independent of anything the client claims.
 */
struct SessionResyncService
{
    SessionResyncService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~SessionResyncService() noexcept = default;

    TP_NOCOPYMOVE(SessionResyncService);

private:
    void OnRequestResync(const PacketEvent<RequestResync>& acMessage) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    World& m_world;
    std::unordered_map<uint32_t, Session::Resync::ServerBudget> m_budgetByPlayerId{};
    uint64_t m_requestsServed{0};

    entt::scoped_connection m_requestConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
