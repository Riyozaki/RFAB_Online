#pragma once

#include "ExtraData.h"

#include <ExtraData/ExtraSoul.h>

struct AlchemyItem;
struct EnchantmentItem;

struct ExtraDataList
{
    static ExtraDataList* New() noexcept;

    bool Contains(ExtraDataType aType) const;
    void Set(ExtraDataType aType, bool aSet);

    bool Add(ExtraDataType aType, BSExtraData* apNewData);
    bool Remove(ExtraDataType aType, BSExtraData* apNewData);

    uint32_t GetCount() const;

    void SetType(ExtraDataType aType, bool aClear);
    BSExtraData* GetByType(ExtraDataType type) const;

    void SetSoulData(SOUL_LEVEL aSoulLevel) noexcept;
    void SetChargeData(float aCharge) noexcept;
    void SetWorn(bool aWornLeft) noexcept;
    void SetPoison(AlchemyItem* apItem, uint32_t aCount) noexcept;
    void SetHealth(float aHealth) noexcept;
    void SetEnchantmentData(EnchantmentItem* apItem, uint16_t aCharge, bool aRemoveOnUnequip) noexcept;

    [[nodiscard]] bool HasQuestObjectAlias() noexcept;

    // RFAB Online (Skyrim SE 1.5.97): BaseExtraList has NO vtable before the
    // 1.6.629 game update. Upstream added the virtual destructor for 629 in
    // commit 553793fc ("fix: Skyrim new update crashes"); keeping it here
    // would shift data/bitfield/lock and every embedding struct
    // (TESObjectREFR, TESObjectCELL, Actor, PlayerCharacter) by 8 bytes.
    BSExtraData* data = nullptr;

    struct Bitfield
    {
        uint8_t data[0x18];
    };

    Bitfield* bitfield{};
    mutable BSRecursiveLock lock{};
};

static_assert(sizeof(ExtraDataList) == 0x18);
static_assert(offsetof(ExtraDataList, data) == 0x0);
static_assert(offsetof(ExtraDataList, bitfield) == 0x8);
static_assert(offsetof(ExtraDataList, lock) == 0x10);
