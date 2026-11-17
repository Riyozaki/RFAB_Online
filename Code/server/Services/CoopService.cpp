#include <Services/CoopService.h>
#include <World.h>
#include <GameServer.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/NotifyCoopAction.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Coop/EnemyHealth.h>
#include <Setting.h>
#include <chrono>
#include <limits>
#include <random>

namespace
{
Console::Setting bRfabCoop{"RFAB:bEnableCoop", "Enable negotiated RFAB co-op mechanics (two-player cohorts)", true};
constexpr uint32_t kBuiltInCapabilities = Coop::NativeDowned | Coop::GroundedAnchors;
uint64_t MonotonicMs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
float MaximumHealth(World& aWorld, uint32_t aActor) noexcept
{
    const auto* values = aWorld.try_get<ActorValuesComponent>(static_cast<entt::entity>(aActor));
    if (!values)
        return 0.f;
    const auto it = values->CurrentActorValues.ActorMaxValuesList.find(24);
    return it != values->CurrentActorValues.ActorMaxValuesList.end() && std::isfinite(it->second) && it->second > 0.f ? it->second : 0.f;
}
} // namespace

CoopService::CoopService(World& aWorld, entt::dispatcher& aDispatcher) : m_world(aWorld)
{
    std::random_device random;
    m_session = (static_cast<uint64_t>(random()) << 32) ^ random() ^ MonotonicMs();
    if (!m_session)
        m_session = 1;
    m_now = MonotonicMs();
    m_scaleNonce = m_session & ((1ull << 60) - 1);
    InitializeInventory();
    InitializeRewards();
    m_packetConnection = aDispatcher.sink<PacketEvent<RequestCoopAction>>().connect<&CoopService::OnPacket>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CoopService::OnUpdate>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&CoopService::OnLeave>(this);
}

