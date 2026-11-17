#include <Services/CoopService.h>
#include <World.h>
#include <GameServer.h>
#include <Components.h>
#include <Events/UpdateEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Messages/NotifyCoopAction.h>
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
    InitializeInventory();
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
        std::holds_alternative<Coop::TransferReceipt>(packet.Content) || std::holds_alternative<Coop::InventoryQuery>(packet.Content);
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
        m_groups.at(group).Encounter.Rebind(old->second.second, actor);
    }
    auto& runtime = m_groups[group].Encounter;
    if (!runtime.Join(actor))
    {
        response.Error = "RFAB cohorts are limited to two characters";
        Send(aPlayer, response);
        return;
    }
    Client client;
    client.PlayerId = aPlayer->GetId(); client.Actor = actor; client.Group = group;
    client.Character = aHello.Character;
    client.LastOperation = aPacket.Operation;
    // Adapter-dependent capabilities are NEVER inferred from a filename or claimed by the server.
    client.Capabilities = aHello.Capabilities & kBuiltInCapabilities;
    if (InventoryReady() && (aHello.Capabilities & Coop::TransactionalLoot))
    {
        const auto key = "player|" + client.Character;
        if (!BindInventory(actor, key, true))
        {
            response.Error = "Inventory receipt mismatch or unresolved transfer: no automatic replay/import of this character's bag";
            Send(aPlayer, response);
            runtime.Disconnect(actor);
            return;
        }
        client.Capabilities |= Coop::TransactionalLoot;
        if (!m_storageAccess.empty() && (aHello.Capabilities & Coop::PersistentStorage))
            client.Capabilities |= Coop::PersistentStorage;
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
    m_openStorage.erase(it->first);
    m_clients.erase(it);
    m_groups.at(group).Encounter.Disconnect(actor);
    Flush(group);
}
