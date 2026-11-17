#include <TiltedOnlinePCH.h>
#include <Services/LootService.h>
#include <Services/CoopService.h>
#include <Services/SaveStateService.h>
#include <Services/StorageService.h>
#include <Services/ObjectService.h>
#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/WorldLoadedEvent.h>
#include <Events/CoopActionEvent.h>
#include <Systems/OwnershipSystem.h>
#include <TESObjectREFR.h>
#include <Actor.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESBoundObject.h>
#include <Games/Overrides.h>

namespace
{
Inventory Contents(const Coop::InventoryState& state)
{
    Inventory items;
    items.Revision = state.Revision;
    items.Entries.assign(state.Items.begin(), state.Items.end());
    return items;
}
}
LootService::LootService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport)
{
    m_nonce = Now() << 16;
    m_update = aDispatcher.sink<UpdateEvent>().connect<&LootService::OnUpdate>(this);
    m_packet = aDispatcher.sink<CoopActionEvent>().connect<&LootService::OnPacket>(this);
    m_disconnected = aDispatcher.sink<DisconnectedEvent>().connect<&LootService::OnDisconnected>(this);
    m_loaded = aDispatcher.sink<WorldLoadedEvent>().connect<&LootService::OnWorldLoaded>(this);
}
uint64_t LootService::Now() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t LootService::ServerId(uint32_t aForm) const noexcept
{
    const auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
        if (view.get<FormIdComponent>(entity).Id == aForm)
            if (const auto id = Utils::GetServerId(entity)) return *id;
    return 0;
}
uint64_t LootService::Revision(uint32_t aEntity) const noexcept
{
    const auto state = m_states.find(aEntity);
    return state == m_states.end() ? 0 : state->second.Revision;
}
bool LootService::InitializeReceipts() noexcept
{
    const bool ready = m_world.ctx().at<SaveStateService>().Ready();
    if (!ready) Utils::ShowHudMessage("RFAB: shared transfer needs a matching SKSE co-save and native receipt timeline. No automatic old-save replay.");
    return ready;
}
bool LootService::WriteReceipt(const Pending& pending, uint8_t stage) noexcept
{
    auto& state = m_world.ctx().at<SaveStateService>();
    const auto& coop = m_world.ctx().at<CoopService>();
    const Coop::EffectId id{coop.Session(), coop.Generation(), pending.NativeOperation};
    if (stage == 0) return state.Begin(Coop::EffectKind::Transfer, id);
    if (stage == 1) return state.Applied(id);
    if (stage == 2) return state.Commit(id);
    if (stage == 3) return state.AbortBeforeNative(id);
    state.Uncertain(id); return false;
}
bool LootService::Intercept(TESObjectREFR* source, TESBoundObject* item, int32_t count, ITEM_REMOVE_REASON reason,
    ExtraDataList* extras, TESObjectREFR* destination) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return true;
    if (!source || !destination || !item || count <= 0) return true;
    // Merchant stock, confiscation/quest stores and ordinary personal home chests remain personal.
    auto* other = source->formID == 0x14 ? destination : source;
    const auto* actor = Cast<Actor>(other);
    if (actor && actor->GetExtension()->IsPlayer()) return false;
    if (!actor && !m_world.ctx().at<ObjectService>().IsSharedInventoryObject(other)) return false;
    if (actor && !actor->IsDead())
    {
        Utils::ShowHudMessage("RFAB: live-NPC pickpocket transfer needs a verified crime adapter. No shared item was moved.");
        return true;
    }
    auto& coop = m_world.ctx().at<CoopService>();
    if (!coop.Supports(Coop::TransactionalLoot) || m_world.IsLoadingWorld() || m_blocked || !InitializeReceipts())
    {
        Utils::ShowHudMessage("RFAB: shared transfer unavailable or still negotiating. Item remains in its source.");
        return true;
    }
    Inventory::Entry snapshot;
    if (!m_world.GetModSystem().GetServerModId(item->formID, snapshot.BaseId)) return true;
    if (extras) TESObjectREFR::GetItemFromExtraData(snapshot, extras);
    snapshot.Count = count; // ExtraCount describes the stack, not the selected quantity
    if (!Coop::Transferable(snapshot))
    {
        Utils::ShowHudMessage("RFAB: unsupported/quest/equipped item instance; no flattening or optimistic transfer.");
        return true;
    }
    for (const auto& pending : m_pending)
        if (pending.SourceForm == source->formID && pending.DestinationForm == destination->formID && pending.Request.Item == snapshot)
            return true; // repeated AutoLoot/UI callback while the original request is outstanding
    if (m_pending.size() >= 16)
    {
        Utils::ShowHudMessage("RFAB: bounded transfer queue is full; nothing moved. Wait for the current receipt.");
        return true;
    }
    Pending pending;
    pending.SourceForm = source->formID; pending.DestinationForm = destination->formID; pending.Reason = reason;
    pending.Request.Nonce = ++m_nonce; pending.Request.Item = std::move(snapshot);
    pending.Request.NativeReason = static_cast<uint8_t>(reason); pending.Began = Now();
    m_pending.push_back(std::move(pending));
    Pump();
    return true;
}
void LootService::Pump() noexcept
{
    if (m_pending.empty() || m_blocked || !m_transport.IsOnline()) return;
    auto& pending = m_pending.front();
    auto& coop = m_world.ctx().at<CoopService>();
    if (!coop.Supports(Coop::TransactionalLoot)) return;
    if (pending.State != Stage::Bindings)
    {
        // Retrying an action uses a NEW packet sequence but the SAME logical nonce. Prepared
        // duplicates are answered from TransferParticipant; they never repeat the native call.
        if (Now() - pending.LastSend >= 500)
        {
            pending.LastSend = Now();
            if (pending.State == Stage::Sent) coop.SendAction(pending.Request);
            else coop.SendAction(Coop::TransferReceipt{pending.NativeOperation, true, false});
        }
        if (Now() - pending.Began >= 5000) Finish(true, "RFAB: transfer acknowledgement timed out. Do not repeat the move; recovery is required.");
        return;
    }
    if (Now() - pending.Began >= 3000)
    {
        Finish(false, "RFAB: source/authority/revision not ready within 3 seconds. Item was not moved.");
        return;
    }
    pending.Request.Source = ServerId(pending.SourceForm);
    pending.Request.Destination = ServerId(pending.DestinationForm);
    auto query = [&](uint32_t form, uint32_t server) {
        if (!server)
        {
            if (auto* object = Cast<TESObjectREFR>(TESForm::GetById(form)); object && !Cast<Actor>(object))
                m_world.ctx().at<ObjectService>().EnsureInventoryObject(form);
            return;
        }
        const auto old = m_queried.find(server);
        if (old == m_queried.end() || Now() - old->second >= 500)
        { m_queried[server] = Now(); coop.SendAction(Coop::InventoryQuery{server}); }
    };
    query(pending.SourceForm, pending.Request.Source); query(pending.DestinationForm, pending.Request.Destination);
    const auto source = m_states.find(pending.Request.Source), destination = m_states.find(pending.Request.Destination);
    if (source == m_states.end() || destination == m_states.end() || source->second.Blocked || destination->second.Blocked) return;
    auto* from = Cast<TESObjectREFR>(TESForm::GetById(pending.SourceForm));
    auto* to = Cast<TESObjectREFR>(TESForm::GetById(pending.DestinationForm));
    if (!from || !to) { Finish(false, "RFAB: inventory reference unloaded before reservation. Nothing moved."); return; }
    pending.BeforeSource = from->GetInventory(); pending.BeforeDestination = to->GetInventory();
    if (!Coop::SameInventoryItems(pending.BeforeSource, Contents(source->second)) ||
        !Coop::SameInventoryItems(pending.BeforeDestination, Contents(destination->second))) return;
    pending.Request.SourceRevision = source->second.Revision; pending.Request.DestinationRevision = destination->second.Revision;
    pending.Request.SourceEpoch = source->second.Epoch; pending.Request.DestinationEpoch = destination->second.Epoch;
    pending.State = Stage::Sent; pending.LastSend = pending.Began = Now();
    coop.HoldInventory(true);
    coop.SendAction(pending.Request);
}
void LootService::Finish(bool aQuarantine, const char* aReason) noexcept
{
    if (aReason && *aReason) Utils::ShowHudMessage(aReason);
    m_blocked |= aQuarantine;
    if (!m_pending.empty() && !aQuarantine) m_pending.pop_front();
    if (aQuarantine) m_pending.clear();
    m_world.ctx().at<CoopService>().HoldInventory(aQuarantine);
}
void LootService::Execute(Pending pending, const Coop::TransferReply& reply) noexcept
{
    using Participant = Coop::TransferParticipant;
    const auto decision = m_participant.Prepared(reply.Operation);
    auto& coop = m_world.ctx().at<CoopService>();
    if (decision != Participant::Decision::Execute)
    {
        if (decision != Participant::Decision::Ignore)
            coop.SendAction(Coop::TransferReceipt{reply.Operation, decision == Participant::Decision::ResendApplied,
                decision == Participant::Decision::Quarantine});
        return;
    }
    pending.NativeOperation = reply.Operation;
    m_pending.front().NativeOperation = reply.Operation;
    const Coop::EffectId effect{coop.Session(), coop.Generation(), reply.Operation};
    auto* source = Cast<TESObjectREFR>(TESForm::GetById(pending.SourceForm));
    auto* destination = Cast<TESObjectREFR>(TESForm::GetById(pending.DestinationForm));
    auto* item = Cast<TESBoundObject>(TESForm::GetById(m_world.GetModSystem().GetGameId(pending.Request.Item.BaseId)));
    ExtraDataList* retained{};
    const bool valid = source && destination && item && ServerId(pending.SourceForm) == reply.Source &&
        ServerId(pending.DestinationForm) == reply.Destination && !m_world.IsLoadingWorld() &&
        Coop::SameInventoryItems(source->GetInventory(), pending.BeforeSource) &&
        Coop::SameInventoryItems(destination->GetInventory(), pending.BeforeDestination) &&
        source->FindExactInventoryInstance(pending.Request.Item, retained);
    if (!valid || !WriteReceipt(pending, 0))
    {
        m_participant.Applied(reply.Operation, false, false);
        coop.SendAction(Coop::TransferReceipt{reply.Operation, false, false});
        const bool timelineReady = m_world.ctx().at<SaveStateService>().Ready();
        Finish(!timelineReady, "RFAB: exact native preflight or write-ahead receipt failed. No transfer was executed.");
        return;
    }
    auto expectedSource = pending.BeforeSource, expectedDestination = pending.BeforeDestination;
    auto removal = pending.Request.Item; removal.Count = -removal.Count;
    if (!expectedSource.AddOrRemoveEntry(removal) || !expectedDestination.AddOrRemoveEntry(pending.Request.Item))
    {
        m_participant.Applied(reply.Operation, false, false);
        coop.SendAction(Coop::TransferReceipt{reply.Operation, false, false});
        const bool aborted = WriteReceipt(pending, 3);
        Finish(!aborted, "RFAB: exact native quantity changed before transfer. Nothing moved.");
        return;
    }
    {
        ScopedInventoryOverride inventory;
        ScopedEquipOverride equipment;
        // ORIGINAL native source->destination operation retains the selected ExtraDataList,
        // crime/script/container side effects. No reconstruct/remove-by-BaseId rollback.
        source->RemoveItem(item, pending.Request.Item.Count, pending.Reason, retained, destination);
    }
    if (m_world.IsLoadingWorld() || coop.Session() != effect.Session || coop.Generation() != effect.Generation ||
        m_pending.empty() || m_pending.front().Request.Nonce != pending.Request.Nonce)
    {
        m_world.ctx().at<SaveStateService>().Uncertain(effect);
        m_participant.Applied(reply.Operation, false, true);
        Finish(true, "RFAB: native transfer re-entered disconnect/load. Old pointers and queued actions discarded; no replay.");
        return;
    }
    source = Cast<TESObjectREFR>(TESForm::GetById(pending.SourceForm));
    destination = Cast<TESObjectREFR>(TESForm::GetById(pending.DestinationForm));
    const bool applied = source && destination && Coop::SameInventoryItems(source->GetInventory(), expectedSource) &&
        Coop::SameInventoryItems(destination->GetInventory(), expectedDestination);
    const bool durable = applied && WriteReceipt(pending, 1);
    m_participant.Applied(reply.Operation, applied, !durable);
    coop.SendAction(Coop::TransferReceipt{reply.Operation, applied && durable, !durable});
    if (!durable)
    {
        WriteReceipt(pending, 4);
        Finish(true, "RFAB: native result differs or its receipt could not be saved. Endpoints locked; no guessed rollback/retry.");
        return;
    }
    m_pending.front().State = Stage::Applied; m_pending.front().LastSend = Now();
    source->UpdateItemList(nullptr); destination->UpdateItemList(nullptr);
}
void LootService::Apply(const Coop::TransferReply& reply) noexcept
{
    if (m_pending.empty() || reply.Requester != ServerId(0x14) || reply.Nonce != m_pending.front().Request.Nonce) return;
    auto& pending = m_pending.front();
    if (reply.Result == Coop::TransferResult::Prepared)
    {
        if (reply.Source != pending.Request.Source || reply.Destination != pending.Request.Destination ||
            reply.SourceEpoch != pending.Request.SourceEpoch || reply.DestinationEpoch != pending.Request.DestinationEpoch ||
            reply.SourceRevision != pending.Request.SourceRevision || reply.DestinationRevision != pending.Request.DestinationRevision ||
            reply.Item != pending.Request.Item || !reply.Operation)
        { Finish(true, "RFAB: transfer reservation did not match the exact requested operation."); return; }
        Execute(pending, reply);
    }
    else if (reply.Result == Coop::TransferResult::Committed)
    {
        if (pending.State != Stage::Applied || pending.NativeOperation != reply.Operation)
        { Finish(true, "RFAB: server has a committed receipt absent from this native timeline. No replay was attempted."); return; }
        if (reply.Source != pending.Request.Source || reply.Destination != pending.Request.Destination ||
            reply.SourceEpoch != pending.Request.SourceEpoch || reply.DestinationEpoch != pending.Request.DestinationEpoch ||
            reply.SourceRevision != pending.Request.SourceRevision + 1 || reply.DestinationRevision != pending.Request.DestinationRevision + 1 ||
            reply.Item != pending.Request.Item)
        { WriteReceipt(pending, 4); Finish(true, "RFAB: committed transfer receipt identity/revisions changed. No cache rollback or replay."); return; }
        auto sourceAfter = pending.BeforeSource, destinationAfter = pending.BeforeDestination;
        auto removal = pending.Request.Item; removal.Count = -removal.Count;
        auto& storage = m_world.ctx().at<StorageService>();
        if (!sourceAfter.AddOrRemoveEntry(removal) || !destinationAfter.AddOrRemoveEntry(pending.Request.Item) ||
            !storage.TransferCommitted(pending.SourceForm, reply.SourceRevision, sourceAfter) ||
            !storage.TransferCommitted(pending.DestinationForm, reply.DestinationRevision, destinationAfter))
        { WriteReceipt(pending, 4); Finish(true, "RFAB: bank's native result/stamp changed before commit. Automatic side effects locked."); return; }
        sourceAfter.Revision = reply.SourceRevision; destinationAfter.Revision = reply.DestinationRevision;
        AcceptSnapshot(reply.Source, reply.SourceEpoch, sourceAfter);
        AcceptSnapshot(reply.Destination, reply.DestinationEpoch, destinationAfter);
        m_participant.Committed(reply.Operation);
        if (!WriteReceipt(pending, 2)) { Finish(true, "RFAB: final native receipt could not be persisted."); return; }
        Finish(false, nullptr);
    }
    else if (reply.Result == Coop::TransferResult::Quarantined || pending.State == Stage::Applied)
        Finish(true, "RFAB: transfer is quarantined for coordinated recovery. Shared contents will not be rewritten.");
    else Finish(false, reply.Result == Coop::TransferResult::Conflict ? "RFAB: another transfer won the revision. Nothing moved." : "RFAB: server rejected the transfer. Nothing moved.");
}
bool LootService::AcceptSnapshot(uint32_t aEntity, uint32_t aEpoch, const Inventory& aItems) noexcept
{
    if (!aItems.IsValid()) return false;
    auto& state = m_states[aEntity];
    if (aEpoch < state.Epoch || (aItems.Revision && aItems.Revision < state.Revision)) return false;
    state.Entity = aEntity; state.Epoch = aEpoch; state.Revision = aItems.Revision;
    state.Items.assign(aItems.Entries.begin(), aItems.Entries.end());
    return true;
}
bool LootService::AcceptDelta(uint32_t aEntity, uint32_t aEpoch, uint64_t aRevision, const Inventory::Entry& aItem) noexcept
{
    const auto old = m_states.find(aEntity);
    if (old == m_states.end()) return aItem.WireValid;
    if (aEpoch < old->second.Epoch || aRevision <= old->second.Revision) return false;
    if (aRevision != old->second.Revision + 1)
    { m_world.ctx().at<CoopService>().SendAction(Coop::InventoryQuery{aEntity}); return false; }
    auto items = Contents(old->second);
    if (!items.AddOrRemoveEntry(aItem)) return false;
    old->second.Epoch = aEpoch; old->second.Revision = aRevision;
    old->second.Items.assign(items.Entries.begin(), items.Entries.end());
    return true;
}
void LootService::Apply(const Coop::InventoryState& state) noexcept
{
    const auto old = m_states.find(state.Entity);
    if (!state.Entity || !state.Revision || (old != m_states.end() && (state.Epoch < old->second.Epoch || state.Revision < old->second.Revision))) return;
    const auto desired = Contents(state);
    if (!desired.IsValid()) return;
    m_states[state.Entity] = state;
    if (m_blocked) return;
    for (const auto& pending : m_pending)
        if (pending.State == Stage::Applied && (pending.Request.Source == state.Entity || pending.Request.Destination == state.Entity)) return;
    const auto entity = Utils::FindEntityByServerId(state.Entity);
    auto* object = Utils::GetByServerId<TESObjectREFR>(state.Entity);
    if (!object)
    {
        if (entity)
            if (auto* waiting = m_world.try_get<WaitingFor3D>(*entity))
            {
                waiting->SpawnRequest.InventoryContent.Entries = desired.Entries;
                waiting->SpawnRequest.InventoryContent.Revision = desired.Revision;
            }
        return; // future spawn carries canonical inventory; no cached raw actor
    }
    if (object->formID == 0x14)
    {
        // Personal bag is NEVER replaced by a network snapshot. Verify/re-query instead.
        if (!Coop::SameInventoryItems(object->GetInventory(), desired)) m_states[state.Entity].Blocked = true;
        return;
    }
    if (StorageService::IsBacking(object->formID)) return; // StorageService uses a save-bound cache receipt, never this generic snapshot path
    if (!object->ApplyExactInventory(desired))
    {
        m_states[state.Entity].Blocked = true;
        Utils::ShowHudMessage("RFAB: remote inventory instance cannot be reconciled safely. No RemoveAllItems or BaseId correction was used.");
    }
}
void LootService::OnPacket(const CoopActionEvent& aEvent) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    std::visit([this](const auto& value) { Apply(value); }, aEvent.Data.Content);
}
void LootService::OnUpdate(const UpdateEvent&) noexcept { Pump(); }
void LootService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    bool uncertain{};
    for (const auto& pending : m_pending) uncertain |= pending.State == Stage::Applied || pending.NativeOperation != 0;
    m_blocked |= uncertain;
    m_pending.clear(); m_states.clear(); m_queried.clear();
    if (uncertain) Utils::ShowHudMessage("RFAB: disconnected with a native transfer awaiting commit. Do not replay it after reconnect.");
    // Do not re-resolve an old actor here: this event can be the pre-load teardown boundary.
}
void LootService::OnWorldLoaded(const WorldLoadedEvent&) noexcept
{
    m_pending.clear(); m_states.clear(); m_queried.clear(); m_participant.Clear();
    m_receiptsChecked = false;
    m_blocked = !m_world.ctx().at<SaveStateService>().Ready();
    // An arbitrary ESS load is not permission to delete an unresolved durable receipt.
    InitializeReceipts();
    m_world.ctx().at<CoopService>().HoldInventory(m_blocked);
}