bool CoopService::Enabled() noexcept { return bRfabCoop; }
uint64_t CoopService::Now() const noexcept { return m_now; }
bool CoopService::Manages(uint32_t aActor) const noexcept
{
    for (const auto& [_, client] : m_clients)
        if (client.Actor == aActor)
            return true;
    return false;
}
bool CoopService::IsDowned(uint32_t aActor) const noexcept
{
    for (const auto& [_, group] : m_groups)
        if (const auto it = group.Encounter.Members.find(aActor); it != group.Encounter.Members.end())
            return it->second.Downed;
    return false;
}
void CoopService::Send(Player* aPlayer, Coop::Body aBody) noexcept
{
    if (!aPlayer)
        return;
    NotifyCoopAction message;
    message.Data.Session = m_session;
    message.Data.Generation = m_generation;
    message.Data.Operation = ++m_sequence;
    message.Data.Content = std::move(aBody);
    aPlayer->Send(message);
}
void CoopService::Broadcast(uint32_t aGroup, const Coop::Body& aBody) noexcept
{
    for (const auto& [id, client] : m_clients)
        if (client.Group == aGroup)
            Send(m_world.GetPlayerManager().GetById(id), aBody);
}
void CoopService::OnPacket(const PacketEvent<RequestCoopAction>& aEvent) noexcept
{
    const auto& packet = aEvent.Packet.Data;
    if (!bRfabCoop || !aEvent.pPlayer || !packet.Valid)
        return;
    if (const auto* hello = std::get_if<Coop::Hello>(&packet.Content))
    {
        Hello(aEvent.pPlayer, packet, *hello);
        return;
    }
    auto client = m_clients.find(aEvent.pPlayer->GetId());
    if (client == m_clients.end() || packet.Session != m_session || packet.Generation != m_generation)
        return;
    const bool idempotent = std::holds_alternative<Coop::Transfer>(packet.Content) ||
        std::holds_alternative<Coop::TransferReceipt>(packet.Content) || std::holds_alternative<Coop::InventoryQuery>(packet.Content) ||
        std::holds_alternative<Coop::Reward>(packet.Content) || std::holds_alternative<Coop::RewardReceipt>(packet.Content);
    std::vector<uint8_t> bytes;
    if (idempotent)
    {
        TiltedPhoques::Buffer buffer(8192);
        TiltedPhoques::Buffer::Writer writer(&buffer);
        packet.Serialize(writer);
        bytes.assign(buffer.GetData(), buffer.GetData() + writer.Size());
    }
    if (packet.Operation <= client->second.LastOperation)
    {
        const auto previous = client->second.Retries.find(packet.Operation);
        if (!idempotent || previous == client->second.Retries.end() || previous->second != bytes) return;
        // Same sequence AND exact body: answer from durable nonce/receipt state. No unrelated
        // presence/revive/reset action can be executed by reusing an old packet sequence.
    }
    else
    {
        client->second.LastOperation = packet.Operation;
        if (idempotent)
        {
            auto& retries = client->second.Retries;
            retries.emplace(packet.Operation, std::move(bytes));
            if (retries.size() > 8) retries.erase(retries.begin());
        }
    }
    std::visit([&](const auto& action) { Handle(aEvent.pPlayer, client->second, action); }, packet.Content);
    Flush(client->second.Group);
}
void CoopService::Hello(Player* aPlayer, const Coop::Packet& aPacket, const Coop::Hello& aHello) noexcept
{
    Coop::Welcome response;
    response.Campaign = m_campaign;
    if (aHello.Protocol != Coop::kProtocol || aPacket.Session != 0 || aPacket.Generation != 0 ||
        aHello.Character.empty() || aHello.Character.size() > 96 || !aPlayer->GetCharacter() ||
        !(aHello.Capabilities & Coop::NativeDowned))
    {
        response.Error = "Missing RFAB protocol, character identity, or native downed capability";
        Send(aPlayer, response);
        return;
    }
    if (auto it = m_clients.find(aPlayer->GetId()); it != m_clients.end())
    {
        response.Capabilities = it->second.Capabilities;
        Send(aPlayer, response);
        m_groups.at(it->second.Group).Encounter.Snapshot();
        Flush(it->second.Group);
        return;
    }
    for (const auto& [_, client] : m_clients)
        if (client.Character == aHello.Character)
        {
            response.Error = "This character identity is already connected";
            Send(aPlayer, response);
            return;
        }
    if ((aHello.Capabilities & Coop::TransactionalLoot) && !RestoreInventoryFor(aPlayer))
    {
        response.Error = "Durable campaign inventory unavailable or its saved forms are absent from this negotiated profile; no empty bootstrap";
        Send(aPlayer, response);
        return;
    }
    bool storageProfile = !m_storageAccess.empty();
    if (aHello.Capabilities & Coop::PersistentStorage)
        for (const auto& rule : m_storageCatalog.Rules())
            for (const auto& form : {rule.Access, rule.Backing, rule.Location.Cell, rule.Location.World.value_or(Coop::StableForm{})})
            {
                if (!form.Local) continue; // no world only for an interior
                const auto id = m_world.ctx().at<ModsComponent>().Find(String(form.Plugin.data(), form.Plugin.size()), form.Light);
                storageProfile &= id && aPlayer->HasMod(*id);
            }
    if ((aHello.Capabilities & Coop::PersistentStorage) &&
        (!storageProfile || !aHello.StorageSignature || aHello.StorageSignature != m_storageCatalog.Signature()))
    {
        response.Error = "Client/server secure-storage catalogs differ or are missing. Deploy the same installed-profile output to both ends";
        Send(aPlayer, response);
        return;
    }
    const uint32_t actor = World::ToInteger(*aPlayer->GetCharacter());
    const auto currentParty = aPlayer->GetParty().JoinedPartyId;
    uint32_t group = currentParty ? *currentParty : aPlayer->GetId() | 0x80000000u;
    if (m_characters.size() >= 256 && !m_characters.contains(aHello.Character))
    {
        response.Error = "Session character limit reached; end the campaign session cleanly before admitting new identities";
        Send(aPlayer, response);
        return;
    }
    const auto old = m_characters.find(aHello.Character);
    if (old != m_characters.end() && m_groups.contains(old->second.first))
    {
        group = old->second.first;
    }
    if (!AttachRewards(aHello, group))
    {
        response.Error = "Semantic XP adapter/profile or saved reward receipts differ; unresolved awards are never replayed or replaced by legacy XP";
        Send(aPlayer, response);
        return;
    }
    auto& runtime = m_groups[group].Encounter;
    Client client;
    client.PlayerId = aPlayer->GetId(); client.Actor = actor; client.Group = group;
    client.Character = aHello.Character;
    client.LastOperation = aPacket.Operation;
    // Adapter-dependent capabilities are NEVER inferred from a filename or claimed by the server.
    client.Capabilities = aHello.Capabilities & kBuiltInCapabilities;
    if (aHello.Capabilities & Coop::SemanticRewards)
    {
        client.Capabilities |= Coop::SemanticRewards | (aHello.Capabilities & Coop::BizarreAdapter);
        client.RewardSignature = aHello.RewardSignature; client.RewardKinds = aHello.RewardKinds;
    }
    if (InventoryReady() && (aHello.Capabilities & Coop::TransactionalLoot))
    {
        const auto key = "player|" + client.Character;
        if (!BindInventory(actor, key, true))
        {
            response.Error = "Inventory receipt mismatch or unresolved transfer: no automatic replay/import of this character's bag";
            Send(aPlayer, response);
            return;
        }
        client.Capabilities |= Coop::TransactionalLoot;
        if (aHello.Capabilities & Coop::EnemyScaling) client.Capabilities |= Coop::EnemyScaling;
        if (!m_storageAccess.empty() && (aHello.Capabilities & Coop::PersistentStorage))
            client.Capabilities |= Coop::PersistentStorage;
    }
    // Do not rebind/remove a recovering cohort member before its character and durable bag
    // passed validation. Failed handshakes must not consume a slot or move the old downed latch.
    if (old != m_characters.end() && old->second.first == group)
        runtime.Rebind(old->second.second, actor);
    if (!runtime.Join(actor))
    {
        response.Error = "RFAB cohorts are limited to two characters";
        Send(aPlayer, response);
        return;
    }
    m_clients.emplace(client.PlayerId, client);
    m_characters[aHello.Character] = {group, actor};
    response.Capabilities = client.Capabilities;
    Send(aPlayer, response);
    runtime.Snapshot();
    Flush(group);
    spdlog::info("[RFAB] joined actor {:X}, cohort {}, session {}, capabilities {}", actor, group, m_session, client.Capabilities);
}
void CoopService::Handle(Player* aPlayer, Client& aClient, const Coop::Presence& aPresence) noexcept
{
    if (aPresence.Flags & Coop::Loading)
    {
        m_groups.at(aClient.Group).Encounter.Observe(aClient.Actor, aPresence, Now());
        return;
    }
    const auto entity = static_cast<entt::entity>(aClient.Actor);
    const auto* movement = m_world.try_get<MovementComponent>(entity);
    const auto& cell = aPlayer->GetCellComponent();
    if (!movement || !aPresence.Location.Valid() || aPresence.Location.World != cell.WorldSpaceId ||
        (!cell.WorldSpaceId && aPresence.Location.Cell != cell.Cell))
        return;
    Coop::Pose authoritative = aPresence.Location;
    authoritative.X = movement->Position.x; authoritative.Y = movement->Position.y; authoritative.Z = movement->Position.z;
    if (Coop::DistanceSquared(authoritative, aPresence.Location) > 384.0 * 384.0)
        return;
    m_groups.at(aClient.Group).Encounter.Observe(aClient.Actor, aPresence, Now());
}
void CoopService::Handle(Player*, Client& aClient, const Coop::Revive& aRevive) noexcept
{
    m_groups.at(aClient.Group).Encounter.ReviveHold(aClient.Actor, aRevive, Now());
}
void CoopService::Handle(Player*, Client& aClient, const Coop::Anchor& aAnchor) noexcept
{
    if (aClient.Capabilities & Coop::GroundedAnchors)
        m_groups.at(aClient.Group).Encounter.RegisterAnchor(aClient.Actor, aAnchor, Now());
}
void CoopService::Handle(Player*, Client& aClient, const Coop::ResetReceipt& aReceipt) noexcept
{
    m_groups.at(aClient.Group).Encounter.ResetAck(aClient.Actor, aReceipt, Now());
}
uint32_t CoopService::AdvanceEpoch(uint32_t aActor, float aHealth) noexcept
{
    const auto entity = static_cast<entt::entity>(aActor);
    auto* owner = m_world.try_get<OwnerComponent>(entity);
    auto* values = m_world.try_get<ActorValuesComponent>(entity);
    if (!owner || !values || !std::isfinite(aHealth))
        return 0;
    if (++owner->OwnershipEpoch == 0)
        ++owner->OwnershipEpoch;
    if (auto* movement = m_world.try_get<MovementComponent>(entity))
        movement->Tick = 0;
    if (auto* animation = m_world.try_get<AnimationComponent>(entity))
        animation->ActionsReplayCache.Clear();
    values->HealthEpoch = owner->OwnershipEpoch;
    values->CurrentActorValues.HealthSequences.clear();
    values->CurrentActorValues.ActorValuesList[24] = std::max(0.f, aHealth);
    ++values->CurrentActorValues.HealthRevision;
    return owner->OwnershipEpoch;
}
void CoopService::FillRecovery(Coop::DownedState& aState) noexcept
{
    for (auto& [_, client] : m_clients)
    {
        if (client.Actor != aState.Actor)
            continue;
        if (client.Downed != aState.Downed)
        {
            client.Downed = aState.Downed;
            const float maximum = MaximumHealth(m_world, aState.Actor);
            AdvanceEpoch(aState.Actor, aState.Downed ? 0.f : std::max(1.f, maximum * 0.25f));
        }
    }
    const auto entity = static_cast<entt::entity>(aState.Actor);
    if (const auto* owner = m_world.try_get<OwnerComponent>(entity))
        aState.Epoch = owner->OwnershipEpoch;
    if (const auto* values = m_world.try_get<ActorValuesComponent>(entity))
    {
        aState.HealthRevision = values->CurrentActorValues.HealthRevision;
        const auto it = values->CurrentActorValues.ActorValuesList.find(24);
        if (it != values->CurrentActorValues.ActorValuesList.end())
            aState.Health = it->second;
    }
}
void CoopService::FillReset(uint32_t aGroup, Coop::ResetPlan& aPlan) noexcept
{
    if (aPlan.Phase != Coop::ResetPhase::Commit)
        return;
    // Only surviving enemies of this encounter. No resurrection, inventory reset or quest rollback.
    for (auto id : m_groups.at(aGroup).Enemies)
    {
        const auto entity = static_cast<entt::entity>(id);
        const auto* character = m_world.try_get<CharacterComponent>(entity);
        if (!character || character->IsPlayer() || character->IsDead())
            continue;
        Coop::ResetActor actor;
        actor.Actor = id;
        aPlan.Actors.push_back(actor);
    }
    for (auto& actor : aPlan.Actors)
    {
        actor.Health = MaximumHealth(m_world, actor.Actor);
        actor.Epoch = AdvanceEpoch(actor.Actor, actor.Health);
        if (actor.IsPlayer)
            for (auto& [_, client] : m_clients)
                if (client.Actor == actor.Actor)
                    client.Downed = false; // Complete's downed snapshot must not re-apply 25% revive HP
        if (const auto* values = m_world.try_get<ActorValuesComponent>(static_cast<entt::entity>(actor.Actor)))
            actor.HealthRevision = values->CurrentActorValues.HealthRevision;
    }
}
void CoopService::Handle(Player* aPlayer, Client& aClient, const Coop::EnemyContact& aContact) noexcept
{
    auto& group = m_groups.at(aClient.Group);
    const auto entity = static_cast<entt::entity>(aContact.Actor);
    const auto* character = m_world.try_get<CharacterComponent>(entity);
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    if (!group.Encounter.Active || !aContact.Hostile || aContact.Allied || !character ||
        character->IsPlayer() || character->IsDead() || !cell || !owner || !owner->GetOwner() ||
        owner->OwnershipEpoch != aContact.Epoch || !aPlayer->GetCellComponent().IsInRange(*cell, character->IsDragon()))
        return;
    const auto ownerClient = m_clients.find(owner->GetOwner()->GetId());
    if (aContact.Scripted || ownerClient == m_clients.end() || ownerClient->second.Group != aClient.Group ||
        group.Enemies.size() >= 512)
    {
        group.Encounter.Unsafe = true; // no reset of an arena/AI participant for which there is no coordinated adapter
        return;
    }
    group.Enemies.insert(aContact.Actor);
    ScaleEnemy(aPlayer, aClient, aContact);
}

