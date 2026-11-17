#pragma once
#include <Coop/ScopedValue.h>
template <class T> using ScopedTlsValue = Coop::ScopedValue<T>;

template <class T> struct ScopedOverride
{
    explicit ScopedOverride(bool aEnabled = true) : m_enabled(aEnabled) { if (m_enabled) s_refCount++; }

    ~ScopedOverride() { if (m_enabled) s_refCount--; }

    TP_NOCOPYMOVE(ScopedOverride);

    static bool IsOverriden() { return s_refCount > 0; }

private:
    bool m_enabled;
    static thread_local uint32_t s_refCount;
};

namespace details
{
struct Equip
{
};
struct Unequip
{
};
struct SaveLoad
{
};
struct Experience
{
};
struct Activate
{
};
struct Inventory
{
};
struct PersonalInventory {};
struct ExtraData
{
};
struct Quest
{
};
struct SpellCast
{
};
} // namespace details

using ScopedReferencesOverride = ScopedOverride<TESObjectREFR>;
using ScopedEquipOverride = ScopedOverride<details::Equip>;
using ScopedUnequipOverride = ScopedOverride<details::Unequip>;
using ScopedSaveLoadOverride = ScopedOverride<details::SaveLoad>;
using ScopedExperienceOverride = ScopedOverride<details::Experience>;
using ScopedActivateOverride = ScopedOverride<details::Activate>;
using ScopedInventoryOverride = ScopedOverride<details::Inventory>;
using ScopedExtraDataOverride = ScopedOverride<details::ExtraData>;
using ScopedQuestOverride = ScopedOverride<details::Quest>;
using ScopedSpellCastOverride = ScopedOverride<details::SpellCast>;

using ScopedPersonalInventoryOverride = ScopedOverride<details::PersonalInventory>;
