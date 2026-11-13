#pragma once

struct UpdateEvent;
struct World;
struct Player;

/**
 * @brief Handles string caching replication
 */
struct StringCacheService
{
    StringCacheService(World& aWorld, entt::dispatcher& aDispatcher);

    // Self-healing (common/Session/Resync.h): sends the whole table from id 0 right now, so it is
    // on the wire before the NotifyResyncComplete that follows. Returns the number of strings sent.
    uint32_t ResendAllTo(Player* apPlayer) const noexcept;

protected:
    void HandleUpdate(const UpdateEvent&) const noexcept;

private:
    World& m_world;

    entt::scoped_connection m_updateConnection;
};