bool CoopService::CanScaleEncounter(uint32_t aGroup) const noexcept
{
    const auto& runtime = m_groups.at(aGroup).Encounter;
    if (runtime.Cohort.size() != 2) return false;
    const Coop::EncounterRuntime::Member* first{};
    for (auto actor : runtime.Cohort)
    {
        const auto& member = runtime.Members.at(actor);
        if (!member.Connected || !member.Seen || Now() < member.Seen || Now() - member.Seen > Coop::kHeartbeatTimeoutMs ||
            (member.Flags & Coop::Loading)) return false;
        bool capable{};
        for (const auto& [_, client] : m_clients)
            if (client.Group == aGroup && client.Actor == actor) capable = (client.Capabilities & Coop::EnemyScaling) != 0;
        if (!capable || (first && Coop::DistanceSquared(first->Location, member.Location) > 4096.0 * 4096.0)) return false;
        first = &member;
    }
    return true;
}
void CoopService::ScaleEnemy(Player* player, const Client& client, const Coop::EnemyContact& contact) noexcept
{
    const auto actor = contact.Actor;
    auto& group = m_groups.at(client.Group);
    const auto entity = static_cast<entt::entity>(actor);
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    const auto* character = m_world.try_get<CharacterComponent>(entity);
    auto* component = m_world.try_get<ActorValuesComponent>(entity);
    if (!owner || owner->GetOwner() != player || !character || !component || !group.ScaleEncounter ||
        !m_world.all_of<FormIdComponent>(entity) || // dynamic spawned refs need a separate persistent-life adapter
        group.Encounter.Blocked || group.Encounter.Resetting || group.Encounter.Unsafe || !(client.Capabilities & Coop::EnemyScaling) ||
        !Coop::EnemyEligibility{character->IsPlayer(), contact.Allied, character->IsPlayerSummon(), character->IsMount(),
            character->IsDead(), contact.Scripted, contact.Hostile}.Allowed()) return;
    for (const auto* factions : {&character->FactionsContent.NpcFactions, &character->FactionsContent.ExtraFactions})
        for (const auto& faction : *factions)
            if (faction.Rank >= 0)
                if (const auto form = NameForm(faction.Id); form && form->Local == 0xDB1)
                    if (const auto plugin = Coop::NormalizePlugin(form->Plugin); plugin && *plugin == "skyrim.esm") return;
    auto& values = component->CurrentActorValues;
    if (values.Scale.Life) return; // save import/handoff/next fight is not a second multiplier
    if (m_scaleLives.size() >= 8192 || values.HealthRevision == std::numeric_limits<uint64_t>::max())
    { group.Encounter.Unsafe = true; return; }
    uint64_t life{};
    for (size_t attempt = 0; attempt <= 8192; ++attempt)
    {
        m_scaleNonce = (m_scaleNonce + 1) & ((1ull << 60) - 1);
        if (!m_scaleNonce || m_scaleLives.contains(m_scaleNonce)) continue;
        bool used{};
        for (auto other : m_world.view<ActorValuesComponent>())
            used |= m_world.get<ActorValuesComponent>(other).CurrentActorValues.Scale.Life == m_scaleNonce;
        if (!used) { life = m_scaleNonce; break; }
    }
    const auto maximum = values.ActorMaxValuesList.find(24), health = values.ActorValuesList.find(24);
    if (maximum == values.ActorMaxValuesList.end() || health == values.ActorValuesList.end()) return;
    const auto plan = Coop::PlanEnemyHealth(life, maximum->second, health->second);
    if (!plan) { group.Encounter.Unsafe = true; return; }
    m_scaleLives.insert(life);
    values.Scale = plan->Scale;
    values.ActorMaxValuesList[24] = plan->Scale.Maximum;
    values.ActorValuesList[24] = plan->Health;
    ++values.HealthRevision;
    // Each recipient gets its own included watermark. A scale control snapshot must not double
    // a hit whose older health ACK was overtaken by this absolute canonical revision.
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    for (auto* recipient : m_world.GetPlayerManager())
    {
        if (!cell || !cell->IsInRange(recipient->GetCellComponent(), character->IsDragon())) continue;
        NotifyHealthChangeBroadcast notify;
        notify.Id = actor; notify.OwnershipEpoch = owner->OwnershipEpoch;
        notify.Revision = values.HealthRevision; notify.Health = health->second; notify.Scale = values.Scale;
        notify.SourcePlayerId = recipient->GetId();
        const auto ack = values.HealthSequences.find(recipient->GetId());
        if (ack != values.HealthSequences.end()) notify.Sequence = ack->second;
        recipient->Send(notify);
    }
    spdlog::info("[RFAB] enemy {:X} life {} HP x2: base {}, maximum {}, health {}, epoch {}", actor, life,
        values.Scale.Base, values.Scale.Maximum, health->second, owner->OwnershipEpoch);
}

