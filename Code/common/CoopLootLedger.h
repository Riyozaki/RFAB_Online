#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace CoopLootLedger
{
struct LootSlotKey
{
    uint32_t ContainerServerId{0};
    uint32_t ModId{0};
    uint32_t BaseId{0};

    bool operator==(const LootSlotKey& acRhs) const noexcept
    {
        return ContainerServerId == acRhs.ContainerServerId &&
               ModId == acRhs.ModId &&
               BaseId == acRhs.BaseId;
    }
};

struct LootSlotKeyHash
{
    size_t operator()(const LootSlotKey& acKey) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(acKey.ContainerServerId) * 0x9E3779B97F4A7C15ULL;
        h ^= (static_cast<uint64_t>(acKey.ModId) << 32) | static_cast<uint64_t>(acKey.BaseId);
        h ^= (h >> 30);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= (h >> 27);
        return static_cast<size_t>(h);
    }
};

struct ClaimResolution
{
    int32_t GrantedTake{0};
    int32_t OverdrawnRollback{0};
};

/**
 * SkyMP-grade server-authoritative container & corpse anti-duplication ledger.
 * Depletion claims persist for the lifetime of the container/corpse (not a short 15s window) so
 * late-arriving peers cannot re-loot dynamic RFAB boss tokens / death drops minutes later, while
 * supporting exact checkpoint snapshot capture/restore, explicit deposits, and container respawns.
 */
class ContainerLootLedger
{
public:
    // 0 means container-lifetime persistence (no time-based expiry); finite >0 honors an explicit window.
    static constexpr uint64_t kPersistentDepletionWindowMs = 0ULL;
    static constexpr uint64_t kDefaultConflictWindowMs = kPersistentDepletionWindowMs;
    static constexpr size_t kMaxTrackedDepletions = 32768;

    struct SnapshotEntry
    {
        uint32_t ContainerServerId{0};
        uint32_t ModId{0};
        uint32_t BaseId{0};
        uint32_t ClaimantPlayerId{0};
        uint64_t DepletedAtMs{0};
    };

    using Snapshot = std::vector<SnapshotEntry>;

    void Clear() noexcept
    {
        m_depletions.clear();
        m_fifoOrder.clear();
        m_nextSequence = 1;
    }

    [[nodiscard]] size_t Size() const noexcept
    {
        return m_depletions.size();
    }

    void RecordDeposit(uint32_t aContainerServerId, uint32_t aModId, uint32_t aBaseId) noexcept
    {
        if (aContainerServerId == 0 || (aModId == 0 && aBaseId == 0))
            return;

        const LootSlotKey key{aContainerServerId, aModId, aBaseId};
        m_depletions.erase(key);
    }

    template <class TGameId>
        requires requires(const TGameId& id) { id.ModId; id.BaseId; }
    void RecordDeposit(uint32_t aContainerServerId, const TGameId& acItemId) noexcept
    {
        if (!acItemId)
            return;
        RecordDeposit(aContainerServerId, acItemId.ModId, acItemId.BaseId);
    }

    void ForgetContainer(uint32_t aContainerServerId) noexcept
    {
        if (aContainerServerId == 0 || m_depletions.empty())
            return;

        for (auto it = m_depletions.begin(); it != m_depletions.end();)
        {
            if (it->first.ContainerServerId == aContainerServerId)
                it = m_depletions.erase(it);
            else
                ++it;
        }
    }

    [[nodiscard]] Snapshot CaptureSnapshot() const
    {
        Snapshot out;
        out.reserve(m_depletions.size());
        for (const auto& fifoItem : m_fifoOrder)
        {
            const auto it = m_depletions.find(fifoItem.Key);
            if (it == m_depletions.end() || it->second.Sequence != fifoItem.Sequence)
                continue;
            out.push_back(SnapshotEntry{
                it->first.ContainerServerId,
                it->first.ModId,
                it->first.BaseId,
                it->second.ClaimantPlayerId,
                it->second.DepletedAtMs});
        }
        return out;
    }

    void RestoreSnapshot(const Snapshot& acSnapshot)
    {
        Clear();
        for (const auto& entry : acSnapshot)
        {
            if (entry.ContainerServerId == 0 || (entry.ModId == 0 && entry.BaseId == 0))
                continue;
            RecordDepletion(
                LootSlotKey{entry.ContainerServerId, entry.ModId, entry.BaseId},
                entry.ClaimantPlayerId,
                entry.DepletedAtMs);
        }
    }

