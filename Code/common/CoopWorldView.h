#pragma once

#include <Structs/GameId.h>
#include <Structs/GridCellCoords.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace CoopWorldView
{
// Size of one Skyrim exterior cell in game units.
constexpr float kExteriorCellSizeUnits = 4096.0f;

// Entry margin inside the outer border of the 5x5 uGridsToLoad window (~5.4m).
// Once materialized inside this margin, a remote FormView stays materialized all the way
// to the outer edge of the 5x5 loaded cells, eliminating boundary oscillation / FormID churn.
constexpr float kExteriorEntryMarginUnits = 384.0f;

// Teleport snap threshold (~29m) matching InterpolationSystem.
constexpr float kCrossPositionSnapDistSq = 2048.0f * 2048.0f;

// Short grace hold when an already-materialized remote human player crosses a load door right ahead of us,
// preventing 1-ms Delete() + CreateInCell() FormView churn when party teammates enter/exit doors together.
constexpr uint64_t kRemotePlayerDoorTransitionGraceMs = 350ULL;

struct WorldOrCell
{
    GameId WorldSpaceId{};
    GameId CellId{};
    GridCellCoords GridCoords{};

    [[nodiscard]] bool IsInterior() const noexcept { return !static_cast<bool>(WorldSpaceId); }
    [[nodiscard]] bool IsExterior() const noexcept { return static_cast<bool>(WorldSpaceId); }

    [[nodiscard]] static bool HasValidGrid(const GridCellCoords& acCoords) noexcept
    {
        return acCoords.X != std::numeric_limits<int32_t>::max() &&
               acCoords.Y != std::numeric_limits<int32_t>::max();
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        if (IsExterior())
            return HasValidGrid(GridCoords);

        // Interior cells MUST have a non-zero static GameId (never 0:0 and never 0xFFFFFFFF client-temp ID).
        return static_cast<bool>(CellId) && CellId.ModId != 0xFFFFFFFFu;
    }

    [[nodiscard]] static WorldOrCell MakeInterior(const GameId& acCellId) noexcept
    {
        WorldOrCell loc{};
        loc.CellId = acCellId;
        loc.GridCoords.Reset();
        return loc;
    }

    [[nodiscard]] static WorldOrCell MakeExterior(
        const GameId& acWorldSpaceId,
        const GameId& acCellId,
        const GridCellCoords& acGridCoords) noexcept
    {
        WorldOrCell loc{};
        loc.WorldSpaceId = acWorldSpaceId;
        loc.CellId = (acCellId.ModId == 0xFFFFFFFFu) ? GameId{} : acCellId;
        loc.GridCoords = acGridCoords;
        return loc;
    }

    [[nodiscard]] static WorldOrCell MakeExterior(
        const GameId& acWorldSpaceId,
        const GridCellCoords& acGridCoords) noexcept
    {
        return MakeExterior(acWorldSpaceId, GameId{}, acGridCoords);
    }

    [[nodiscard]] static WorldOrCell FromPacket(
        const GameId& acWorldSpaceId,
        const GameId& acCellId,
        float aPosX,
        float aPosY) noexcept
    {
        if (static_cast<bool>(acWorldSpaceId))
        {
            const GridCellCoords coords =
                (std::isfinite(aPosX) && std::isfinite(aPosY))
                    ? GridCellCoords::CalculateGridCellCoords(aPosX, aPosY)
                    : GridCellCoords{};
            return MakeExterior(acWorldSpaceId, acCellId, coords);
        }

        return MakeInterior(acCellId);
    }

    // Strict topological space equality (same interior CellId, OR same exterior WorldSpaceId).
    [[nodiscard]] bool IsSameSpace(const WorldOrCell& acOther) const noexcept
    {
        if (!IsValid() || !acOther.IsValid())
            return false;

        if (IsInterior() != acOther.IsInterior())
            return false;

        if (IsInterior())
            return CellId == acOther.CellId;

        return WorldSpaceId == acOther.WorldSpaceId;
    }

    bool operator==(const WorldOrCell& acOther) const noexcept
    {
        if (IsInterior() != acOther.IsInterior())
            return false;

        if (IsInterior())
            return CellId == acOther.CellId;

        return WorldSpaceId == acOther.WorldSpaceId &&
               GridCoords == acOther.GridCoords;
    }

    bool operator!=(const WorldOrCell& acOther) const noexcept
    {
        return !(*this == acOther);
    }
};

struct ObserverState
{
    bool EngineReady{false};
    WorldOrCell Location{}; // For exteriors: Location.GridCoords holds TES::centerGridX / centerGridY.
    float PosX{0.0f};
    float PosY{0.0f};
};

struct RemoteViewQuery
{
    WorldOrCell Location{}; // For exteriors: Location.GridCoords holds CalculateGridCellCoords(PosX, PosY).
    float PosX{0.0f};
    float PosY{0.0f};
    bool IsDragon{false};
    bool IsPlayer{false};
    bool CurrentlyMaterialized{false};
    bool TargetCellLoadedInEngine{false};
    uint64_t NowSteadyMs{0};
    uint64_t DematerializeGraceUntilMs{0};
};

enum class ViewDecision : uint8_t
{
    kKeepDematerialized = 0,
    kMaterialize = 1,
    kKeepMaterialized = 2,
    kDematerialize = 3
};

[[nodiscard]] inline bool IsInsideExteriorEntryMargin(
    const GridCellCoords& acCenterGrid,
    float aRemotePosX,
    float aRemotePosY,
    bool aIsDragon) noexcept
{
    if (!std::isfinite(aRemotePosX) || !std::isfinite(aRemotePosY))
        return false;

    if (aIsDragon)
        return true;

    const int32_t halfGrids = GridCellCoords::m_gridsToLoad / 2; // 2 for 5x5 uGridsToLoad
    const float minBoundX = static_cast<float>(acCenterGrid.X - halfGrids) * kExteriorCellSizeUnits + kExteriorEntryMarginUnits;
    const float maxBoundX = static_cast<float>(acCenterGrid.X + halfGrids + 1) * kExteriorCellSizeUnits - kExteriorEntryMarginUnits;
    const float minBoundY = static_cast<float>(acCenterGrid.Y - halfGrids) * kExteriorCellSizeUnits + kExteriorEntryMarginUnits;
    const float maxBoundY = static_cast<float>(acCenterGrid.Y + halfGrids + 1) * kExteriorCellSizeUnits - kExteriorEntryMarginUnits;

    return aRemotePosX >= minBoundX && aRemotePosX <= maxBoundX &&
           aRemotePosY >= minBoundY && aRemotePosY <= maxBoundY;
}

[[nodiscard]] inline ViewDecision EvaluateViewDecision(
    const ObserverState& acObserver,
    const RemoteViewQuery& acQuery) noexcept
{
    const auto dematerializeOrStay = [&]() noexcept -> ViewDecision
    {
        return acQuery.CurrentlyMaterialized ? ViewDecision::kDematerialize : ViewDecision::kKeepDematerialized;
    };

    if (!acObserver.EngineReady || !acObserver.Location.IsValid() || !acQuery.Location.IsValid())
        return dematerializeOrStay();

    // Hard topological barrier:
    // - Exterior entities NEVER materialize inside interiors (prevents Mammoth/Giant in bandit forts).
    // - Interior entities NEVER materialize in exteriors or other interiors.
    // - Entities from another WorldSpace (e.g. Tamriel 0:3C vs WhiterunWorld 0:1A26F) NEVER leak across worldspaces.
    // Exception: an ALREADY-materialized remote human player crossing a load door holds its existing FormView
    // for up to 350ms while our local cell remains live, avoiding 1-ms Delete()+CreateInCell() churn when teammates
    // step through the same door together.
    if (!acObserver.Location.IsSameSpace(acQuery.Location))
    {
        if (acQuery.CurrentlyMaterialized &&
            acQuery.IsPlayer &&
            acQuery.NowSteadyMs != 0 &&
            acQuery.NowSteadyMs < acQuery.DematerializeGraceUntilMs)
        {
            return ViewDecision::kKeepMaterialized;
        }
        return dematerializeOrStay();
    }

    // Never materialize or keep an actor in an engine cell whose loadedCellData is null (prevents +0x23D000 crash).
    if (!acQuery.TargetCellLoadedInEngine)
        return dematerializeOrStay();

    if (acObserver.Location.IsInterior())
    {
        return acQuery.CurrentlyMaterialized ? ViewDecision::kKeepMaterialized : ViewDecision::kMaterialize;
    }

    // Exterior space: verify 5x5 uGridsToLoad membership first.
    if (!GridCellCoords::IsCellInGridCell(acQuery.Location.GridCoords, acObserver.Location.GridCoords, acQuery.IsDragon))
        return dematerializeOrStay();

    // If already materialized, hold the FormView across the full loaded 5x5 window without edge flicker.
    if (acQuery.CurrentlyMaterialized)
        return ViewDecision::kKeepMaterialized;

    // Require stepping inside the 384-unit inner margin of the 5x5 loaded box before creating a new FormView
    // (unless already in the inner 3x3 cells around centerGrid, which is guaranteed well inside the margin).
    const int32_t dx = std::abs(acQuery.Location.GridCoords.X - acObserver.Location.GridCoords.X);
    const int32_t dy = std::abs(acQuery.Location.GridCoords.Y - acObserver.Location.GridCoords.Y);
    if (dx <= 1 && dy <= 1)
        return ViewDecision::kMaterialize;

    if (!IsInsideExteriorEntryMargin(acObserver.Location.GridCoords, acQuery.PosX, acQuery.PosY, acQuery.IsDragon))
        return ViewDecision::kKeepDematerialized;

    return ViewDecision::kMaterialize;
}

struct MovementTransition
{
    bool HasAuthoritativeLocation{false};
    bool SpaceChanged{false};
    bool LargePositionJump{false};
    bool CompatibleWithObserver{false};
    bool FlushInterpolationHistory{false};
};

[[nodiscard]] inline MovementTransition EvaluateMovementTransition(
    const ObserverState& acObserver,
    const WorldOrCell& acPreviousRemoteLoc,
    float aPrevPosX,
    float aPrevPosY,
    float aPrevPosZ,
    const WorldOrCell& acIncomingLoc,
    float aNewPosX,
    float aNewPosY,
    float aNewPosZ) noexcept
{
    MovementTransition result{};
    result.HasAuthoritativeLocation = acIncomingLoc.IsValid();

    const WorldOrCell& effectiveLoc = result.HasAuthoritativeLocation ? acIncomingLoc : acPreviousRemoteLoc;

    if (result.HasAuthoritativeLocation && acPreviousRemoteLoc.IsValid())
        result.SpaceChanged = !acPreviousRemoteLoc.IsSameSpace(acIncomingLoc);
    else if (result.HasAuthoritativeLocation && !acPreviousRemoteLoc.IsValid())
        result.SpaceChanged = true;

    const float dx = aNewPosX - aPrevPosX;
    const float dy = aNewPosY - aPrevPosY;
    const float dz = aNewPosZ - aPrevPosZ;
    const float distSq = dx * dx + dy * dy + dz * dz;
    result.LargePositionJump = !std::isfinite(distSq) || (distSq > kCrossPositionSnapDistSq);

    result.CompatibleWithObserver =
        acObserver.EngineReady &&
        acObserver.Location.IsValid() &&
        effectiveLoc.IsValid() &&
        acObserver.Location.IsSameSpace(effectiveLoc);

    if (result.CompatibleWithObserver && effectiveLoc.IsExterior())
    {
        result.CompatibleWithObserver =
            GridCellCoords::IsCellInGridCell(effectiveLoc.GridCoords, acObserver.Location.GridCoords, false);
    }

    // Never interpolate between coordinates belonging to two different cells/worldspaces or across fast-travel jumps!
    result.FlushInterpolationHistory = result.SpaceChanged || !result.CompatibleWithObserver || result.LargePositionJump;
    return result;
}

// Server-side per-observer replication state machine:
// - Human player models stay persistently known on peers (never wiped by NotifyRemoveCharacter on cell/grid shifts),
//   while receiving a fresh CharacterSpawnRequest whenever transitioning from out-of-range -> in-range!
// - Non-player entities (NPCs, summons) send CharacterSpawnRequest on enter-range and NotifyRemoveCharacter on exit-range.
enum class ServerPeerSyncAction : uint8_t
{
    kNone = 0,
    kSendSpawn = 1,
    kSendRemove = 2,
    kSendPlayerCellChangedOnly = 3
};

class ServerInterestRegistry
{
public:
    void Clear() noexcept { m_inRangeByObserver.clear(); }

    void RemoveObserver(uint32_t aObserverPlayerId) noexcept
    {
        m_inRangeByObserver.erase(aObserverPlayerId);
    }

    void ForgetEntityEverywhere(uint32_t aServerId) noexcept
    {
        for (auto& [_, set] : m_inRangeByObserver)
            set.erase(aServerId);
    }

    [[nodiscard]] bool IsMarkedInRange(uint32_t aObserverPlayerId, uint32_t aServerId) const noexcept
    {
        const auto it = m_inRangeByObserver.find(aObserverPlayerId);
        if (it == m_inRangeByObserver.end())
            return false;
        return it->second.find(aServerId) != it->second.end();
    }

    void MarkInRange(uint32_t aObserverPlayerId, uint32_t aServerId)
    {
        if (aObserverPlayerId != 0)
            m_inRangeByObserver[aObserverPlayerId].insert(aServerId);
    }

    void UnmarkInRange(uint32_t aObserverPlayerId, uint32_t aServerId) noexcept
    {
        const auto it = m_inRangeByObserver.find(aObserverPlayerId);
        if (it != m_inRangeByObserver.end())
            it->second.erase(aServerId);
    }

    [[nodiscard]] ServerPeerSyncAction UpdateInterest(
        uint32_t aObserverPlayerId,
        uint32_t aServerId,
        bool aEntityIsPlayer,
        bool aNowInRange)
    {
        if (aObserverPlayerId == 0)
            return ServerPeerSyncAction::kNone;

        const bool wasInRange = IsMarkedInRange(aObserverPlayerId, aServerId);
        if (aNowInRange && !wasInRange)
        {
            MarkInRange(aObserverPlayerId, aServerId);
            return ServerPeerSyncAction::kSendSpawn;
        }

        if (!aNowInRange && wasInRange)
        {
            UnmarkInRange(aObserverPlayerId, aServerId);
            return aEntityIsPlayer
                ? ServerPeerSyncAction::kSendPlayerCellChangedOnly
                : ServerPeerSyncAction::kSendRemove;
        }

        return ServerPeerSyncAction::kNone;
    }

private:
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> m_inRangeByObserver{};
};

// Fixed-capacity ring buffer tracking recently cancelled AssignCharacterRequest cookies so
// in-flight AssignCharacterResponse packets arriving 1 RTT after cancellation are cleanly absorbed.
template <size_t Capacity = 64>
class CancelledCookieTracker
{
public:
    void RecordCancelled(uint32_t aCookie) noexcept
    {
        for (size_t i = 0; i < Capacity; ++i)
        {
            if (m_valid[i] && m_entries[i] == aCookie)
                return;
        }
        m_entries[m_cursor % Capacity] = aCookie;
        m_valid[m_cursor % Capacity] = true;
        ++m_cursor;
    }

    [[nodiscard]] bool ConsumeIfCancelled(uint32_t aCookie) noexcept
    {
        for (size_t i = 0; i < Capacity; ++i)
        {
            if (m_valid[i] && m_entries[i] == aCookie)
            {
                m_valid[i] = false;
                return true;
            }
        }
        return false;
    }

    void Clear() noexcept
    {
        m_valid.fill(false);
        m_cursor = 0;
    }

private:
    std::array<uint32_t, Capacity> m_entries{};
    std::array<bool, Capacity> m_valid{};
    size_t m_cursor{0};
};

[[nodiscard]] constexpr uint64_t PackGameIdKey(uint32_t aModId, uint32_t aBaseId) noexcept
{
    return (static_cast<uint64_t>(aModId) << 32) | static_cast<uint64_t>(aBaseId);
}

// Deterministic, case-normalized player identity key so server checkpoints survive mid-session
// reconnects (where ephemeral Player::GetId() increments) and server restarts (where join order changes).
[[nodiscard]] inline std::string BuildPlayerIdentityKey(
    uint64_t aDiscordId,
    std::string_view aUsername,
    uint32_t aFallbackPlayerId = 0)
{
    if (aDiscordId != 0)
        return "discord:" + std::to_string(aDiscordId);

    size_t first = 0;
    while (first < aUsername.size() && static_cast<unsigned char>(aUsername[first]) <= 0x20u)
        ++first;

    size_t last = aUsername.size();
    while (last > first && static_cast<unsigned char>(aUsername[last - 1]) <= 0x20u)
        --last;

    if (last > first)
    {
        std::string norm;
        norm.reserve(5 + (last - first));
        norm.append("name:");
        for (size_t i = first; i < last; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(aUsername[i]);
            if (ch >= 'A' && ch <= 'Z')
                norm.push_back(static_cast<char>(ch + ('a' - 'A')));
            else
                norm.push_back(static_cast<char>(ch));
        }
        return norm;
    }

    return "pid:" + std::to_string(aFallbackPlayerId);
}

// Exact bidirectional O(1) ECS entity index maintaining:
//   - Actor FormId  <-> Entity
//   - Object FormId <-> Entity
//   - ServerId      <-> Entity across overlapping (LocalComponent, RemoteComponent, ObjectComponent) lifecycles.
// Guarantees that `TakeOwnership` (emplace Local before removing Remote) and `OnNotifyMount` (emplace Remote before
// removing Local) NEVER drop the live ServerId mapping, and `on_update` ALWAYS erases superseded keys.
template <class TEntity, TEntity NullEntity>
class EntityIndexTable
{
public:
    enum ServerBindingKind : uint8_t
    {
        kNone = 0,
        kLocal = 1u << 0,
        kRemote = 1u << 1,
        kObject = 1u << 2,
    };

    void Clear() noexcept
    {
        m_actorByFormId.clear();
        m_objectByFormId.clear();
        m_formByEntity.clear();
        m_entityByServerId.clear();
        m_serverByEntity.clear();
    }

    void UpsertFormId(TEntity aEntity, uint32_t aFormId, bool aIsObject)
    {
        if (aEntity == NullEntity)
            return;

        EraseFormId(aEntity);
        if (aFormId == 0)
            return;

        m_formByEntity[aEntity] = FormReverseEntry{aFormId, aIsObject};
        if (aIsObject)
            m_objectByFormId[aFormId] = aEntity;
        else
            m_actorByFormId[aFormId] = aEntity;
    }

    void PromoteFormToObject(TEntity aEntity, uint32_t aFormId)
    {
        if (aEntity == NullEntity || aFormId == 0)
            return;
        UpsertFormId(aEntity, aFormId, true);
    }

    void EraseFormId(TEntity aEntity) noexcept
    {
        const auto revIt = m_formByEntity.find(aEntity);
        if (revIt == m_formByEntity.end())
            return;

        const uint32_t oldFormId = revIt->second.FormId;
        if (revIt->second.IsObject)
        {
            const auto fwdIt = m_objectByFormId.find(oldFormId);
            if (fwdIt != m_objectByFormId.end() && fwdIt->second == aEntity)
                m_objectByFormId.erase(fwdIt);
        }
        else
        {
            const auto fwdIt = m_actorByFormId.find(oldFormId);
            if (fwdIt != m_actorByFormId.end() && fwdIt->second == aEntity)
                m_actorByFormId.erase(fwdIt);
        }
        m_formByEntity.erase(revIt);
    }

    void BindServerId(TEntity aEntity, ServerBindingKind aKind, uint32_t aServerId)
    {
        if (aEntity == NullEntity || aKind == kNone)
            return;

        auto& slot = m_serverByEntity[aEntity];
        uint32_t prevIdForKind = 0;
        bool hadKind = false;
        if (aKind == kLocal)
        {
            hadKind = (slot.ActiveMask & kLocal) != 0;
            prevIdForKind = slot.LocalId;
            slot.LocalId = aServerId;
        }
        else if (aKind == kRemote)
        {
            hadKind = (slot.ActiveMask & kRemote) != 0;
            prevIdForKind = slot.RemoteId;
            slot.RemoteId = aServerId;
        }
        else if (aKind == kObject)
        {
            hadKind = (slot.ActiveMask & kObject) != 0;
            prevIdForKind = slot.ObjectId;
            slot.ObjectId = aServerId;
        }
        slot.ActiveMask = static_cast<uint8_t>(slot.ActiveMask | aKind);

        if (hadKind && prevIdForKind != aServerId && !SlotStillReferencesId(slot, prevIdForKind))
        {
            const auto oldFwd = m_entityByServerId.find(prevIdForKind);
            if (oldFwd != m_entityByServerId.end() && oldFwd->second == aEntity)
                m_entityByServerId.erase(oldFwd);
        }

        m_entityByServerId[aServerId] = aEntity;
    }

    void UnbindServerId(TEntity aEntity, ServerBindingKind aKind) noexcept
    {
        if (aEntity == NullEntity || aKind == kNone)
            return;

        const auto revIt = m_serverByEntity.find(aEntity);
        if (revIt == m_serverByEntity.end())
            return;

        auto& slot = revIt->second;
        if ((slot.ActiveMask & aKind) == 0)
            return;

        uint32_t removedId = 0;
        if (aKind == kLocal)
        {
            removedId = slot.LocalId;
            slot.LocalId = 0;
        }
        else if (aKind == kRemote)
        {
            removedId = slot.RemoteId;
            slot.RemoteId = 0;
        }
        else if (aKind == kObject)
        {
            removedId = slot.ObjectId;
            slot.ObjectId = 0;
        }
        slot.ActiveMask = static_cast<uint8_t>(slot.ActiveMask & ~aKind);

        if (!SlotStillReferencesId(slot, removedId))
        {
            const auto fwdIt = m_entityByServerId.find(removedId);
            if (fwdIt != m_entityByServerId.end() && fwdIt->second == aEntity)
                m_entityByServerId.erase(fwdIt);
        }

        if (slot.ActiveMask == 0)
            m_serverByEntity.erase(revIt);
    }

    [[nodiscard]] TEntity FindActorByFormId(uint32_t aFormId) const noexcept
    {
        if (aFormId == 0)
            return NullEntity;
        const auto it = m_actorByFormId.find(aFormId);
        return it != m_actorByFormId.end() ? it->second : NullEntity;
    }

    [[nodiscard]] TEntity FindObjectByFormId(uint32_t aFormId) const noexcept
    {
        if (aFormId == 0)
            return NullEntity;
        const auto it = m_objectByFormId.find(aFormId);
        return it != m_objectByFormId.end() ? it->second : NullEntity;
    }

    [[nodiscard]] TEntity FindAnyByFormId(uint32_t aFormId) const noexcept
    {
        if (const TEntity actor = FindActorByFormId(aFormId); actor != NullEntity)
            return actor;
        return FindObjectByFormId(aFormId);
    }

    [[nodiscard]] TEntity FindByServerId(uint32_t aServerId) const noexcept
    {
        const auto it = m_entityByServerId.find(aServerId);
        return it != m_entityByServerId.end() ? it->second : NullEntity;
    }

private:
    struct FormReverseEntry
    {
        uint32_t FormId{0};
        bool IsObject{false};
    };

    struct ServerReverseSlot
    {
        uint32_t LocalId{0};
        uint32_t RemoteId{0};
        uint32_t ObjectId{0};
        uint8_t ActiveMask{0};
    };

    [[nodiscard]] static constexpr bool SlotStillReferencesId(const ServerReverseSlot& acSlot, uint32_t aId) noexcept
    {
        if (((acSlot.ActiveMask & kLocal) != 0) && acSlot.LocalId == aId)
            return true;
        if (((acSlot.ActiveMask & kRemote) != 0) && acSlot.RemoteId == aId)
            return true;
        if (((acSlot.ActiveMask & kObject) != 0) && acSlot.ObjectId == aId)
            return true;
        return false;
    }

    std::unordered_map<uint32_t, TEntity> m_actorByFormId{};
    std::unordered_map<uint32_t, TEntity> m_objectByFormId{};
    std::unordered_map<TEntity, FormReverseEntry> m_formByEntity{};
    std::unordered_map<uint32_t, TEntity> m_entityByServerId{};
    std::unordered_map<TEntity, ServerReverseSlot> m_serverByEntity{};
};

} // namespace CoopWorldView