void CoopService::Flush(uint32_t aGroup) noexcept
{
    auto it = m_groups.find(aGroup);
    if (it == m_groups.end())
        return;
    for (auto body : it->second.Encounter.Drain())
    {
        if (const auto* encounter = std::get_if<Coop::EncounterState>(&body);
            encounter && encounter->Phase == Coop::EncounterPhase::Started && encounter->Encounter != it->second.LastEncounter)
        {
            it->second.LastEncounter = encounter->Encounter;
            it->second.Enemies.clear();
            it->second.ScaleEncounter = CanScaleEncounter(aGroup);
        }
        if (auto* downed = std::get_if<Coop::DownedState>(&body))
            FillRecovery(*downed);
        if (auto* reset = std::get_if<Coop::ResetPlan>(&body))
            FillReset(aGroup, *reset);
        Broadcast(aGroup, body);
    }
}
void CoopService::Tick(uint64_t aNowMs) noexcept
{
    if (aNowMs < m_now)
        return;
    m_now = aNowMs;
    TickInventory();
    TickRewards();
    for (auto& [id, group] : m_groups)
    {
        group.Encounter.Tick(m_now);
        group.Encounter.CheckTimeout(m_now);
        Flush(id);
    }
}
void CoopService::OnUpdate(const UpdateEvent&) noexcept { Tick(MonotonicMs()); }
void CoopService::OnLeave(const PlayerLeaveEvent& aEvent) noexcept
{
    if (!aEvent.pPlayer)
        return;
    const auto it = m_clients.find(aEvent.pPlayer->GetId());
    if (it == m_clients.end())
        return;
    const auto group = it->second.Group, actor = it->second.Actor;
    if (m_rewards) m_rewards->Disconnect(it->second.Character);
    m_openStorage.erase(it->first);
    m_clients.erase(it);
    m_groups.at(group).Encounter.Disconnect(actor);
    Flush(group);
}
