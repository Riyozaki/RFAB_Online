#pragma once

#include <chrono>

#include <Events/PacketEvent.h>
#include <TiltedCore/Stl.hpp>

struct World;
struct UpdateEvent;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct CharacterSpawnedEvent;
struct RequestActorMaxValueChanges;

/**
 * @brief Party-size health scaling for NPCs (server authoritative).
 *
 * Requirement: with N players on the server every NPC has (N + 1) times its
 * vanilla maximum health (1 player -> x1, 2 -> x3, 3 -> x4, ...). The number of
 * enemies never changes - only their health pool.
 *
 * The server owns both the formula and the unscaled base value, so:
 *   * every client sees the same maximum for the same NPC;
 *   * a late joiner gets the scaled value from AssignCharacter/CharacterSpawn;
 *   * when the player count changes the maximum is recomputed from the base,
 *     not from the already scaled value (no drift, no compounding).
 *
 * The unscaled base is remembered per *form*, not per entity. An entity is
 * destroyed when the last player leaves its cell and re-created from another
 * player's (already scaled) copy when someone comes back; capturing the base
 * from the entity alone would then capture a scaled value and compound the
 * multiplier (x3 followed by x3 = x9).
 *
 * Clients only apply what the server tells them (see the client
 * ActorValueService); they never scale on their own.
 */
struct HealthScalingService
{
    HealthScalingService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~HealthScalingService() noexcept = default;

    TP_NOCOPYMOVE(HealthScalingService);

    /**
     * @brief Currently applied multiplier (1.0 when scaling is disabled).
     */
    [[nodiscard]] float GetMultiplier() const noexcept { return m_multiplier; }

    /**
     * @brief Forces a recompute on the next tick (config reload, admin command).
     */
    void MarkDirty() noexcept { m_dirty = true; }

private:
    void OnUpdate(const UpdateEvent& acEvent) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) noexcept;

    //! Recomputes m_multiplier from the player count and publishes it if changed.
    void RefreshMultiplier() noexcept;

    void Recompute() noexcept;

    //! Unscaled maximum health of a form, capturing it the first time.
    float GetOrCaptureBaseHealth(entt::entity aEntity, float aCurrentMax) noexcept;
    void ApplyToCharacter(entt::entity aEntity) noexcept;
    void PublishMultiplier(float aMultiplier) noexcept;

    World& m_world;

    // Form key -> unscaled base maximum health.
    TiltedPhoques::Map<uint64_t, float> m_baseHealthByForm{};

    // Periodic drift correction (respawned actors, RFAB recomputing a maximum
    // on its own, a lost packet). Nothing is sent when nothing changed.
    std::chrono::steady_clock::time_point m_lastValidation{};
    static constexpr std::chrono::seconds kValidationInterval{10};

    float m_multiplier{1.0f};
    uint32_t m_lastPlayerCount{0};
    bool m_dirty{true};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_characterSpawnedConnection;
};
