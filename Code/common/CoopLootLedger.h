#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <unordered_map>

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
 * Eliminates QuickLootIE / simultaneous ContainerMenu race-condition item duplication across players
 * while remaining 100% transparent to single-looter dynamic/scripted RFAB loot and shared quest items.
 */
class ContainerLootLedger
{
public:
    static constexpr uint64_t kDefaultConflictWindowMs = 15000ULL;
    static constexpr size_t kMaxTrackedDepletions = 4096;

    void Clear() noexcept
    {
        m_depletions.clear();
        m_fifoOrder.clear();
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
        const bool recentlyDepletedByPeer =
            (it != m_depletions.end()) &&
            (it->second.ClaimantPlayerId != aClaimantPlayerId) &&
            (aNowMs >= it->second.DepletedAtMs) &&
            ((aNowMs - it->second.DepletedAtMs) <= aConflictWindowMs);

        if (safeAvailable > 0 || recentlyDepletedByPeer)
        {
            // Simultaneous multi-player QuickLoot / ContainerMenu race condition detected!
            // Grant only the remaining server balance (if any) and roll back the overdrawn duplicate on the late peer.
            res.GrantedTake = safeAvailable;
            res.OverdrawnRollback = aRequestedTake - safeAvailable;
            if (safeAvailable > 0)
                RecordDepletion(key, aClaimantPlayerId, aNowMs);
            return res;
        }

        // Case 3: `safeAvailable == 0` with no prior peer depletion — dynamic/script-spawned loot not present in initial snapshot.
        // Allow the first player's claim AND stamp the depletion marker so a second player clicking 30ms later cannot dupe it!
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
    };

    void RecordDepletion(const LootSlotKey& acKey, uint32_t aPlayerId, uint64_t aNowMs)
    {
        auto it = m_depletions.find(acKey);
        if (it != m_depletions.end())
        {
            it->second.ClaimantPlayerId = aPlayerId;
            it->second.DepletedAtMs = aNowMs;
            return;
        }

        if (m_depletions.size() >= kMaxTrackedDepletions && !m_fifoOrder.empty())
        {
            m_depletions.erase(m_fifoOrder.front());
            m_fifoOrder.pop_front();
        }

        m_depletions.emplace(acKey, DepletionEntry{aPlayerId, aNowMs});
        m_fifoOrder.push_back(acKey);
    }

    void PruneExpired(uint64_t aNowMs, uint64_t aWindowMs) noexcept
    {
        while (!m_fifoOrder.empty())
        {
            const auto it = m_depletions.find(m_fifoOrder.front());
            if (it == m_depletions.end())
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
    std::deque<LootSlotKey> m_fifoOrder{};
};
} // namespace CoopLootLedger
