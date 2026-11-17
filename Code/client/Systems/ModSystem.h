#pragma once

#include <shared_mutex>
#include <ThreadAffinity.h>

struct Mods;
struct GameId;
struct DisconnectedEvent;

/**
 * @brief Makes sure that different clients can have independent load orders and mods.
 */
struct ModSystem
{
    static constexpr uint32_t kRuntimeDynamicModId = 0xFFFFFFFFu;
    static constexpr uint32_t kUnmappedServerModId = 0xFFFFFFFEu;

    ModSystem(entt::dispatcher& aDispatcher) noexcept;
    ~ModSystem() = default;

    TP_NOCOPYMOVE(ModSystem);

    RFAB_ANY_THREAD bool GetServerModId(uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept;
    RFAB_ANY_THREAD bool GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept;
    RFAB_ANY_THREAD uint32_t GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept;
    RFAB_ANY_THREAD uint32_t GetGameId(const GameId& acGameId) const noexcept;

protected:
    void HandleMods(const Mods& acMods) noexcept;
    void HandleDisconnected(const DisconnectedEvent& acEvent) noexcept;

private:
    struct GameMod
    {
        uint16_t id;
        bool isLite;
    };

    mutable std::shared_mutex m_mutex;
    entt::scoped_connection m_modsConnection;
    entt::scoped_connection m_disconnectedConnection;
    Map<uint16_t, uint32_t> m_liteToServer;
    Map<uint32_t, GameMod> m_serverToGame;
    uint32_t m_standardToServer[0x100];
};
