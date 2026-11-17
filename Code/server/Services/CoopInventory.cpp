#include <Services/CoopService.h>
#include <World.h>
#include <Components.h>
#include <Setting.h>


namespace
{
Console::Setting sRfabCampaign{"RFAB:sCampaign", "Stable campaign journal name (letters, digits, dash, underscore)", "rfab-coop"};

}

std::optional<Coop::StableForm> CoopService::NameForm(GameId aId) const
{
    if (!aId || aId.ModId >= 0xFFFFFFFEu) return {};
    const auto& mods = m_world.ctx().at<ModsComponent>();
    for (const auto& [name, entry] : mods.GetStandardMods())
        if (entry.id == aId.ModId)
            return Coop::StableForm{std::string(name.data(), name.size()), aId.BaseId, false};
    for (const auto& [name, entry] : mods.GetLiteMods())
        if (entry.id == aId.ModId)
            return Coop::StableForm{std::string(name.data(), name.size()), aId.BaseId, true};
    return {};
}
bool CoopService::ItemAvailable(Player* player, const Inventory::Entry& item) const noexcept
{
    if (!player || !Coop::Transferable(item)) return false;
    const auto supports = [this, player](GameId id, bool dynamicEnchantment = false) {
        if (!id) return true;
        if (dynamicEnchantment && id.ModId == 0xFFFFFFFFu) return true;
        const auto form = NameForm(id);
        return form && player->HasMod(id.ModId) && Coop::StorageKey("validate", *form).has_value();
    };
    bool valid = supports(item.BaseId) && supports(item.ExtraOwnerId) && supports(item.ExtraUniqueBaseId) &&
        supports(item.ExtraEnchantId, !item.EnchantData.Effects.empty()) && supports(item.ExtraPoisonId);
    for (const auto& effect : item.EnchantData.Effects) valid &= supports(effect.EffectId);
    return valid;
}
void CoopService::InitializeInventory() noexcept
{
    m_campaign = sRfabCampaign.value();
    if (!Coop::StorageKey(m_campaign, {"skyrim.esm", 1, false}))
    {
        spdlog::error("[RFAB] Invalid campaign name; durable inventory disabled, not replaced with an empty campaign");
        return;
    }
    m_inventoryLease = std::make_unique<Coop::DurableLease>(std::filesystem::path("Data/rfab_campaign") / (m_campaign + ".lock"));
    if (!m_inventoryLease->Acquired())
    {
        spdlog::error("[RFAB] Campaign journal already belongs to another process or cannot be locked; no concurrent writer/bootstrap");
        return;
    }
    m_inventoryFile = std::make_unique<Coop::DurableFile>(std::filesystem::path("Data/rfab_campaign") / (m_campaign + ".inventory"));
    m_inventory = std::make_unique<Coop::InventoryLedger>(m_campaign, [this](const auto& state) {
        const auto bytes = Coop::InventoryLedger::Encode(state, [this](GameId id) { return NameForm(id); });
        if (!bytes || !m_inventoryFile->Write(*bytes))
        {
            spdlog::error("[RFAB] Campaign journal write failed. No native transfer may be retried or reported committed");
            return false;
        }
        return true;
    });
    std::vector<uint8_t> bytes;
    const auto read = m_inventoryFile->Read(bytes);
    if (read == Coop::DurableFile::ReadResult::Failed)
    {
        spdlog::error("[RFAB] Existing campaign journal is unreadable/corrupt; refusing empty bootstrap");
        m_inventory.reset();
        return;
    }
    if (read == Coop::DurableFile::ReadResult::Loaded) m_pendingInventory = std::move(bytes);
    // Both ends read the same generated TAB-separated installed-profile catalog. A malformed
    // file disables the WHOLE catalog; spaces in a plugin basename are not extra columns.
    if (m_storageCatalog.Read("config/RFAB_Storage.tsv"))
        for (const auto& rule : m_storageCatalog.Rules())
            m_storageAccess.emplace(*Coop::StorageKey(m_campaign, rule.Access), *Coop::StorageKey(m_campaign, rule.Backing));
}
bool CoopService::InventoryReady() const noexcept
{
    return m_inventory && !m_pendingInventory && !m_inventory->Faulted();
}
bool CoopService::RestoreInventoryFor(Player* player) noexcept
{
    if (!m_pendingInventory) return InventoryReady();
    auto state = Coop::InventoryLedger::Decode(*m_pendingInventory, [this, player](const Coop::StableForm& form) -> std::optional<GameId> {
        const String name(form.Plugin.data(), form.Plugin.size());
        const auto id = m_world.ctx().at<ModsComponent>().Find(name, form.Light);
        if (!id || !player || !player->HasMod(*id)) return {};
        return GameId(*id, form.Local);
    });
    if (!state)
    {
        spdlog::error("[RFAB] Campaign journal cannot resolve against this client's negotiated plugin profile (or schema is invalid). No invented IDs, empty bootstrap or overwrite");
        return false;
    }
    if (!m_inventory || !m_inventory->Restore(std::move(*state))) return false;
    m_pendingInventory.reset();
    return InventoryReady();
}
bool CoopService::UsesTransactionalInventory(Player* aPlayer) const noexcept
{
    if (!aPlayer) return false;
    const auto client = m_clients.find(aPlayer->GetId());
    return client != m_clients.end() && (client->second.Capabilities & Coop::TransactionalLoot);
}
bool CoopService::InventoryLocked(uint32_t aEntity) const noexcept
{
    const auto binding = m_inventoryBindings.find(aEntity);
    return binding != m_inventoryBindings.end() && (!InventoryReady() || m_inventory->Locked(binding->second));
}
bool CoopService::AllowsLegacyInventory(Player* player, uint32_t id) const noexcept
{
    if (!m_inventoryBindings.contains(id)) return true;
    if (InventoryLocked(id)) return false;
    const auto entity = static_cast<entt::entity>(id);
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    const auto* actor = m_world.try_get<CharacterComponent>(entity);
    // The owner's live actor can consume/use/equip items. Shared containers/corpses can
    // change only through a paired reservation; neither a legacy client nor script delta
    // may remove the source without the matching recipient, even when it is not locked.
    return owner && owner->GetOwner() == player && actor && !actor->IsDead();
}
std::optional<std::string> CoopService::InventoryKey(uint32_t aEntity) const
{
    if (const auto it = m_inventoryBindings.find(aEntity); it != m_inventoryBindings.end()) return it->second;
    const auto entity = static_cast<entt::entity>(aEntity);
    if (const auto* character = m_world.try_get<CharacterComponent>(entity); character && character->IsPlayer()) return {};
    if (const auto* id = m_world.try_get<FormIdComponent>(entity))
        if (const auto form = NameForm(id->Id))
            if (const auto key = Coop::StorageKey(m_campaign, *form)) return "world|" + *key;
    // Runtime-only NPC identity is explicitly session-scoped. It is never mistaken for a new
    // creature with the same base NPC form after a server restart.
    if (m_world.all_of<CharacterComponent>(entity)) return "npc|" + std::to_string(m_session) + "|" + std::to_string(aEntity);
    return {};
}
bool CoopService::BindInventory(uint32_t aEntity, const std::string& aKey, bool aOwnBag) noexcept
{
    if (!InventoryReady()) return false;
    const auto entity = static_cast<entt::entity>(aEntity);
    auto* inventory = m_world.try_get<InventoryComponent>(entity);
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    const uint32_t epoch = owner ? owner->OwnershipEpoch : 0;
    if (!inventory || !inventory->Content.IsValid()) return false;
    if (const auto* old = m_inventory->Find(aKey))
    {
        if (aOwnBag)
        {
            // An old ESS/bag cannot overwrite a committed receipt and cannot be 'corrected' by
            // blind AddItem replay. Only an already matching live bag can bind/reconnect.
            if (!Coop::SameInventoryItems(old->Items, inventory->Content) || m_inventory->Locked(aKey) || !m_inventory->Observe(aKey, inventory->Content, epoch)) return false;
        }
        else
        {
            inventory->Content = old->Items;
            // Authority changes have no inventory mutation. Fence new transfers with the new epoch.
            if (!old->Storage && old->Epoch != epoch && !m_inventory->Locked(aKey) &&
                !m_inventory->Observe(aKey, inventory->Content, epoch)) return false;
        }
    }
    else
    {
        Coop::InventoryLedger::Endpoint endpoint;
        endpoint.Items = inventory->Content;
        endpoint.Epoch = epoch;
        if (!m_inventory->Register(aKey, std::move(endpoint))) return false;
    }
    m_inventoryBindings[aEntity] = aKey;
    inventory->Content.Revision = m_inventory->Find(aKey)->Revision;
    return true;
}
bool CoopService::BindObjectInventory(uint32_t aEntity) noexcept
{
    if (!Enabled() || !InventoryReady()) return false;
    const auto key = InventoryKey(aEntity);
    return key && BindInventory(aEntity, *key, false);
}
bool CoopService::ObserveInventory(uint32_t aEntity, Inventory& aContent, uint32_t aEpoch) noexcept
{
    const auto binding = m_inventoryBindings.find(aEntity);
    if (binding == m_inventoryBindings.end()) return true;
    if (!InventoryReady() || !m_inventory->Observe(binding->second, aContent, aEpoch)) return false;
    aContent.Revision = m_inventory->Find(binding->second)->Revision;
    return true;
}
bool CoopService::NearStorageAccess(Player* player, GameId point) const noexcept
{
    if (!player || !player->GetCharacter() || !player->HasMod(point.ModId)) return false;
    const auto* movement = m_world.try_get<MovementComponent>(*player->GetCharacter());
    const auto* owner = m_world.try_get<OwnerComponent>(*player->GetCharacter());
    const auto* location = m_world.try_get<CellIdComponent>(*player->GetCharacter());
    const auto access = NameForm(point), cell = NameForm(player->GetCellComponent().Cell);
    const auto worldId = player->GetCellComponent().WorldSpaceId;
    const auto world = worldId ? NameForm(worldId) : std::optional<Coop::StableForm>{};
    if (!movement || !owner || owner->GetOwner() != player || !location || location->WorldSpaceId != worldId ||
        (!worldId && location->Cell != player->GetCellComponent().Cell) || !access || !cell || (worldId && !world)) return false;
    return m_storageCatalog.InReach(*access, *cell, world, movement->Position.x, movement->Position.y, movement->Position.z);
}
bool CoopService::CanAccessInventory(Player* aPlayer, const Client& aClient, uint32_t aEntity, uint8_t aReason) const noexcept
{
    if (!aPlayer) return false;
    if (aEntity == aClient.Actor) return true;
    const auto binding = m_inventoryBindings.find(aEntity);
    if (binding != m_inventoryBindings.end())
    {
        const auto open = m_openStorage.find(aClient.PlayerId);
        const auto* endpoint = m_inventory ? m_inventory->Find(binding->second) : nullptr;
        if (endpoint && endpoint->Storage)
        {
            if (!(aClient.Capabilities & Coop::PersistentStorage) || open == m_openStorage.end()) return false;
            const auto lease = open->second.find(binding->second);
            if (lease == open->second.end() || Now() < lease->second.Seen || Now() - lease->second.Seen > 2000 ||
                !NearStorageAccess(aPlayer, lease->second.Point)) return false;
            const auto points = m_world.view<FormIdComponent, CellIdComponent>();
            for (auto point : points)
                if (points.get<FormIdComponent>(point).Id == lease->second.Point &&
                    aPlayer->GetCellComponent().WorldSpaceId == points.get<CellIdComponent>(point).WorldSpaceId &&
                    (aPlayer->GetCellComponent().WorldSpaceId || aPlayer->GetCellComponent().Cell == points.get<CellIdComponent>(point).Cell))
                {
                    const auto* object = m_world.try_get<ObjectComponent>(point);
                    return object && !object->CurrentLockData.IsLocked; // missing lock state is not unlocked proof
                }
            return false;
        }
    }
    const auto entity = static_cast<entt::entity>(aEntity);
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    const auto* character = m_world.try_get<CharacterComponent>(entity);
    if (character && (character->IsPlayer() || character->IsPlayerSummon() || character->IsMount())) return false;
    if (character && !character->IsDead()) return false; // no claimed remove reason can authorize live-NPC theft/merchant stock
    if (!character)
    {
        const auto* object = m_world.try_get<ObjectComponent>(entity);
        if (!object || object->CurrentLockData.IsLocked) return false;
    }
    return cell && aPlayer->GetCellComponent().IsInRange(*cell, character && character->IsDragon());
}
void CoopService::SendInventory(Player* aPlayer, uint32_t aEntity) noexcept
{
    const auto binding = m_inventoryBindings.find(aEntity);
    if (binding == m_inventoryBindings.end() || !m_inventory) return;
    const auto* endpoint = m_inventory->Find(binding->second);
    if (!endpoint) return;
    Coop::InventoryState state;
    state.Entity = aEntity; state.Epoch = endpoint->Epoch; state.Revision = endpoint->Revision;
    state.Blocked = !InventoryReady() || endpoint->NeedsImport || m_inventory->Locked(binding->second);
    state.Items.assign(endpoint->Items.Entries.begin(), endpoint->Items.Entries.end());
    Send(aPlayer, std::move(state));
}
void CoopService::Handle(Player* aPlayer, Client& aClient, const Coop::InventoryQuery& aQuery) noexcept
{
    if (!(aClient.Capabilities & Coop::TransactionalLoot) || !CanAccessInventory(aPlayer, aClient, aQuery.Entity, 1)) return;
    const auto key = InventoryKey(aQuery.Entity);
    if (!key || !BindInventory(aQuery.Entity, *key, aQuery.Entity == aClient.Actor)) return;
    SendInventory(aPlayer, aQuery.Entity);
}
Coop::TransferReply CoopService::TransferReply(const Coop::InventoryLedger::Record& aRecord, uint32_t aRequester) const
{
    Coop::TransferReply reply;
    reply.Operation = aRecord.Id; reply.Nonce = aRecord.Action.Nonce; reply.Requester = aRequester;
    reply.Result = static_cast<Coop::TransferResult>(aRecord.State);
    reply.Item = aRecord.Action.Item;
    reply.SourceEpoch = aRecord.Action.SourceEpoch; reply.DestinationEpoch = aRecord.Action.DestinationEpoch;
    for (const auto& [entity, key] : m_inventoryBindings)
    {
        if (!m_world.valid(static_cast<entt::entity>(entity))) continue;
        if (key == aRecord.Action.Source) reply.Source = entity;
        if (key == aRecord.Action.Destination) reply.Destination = entity;
    }
    if (aRecord.State == Coop::InventoryLedger::Status::Committed)
    {
        // An operation receipt is immutable even after later consumption/deposits. The separate
        // InventoryState messages carry the newest snapshots, not a rewritten historical receipt.
        reply.SourceRevision = aRecord.Action.SourceRevision + 1;
        reply.DestinationRevision = aRecord.Action.DestinationRevision + 1;
    }
    else if (m_inventory)
    {
        if (const auto* e = m_inventory->Find(aRecord.Action.Source)) reply.SourceRevision = e->Revision;
        if (const auto* e = m_inventory->Find(aRecord.Action.Destination)) reply.DestinationRevision = e->Revision;
    }
    if (reply.Result == Coop::TransferResult::Conflict) reply.Reason = "Inventory changed or another transfer holds this endpoint; nothing moved";
    if (reply.Result == Coop::TransferResult::Rejected) reply.Reason = "Exact instance/quantity/authority not available; nothing moved";
    if (reply.Result == Coop::TransferResult::Quarantined) reply.Reason = "Transfer outcome uncertain. Endpoints locked; do not retry by BaseId";
    return reply;
}
void CoopService::PublishTransfer(const Coop::InventoryLedger::Record& aRecord, uint32_t aRequester, uint32_t aGroup) noexcept
{
    const auto reply = TransferReply(aRecord, aRequester);
    if (aRecord.State == Coop::InventoryLedger::Status::Committed && InventoryReady())
        for (const auto& [entity, key] : m_inventoryBindings)
            if ((key == aRecord.Action.Source || key == aRecord.Action.Destination) && m_world.valid(static_cast<entt::entity>(entity)))
                if (auto* inventory = m_world.try_get<InventoryComponent>(static_cast<entt::entity>(entity)))
                {
                    const auto* endpoint = m_inventory->Find(key);
                    inventory->Content = endpoint->Items;
                    inventory->Content.Revision = endpoint->Revision;
                }
    Broadcast(aGroup, reply);
    for (const auto& [id, client] : m_clients)
        {
            auto* player = m_world.GetPlayerManager().GetById(id);
            // Both absolute states follow the receipt on the reliable stream. The origin verifies
            // its already-applied bag instead of replaying the transfer from this notification.
            if (reply.Source && CanAccessInventory(player, client, reply.Source, 1)) SendInventory(player, reply.Source);
            if (reply.Destination && CanAccessInventory(player, client, reply.Destination, 1)) SendInventory(player, reply.Destination);
        }
}
void CoopService::Handle(Player* aPlayer, Client& aClient, const Coop::Transfer& aTransfer) noexcept
{
    Coop::TransferReply denied;
    denied.Nonce = aTransfer.Nonce; denied.Requester = aClient.Actor;
    denied.Source = aTransfer.Source; denied.Destination = aTransfer.Destination;
    denied.Result = Coop::TransferResult::Rejected;
    denied.Reason = "Transfer requires negotiated durable inventory, own bag and an accessible shared endpoint";
    if (!(aClient.Capabilities & Coop::TransactionalLoot) || !InventoryReady() || aClient.Downed ||
        (aTransfer.Source != aClient.Actor && aTransfer.Destination != aClient.Actor) ||
        !CanAccessInventory(aPlayer, aClient, aTransfer.Source, aTransfer.NativeReason) ||
        !CanAccessInventory(aPlayer, aClient, aTransfer.Destination, aTransfer.NativeReason))
    { Send(aPlayer, denied); return; }
    if (!ItemAvailable(aPlayer, aTransfer.Item))
    { denied.Reason = "Unsupported item or form/plugin kind absent from this character's negotiated profile"; Send(aPlayer, denied); return; }
    const auto source = InventoryKey(aTransfer.Source), destination = InventoryKey(aTransfer.Destination);
    if (!source || !destination || !BindInventory(aTransfer.Source, *source, false) || !BindInventory(aTransfer.Destination, *destination, false))
    { Send(aPlayer, denied); return; }
    Coop::InventoryLedger::Request request;
    request.Character = aClient.Character; request.Source = *source; request.Destination = *destination;
    request.Nonce = aTransfer.Nonce; request.SourceRevision = aTransfer.SourceRevision; request.DestinationRevision = aTransfer.DestinationRevision;
    request.SourceEpoch = aTransfer.SourceEpoch; request.DestinationEpoch = aTransfer.DestinationEpoch; request.Item = aTransfer.Item;
    const auto result = m_inventory->Prepare(request, Now());
    auto reply = TransferReply(result, aClient.Actor);
    // Rejected requests have no persisted record and therefore no reverse binding to consult.
    reply.Source = aTransfer.Source; reply.Destination = aTransfer.Destination;
    Send(aPlayer, std::move(reply));
    if (result.State != Coop::InventoryLedger::Status::Prepared)
    { SendInventory(aPlayer, aTransfer.Source); SendInventory(aPlayer, aTransfer.Destination); }
}
void CoopService::Handle(Player*, Client& aClient, const Coop::TransferReceipt& aReceipt) noexcept
{
    if (!(aClient.Capabilities & Coop::TransactionalLoot) || !m_inventory) return;
    const auto result = m_inventory->Receipt(aReceipt.Operation, aClient.Character, aReceipt.Applied, aReceipt.Uncertain);
    if (result.Id) PublishTransfer(result, aClient.Actor, aClient.Group);
}
void CoopService::TickInventory() noexcept
{
    if (!InventoryReady()) return;
    std::vector<uint64_t> expired;
    for (const auto& [id, record] : m_inventory->Snapshot().Records)
        if (record.State == Coop::InventoryLedger::Status::Prepared && Now() >= record.Deadline) expired.push_back(id);
    if (expired.empty()) return;
    m_inventory->Expire(Now());
    for (auto id : expired)
        if (const auto* record = m_inventory->Find(id))
            for (const auto& [_, client] : m_clients)
                if (client.Character == record->Action.Character) PublishTransfer(*record, client.Actor, client.Group);
}
void CoopService::Handle(Player* aPlayer, Client& aClient, const Coop::StorageOpen& aOpen) noexcept
{
    if (!aPlayer || aClient.Downed || !(aClient.Capabilities & Coop::PersistentStorage) || !InventoryReady()) return;
    const auto accessForm = NameForm(aOpen.AccessPoint), backingForm = NameForm(aOpen.Backing);
    const auto access = accessForm ? Coop::StorageKey(m_campaign, *accessForm) : std::nullopt;
    const auto backing = backingForm ? Coop::StorageKey(m_campaign, *backingForm) : std::nullopt;
    const auto claimed = Coop::NormalizePlugin(aOpen.Plugin);
    if (!aPlayer->HasMod(aOpen.AccessPoint.ModId) || !aPlayer->HasMod(aOpen.Backing.ModId)) return;
    if (!access || !backing || !claimed || !backingForm || *claimed != *Coop::NormalizePlugin(backingForm->Plugin) ||
        backingForm->Local != aOpen.LocalRef || !m_storageAccess.contains(*access) || m_storageAccess.at(*access) != *backing) return;
    bool nearby{};
    const auto refs = m_world.view<FormIdComponent, CellIdComponent, ObjectComponent>();
    for (auto entity : refs)
        if (refs.get<FormIdComponent>(entity).Id == aOpen.AccessPoint &&
            !refs.get<ObjectComponent>(entity).CurrentLockData.IsLocked &&
            aPlayer->GetCellComponent().WorldSpaceId == refs.get<CellIdComponent>(entity).WorldSpaceId &&
            (aPlayer->GetCellComponent().WorldSpaceId || aPlayer->GetCellComponent().Cell == refs.get<CellIdComponent>(entity).Cell)) nearby = true;
    if (!nearby || !NearStorageAccess(aPlayer, aOpen.AccessPoint)) return;
    // Validate BEFORE registering/writing any bank. A forged foreign form must not turn a
    // harmless rejected import into a failed durable journal write that faults the campaign.
    if (aOpen.ConfirmImport)
        for (const auto& item : aOpen.Import) if (!ItemAvailable(aPlayer, item)) return;
    if (!m_inventory->Find(*backing))
    {
        Coop::InventoryLedger::Endpoint endpoint;
        endpoint.Storage = endpoint.NeedsImport = true;
        if (!m_inventory->Register(*backing, std::move(endpoint))) return;
    }
    if (aOpen.ConfirmImport && m_inventory->Find(*backing)->NeedsImport)
    {
        Inventory imported;
        imported.Entries.assign(aOpen.Import.begin(), aOpen.Import.end());
        if (!m_inventory->ImportStorage(*backing, imported, true)) return;
    }
    uint32_t server{};
    for (const auto& [id, key] : m_inventoryBindings)
        if (key == *backing && m_world.valid(static_cast<entt::entity>(id))) { server = id; break; }
    if (!server)
    {
        const auto entity = m_world.create();
        server = World::ToInteger(entity);
        m_world.emplace<FormIdComponent>(entity, aOpen.Backing);
        m_world.emplace<InventoryComponent>(entity);
        m_inventoryBindings[server] = *backing;
    }
    const auto* endpoint = m_inventory->Find(*backing);
    auto& inventory = m_world.get<InventoryComponent>(static_cast<entt::entity>(server));
    inventory.Content = endpoint->Items; inventory.Content.Revision = endpoint->Revision;
    m_openStorage[aClient.PlayerId][*backing] = {aOpen.AccessPoint, Now()};
    Coop::StorageState state;
    state.Backing = aOpen.Backing;
    state.Key = *backing; state.Entity = server; state.Revision = endpoint->Revision; state.NeedsImport = endpoint->NeedsImport;
    state.Items.assign(endpoint->Items.Entries.begin(), endpoint->Items.Entries.end());
    Send(aPlayer, std::move(state));
}
