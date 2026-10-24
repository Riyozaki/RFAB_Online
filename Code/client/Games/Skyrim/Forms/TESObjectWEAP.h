#pragma once

#include <Forms/TESBoundObject.h>
#include <DefaultObjectManager.h>

struct TESObjectWEAP : TESBoundObject
{
    enum WeaponAnimationType : uint8_t
    {
        kHandToHandMelee = 0,
        kOneHandSword = 1,
        kOneHandDagger = 2,
        kOneHandAxe = 3,
        kOneHandMace = 4,
        kTwoHandSword = 5,
        kTwoHandAxe = 6,
        kBow = 7,
        kStaff = 8,
        kCrossbow = 9,
    };

    // BGSEquipType::equipSlot at +0x0E8 and Data::animationType at +0x19D on Skyrim SE 1.5.97 (sizeof(TESObjectWEAP) == 0x220).
    [[nodiscard]] TESForm* GetEquipSlot() const noexcept
    {
        return *reinterpret_cast<TESForm* const*>(reinterpret_cast<const uint8_t*>(this) + 0x0E8);
    }

    [[nodiscard]] uint8_t GetAnimationType() const noexcept
    {
        return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const uint8_t*>(this) + 0x19D);
    }

    [[nodiscard]] bool UsesSingleHandEquipSlot(const DefaultObjectManager& acDom) const noexcept
    {
        const uint8_t animType = GetAnimationType();
        const bool isOneHandOrStaffType = (animType <= kOneHandMace || animType == kStaff);
        if (!isOneHandOrStaffType)
            return false;

        TESForm* const pEquipSlot = GetEquipSlot();
        if (!pEquipSlot)
            return true;

        return pEquipSlot == acDom.leftEquipSlot ||
               pEquipSlot == acDom.rightEquipSlot ||
               pEquipSlot == acDom.eitherEquipSlot;
    }
};

