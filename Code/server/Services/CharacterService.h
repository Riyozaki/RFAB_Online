#pragma once

#include <Events/PacketEvent.h>
#include <Structs/ActorData.h>
#include <CoopWorldView.h>
#include <unordered_set>

struct UpdateEvent;
struct CharacterInteriorCellChangeEvent;
struct CharacterSpawnedEvent;
struct PlayerLeaveEvent;
struct World;
struct Player;
struct AssignCharacterRequest;
struct CharacterSpawnRequest;
struct ClientReferencesMoveRequest;
struct RequestFactionsChanges;
struct GridCellCoords;
struct RequestOwnershipTransfer;
struct CharacterRemoveEvent;
struct CharacterExteriorCellChangeEvent;
struct RequestOwnershipClaim;
struct OwnershipTransferEvent;
struct MountRequest;
struct NewPackageRequest;
struct RequestRespawn;
struct SyncExperienceRequest;
struct SyncRfabExperienceRequest;
struct DialogueRequest;
struct SubtitleRequest;

/**
 * @brief Manages player and actor state.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void Serialize(World& aRegistry, entt::entity aEntity, CharacterSpawnRequest* apSpawnRequest) noexcept;

    void ReconcileEntityForAllObservers(entt::entity aEntity) const noexcept;
    void ReconcileAllEntitiesForObserver(Player* apObserver) const noexcept;
    void ForceResyncObserverAfterRollback(Player* apObserver) const noexcept;
    void ResetWorldEntitiesForCheckpointReload(
        const std::unordered_set<uint64_t>& acCheckpointPersistentKeys,
        bool aHasCheckpointBaseline) const noexcept;
    [[nodiscard]] entt::entity FindPersistentCharacterByFormId(const GameId& acFormId) const noexcept;
    entt::entity EnsureDormantPersistentCharacter(
        const GameId& acFormId,
        const GameId& acBaseId,
        const GameId& acCellId,
        const GameId& acWorldSpaceId,
        const GridCellCoords& acCenterCoords,
        const glm::vec3& acPosition,
        const glm::vec3& acRotation,
        bool aIsDead,
        float aCurrentHealth = 0.0f,
        float aMaxHealth = 0.0f) const noexcept;
    [[nodiscard]] const CoopWorldView::ServerInterestRegistry& GetInterestRegistry() const noexcept { return m_interestRegistry; }

protected:
    void OnUpdate(const UpdateEvent& acEvent) const noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) const noexcept;
    void OnCharacterExteriorCellChange(const CharacterExteriorCellChangeEvent& acEvent) const noexcept;
    void OnCharacterInteriorCellChange(const CharacterInteriorCellChangeEvent& acEvent) const noexcept;
    void OnAssignCharacterRequest(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    void OnOwnershipTransferRequest(const PacketEvent<RequestOwnershipTransfer>& acMessage) const noexcept;
    void OnOwnershipTransferEvent(const OwnershipTransferEvent& acEvent) const noexcept;
    void OnOwnershipClaimRequest(const PacketEvent<RequestOwnershipClaim>& acMessage) const noexcept;
    void OnCharacterRemoveEvent(const CharacterRemoveEvent& acEvent) const noexcept;
    void OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) const noexcept;
    void OnReferencesMoveRequest(const PacketEvent<ClientReferencesMoveRequest>& acMessage) const noexcept;
    void OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept;
    void OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept;
    void OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept;
    void OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept;
    void OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept;
    void OnSyncRfabExperienceRequest(const PacketEvent<SyncRfabExperienceRequest>& acMessage) const noexcept;
    void OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept;
    void OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept;

    void CreateCharacter(const PacketEvent<AssignCharacterRequest>& acMessage) const noexcept;
    void TransferOwnership(Player* apPlayer, const uint32_t acServerId, const ActorData& acActorData) const noexcept;
    ActorData BuildActorData(const entt::entity acEntity) const noexcept;
    void ApplyActorData(const entt::entity acEntity, const ActorData& acActorData) const noexcept;
    void BroadcastActorData(Player* apPlayer, const entt::entity acEntity, const ActorData& acActorData) const noexcept;

    void ProcessFactionsChanges() const noexcept;
    void ProcessMovementChanges() const noexcept;
    void WakeDormantCharactersInActiveRanges() const noexcept;
    void PruneExcessDormantCharacters() const noexcept;

    void OnCharacterIndexConstructed(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnCharacterIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnFormIdIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;

private:
    World& m_world;
    mutable CoopWorldView::ServerInterestRegistry m_interestRegistry{};
    mutable std::unordered_map<uint64_t, entt::entity> m_characterByFormKey{};

    entt::scoped_connection m_charIndexConstructConn;
    entt::scoped_connection m_charIndexDestroyConn;
    entt::scoped_connection m_formIndexDestroyConn;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_playerLeaveConnection;
    entt::scoped_connection m_exteriorCellChangeEventConnection;
    entt::scoped_connection m_interiorCellChangeEventConnection;
    entt::scoped_connection m_characterAssignRequestConnection;
    entt::scoped_connection m_transferOwnershipConnection;
    entt::scoped_connection m_ownershipTransferEventConnection;
    entt::scoped_connection m_claimOwnershipConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_characterSpawnedConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_factionsChangesConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_requestRespawnConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_syncRfabExperienceConnection;
    entt::scoped_connection m_dialogueConnection;
    entt::scoped_connection m_subtitleConnection;
};
