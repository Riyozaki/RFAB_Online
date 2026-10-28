#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace CoopWorldStabilizer
{
    /**
     * Thread-safe registry mapping placed reference FormIDs (`0x00..0xFE`) to their
     * server-authoritative resolved `TESNPC` base FormIDs.
     *
     * Queried inside `HookSpawnActorInWorld` (`TESObjectREFR::UpdateReference3D`) BEFORE
     * Skyrim constructs the actor's 3D `NiNode` and `BShkbAnimationGraph`, ensuring
     * `TESLevCharacter` wildlife/bandit spawns materialize as the exact same variant/species
     * on every client with zero `NiNode` teardown and zero Havok/TBB worker-thread races.
     */
    class AuthoritativePlacedActorRegistry
    {
    public:
        void Record(uint32_t aPlacedRefFormId, uint32_t aAuthoritativeBaseFormId) noexcept
        {
            if (aPlacedRefFormId == 0 || aPlacedRefFormId == 0x14 ||
                (aPlacedRefFormId & 0xFF000000u) == 0xFF000000u ||
                aAuthoritativeBaseFormId == 0 ||
                (aAuthoritativeBaseFormId & 0xFF000000u) == 0xFF000000u)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_baseByPlacedRef.size() > 4096)
                m_baseByPlacedRef.clear();
            m_baseByPlacedRef[aPlacedRefFormId] = aAuthoritativeBaseFormId;
        }

        [[nodiscard]] uint32_t Lookup(uint32_t aPlacedRefFormId) const noexcept
        {
            if (aPlacedRefFormId == 0 || (aPlacedRefFormId & 0xFF000000u) == 0xFF000000u)
                return 0;

            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_baseByPlacedRef.find(aPlacedRefFormId);
            return (it != m_baseByPlacedRef.end()) ? it->second : 0u;
        }

        [[nodiscard]] uint32_t LookupBaseFormId(uint32_t aPlacedRefFormId) const noexcept
        {
            return Lookup(aPlacedRefFormId);
        }

        void Erase(uint32_t aPlacedRefFormId) noexcept
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_baseByPlacedRef.erase(aPlacedRefFormId);
        }

        void Clear() noexcept
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_baseByPlacedRef.clear();
        }

    private:
        mutable std::mutex m_mutex{};
        std::unordered_map<uint32_t, uint32_t> m_baseByPlacedRef{};
    };

    /**
     * Hysteresis tracker for `DiscoveryService::VisitForms()`.
     *
     * Skyrim's `ProcessLists::highActorHandleArray` drops actors the instant they step across
     * the `High <-> Middle-High` AI process boundary (~45-60m), even while the actor remains
     * alive and enabled in an attached loaded cell. Immediately emitting `ActorRemovedEvent`
     * caused both continuous `8433C` server re-assignment storms and `RIP=0x0` teardown crashes
     * when the actor stepped back into `High` 50-150ms later.
     *
     * - Genuine removals (deleted, disabled, or cell detached/unloaded) fire IMMEDIATELY (0ms).
     * - Temporary `High -> Middle-High` demotions and explicit 3D species-rebuild holds retain
     *   discovery membership up to `kDefaultDemotionHoldMs` (`3500ms`).
     */
    class ActorDemotionHoldTracker
    {
    public:
        static constexpr uint64_t kDefaultDemotionHoldMs = 3500ULL;

        void NotifyPresentInHigh(uint32_t aFormId) noexcept
        {
            m_holdUntilMs.erase(aFormId);
        }

        void ArmExplicitHold(uint32_t aFormId, uint64_t aNowMs, uint64_t aDurationMs = kDefaultDemotionHoldMs) noexcept
        {
            if (aFormId == 0)
                return;
            if (m_holdUntilMs.size() > 2048)
                m_holdUntilMs.clear();
            const uint64_t deadline = aNowMs + aDurationMs;
            auto& slot = m_holdUntilMs[aFormId];
            if (deadline > slot)
                slot = deadline;
        }

        [[nodiscard]] bool ShouldRetainMissingFromHigh(
            uint32_t aFormId,
            bool aExistsInAttachedLoadedCell,
            bool aIsDisabledOrDeleted,
            uint64_t aNowMs,
            uint64_t aHoldDurationMs = kDefaultDemotionHoldMs) noexcept
        {
            if (aFormId == 0 || !aExistsInAttachedLoadedCell || aIsDisabledOrDeleted)
            {
                m_holdUntilMs.erase(aFormId);
                return false;
            }

            const auto it = m_holdUntilMs.find(aFormId);
            if (it == m_holdUntilMs.end())
            {
                if (m_holdUntilMs.size() > 2048)
                    m_holdUntilMs.clear();
                m_holdUntilMs[aFormId] = aNowMs + aHoldDurationMs;
                return true;
            }

            if (aNowMs < it->second)
                return true;

            m_holdUntilMs.erase(it);
            return false;
        }

        void Clear() noexcept
        {
            m_holdUntilMs.clear();
        }

    private:
        std::unordered_map<uint32_t, uint64_t> m_holdUntilMs{};
    };

    /**
     * Suppresses redundant non-leader local `0xFF00xxxx` dynamic ambush/wildlife spawns when
     * the Party Leader is co-located in the same interior cell or within 2 exterior cells,
     * while ALWAYS preserving player summons, followers/teammates, players, and solo exploration.
     */
    [[nodiscard]] constexpr bool ShouldSuppressCoLocatedGuestDynamicSpawn(
        bool aIsInParty,
        bool aIsPartyLeader,
        bool aIsTemporaryFormId,
        bool aIsRemoteActor,
        bool aIsPlayerOrClone,
        bool aIsPlayerSummonOrCommanded,
        bool aIsPlayerTeammate,
        bool aIsMount,
        bool aLeaderInSameSpace,
        float aDistToLeaderSq,
        float aCoLocatedRadiusUnits = 8192.0f) noexcept
    {
        if (!aIsInParty || aIsPartyLeader || !aIsTemporaryFormId)
            return false;
        if (aIsRemoteActor || aIsPlayerOrClone || aIsPlayerSummonOrCommanded || aIsPlayerTeammate || aIsMount)
            return false;
        if (!aLeaderInSameSpace)
            return false;
        return aDistToLeaderSq <= (aCoLocatedRadiusUnits * aCoLocatedRadiusUnits);
    }

    /**
     * Evaluates whether a loaded remote placed actor (`0x00..0xFE`) whose 3D already streamed in
     * before `CharacterSpawnRequest` arrived must undergo a protected post-3D species swap
     * (e.g. Wolf -> Mudcrab / Fox) while keeping `DiscoveryService` held across `QueueUpdate()`.
     */
    [[nodiscard]] constexpr bool ShouldForcePost3DSpeciesSwap(
        bool aIsTemporaryActor,
        bool aIsPlayerOrClone,
        bool aHasLocalComponent,
        bool aIsDead,
        bool aIsInCombat,
        uint32_t aCurrentRaceFormId,
        uint32_t aExpectedRaceFormId) noexcept
    {
        if (aIsTemporaryActor || aIsPlayerOrClone || aHasLocalComponent || aIsDead || aIsInCombat)
            return false;
        if (aCurrentRaceFormId == 0 || aExpectedRaceFormId == 0)
            return false;
        return aCurrentRaceFormId != aExpectedRaceFormId;
    }

    /**
     * Suppresses non-leader local `0xFF00xxxx` wild encounter spawns when a party peer is nearby
     * in the same space, while preserving summons, teammates, mounts, network actors, and solo play.
     */
    [[nodiscard]] constexpr bool ShouldSuppressNonLeaderTempWildSpawn(
        bool aIsTemporaryFormId,
        bool aIsLocalPlayer,
        bool aIsInParty,
        bool aIsPartyLeader,
        bool aIsRemoteNetworkCreated,
        bool aIsPlayerSummonOrTeammate,
        bool aIsMount,
        bool aHasNearbyPartyPeer) noexcept
    {
        if (!aIsTemporaryFormId || aIsLocalPlayer || !aIsInParty || aIsPartyLeader)
            return false;
        if (aIsRemoteNetworkCreated || aIsPlayerSummonOrTeammate || aIsMount)
            return false;
        return aHasNearbyPartyPeer;
    }

    /**
     * Preserves `kUseFactions` (`npcTemplate`) and `BGSRefAlias` dynamic quest factions on world NPCs
     * by never calling `RemoveFromAllFactions()` during `Actor::SetFactions` on non-player actors.
     * Remote Player Clones DO clear stale template factions and always receive `PlayerFaction` (`0xDB1`).
     */
    [[nodiscard]] constexpr bool ShouldPreserveExistingNpcFactionsOnSync(
        bool aIsRemotePlayerClone,
        bool aIsLocalPlayer) noexcept
    {
        return !aIsRemotePlayerClone && !aIsLocalPlayer;
    }

    /**
     * Symmetric co-op proximity aggro check for locally-owned active enemies when a Remote Player
     * Clone approaches closely while the owning Host player stands farther back.
     * Never wakes sleeping/furniture Draugr, ethereal/statue actors (`IsGhost`), or scripted scenes.
     */
    [[nodiscard]] constexpr bool ShouldWakeLocalHostileNpcForPeer(
        bool aNpcIsDead,
        bool aNpcIsInCombat,
        bool aNpcIsPlayerOrClone,
        bool aNpcIsTeammateOrCommanded,
        bool aNpcIsGhost,
        bool aNpcHasFurniture,
        bool aNpcIsInScene,
        bool aNpcIsHostileToParty,
        bool aPeerIsDeadOrDowned,
        bool aPeerIsSneaking,
        bool aPeerIsGhost,
        float aPlanarDistSq,
        float aAbsDeltaZ,
        float aMaxPlanarRadiusUnits = 570.0f,
        float aMaxVerticalDeltaUnits = 220.0f) noexcept
    {
        if (aNpcIsDead || aNpcIsInCombat || aNpcIsPlayerOrClone || aNpcIsTeammateOrCommanded)
            return false;
        if (aNpcIsGhost || aNpcHasFurniture || aNpcIsInScene || !aNpcIsHostileToParty)
            return false;
        if (aPeerIsDeadOrDowned || aPeerIsSneaking || aPeerIsGhost)
            return false;
        if (aAbsDeltaZ > aMaxVerticalDeltaUnits)
            return false;
        return aPlanarDistSq <= (aMaxPlanarRadiusUnits * aMaxPlanarRadiusUnits);
    }

    /**
     * Freezes continuous `ForcePosition` / `SetRotation` / `LoadAnimationVariables` on dead ragdolls
     * so the `TESObjectREFR` loot-activation bounding box stays 1:1 aligned with the local Havok
     * ragdoll mesh and `BShkbAnimationGraph` is never mutated during ragdoll teardown.
     */
    [[nodiscard]] constexpr bool ShouldFreezeCorpseInterpolation(bool aIsDead) noexcept
    {
        return aIsDead;
    }

    /**
     * Filters `ObjectService` activation packets:
     * 1) Actors/corpses (`aIsActor`) sync loot via `InventoryService` (`ServerId`), never `ObjectService`.
     * 2) Temporary `0xFF00xxxx` forms (`aIsTempFormId`) have no static plugin `ModId` across clients.
     * 3) Load Doors (`aIsTeleportLoadDoor`, `ExtraDataType::Teleport`) teleport their activator to
     *    another cell — broadcasting `Activate` to peers yanked peers into adjacent cells!
     */
    [[nodiscard]] constexpr bool ShouldBroadcastObjectActivation(
        bool aIsActor,
        bool aIsTempFormId,
        bool aIsTeleportLoadDoor) noexcept
    {
        return !aIsActor && !aIsTempFormId && !aIsTeleportLoadDoor;
    }

    [[nodiscard]] constexpr bool ShouldExecuteRemoteObjectActivate(
        bool aIsActor,
        bool aIsTeleportLoadDoor) noexcept
    {
        return !aIsActor && !aIsTeleportLoadDoor;
    }

    /**
     * Skyrim SE `BGSSaveLoadManager::QueueSaveLoadTask` bitmask classifier for Quickload (`F9`).
     * Covers vanilla `0x20` (`kQuickload`), `0x80` (`kLoadMostRecent`), and EngineFixes custom masks
     * (`0xD0000100`, `0xD0000200`) even when no vanilla `Quicksave0_*.ess` exists on disk.
     */
    [[nodiscard]] constexpr bool IsQuickloadTaskMask(uint32_t aTaskMask) noexcept
    {
        if (aTaskMask == 0xD0000100u || aTaskMask == 0xD0000200u)
            return true;
        // Vanilla BGSSaveLoadManager task flags have top nibble 0 (unlike SkyrimSoulsRE 0xF0004000).
        if ((aTaskMask & 0xF0000000u) == 0u && (aTaskMask & (0x20u | 0x80u)) != 0u)
            return true;
        return false;
    }

    [[nodiscard]] constexpr bool ShouldRetryTransientRejectedSave(
        uint32_t aCompletedAttempts,
        uint32_t aMaxAttempts = 8u) noexcept
    {
        return aCompletedAttempts < aMaxAttempts;
    }

    /**
     * Ensures lethal blows dealt on a non-owner client carry a bounded overkill margin past Skyrim's
     * `0.0f` health clamp so 50-250ms of combat health regeneration on the authoritative owner never
     * leaves an enemy alive at `0.5-2.0` HP on one screen while dead on the attacker's screen.
     */
    [[nodiscard]] inline float EnsureLethalOverkillDelta(
        float aClampedActualDelta,
        float aintendedNegativeDelta,
        float aHealthBeforeHit,
        bool aTargetDiedOrReachedZeroHp) noexcept
    {
        if (!std::isfinite(aClampedActualDelta))
            return 0.0f;
        if (!aTargetDiedOrReachedZeroHp || aClampedActualDelta >= 0.0f)
            return aClampedActualDelta;

        const float regenSafetyMargin = std::clamp(aHealthBeforeHit * 0.20f, 6.0f, 35.0f);
        float lethalDelta = aClampedActualDelta - regenSafetyMargin;
        if (std::isfinite(aintendedNegativeDelta) && aintendedNegativeDelta < lethalDelta)
            lethalDelta = aintendedNegativeDelta;
        return lethalDelta;
    }

    /**
     * Reconciles premature local death on a non-owner client (e.g. local physics rockfall/trap)
     * when the authoritative owner reports the NPC is alive with substantial HP outside any recent
     * local player predicted-hit window.
     */
    [[nodiscard]] constexpr bool ShouldResurrectPrematureLocalRemoteDeath(
        bool aIsRemoteNonPlayer,
        bool aLocalIsDead,
        bool aServerIsDead,
        float aAuthoritativeHp,
        uint64_t aNowMs,
        uint64_t aLastLocalPredictedHitMs,
        uint64_t aPredictedHitGraceMs = 3000ULL) noexcept
    {
        if (!aIsRemoteNonPlayer || !aLocalIsDead || aServerIsDead)
            return false;
        if (!(aAuthoritativeHp > 5.0f))
            return false;
        if (aLastLocalPredictedHitMs != 0 && aNowMs >= aLastLocalPredictedHitMs &&
            (aNowMs - aLastLocalPredictedHitMs) < aPredictedHitGraceMs)
        {
            return false;
        }
        return true;
    }
} // namespace CoopWorldStabilizer
