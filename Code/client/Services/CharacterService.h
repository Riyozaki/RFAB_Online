#pragma once
#include "Structs/Inventory.h"
#include "Structs/ActorData.h"
#include <Messages/CharacterSpawnRequest.h>
#include <CoopOwnership.h>
#include <CoopWorldView.h>

struct ActorAddedEvent;
struct ActorRemovedEvent;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct EquipmentChangeEvent;
struct FormIdComponent;
struct ActionEvent;
struct AssignCharacterResponse;
struct CharacterSpawnRequest;
struct ServerReferencesMoveRequest;
struct NotifyInventoryChanges;
struct NotifyFactionsChanges;
struct NotifyPlayerCellChanged;
struct NotifyPlayerLeft;
struct NotifyRemoveCharacter;
struct NotifySpawnData;
struct NotifyOwnershipTransfer;
struct SpellCastEvent;
struct NotifySpellCast;
struct InterruptCastEvent;
struct NotifyInterruptCast;
struct AddTargetEvent;
struct NotifyAddTarget;
struct ProjectileLaunchedEvent;
struct NotifyProjectileLaunch;
struct MountEvent;
struct NotifyMount;
struct InitPackageEvent;
struct NotifyNewPackage;
struct NotifyRespawn;
struct BeastFormChangeEvent;
struct AddExperienceEvent;
struct NotifySyncExperience;
struct NotifySyncRfabExperience;
struct DialogueEvent;
struct NotifyDialogue;
struct SubtitleEvent;
struct NotifySubtitle;
struct NotifyActorTeleport;
struct NotifyRelinquishControl;
struct PartyJoinedEvent;

struct Actor;
struct TESObjectCELL;
struct TESWorldSpace;
struct World;
struct TransportService;

/**
 * @brief Handles actors and players.
 */
struct CharacterService
{
    CharacterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~CharacterService() noexcept = default;

    TP_NOCOPYMOVE(CharacterService);

    static void DeleteTempActor(const uint32_t aFormId) noexcept;

    bool TakeOwnership(const uint32_t acFormId, const uint32_t acServerId, const entt::entity acEntity, uint64_t aOwnershipEpoch = 0) const noexcept;

    void OnActorAdded(const ActorAddedEvent& acEvent) noexcept;
    void OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;
    void OnConnected(const ConnectedEvent& acConnectedEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acDisconnectedEvent) noexcept;
    void NotifyRemoteQuestStageApplied() noexcept;
    void OnAssignCharacter(const AssignCharacterResponse& acMessage) noexcept;
    void OnCharacterSpawn(const CharacterSpawnRequest& acMessage) const noexcept;
    void OnReferencesMoveRequest(const ServerReferencesMoveRequest& acMessage) const noexcept;
    void OnActionEvent(const ActionEvent& acActionEvent) const noexcept;
    void OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept;
    void OnOwnershipTransfer(const NotifyOwnershipTransfer& acMessage) const noexcept;
    void OnRemoveCharacter(const NotifyRemoveCharacter& acMessage) const noexcept;
    void OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept;
    void OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept;
    void OnRemoteSpawnDataReceived(const NotifySpawnData& acEvent) noexcept;
    void OnMountEvent(const MountEvent& acEvent) const noexcept;
    void OnNotifyMount(const NotifyMount& acMessage) const noexcept;
    void OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept;
    void OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept;
    void OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept;
    void OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept;
    void OnAddExperienceEvent(const AddExperienceEvent& acEvent) noexcept;
    void OnNotifySyncExperience(const NotifySyncExperience& acMessage) noexcept;
    void OnNotifySyncRfabExperience(const NotifySyncRfabExperience& acMessage) noexcept;
    void OnDialogueEvent(const DialogueEvent& acEvent) noexcept;
    void OnNotifyDialogue(const NotifyDialogue& acMessage) noexcept;
    void OnSubtitleEvent(const SubtitleEvent& acEvent) noexcept;
    void OnNotifySubtitle(const NotifySubtitle& acMessage) noexcept;
    void OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept;
    void OnNotifyRelinquishControl(const NotifyRelinquishControl& acMessage) noexcept;
    void OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept;

