#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace CoopInventoryDiff
{
    enum class EquipActionKind : uint8_t
    {
        None = 0,
        Equip = 1,
        Unequip = 2,
    };

    template <class TEntry>
    struct EquipTransition
    {
        TEntry Item{};
        EquipActionKind Action{EquipActionKind::None};
    };

    template <class TEntry>
    struct InventoryDeltaPlan
    {
        // Entries with negative Count (< 0) to remove from container/actor
        std::vector<TEntry> ItemsToRemove{};
        // Entries with positive Count (> 0) to add (and equip if ExtraWorn/ExtraWornLeft)
        std::vector<TEntry> ItemsToAdd{};
        // Existing items whose stack count stayed > 0, but whose worn state changed
        std::vector<EquipTransition<TEntry>> WornStateChanges{};

        [[nodiscard]] bool IsNoOp() const noexcept
        {
            return ItemsToRemove.empty() && ItemsToAdd.empty() && WornStateChanges.empty();
        }
    };

    template <class TEffect>
    [[nodiscard]] inline bool AreEnchantEffectsEqual(const TEffect& aLhs, const TEffect& aRhs) noexcept
    {
        return aLhs.EffectId == aRhs.EffectId &&
               aLhs.Area == aRhs.Area &&
               aLhs.Duration == aRhs.Duration &&
               std::fabs(aLhs.Magnitude - aRhs.Magnitude) <= 0.05f &&
               std::fabs(aLhs.RawCost - aRhs.RawCost) <= 0.05f;
    }

    /**
     * Compares two Inventory::Entry records ignoring stack Count AND ignoring Worn/WornLeft flags,
     * so equipping or unequipping an already-owned sword/armor piece toggles EquipManager cleanly
     * instead of destroying and recreating the item in ExtraContainerChanges!
     */
    template <class TEntry>
    [[nodiscard]] inline bool ArePhysicalItemsEquivalent(const TEntry& aLhs, const TEntry& aRhs) noexcept
    {
        if (!(aLhs.BaseId == aRhs.BaseId))
            return false;
        if (!(aLhs.ExtraEnchantId == aRhs.ExtraEnchantId))
            return false;
        if (aLhs.ExtraEnchantCharge != aRhs.ExtraEnchantCharge)
            return false;
        if (aLhs.ExtraEnchantRemoveUnequip != aRhs.ExtraEnchantRemoveUnequip)
            return false;
        if (std::fabs(aLhs.ExtraHealth - aRhs.ExtraHealth) > 0.05f)
            return false;
        if (std::fabs(aLhs.ExtraCharge - aRhs.ExtraCharge) > 0.5f)
            return false;
        if (!(aLhs.ExtraPoisonId == aRhs.ExtraPoisonId))
            return false;
        if (aLhs.ExtraPoisonCount != aRhs.ExtraPoisonCount)
            return false;
        if (aLhs.ExtraSoulLevel != aRhs.ExtraSoulLevel)
            return false;
        if (aLhs.EnchantData.IsWeapon != aRhs.EnchantData.IsWeapon)
            return false;
        if (aLhs.EnchantData.Effects.size() != aRhs.EnchantData.Effects.size())
            return false;
        for (size_t i = 0; i < aLhs.EnchantData.Effects.size(); ++i)
        {
            if (!AreEnchantEffectsEqual(aLhs.EnchantData.Effects[i], aRhs.EnchantData.Effects[i]))
                return false;
        }
        return true;
    }

    template <class TEntry, class TRange>
    [[nodiscard]] inline std::vector<TEntry> SquashInventoryEntries(const TRange& acEntries)
    {
        std::vector<TEntry> squashed;
        for (const auto& raw : acEntries)
        {
            if (raw.Count <= 0 || !raw.BaseId)
                continue;

            bool merged = false;
            for (auto& existing : squashed)
            {
                if (ArePhysicalItemsEquivalent(existing, raw))
                {
                    const int64_t sum = static_cast<int64_t>(existing.Count) + static_cast<int64_t>(raw.Count);
                    existing.Count = sum > 1'000'000LL ? 1'000'000 : static_cast<int32_t>(sum);
                    existing.ExtraWorn = existing.ExtraWorn || raw.ExtraWorn;
                    existing.ExtraWornLeft = existing.ExtraWornLeft || raw.ExtraWornLeft;
                    existing.IsQuestItem = existing.IsQuestItem || raw.IsQuestItem;
                    merged = true;
                    break;
                }
            }

            if (!merged)
                squashed.push_back(raw);
        }
        return squashed;
    }

    /**
     * SkyMP-style Differential Inventory Reconciliation:
     * Computes the minimal delta from `acCurrentEntries` -> `acTargetEntries` without calling `RemoveAllItems()`.
     * - Already-matching items and worn Requiem/RFAB Fortify-Health armor remain untouched in ExtraContainerChanges.
     * - When `aRetainQuestItems == true`, existing quest items (`IsQuestItem`) are never removed or duplicated.
     */
    template <class TEntry, class TCurrentRange, class TTargetRange>
    [[nodiscard]] inline InventoryDeltaPlan<TEntry> ComputeInventoryDelta(
        const TCurrentRange& acCurrentEntries,
        const TTargetRange& acTargetEntries,
        bool aRetainQuestItems = false)
    {
        InventoryDeltaPlan<TEntry> plan{};

        const std::vector<TEntry> current = SquashInventoryEntries<TEntry>(acCurrentEntries);
        const std::vector<TEntry> target = SquashInventoryEntries<TEntry>(acTargetEntries);
        std::vector<bool> targetMatched(target.size(), false);

        for (const auto& curEntry : current)
        {
            size_t matchIdx = target.size();
            for (size_t j = 0; j < target.size(); ++j)
            {
                if (!targetMatched[j] && ArePhysicalItemsEquivalent(curEntry, target[j]))
                {
                    matchIdx = j;
                    break;
                }
            }

            if (matchIdx == target.size())
            {
                // Item is not in target inventory at all
                if (aRetainQuestItems && curEntry.IsQuestItem)
                    continue;

                TEntry rem = curEntry;
                rem.Count = -curEntry.Count;
                plan.ItemsToRemove.push_back(std::move(rem));
                continue;
            }

            targetMatched[matchIdx] = true;
            const auto& tgtEntry = target[matchIdx];

            if (aRetainQuestItems && curEntry.IsQuestItem)
                continue;

            if (tgtEntry.Count < curEntry.Count)
            {
                TEntry rem = curEntry;
                rem.Count = tgtEntry.Count - curEntry.Count; // negative delta
                plan.ItemsToRemove.push_back(std::move(rem));
            }
            else if (tgtEntry.Count > curEntry.Count)
            {
                TEntry add = tgtEntry;
                add.Count = tgtEntry.Count - curEntry.Count; // positive delta
                // If already worn in the desired hand, don't re-trigger Equip on stack increase alone
                if (curEntry.ExtraWorn == tgtEntry.ExtraWorn && curEntry.ExtraWornLeft == tgtEntry.ExtraWornLeft)
                {
                    add.ExtraWorn = false;
                    add.ExtraWornLeft = false;
                }
                plan.ItemsToAdd.push_back(std::move(add));
            }

            // Check if worn state changed on an item that remained in inventory!
            const bool curWornAny = curEntry.ExtraWorn || curEntry.ExtraWornLeft;
            const bool tgtWornAny = tgtEntry.ExtraWorn || tgtEntry.ExtraWornLeft;
            if (curEntry.ExtraWorn != tgtEntry.ExtraWorn || curEntry.ExtraWornLeft != tgtEntry.ExtraWornLeft)
            {
                if (tgtWornAny)
                {
                    TEntry eq = tgtEntry;
                    eq.Count = std::max(1, tgtEntry.Count);
                    plan.WornStateChanges.push_back(EquipTransition<TEntry>{std::move(eq), EquipActionKind::Equip});
                }
                else if (curWornAny)
                {
                    TEntry uneq = curEntry;
                    uneq.Count = std::max(1, curEntry.Count);
                    plan.WornStateChanges.push_back(EquipTransition<TEntry>{std::move(uneq), EquipActionKind::Unequip});
                }
            }
        }

        // Add brand-new items present only in target
        for (size_t j = 0; j < target.size(); ++j)
        {
            if (targetMatched[j])
                continue;

            const auto& tgtEntry = target[j];
            if (aRetainQuestItems)
            {
                bool alreadyHeldAsQuestItem = false;
                for (const auto& curEntry : current)
                {
                    if (curEntry.IsQuestItem && curEntry.BaseId == tgtEntry.BaseId)
                    {
                        alreadyHeldAsQuestItem = true;
                        break;
                    }
                }
                if (alreadyHeldAsQuestItem)
                    continue;
            }

            plan.ItemsToAdd.push_back(tgtEntry);
        }

        return plan;
    }
} // namespace CoopInventoryDiff
