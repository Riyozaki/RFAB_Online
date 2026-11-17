#include <TiltedOnlinePCH.h>
#include <Services/InventoryHook.h>
#include <Services/LootService.h>
#include <Services/StorageService.h>
#include <Services/CoopService.h>
#include <World.h>
#include <TESObjectREFR.h>
#include <Games/Overrides.h>

namespace
{
using Remove = BSPointerHandle<TESObjectREFR>* (__fastcall*)(TESObjectREFR*, BSPointerHandle<TESObjectREFR>*,
    TESBoundObject*, int32_t, ITEM_REMOVE_REASON, ExtraDataList*, TESObjectREFR*, const NiPoint3*, const NiPoint3*);
Remove s_actorRemove{}, s_playerRemove{};
std::atomic_bool s_installed{};

BSPointerHandle<TESObjectREFR>* HookActorInventoryRemove(TESObjectREFR* self, BSPointerHandle<TESObjectREFR>* result,
    TESBoundObject* item, int32_t count, ITEM_REMOVE_REASON reason, ExtraDataList* extras, TESObjectREFR* destination,
    const NiPoint3* drop, const NiPoint3* rotate)
{
    if (InventoryHook::Intercept(self, item, count, reason, extras, destination))
    { *result = {}; return result; }
    ScopedPersonalInventoryOverride personal(reason == ITEM_REMOVE_REASON::kSelling);
    return s_actorRemove(self, result, item, count, reason, extras, destination, drop, rotate);
}
BSPointerHandle<TESObjectREFR>* HookPlayerInventoryRemove(TESObjectREFR* self, BSPointerHandle<TESObjectREFR>* result,
    TESBoundObject* item, int32_t count, ITEM_REMOVE_REASON reason, ExtraDataList* extras, TESObjectREFR* destination,
    const NiPoint3* drop, const NiPoint3* rotate)
{
    if (InventoryHook::Intercept(self, item, count, reason, extras, destination))
    { *result = {}; return result; }
    ScopedPersonalInventoryOverride personal(reason == ITEM_REMOVE_REASON::kSelling);
    return s_playerRemove(self, result, item, count, reason, extras, destination, drop, rotate);
}
bool Install(void** table, Remove hook, Remove& original)
{
    // Actor overrides TESObjectREFR slot 0x56; hooking only the base implementation misses
    // player deposits and many QuickLoot/AutoLoot paths. Keep the previous vtable target to chain.
    if (!table || !table[0x56]) return false;
    DWORD protection{};
    if (!VirtualProtect(table + 0x56, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    original = reinterpret_cast<Remove>(table[0x56]);
    table[0x56] = reinterpret_cast<void*>(hook);
    DWORD ignored{};
    const bool ok = VirtualProtect(table + 0x56, sizeof(void*), protection, &ignored) != 0;
    if (!ok) spdlog::error("[RFAB] Could not restore inventory vtable protection; transactional capability disabled");
    return ok;
}
#define RFAB_VTABLE_HOOK(table, hook, original) Install(table, hook, original)
TiltedPhoques::Initializer s_hooks([] {
    // CommonLibSSE-NG b93280e... Offsets_VTABLE: FIRST VariantID parameter is SE, not AE.
    // Both IDs/RVAs checked against the pinned public 1.5.97 Address Library, not guessed IDs.
    POINTER_SKYRIMSE(void*, actorTable, 260538);
    POINTER_SKYRIMSE(void*, playerTable, 261916);
    const bool actor = RFAB_VTABLE_HOOK(actorTable.Get(), HookActorInventoryRemove, s_actorRemove);
    const bool player = RFAB_VTABLE_HOOK(playerTable.Get(), HookPlayerInventoryRemove, s_playerRemove);
    s_installed.store(actor && player, std::memory_order_release);
});
}
bool InventoryHook::Installed() noexcept { return s_installed.load(std::memory_order_acquire); }
bool InventoryHook::Intercept(TESObjectREFR* source, TESBoundObject* item, int32_t count, ITEM_REMOVE_REASON reason,
    ExtraDataList* extras, TESObjectREFR* destination) noexcept
{
    if (ScopedInventoryOverride::IsOverriden() || !source || !destination ||
        (source->formID != 0x14 && destination->formID != 0x14)) return false;
    const bool bank = StorageService::IsBacking(source->formID) || StorageService::IsBacking(destination->formID);
    if (!World::IsOnlineCached()) return bank; // an adopted campaign cache is not a free offline duplicate
    if (reason == ITEM_REMOVE_REASON::kSelling) return bank; // merchant stock remains personal, not a bank bypass

    // UI/VM entry can be invoked by another mod from a tasklet. Never enter EnTT there or
    // optimistically move a shared item; the original item remains untouched on rejection.
    if (World::IsMainThread())
    {
        if (bank && !World::Get().ctx().at<CoopService>().Supports(Coop::PersistentStorage)) return true;
        return World::Get().ctx().at<LootService>().Intercept(source, item, count, reason, extras, destination);
    }
    RFAB_EXPECT_MAIN_THREAD();
    return true;
}