    void ProcessNewEntity(entt::entity aEntity) const noexcept;
    void ApplyAuthoritativeRfabXpDelta(int32_t aDelta) noexcept;
    void ClaimCombatActorsFromDownedPeer(uint32_t aDownedPeerPlayerId) noexcept;
    void NotifySaveLoadReset() noexcept;
    void NotifyInSessionRollbackApplied() noexcept;
    void PreSavePurgeDynamicActors() noexcept;
    void SchedulePostSaveRemoteRestore() noexcept;
    [[nodiscard]] bool IsSavePurgeActive() const noexcept { return m_savePurgeActive; }
    void RestorePurgedRemoteActorsAfterSave() noexcept;
    void PurgeAllDynamicActorsForWorldLoad() noexcept;
    void BeginEngineWorldLoad() noexcept;
    void EndEngineWorldLoad() noexcept;
    [[nodiscard]] bool IsEngineWorldLoading() const noexcept { return m_engineWorldLoadInProgress; }

    [[nodiscard]] entt::entity FindActorEntityByFormId(uint32_t aFormId) const noexcept;
    [[nodiscard]] entt::entity FindAnyEntityByFormId(uint32_t aFormId) const noexcept;
    [[nodiscard]] entt::entity FindEntityByServerId(uint32_t aServerId) const noexcept;

private:
    struct RemoteEntityModel
    {
        CharacterSpawnRequest SpawnRequest{};
        CoopWorldView::WorldOrCell Location{};
        Vector3_NetQuantize LatestPosition{};
        Rotator2_NetQuantize LatestRotation{};
        uint64_t DematerializeGraceUntilMs{0};
        bool IsMaterialized{false};
    };

    struct PendingMountAttach
    {
        uint32_t MountServerId{0};
        uint64_t ExpiresAtMs{0};
    };

    struct PendingLocalMountRequest
    {
        uint32_t RiderFormId{0};
        uint32_t MountFormId{0};
        uint64_t ExpiresAtMs{0};
    };

    void EnsureLocalPlayerRegistered() noexcept;
    void DetachAllActorsForEngineLoad() noexcept;
    void PurgeNetworkActorsOnLiveDisconnect() noexcept;
    bool MoveActor(const Actor* apActor, const GameId& acWorldSpaceId, const GameId& acCellId, const Vector3_NetQuantize& acPosition) const noexcept;
    [[nodiscard]] CoopWorldView::ObserverState BuildLocalObserverState() const noexcept;
    [[nodiscard]] TESObjectCELL* ResolveCompatibleTargetCell(const CoopWorldView::WorldOrCell& acTargetLoc, TESWorldSpace*& apOutWorldSpace, bool aAllowDistantWorldSpaceAnchor = false) const noexcept;
    void DematerializeRemoteView(entt::entity aEntity, RemoteEntityModel* apModel) const noexcept;
    Actor* MaterializeRemoteView(entt::entity aEntity, RemoteEntityModel& aModel, TESObjectCELL* apTargetCell, TESWorldSpace* apTargetWorldSpace) const noexcept;
    void ReconcileRemoteWorldViews() const noexcept;

    bool RequestServerAssignment(entt::entity aEntity) const noexcept;
    void CleanupUnassignedActorEntity(entt::entity aEntity) const noexcept;
    void CancelServerAssignment(entt::entity aEntity, uint32_t aFormId) const noexcept;
    void DeleteRemoteEntityComponents(entt::entity aEntity) const noexcept;

    Actor* CreateCharacterForEntity(entt::entity aEntity) const noexcept;
    ActorData BuildActorData(Actor* apActor) const noexcept;

    void RunLocalUpdates() const noexcept;
    void RunRemoteUpdates() noexcept;
    void RunFactionsUpdates() const noexcept;
    void RunSpawnUpdates() const noexcept;
    void RunExperienceUpdates() noexcept;
    void RunRfabExperienceUpdates() noexcept;
    void ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept;

    void OnFormIdIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnFormIdIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnLocalIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnLocalIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnRemoteIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnRemoteIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnObjectIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept;
    void OnObjectIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept;

    World& m_world;
    entt::dispatcher& m_dispatcher;
    TransportService& m_transport;

