#pragma once

#include <common/CoopTeleportGuard.h>
#include <common/Session/Lifecycle.h>

#include <chrono>

struct World;
struct TransportService;

struct UpdateEvent;
struct ServerSettings;
struct GridCellChangeEvent;
struct CellChangeEvent;
struct PlayerDialogueEvent;
struct PlayerLevelEvent;
struct PartyJoinedEvent;
struct PartyLeftEvent;

struct NotifyPlayerRespawn;

/**
 * @brief Handles logic related to the local player.
 */
struct PlayerService final : Session::IParticipant
{
    PlayerService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~PlayerService() noexcept = default;

    TP_NOCOPYMOVE(PlayerService);

    void RestoreMultiplayerGlobalsAfterWorldReload() noexcept;

    // Session::ParticipantId::kPlayer. Captures the offline baseline (globals, greet distance) at
    // session begin and restores it at session end - only for sessions that actually began, so a
    // failed connection attempt no longer "restores" a baseline that was never captured. Sole owner of
    // the session-level PlayerCharacter respawn mode.
    void OnSessionBegin(const Session::Transition& acTransition) noexcept override;
    void OnSessionEnd(const Session::Transition& acTransition) noexcept override;

protected:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnServerSettingsReceived(const ServerSettings& acSettings) noexcept;
    void OnNotifyPlayerRespawn(const NotifyPlayerRespawn& acMessage) const noexcept;
    void OnGridCellChangeEvent(const GridCellChangeEvent& acEvent) const noexcept;
    void OnCellChangeEvent(const CellChangeEvent& acEvent) const noexcept;
    void OnPlayerDialogueEvent(const PlayerDialogueEvent& acEvent) const noexcept;
    void OnPlayerLevelEvent(const PlayerLevelEvent& acEvent) const noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;
    void OnPartyLeftEvent(const PartyLeftEvent& acEvent) noexcept;

private:
    /**
     * @brief Run the respawn timer, and if it hits 0, respawn the player.
     */
    void RunRespawnUpdates() noexcept;
    void RunPostDeathUpdates() noexcept;
    /**
     * @brief Make sure difficulty doesn't get changed while connected
     */
    void RunDifficultyUpdates() const noexcept;
    void RunLevelUpdates() const noexcept;
    void RunBeastFormDetection() const noexcept;

    void ToggleDeathSystem(bool aSet) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    std::chrono::steady_clock::time_point m_respawnDeadline{};
    int32_t m_serverDifficulty = 6;
    int32_t m_previousDifficulty = 6;
    float m_previousGreetDistance = 70.0f;
    float m_previousKillMove = 1.0f;
    float m_previousWorldEncounters = 1.0f;
    bool m_hasCachedRfabBaseline = false;

    bool m_isDeathSystemEnabled = false;
    CoopTeleportGuard::LatchedDownedRespawnGuard m_respawnTeleportGuard{};

    bool m_knockdownStart = false;
    std::chrono::steady_clock::time_point m_knockdownDeadline{};

    bool m_godmodeStart = false;
    std::chrono::steady_clock::time_point m_godmodeDeadline{};

    uint32_t m_cachedMainSpellId = 0;
    uint32_t m_cachedSecondarySpellId = 0;
    uint32_t m_cachedPowerId = 0;

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_settingsConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_gridCellChangeConnection;
    entt::scoped_connection m_cellChangeConnection;
    entt::scoped_connection m_playerDialogueConnection;
    entt::scoped_connection m_playerLevelConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_partyLeftConnection;
};
