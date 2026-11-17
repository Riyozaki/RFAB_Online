#include <TiltedOnlinePCH.h>
#include <Games/ActorExtension.h>
#include <Services/StorageService.h>
#include <Services/CoopService.h>
#include <Services/SaveStateService.h>
#include <Services/ObjectService.h>
#include <Services/LootService.h>
#include <Coop/ScopedValue.h>
#include <Coop/StorageCache.h>
#include <World.h>
#include <Games/TES.h>
#include <Events/ActivateEvent.h>
#include <Events/CoopActionEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/WorldLoadedEvent.h>
#include <Events/UpdateEvent.h>
#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Forms/TESObjectCELL.h>
#include <Interface/UI.h>
#include <atomic>
#include <memory>
#include <set>

namespace
{
std::atomic<std::shared_ptr<const std::set<uint32_t>>> s_backings;
uint32_t Resolve(const Coop::StableForm& form)
{
    auto* manager = ModManager::Get();
    auto* plugin = manager ? manager->GetByName(form.Plugin.c_str()) : nullptr;
    return plugin && plugin->IsLoaded() && plugin->IsLite() == form.Light ? plugin->GetFormId(form.Local) : 0;
}
Inventory Contents(const Coop::StorageState& state)
{
    Inventory inventory;
    inventory.Revision = state.Revision;
    inventory.Entries.assign(state.Items.begin(), state.Items.end());
    return inventory;
}
}
StorageService::StorageService(World& world, entt::dispatcher& dispatcher) noexcept : m_world(world)
{
    m_catalog.Read("Data/SKSE/Plugins/RFAB_Storage.tsv");
    m_loaded = dispatcher.sink<WorldLoadedEvent>().connect<&StorageService::OnLoaded>(this);
    m_disconnected = dispatcher.sink<DisconnectedEvent>().connect<&StorageService::OnDisconnected>(this);
    m_activate = dispatcher.sink<ActivateEvent>().connect<&StorageService::OnActivate>(this);
    m_packet = dispatcher.sink<CoopActionEvent>().connect<&StorageService::OnPacket>(this);
    m_update = dispatcher.sink<UpdateEvent>().connect<&StorageService::OnUpdate>(this);
}
uint64_t StorageService::Now() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool StorageService::IsBacking(uint32_t form) noexcept
{
    const auto snapshot = s_backings.load(std::memory_order_acquire);
    return snapshot && snapshot->contains(form);
}
void StorageService::OnLoaded(const WorldLoadedEvent&) noexcept
{
    m_open.clear(); m_access.clear(); m_backings.clear(); m_lastOpen = 0;
    auto guards = std::make_shared<std::set<uint32_t>>();
    // An adapter update cannot make an already adopted campaign cache a personal offline bag.
    for (const auto& saved : m_world.ctx().at<SaveStateService>().State().Storages) guards->insert(saved.Form);
    m_valid = !m_catalog.Rules().empty();
    for (const auto& rule : m_catalog.Rules())
    {
        const auto access = Resolve(rule.Access), backing = Resolve(rule.Backing);
        auto* point = Cast<TESObjectREFR>(TESForm::GetById(access));
        auto* storage = Cast<TESObjectREFR>(TESForm::GetById(backing));
        if (!access || !backing || !point || !point->baseForm || !storage || !storage->baseForm ||
            storage->baseForm->formType != FormType::Container || storage->IsTemporary() || point->IsTemporary() ||
            (m_access.contains(access) && m_access.at(access) != backing))
        {
            m_valid = false;
            continue;
        }
        m_access[access] = backing; m_backings[backing] = rule.Backing; guards->insert(backing);
    }
    if (!m_valid) { m_access.clear(); m_backings.clear(); }
    s_backings.store(std::move(guards), std::memory_order_release);
}
void StorageService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_open.clear(); m_lastOpen = 0;
}
bool StorageService::NearAccess(uint32_t form) const noexcept
{
    auto* player = PlayerCharacter::Get();
    auto* access = Cast<TESObjectREFR>(TESForm::GetById(form));
    return player && access && player->GetNiNode() && access->GetNiNode() && player->parentCell &&
        player->parentCell == access->parentCell && player->parentCell->IsAttached() &&
        glm::length2(player->position - access->position) <= 256.f * 256.f && !player->IsDead() &&
        !player->GetExtension()->CoopDowned.load(std::memory_order_relaxed);
}
void StorageService::OnActivate(const ActivateEvent& event) noexcept
{
    if (event.ActivatorId != 0x14 || !m_valid || !m_access.contains(event.ObjectId) || !NearAccess(event.ObjectId)) return;
    const auto backing = m_access.at(event.ObjectId);
    if (m_open.size() >= 8 && !m_open.contains(backing)) return;
    auto& open = m_open[backing];
    open.Access = event.ObjectId; open.Began = Now(); open.LastSend = 0; open.SawMenu = false;
    m_lastOpen = backing;
    m_world.ctx().at<ObjectService>().EnsureStorageAccess(event.ObjectId);
}
bool StorageService::EnsureBacking(uint32_t form) noexcept
{
    if (!IsBacking(form)) return false;
    if (!m_valid || !m_open.contains(form))
        Utils::ShowHudMessage("RFAB: open a verified nearby secure access point before moving items. Campaign caches are read-only offline.");
    return true; // never register the hidden cell as an ordinary ephemeral chest
}
void StorageService::ConfirmImport() noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !m_lastOpen || !m_open.contains(m_lastOpen)) return;
    auto& open = m_open.at(m_lastOpen);
    auto* storage = Cast<TESObjectREFR>(TESForm::GetById(m_lastOpen));
    if (!storage || !NearAccess(open.Access) || !open.Desired.NeedsImport)
    { Utils::ShowHudMessage("RFAB: no nearby uninitialized campaign bank is awaiting import. Existing data was not merged or overwritten."); return; }
    const auto items = storage->GetInventory();
    if (!items.IsValid()) return;
    for (const auto& item : items.Entries)
        if (!Coop::Transferable(item))
        { Utils::ShowHudMessage("RFAB: unsupported/quest item in old storage; import refused without losing or flattening it."); return; }
    open.ImportItems = items; open.Import = open.Attach = true; open.LastSend = 0;
}
void StorageService::ConfirmAttach() noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !m_lastOpen || !m_open.contains(m_lastOpen)) return;
    auto& open = m_open.at(m_lastOpen);
    auto* storage = Cast<TESObjectREFR>(TESForm::GetById(m_lastOpen));
    if (!storage || !NearAccess(open.Access) || open.Desired.NeedsImport || !open.Desired.Revision) return;
    const auto local = storage->GetInventory(), desired = Contents(open.Desired);
    if (!local.Entries.empty() && !Coop::SameInventoryItems(local, desired))
    { Utils::ShowHudMessage("RFAB: old personal contents differ from the campaign. They remain untouched; no automatic merge or erasure."); return; }
    open.Attach = true; open.Notified = false;
}
void StorageService::OnPacket(const CoopActionEvent& event) noexcept
{
    auto& coop = m_world.ctx().at<CoopService>();
    if (!coop.Supports(Coop::PersistentStorage)) return;
    if (const auto* state = std::get_if<Coop::StorageState>(&event.Data.Content))
    {
        const auto backing = m_world.GetModSystem().GetGameId(state->Backing);
        if (!m_open.contains(backing) || !m_backings.contains(backing) || !state->Entity || !state->Revision ||
            Coop::StorageKey(coop.Campaign(), m_backings.at(backing)) != state->Key || !Contents(*state).IsValid()) return;
        auto& open = m_open.at(backing);
        if (state->Revision < open.Desired.Revision || (open.Desired.Entity && open.Desired.Entity != state->Entity)) return;
        open.Desired = *state; open.Operation = event.Data.Operation;
    }
    if (const auto* state = std::get_if<Coop::InventoryState>(&event.Data.Content))
        for (auto& [_, open] : m_open)
            if (open.Desired.Entity == state->Entity && state->Revision >= open.Desired.Revision && !state->Blocked)
            { open.Desired.Revision = state->Revision; open.Desired.Items = state->Items; open.Operation = event.Data.Operation; }
}
void StorageService::Reconcile(uint32_t backing, Open open) noexcept
{
    if (m_executing || open.Desired.NeedsImport || !open.Desired.Entity || !open.Operation || !NearAccess(open.Access)) return;
    auto& saves = m_world.ctx().at<SaveStateService>();
    auto& coop = m_world.ctx().at<CoopService>();
    auto* storage = Cast<TESObjectREFR>(TESForm::GetById(backing));
    if (!storage || !storage->baseForm || storage->baseForm->formType != FormType::Container) return;
    const auto desired = Contents(open.Desired), before = storage->GetInventory();
    if (!desired.IsValid() || !before.IsValid()) return;
    const Coop::SavedStorage* stamp{};
    for (const auto& saved : saves.State().Storages) if (saved.Form == backing) { stamp = &saved; break; }
    const bool same = Coop::SameInventoryItems(before, desired);
    const auto decision = Coop::StorageCachePolicy(stamp, coop.Campaign(), open.Desired.Revision,
        open.Desired.NeedsImport, before.Entries.empty(), same, open.Attach);
    if (decision == Coop::StorageCacheDecision::Blocked)
    {
        if (!open.Notified)
        {
            m_open.at(backing).Notified = true;
            Utils::ShowHudMessage("RFAB: campaign bank not attached. Local items are untouched. For an empty/matching cache type rfab_storage_attach; different private contents need explicit recovery.");
        }
        return;
    }
    if (decision == Coop::StorageCacheDecision::AwaitImport || decision == Coop::StorageCacheDecision::AwaitNewRevision) return;
    if (decision != Coop::StorageCacheDecision::Current)
    {
        Coop::ScopedValue executing(m_executing, true);
        const auto effect = Coop::NativeEffect(Coop::EffectKind::Storage, coop.Session(), coop.Generation(), open.Operation);
        if (!saves.Begin(Coop::EffectKind::Storage, effect)) return; // a paired transfer may own the timeline
        if (!same && !storage->ApplyExactInventory(desired))
        { saves.Uncertain(effect); Utils::ShowHudMessage("RFAB: bank cache instance reconciliation failed; native effects locked, no guessed rollback."); return; }
        if (m_world.IsLoadingWorld() || coop.Session() != effect.Session || coop.Generation() != effect.Generation || !m_open.contains(backing))
        { saves.Uncertain(effect); return; }
        storage = Cast<TESObjectREFR>(TESForm::GetById(backing));
        if (!storage || !Coop::SameInventoryItems(storage->GetInventory(), desired) ||
            !saves.StampStorage({backing, coop.Campaign(), open.Desired.Revision}) || !saves.Applied(effect) || !saves.Commit(effect))
        { saves.Uncertain(effect); return; }
    }
    const auto found = Utils::FindEntityByServerId(open.Desired.Entity);
    if (found)
    {
        const auto* id = m_world.try_get<FormIdComponent>(*found);
        if (!id || id->Id != backing) return; // server-id collision is not permission to replace a native ref
    }
    else
    {
        const auto entity = m_world.create();
        m_world.emplace<FormIdComponent>(entity, backing);
        m_world.emplace<ObjectComponent>(entity, open.Desired.Entity);
    }
    m_world.ctx().at<LootService>().AcceptSnapshot(open.Desired.Entity, 0, desired);
}
bool StorageService::TransferCommitted(uint32_t form, uint64_t revision, const Inventory& after) noexcept
{
    if (!IsBacking(form)) return true;
    auto& saves = m_world.ctx().at<SaveStateService>();
    auto& coop = m_world.ctx().at<CoopService>();
    auto* storage = Cast<TESObjectREFR>(TESForm::GetById(form));
    if (!storage || !Coop::SameInventoryItems(storage->GetInventory(), after) || !saves.StampStorage({form, coop.Campaign(), revision})) return false;
    if (auto it = m_open.find(form); it != m_open.end() && revision >= it->second.Desired.Revision)
    {
        it->second.Desired.Revision = revision;
        it->second.Desired.Items.assign(after.Entries.begin(), after.Entries.end());
    }
    return true;
}
void StorageService::OnUpdate(const UpdateEvent&) noexcept
{
    auto& coop = m_world.ctx().at<CoopService>();
    if (!m_valid || m_executing || !coop.Supports(Coop::PersistentStorage) || m_world.IsLoadingWorld()) return;
    auto* ui = UI::Get();
    const bool menu = ui && ui->GetMenuOpen(BSFixedString("ContainerMenu"));
    std::vector<std::pair<uint32_t, Open>> pending(m_open.begin(), m_open.end());
    for (auto& [backing, open] : pending)
    {
        if (!m_open.contains(backing)) continue;
        if (!NearAccess(open.Access) || (!menu && (open.SawMenu || Now() - open.Began > 3000)))
        { m_open.erase(backing); continue; }
        m_open.at(backing).SawMenu |= menu;
        if (Now() - open.LastSend >= 500 || !open.LastSend)
        {
            m_world.ctx().at<ObjectService>().EnsureStorageAccess(open.Access);
            Coop::StorageOpen request;
            const auto& form = m_backings.at(backing);
            request.Plugin = form.Plugin; request.LocalRef = form.Local;
            if (!m_world.GetModSystem().GetServerModId(backing, request.Backing) ||
                !m_world.GetModSystem().GetServerModId(open.Access, request.AccessPoint)) continue;
            request.ConfirmImport = open.Import;
            if (open.Import) request.Import.assign(open.ImportItems.Entries.begin(), open.ImportItems.Entries.end());
            coop.SendAction(std::move(request)); m_open.at(backing).LastSend = Now();
        }
        if (open.Desired.NeedsImport)
        {
            if (!open.Notified)
            {
                m_open.at(backing).Notified = true;
                Utils::ShowHudMessage("RFAB: new campaign bank. Contents are untouched and transfers blocked. To explicitly import this old personal chest type rfab_storage_import in the console.");
            }
            continue;
        }
        Reconcile(backing, std::move(open));
    }
}