    float m_cachedExperience = 0.f;
    int32_t m_lastRfabExperience = 0;
    bool m_rfabExperienceInitialized = false;
    std::chrono::steady_clock::time_point m_remoteQuestXpWindowUntil{};
    int32_t m_localQuestXpGainedInWindow = 0;
    int32_t m_lastAppliedRemoteXpAmount = 0;
    std::chrono::steady_clock::time_point m_lastAppliedRemoteXpTime{};
    int32_t m_creditToRefundOnLocalQuestXp = 0;
    mutable CoopOwnership::ClientEpochTracker m_ownershipEpochTracker{};
    mutable CoopWorldView::CancelledCookieTracker<64> m_cancelledCookies{};
    mutable Map<uint32_t, CharacterSpawnRequest> m_cachedRemoteSpawns{};
    mutable Map<uint32_t, RemoteEntityModel> m_remoteModels{};
    mutable Map<uint32_t, uint32_t> m_playerServerIds{};
    mutable Map<uint32_t, uint32_t> m_playerBaseNpcPool{};
    mutable Set<uint32_t> m_createdNetworkFormIds{};
    mutable Set<uint32_t> m_suppressedPlacedActors{};
    mutable Map<uint32_t, entt::entity> m_actorEntityByFormId{};
    mutable Map<uint32_t, entt::entity> m_objectEntityByFormId{};
    mutable Map<uint32_t, entt::entity> m_entityByServerId{};
    mutable Map<entt::entity, uint8_t> m_pendingLocalTempSpawns{};
    mutable Map<uint32_t, PendingMountAttach> m_pendingRemoteMounts{};
    mutable PendingLocalMountRequest m_pendingLocalMount{};
    bool m_pendingPostSaveRemoteRestore{false};
    bool m_savePurgeActive{false};
    bool m_engineWorldLoadInProgress{false};

    // TODO: revamp this, read the local anim var like vampire lord?
    struct WeaponDrawData
    {
        WeaponDrawData() = default;
        WeaponDrawData(bool aDrawWeapon)
            : m_drawWeapon(aDrawWeapon)
        {
        }

        double m_timer = 0.0;
        bool m_drawWeapon = false;
        bool m_isFirstPass = true;
    };

    mutable Map<uint32_t, WeaponDrawData> m_weaponDrawUpdates{};

    entt::scoped_connection m_referenceAddedConnection;
    entt::scoped_connection m_referenceRemovedConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_actionConnection;
    entt::scoped_connection m_factionsConnection;
    entt::scoped_connection m_ownershipTransferConnection;
    entt::scoped_connection m_removeCharacterConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_assignCharacterConnection;
    entt::scoped_connection m_characterSpawnConnection;
    entt::scoped_connection m_referenceMovementSnapshotConnection;
    entt::scoped_connection m_remoteSpawnDataReceivedConnection;
    entt::scoped_connection m_mountConnection;
    entt::scoped_connection m_notifyMountConnection;
    entt::scoped_connection m_initPackageConnection;
    entt::scoped_connection m_newPackageConnection;
    entt::scoped_connection m_notifyRespawnConnection;
    entt::scoped_connection m_beastFormChangeConnection;
    entt::scoped_connection m_addExperienceEventConnection;
    entt::scoped_connection m_syncExperienceConnection;
    entt::scoped_connection m_syncRfabExperienceConnection;
    entt::scoped_connection m_dialogueEventConnection;
    entt::scoped_connection m_dialogueSyncConnection;
    entt::scoped_connection m_subtitleEventConnection;
    entt::scoped_connection m_subtitleSyncConnection;
    entt::scoped_connection m_actorTeleportConnection;
    entt::scoped_connection m_relinquishConnection;
    entt::scoped_connection m_partyJoinedConnection;
    entt::scoped_connection m_playerCellChangedConnection;
    entt::scoped_connection m_playerLeftConnection;
    entt::scoped_connection m_formIdConstructConn;
    entt::scoped_connection m_formIdUpdateConn;
    entt::scoped_connection m_formIdDestroyConn;
    entt::scoped_connection m_localConstructConn;
    entt::scoped_connection m_localUpdateConn;
    entt::scoped_connection m_localDestroyConn;
    entt::scoped_connection m_remoteConstructConn;
    entt::scoped_connection m_remoteUpdateConn;
    entt::scoped_connection m_remoteDestroyConn;
    entt::scoped_connection m_objectConstructConn;
    entt::scoped_connection m_objectUpdateConn;
    entt::scoped_connection m_objectDestroyConn;
};
