#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>
#include <TiltedCore/Stl.hpp>

struct World;
struct PlayerLeaveCellEvent;
struct ActivateRequest;
struct LockChangeRequest;
struct AssignObjectsRequest;
struct ScriptAnimationRequest;

/**
 * @brief Manages (interactive) objects and relays interactions with said objects.
 */
class ObjectService
{
public:
    ObjectService(World& aWorld, entt::dispatcher& aDispatcher);

private:
    static constexpr uint64_t PackGameIdKey(const GameId& acId) noexcept
    {
        return (static_cast<uint64_t>(acId.ModId) << 32) | static_cast<uint64_t>(acId.BaseId);
    }

    void OnObjectComponentRemoved(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void PruneExcessUntouchedObjects() noexcept;
    void OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept;
    void OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>&) noexcept;
    void OnActivate(const PacketEvent<ActivateRequest>&) const noexcept;
    void OnLockChange(const PacketEvent<LockChangeRequest>&) noexcept;
    void OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>&) noexcept;

    World& m_world;
    TiltedPhoques::Map<uint64_t, entt::entity> m_objectEntityByKey;

    entt::scoped_connection m_objectRemovedConnection;
    entt::scoped_connection m_leaveCellConnection;
    entt::scoped_connection m_assignObjectConnection;
    entt::scoped_connection m_activateConnection;
    entt::scoped_connection m_lockChangeConnection;
    entt::scoped_connection m_scriptAnimationConnection;
};
