#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct NotifyPlayerDownedState;
struct NotifyReviveState;
struct Actor;
struct ActorValueOwner;

/**
 * Implements RFAB 0-HP Downed-on-Knee bleedout & tactical ally revive on Skyrim SE 1.5.97.0.8:
 * 1. Intercepts fatal damage on the local player BEFORE Actor::Kill / REQ_DeathHandler ever executes,
 *    clamping HP to 1.0f and entering bleedout knee posture without corrupting any character stats/spells.
 * 2. Freezes 100% of positive kHealth heals/potions/food/regen AND negative poison/bleed/DoT ticks
 *    directly at the C++ engine choke points (HookRestoreActorValue SE 37513, HookRegenAttributes,
 *    HookApplyActorEffect, HookDamageActor) while downed unless an authorized revive heal is active.
 * 3. Drops enemy aggro off the downed player and redirects nearby hostiles onto living party allies.
 * 4. Supports ~4.0s hold-E interactive ally revive, 30% MaxHP post-revive recovery, 3.0s grace
 *    invulnerability, and periodic CombatEncounterStateReport telemetry for the server coordinator.
 */
class DownedStateService
{
public:
    struct ScopedReviveHealAllowance
    {
        explicit ScopedReviveHealAllowance(bool& aFlag) noexcept
            : m_flag(aFlag)
            , m_prev(aFlag)
        {
            m_flag = true;
        }
        ~ScopedReviveHealAllowance() noexcept { m_flag = m_prev; }

    private:
        bool& m_flag;
        bool m_prev;
    };

    DownedStateService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~DownedStateService() = default;

    TP_NOCOPYMOVE(DownedStateService);

    [[nodiscard]] bool IsSystemActive() const noexcept;
    [[nodiscard]] bool IsLocalPlayerDowned() const noexcept { return m_isLocalDowned; }
    [[nodiscard]] bool IsLocalPlayerInvulnerable() const noexcept { return m_graceRemainingSec > 0.0; }
    [[nodiscard]] bool ShouldBlockPlayerHealthMutation(float aDelta) const noexcept;
    [[nodiscard]] bool IsRemotePlayerDowned(uint32_t aPlayerId) const noexcept;

    // Invoked directly from HookDamageActor and HookRestoreActorValue when incoming damage would kill the player.
    // Returns true if fatal death was intercepted and converted into Downed-on-Knee state.
    bool InterceptPotentialFatalPlayerDamage(Actor* apPlayer, float aCurrentHealth, float aIncomingDamage) noexcept;

    void EnterLocalDownedState() noexcept;
    void ApplyAuthoritativeRevive(float aHealthFraction, float aGraceSec, bool aShowPostCombatNotice) noexcept;
    void ResetAllStates() noexcept;

private:
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnNotifyPlayerDownedState(const NotifyPlayerDownedState& acMessage) noexcept;
    void OnNotifyReviveState(const NotifyReviveState& acMessage) noexcept;

    void RedirectHostileAggroAwayFromPlayer(Actor* apLocalPlayer) noexcept;
    void UpdateInteractiveReviveInput(double aDeltaSec) noexcept;
    void SendCombatEncounterReport() noexcept;
    [[nodiscard]] uint32_t GetLocalPlayerId() const noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    bool m_isLocalDowned{false};
    bool m_allowAuthorizedHeal{false};
    double m_graceRemainingSec{0.0};
    double m_reportTimerSec{0.0};

    bool m_isChannelingRevive{false};
    uint32_t m_channelTargetPlayerId{0};
    double m_channelElapsedSec{0.0};
    double m_channelDurationSec{4.0};

    bool m_bossDefeatedPendingReport{false};
    std::unordered_set<uint32_t> m_liveBossesSeenInCombat{};
    std::unordered_map<uint32_t, bool> m_remoteDownedByPlayerId{};

    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_notifyDownedConnection;
    entt::scoped_connection m_notifyReviveConnection;
};