    [[nodiscard]] ClaimResolution EvaluateWithdrawal(
        uint32_t aContainerServerId,
        uint32_t aModId,
        uint32_t aBaseId,
        uint32_t aClaimantPlayerId,
        int32_t aAvailableBefore,
        int32_t aRequestedTake,
        bool aIsQuestItem,
        uint64_t aNowMs,
        uint64_t aConflictWindowMs = kDefaultConflictWindowMs) noexcept
    {
        ClaimResolution res{};
        if (aRequestedTake <= 0)
            return res;

        // Shared co-op keys, dragon claws, and quest items intentionally replicate across party members.
        if (aIsQuestItem || aContainerServerId == 0 || (aModId == 0 && aBaseId == 0) || aClaimantPlayerId == 0)
        {
            res.GrantedTake = aRequestedTake;
            return res;
        }

        if (aConflictWindowMs > 0)
            PruneExpired(aNowMs, aConflictWindowMs);

        const int32_t safeAvailable = std::max(0, aAvailableBefore);
        const LootSlotKey key{aContainerServerId, aModId, aBaseId};

        // Case 1: Server container ledger explicitly holds enough items to cover the withdrawal.
        if (safeAvailable >= aRequestedTake)
        {
            res.GrantedTake = aRequestedTake;
            if (safeAvailable == aRequestedTake)
                RecordDepletion(key, aClaimantPlayerId, aNowMs);
            return res;
        }

        // Case 2: Requested more than currently tracked in `InventoryComponent`.
        const auto it = m_depletions.find(key);
        const bool withinWindow =
            (aConflictWindowMs == 0) ||
            (it != m_depletions.end() && aNowMs >= it->second.DepletedAtMs && (aNowMs - it->second.DepletedAtMs) <= aConflictWindowMs);
        const bool depletedByPeer =
            (it != m_depletions.end()) &&
            (it->second.ClaimantPlayerId != aClaimantPlayerId) &&
            withinWindow;

        if (safeAvailable > 0 || depletedByPeer)
        {
            // Simultaneous QuickLoot / ContainerMenu race OR late-peer re-loot of an already-depleted slot!
            res.GrantedTake = safeAvailable;
            res.OverdrawnRollback = aRequestedTake - safeAvailable;
            if (safeAvailable > 0)
                RecordDepletion(key, aClaimantPlayerId, aNowMs);
            return res;
        }

        // Case 3: `safeAvailable == 0` with no prior peer depletion — dynamic/script-spawned loot not present in initial snapshot.
        // Allow the first player's claim AND stamp the persistent depletion marker so no other peer can dupe it later!
        res.GrantedTake = aRequestedTake;
        res.OverdrawnRollback = 0;
        RecordDepletion(key, aClaimantPlayerId, aNowMs);
        return res;
    }

    template <class TGameId>
        requires requires(const TGameId& id) { id.ModId; id.BaseId; }
    [[nodiscard]] ClaimResolution EvaluateWithdrawal(
        uint32_t aContainerServerId,
        const TGameId& acItemId,
        uint32_t aClaimantPlayerId,
        int32_t aAvailableBefore,
        int32_t aRequestedTake,
        bool aIsQuestItem,
        uint64_t aNowMs,
        uint64_t aConflictWindowMs = kDefaultConflictWindowMs) noexcept
    {
        if (!acItemId)
            return ClaimResolution{aRequestedTake > 0 ? aRequestedTake : 0, 0};
        return EvaluateWithdrawal(
            aContainerServerId,
            acItemId.ModId,
            acItemId.BaseId,
            aClaimantPlayerId,
            aAvailableBefore,
            aRequestedTake,
            aIsQuestItem,
            aNowMs,
            aConflictWindowMs);
    }

private:
    struct DepletionEntry
    {
        uint32_t ClaimantPlayerId{0};
        uint64_t DepletedAtMs{0};
        uint64_t Sequence{0};
    };

    struct FifoItem
    {
        LootSlotKey Key{};
        uint64_t Sequence{0};
    };

    void EvictDeadFrontEntries() noexcept
    {
        while (!m_fifoOrder.empty())
        {
            const auto& front = m_fifoOrder.front();
            const auto it = m_depletions.find(front.Key);
            if (it == m_depletions.end() || it->second.Sequence != front.Sequence)
            {
                m_fifoOrder.pop_front();
                continue;
            }
            break;
        }
    }

    void RecordDepletion(const LootSlotKey& acKey, uint32_t aPlayerId, uint64_t aNowMs)
    {
        const uint64_t seq = m_nextSequence++;
        auto it = m_depletions.find(acKey);
        if (it != m_depletions.end())
        {
            it->second.ClaimantPlayerId = aPlayerId;
            it->second.DepletedAtMs = aNowMs;
            it->second.Sequence = seq;
            m_fifoOrder.push_back(FifoItem{acKey, seq});
            if (m_fifoOrder.size() > kMaxTrackedDepletions * 2)
                EvictDeadFrontEntries();
            return;
        }

        while (m_depletions.size() >= kMaxTrackedDepletions)
        {
            EvictDeadFrontEntries();
            if (m_fifoOrder.empty())
                break;
            m_depletions.erase(m_fifoOrder.front().Key);
            m_fifoOrder.pop_front();
        }

        m_depletions.emplace(acKey, DepletionEntry{aPlayerId, aNowMs, seq});
        m_fifoOrder.push_back(FifoItem{acKey, seq});
    }

    void PruneExpired(uint64_t aNowMs, uint64_t aWindowMs) noexcept
    {
        while (!m_fifoOrder.empty())
        {
            const auto& front = m_fifoOrder.front();
            const auto it = m_depletions.find(front.Key);
            if (it == m_depletions.end() || it->second.Sequence != front.Sequence)
            {
                m_fifoOrder.pop_front();
                continue;
            }

            if (aNowMs >= it->second.DepletedAtMs && (aNowMs - it->second.DepletedAtMs) > aWindowMs)
            {
                m_depletions.erase(it);
                m_fifoOrder.pop_front();
                continue;
            }
            break;
        }
    }

    std::unordered_map<LootSlotKey, DepletionEntry, LootSlotKeyHash> m_depletions{};
    std::deque<FifoItem> m_fifoOrder{};
    uint64_t m_nextSequence{1};
};
} // namespace CoopLootLedger
