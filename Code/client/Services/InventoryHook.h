#pragma once
#include <cstdint>
struct TESObjectREFR;
struct TESBoundObject;
struct ExtraDataList;
enum class ITEM_REMOVE_REASON;

#include <ThreadAffinity.h>
struct InventoryHook
{
static RFAB_ANY_THREAD bool Intercept(TESObjectREFR*, TESBoundObject*, int32_t, ITEM_REMOVE_REASON, ExtraDataList*, TESObjectREFR*) noexcept;
static RFAB_ANY_THREAD bool Installed() noexcept;
};
