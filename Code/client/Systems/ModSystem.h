#pragma once

#include <common/Session/Lifecycle.h>
#include <ThreadAffinity.h>

#include <shared_mutex>

struct Mods;
struct GameId;

/**
 * @brief Makes sure that different clients can have independent load orders and mods.
 */
struct ModSystem final : Session::IParticipant
{
    ModSystem(entt::dispatcher& aDispatcher) noexcept;
    ~ModSystem() = default;

    TP_NOCOPYMOVE(ModSystem);

    RFAB_ANY_THREAD bool GetServerModId(uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept;
    RFAB_ANY_THREAD bool GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept;
    RFAB_ANY_THREAD uint32_t GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept;
    RFAB_ANY_THREAD uint32_t GetGameId(const GameId& acGameId) const noexcept;

protected:
    void HandleMods(const Mods& acMods) noexcept;

public:
    // Session::ParticipantId::kModTable. The table of a session is filled by HandleMods (the Mods
    // packet arrives before the session begins) and dropped when the session ends - after the world
    // model, which may still translate ids during its own teardown.
    void OnSessionEnd(const Session::Transition& acTransition) noexcept override;

private:
    struct GameMod
    {
        uint16_t id;
        bool isLite;
    };

    mutable std::shared_mutex m_mutex;
    entt::scoped_connection m_modsConnection;
    Map<uint16_t, uint32_t> m_liteToServer;
    Map<uint32_t, GameMod> m_serverToGame;
    uint32_t m_standardToServer[0x100];
};
