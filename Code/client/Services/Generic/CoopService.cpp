#include <TiltedOnlinePCH.h>
#include <Services/CoopService.h>
#include <Services/ActorValueService.h>
#include <Services/InventoryHook.h>
#include <Services/SaveStateService.h>
#include <Services/InteractionInput.h>
#include <Events/CoopActionEvent.h>
#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HitEvent.h>
#include <Events/WorldLoadedEvent.h>
#include <Messages/NotifyCoopAction.h>
#include <Messages/RequestCoopAction.h>
#include <PlayerCharacter.h>
#include <Actor.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESQuest.h>
#include <AI/AIProcess.h>
#include <Havok/bhkCharacterController.h>
#include <Interface/UI.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Systems/OwnershipSystem.h>
#include <Systems/InterpolationSystem.h>
#include <Systems/AnimationSystem.h>
#include <Coop/Encounter.h>
#include <random>

namespace
{
std::string NewCharacterToken()
{
    std::random_device random;
    return fmt::format("transient-{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}
TESObjectCELL* ResolveCell(World& aWorld, const Coop::Pose& aPose)
{
    if (!aPose.Valid())
        return nullptr;
    auto& mods = aWorld.GetModSystem();
    if (!aPose.World)
        return Cast<TESObjectCELL>(TESForm::GetById(mods.GetGameId(aPose.Cell)));
    auto* world = Cast<TESWorldSpace>(TESForm::GetById(mods.GetGameId(aPose.World)));
    if (!world)
        return nullptr;
    const auto coords = GridCellCoords::CalculateGridCellCoords(aPose.X, aPose.Y);
    return world->LoadCell(coords.X, coords.Y);
}
void NativeDowned(Actor* aActor, bool aDowned, float aHealth)
{
    auto* extension = aActor->GetExtension();
    extension->CoopDowned.store(aDowned, std::memory_order_relaxed);
    aActor->SetPlayerRespawnMode(true);
    aActor->SetNoBleedoutRecovery(aDowned);
    aActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, aHealth);
    // ActorState::ACTOR_LIFE_STATE occupies bits 21..24 in SE 1.5.97. Do not kill,
    // resurrect/reset the reference, or repeatedly restart its whole animation graph.
    const uint32_t lifeState = aDowned ? 7u : 0u;
    aActor->actorState.flags1 = (aActor->actorState.flags1 & ~(0xFu << 21)) | (lifeState << 21);
    BSFixedString event(aDowned ? "BleedoutStart" : "BleedoutStop");
    aActor->SendAnimationEvent(&event);
}
} // namespace

CoopService::CoopService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld), m_transport(aTransport), m_character(NewCharacterToken())
{
    m_update = aDispatcher.sink<UpdateEvent>().connect<&CoopService::OnUpdate>(this);
    m_connected = aDispatcher.sink<ConnectedEvent>().connect<&CoopService::OnConnected>(this);
    m_disconnected = aDispatcher.sink<DisconnectedEvent>().connect<&CoopService::OnDisconnected>(this);
    m_packet = aDispatcher.sink<NotifyCoopAction>().connect<&CoopService::OnPacket>(this);
    m_hit = aDispatcher.sink<HitEvent>().connect<&CoopService::OnHit>(this);
    m_loaded = aDispatcher.sink<WorldLoadedEvent>().connect<&CoopService::OnWorldLoaded>(this);
}
CoopService::~CoopService() noexcept = default;
void CoopService::SetPaused(bool aPaused) noexcept
{
    m_paused = aPaused;
    if (auto* player = PlayerCharacter::Get())
        player->GetExtension()->CoopInputBlocked.store(aPaused || m_inventoryHeld || player->GetExtension()->CoopDowned.load(std::memory_order_relaxed), std::memory_order_relaxed);
}
uint64_t CoopService::Now() const noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t CoopService::LocalActor() const noexcept
{
    const auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (auto entity : view)
        if (view.get<FormIdComponent>(entity).Id == 0x14)
            return view.get<LocalComponent>(entity).Id;
    return 0;
}
bool CoopService::Downed(uint32_t aServerId) const noexcept
{
    const auto it = m_downed.find(aServerId);
    return it != m_downed.end() && it->second.Downed;
}
void CoopService::Send(Coop::Body aBody) noexcept
{
    RequestCoopAction request;
    request.Data.Session = std::holds_alternative<Coop::Hello>(aBody) ? 0 : m_session;
    request.Data.Generation = std::holds_alternative<Coop::Hello>(aBody) ? 0 : m_generation;
    request.Data.Operation = ++m_operation;
    request.Data.Content = std::move(aBody);
    m_transport.Send(request);
}
void CoopService::OnConnected(const ConnectedEvent&) noexcept
{
    m_active = false;
    m_session = m_generation = m_serverOperation = m_operation = m_helloAt = m_lastPoll = 0;
    m_safeSince = m_lastAnchor = m_hold = m_target = m_encounter = 0;
    m_reportedEnemies.clear();
    m_downed.clear(); m_appliedRevision.clear(); m_appliedFormId.clear();
}
void CoopService::OnDisconnected(const DisconnectedEvent& aEvent) noexcept
{
    m_active = false;
    if (aEvent.ForWorldLoad)
        return; // WorldLoaded handles the NEW timeline, without touching an old actor snapshot
    auto* player = PlayerCharacter::Get();
    if (m_session && (m_paused || (player && player->GetExtension()->CoopDowned.load(std::memory_order_relaxed))))
        m_recoverySession = m_session;
    m_session = m_generation = m_hold = m_target = 0;
    SetPaused(m_paused);
    if (player && player->GetExtension()->CoopDowned.load(std::memory_order_relaxed))
        Utils::ShowHudMessage("RFAB: disconnected while downed. Reconnect to the same session or load a coordinated save; no automatic revival.");
    else if (player)
        player->SetPlayerRespawnMode(false);
}
void CoopService::OnWorldLoaded(const WorldLoadedEvent&) noexcept
{
    m_active.store(false, std::memory_order_release);
    m_character = m_world.ctx().at<SaveStateService>().Character(); // actual SKSE save identity, never a new bag on arbitrary load
    m_downed.clear(); m_appliedRevision.clear(); m_appliedFormId.clear();
    m_reset.Clear(); m_recoverySession = m_recoverUntil = 0;
    m_session = m_generation = m_encounter = m_hold = m_target = 0;
    if (auto* player = PlayerCharacter::Get())
    {
        player->GetExtension()->CoopDowned.store(player->actorState.IsBleedingOut(), std::memory_order_relaxed);
        player->GetExtension()->Protection.Active = false;
    }
    SetPaused(false);
}
bool CoopService::CanInteract() const noexcept
{
    const auto* window = BSGraphics::GetMainWindow();
    const auto* ui = UI::Get();
    if (!window || !const_cast<BSGraphics::RendererWindow*>(window)->IsForeground() || !ui ||
        ui->numItemMenus || ui->numPausesGame || ui->modal || m_world.GetOverlayService().GetActive())
        return false;
    auto* player = PlayerCharacter::Get();
    return player && player->GetNiNode() && !player->IsDead() &&
        !player->GetExtension()->CoopDowned.load(std::memory_order_relaxed) && !m_paused && !m_inventoryHeld;
}
Coop::Presence CoopService::Observe() noexcept
{
    Coop::Presence presence;
    auto* player = PlayerCharacter::Get();
    if (!player || !player->parentCell || !player->parentCell->IsAttached() || !player->GetNiNode())
    {
        presence.Flags = Coop::Loading;
        return presence;
    }
    auto& mods = m_world.GetModSystem();
    mods.GetServerModId(player->parentCell->formID, presence.Location.Cell);
    if (auto* world = player->GetWorldSpace())
        mods.GetServerModId(world->formID, presence.Location.World);
    presence.Location.X = player->position.x; presence.Location.Y = player->position.y; presence.Location.Z = player->position.z;
    presence.Location.Heading = player->rotation.z;
    const auto self = LocalActor();
    if (auto it = m_downed.find(self); it != m_downed.end())
        presence.DownedRevision = it->second.Revision;
    const bool downed = player->GetExtension()->CoopDowned.load(std::memory_order_relaxed);
    if (downed || (player->actorState.IsBleedingOut() && Now() >= m_recoverUntil))
        presence.Flags |= Coop::Bleedout;
    if (player->IsInCombat())
        presence.Flags |= Coop::InCombat;
    if (auto* scene = player->GetCurrentScene(); scene && scene->isPlaying)
        presence.Flags |= Coop::UnsafeEncounter;
    if (CanInteract())
        presence.Flags |= Coop::CanInteract;
    if (player->currentProcess)
        if (auto* controller = player->currentProcess->GetCharController(); controller && controller->IsSafeStaticGround())
            presence.Flags |= Coop::Grounded;
    const bool safe = !(presence.Flags & (Coop::Bleedout | Coop::InCombat | Coop::Loading | Coop::UnsafeEncounter)) &&
        (presence.Flags & Coop::Grounded) && (player->actorState.flags1 & ((1u << 10) | (0xFu << 14) | (7u << 18))) == 0;
    if (!safe || Coop::DistanceSquared(presence.Location, m_safeOrigin) > 32.0 * 32.0)
    {
        m_safeSince = safe ? Now() : 0;
        m_safeOrigin = presence.Location;
    }
    if (safe && m_safeSince && Now() - m_safeSince >= Coop::kSafeDwellMs)
        presence.Flags |= Coop::Stable;
    presence.HitSerial = m_hitSerial;
    return presence;
}
void CoopService::OnHit(const HitEvent& aEvent) noexcept
{
    if (aEvent.HitteeId == 0x14)
    {
        ++m_hitSerial;
        m_safeSince = 0;
    }
}
void CoopService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_transport.IsOnline() || !LocalActor())
        return;
    const uint64_t now = Now();
    if (!Active())
    {
        if (!m_helloAt || now - m_helloAt >= 2000)
        {
            m_helloAt = now;
            const uint32_t inventory = InventoryHook::Installed() && m_world.ctx().at<SaveStateService>().Ready() ? Coop::TransactionalLoot : 0u;
            Send(Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::GroundedAnchors | inventory, m_character, "se-1.5.97-native-v2"});
        }
        return;
    }
    // Late-spawned remote players still need the last downed revision, once their 3D exists.
    for (const auto& [_, state] : m_downed)
    {
        auto* actor = Utils::GetByServerId<Actor>(state.Actor);
        if (state.Revision > m_appliedRevision[state.Actor] || (actor && m_appliedFormId[state.Actor] != actor->formID))
            Apply(state);
        else if (actor && actor->GetNiNode() && state.Downed && !actor->actorState.IsBleedingOut() &&
            !(state.Actor == LocalActor() && m_reset.Stage == Coop::ResetPhase::Commit))
            NativeDowned(actor, true, 0.f); // a script/3D rebuild cannot clear the server latch
    }
    UpdateResetReadiness();
    if (m_recoverUntil && now >= m_recoverUntil)
    {
        if (auto* player = PlayerCharacter::Get(); player && !player->GetExtension()->CoopDowned.load(std::memory_order_relaxed))
            player->SetNoBleedoutRecovery(true);
        m_recoverUntil = 0;
    }
    if (now - m_lastPoll < 200)
        return;
    m_lastPoll = now;
    const auto presence = Observe();
    Send(presence);
    ObserveEnemies();
    if ((m_capabilities & Coop::GroundedAnchors) && (presence.Flags & Coop::Stable) && now - m_lastAnchor >= 1000)
    {
        m_lastAnchor = now;
        Send(Coop::Anchor{presence.Location, static_cast<uint32_t>(std::min<uint64_t>(now - m_safeSince, 60000))});
    }
    uint32_t target{};
    if (CanInteract() && InteractionInput::Held())
    {
        double nearest = 192.0 * 192.0;
        for (const auto& [id, state] : m_downed)
        {
            auto* actor = Utils::GetByServerId<Actor>(id);
            auto* player = PlayerCharacter::Get();
            if (id == LocalActor() || !state.Downed || !actor || !actor->GetNiNode() || actor->parentCell != player->parentCell)
                continue;
            const auto distance = glm::length2(actor->position - player->position);
            if (distance < nearest)
            {
                nearest = distance;
                target = id;
            }
        }
    }
    if (m_target && target != m_target)
        Send(Coop::Revive{m_target, m_downed[m_target].Revision, m_hold, false});
    if (target)
    {
        if (target != m_target)
            m_hold = ++m_holdCounter;
        Send(Coop::Revive{target, m_downed[target].Revision, m_hold, true});
    }
    m_target = target;
}
void CoopService::OnPacket(const NotifyCoopAction& aMessage) noexcept
{
    const auto& packet = aMessage.Data;
    if (!m_transport.IsOnline() || !packet.Valid)
        return;
    if (const auto* welcome = std::get_if<Coop::Welcome>(&packet.Content))
    {
        if (!welcome->Error.empty())
        {
            Utils::ShowHudMessage(String(welcome->Error.c_str()));
            m_active = false;
            return;
        }
        if (m_recoverySession && packet.Session != m_recoverySession)
        {
            Utils::ShowHudMessage("RFAB: this is not the recovery session. Load a coordinated save before changing server.");
            m_transport.Close();
            return;
        }
        if (Active() && (packet.Session != m_session || packet.Operation <= m_serverOperation))
            return;
        if (!packet.Session || !packet.Generation || !(welcome->Capabilities & Coop::NativeDowned))
            return;
        m_session = packet.Session; m_generation = packet.Generation; m_capabilities = welcome->Capabilities;
        m_serverOperation = packet.Operation;
        m_active = true;
        PlayerCharacter::Get()->SetPlayerRespawnMode(true);
        SetPaused(m_paused);
        Utils::ShowHudMessage("RFAB co-op: native downed, hold H for 5 seconds to revive. Other systems require negotiated adapters.");
        return;
    }
    if (!Active() || packet.Session != m_session || packet.Generation != m_generation || packet.Operation <= m_serverOperation)
        return;
    m_serverOperation = packet.Operation;
    std::visit([this](const auto& body) { Apply(body); }, packet.Content);
    m_world.GetDispatcher().trigger(CoopActionEvent{packet});
}
void CoopService::ApplyHealth(uint32_t aActor, uint32_t aEpoch, uint64_t aRevision, float aHealth) noexcept
{
    const auto entity = Utils::FindEntityByServerId(aActor);
    if (!entity || !aEpoch || !std::isfinite(aHealth))
        return;
    const auto current = OwnershipSystem::GetOwnershipEpoch(m_world, *entity);
    if (aEpoch < current)
        return;
    if (auto* local = m_world.try_get<LocalComponent>(*entity))
    {
        local->OwnershipEpoch = aEpoch;
        local->MinimumActionTick = m_world.GetTick();
        m_world.emplace_or_replace<LocalAnimationComponent>(*entity);
    }
    if (auto* remote = m_world.try_get<RemoteComponent>(*entity))
    {
        remote->OwnershipEpoch = aEpoch;
        AnimationSystem::Setup(m_world, *entity);
        const auto* form = m_world.try_get<FormIdComponent>(*entity);
        auto* actor = form ? Cast<Actor>(TESForm::GetById(form->Id)) : nullptr;
        const auto position = actor ? actor->position : glm::vec3{};
        InterpolationSystem::Setup(m_world, *entity).Position = position;
    }
    ActorValues values;
    values.ActorValuesList[ActorValueInfo::kHealth] = aHealth;
    values.HealthRevision = aRevision;
    m_world.ctx().at<ActorValueService>().AdoptHealth(aActor, aEpoch, values);
    if (auto* waiting = m_world.try_get<WaitingFor3D>(*entity))
    {
        waiting->SpawnRequest.OwnershipEpoch = aEpoch;
        waiting->SpawnRequest.InitialActorValues.ActorValuesList[ActorValueInfo::kHealth] = aHealth;
        waiting->SpawnRequest.InitialActorValues.HealthRevision = aRevision;
        waiting->SpawnRequest.ActionsToReplay.Actions.clear();
    }
}
void CoopService::Apply(const Coop::DownedState& aState) noexcept
{
    const auto old = m_downed.find(aState.Actor);
    if (old != m_downed.end() && old->second.Revision > aState.Revision)
        return;
    m_downed[aState.Actor] = aState;
    if (aState.Actor == m_target && !aState.Reviver && m_target)
        m_hold = ++m_holdCounter;
    auto* actor = Utils::GetByServerId<Actor>(aState.Actor);
    if (!actor || !actor->GetNiNode() || aState.Revision == 0 || (m_appliedRevision[aState.Actor] >= aState.Revision && m_appliedFormId[aState.Actor] == actor->formID))
        return;
    ApplyHealth(aState.Actor, aState.Epoch, aState.HealthRevision, aState.Health);
    NativeDowned(actor, aState.Downed, aState.Health);
    m_appliedRevision[aState.Actor] = aState.Revision;
    m_appliedFormId[aState.Actor] = actor->formID;
    if (aState.Actor == LocalActor())
        SetPaused(m_paused);
    if (aState.Downed)
    {
        RetargetOnce(aState.Actor);
        Utils::ShowHudMessage(aState.Actor == LocalActor() ? "RFAB: downed. Wait for a teammate; there is no bleedout timeout." : "RFAB: teammate downed. Hold the revive key nearby for 5 seconds.");
    }
    else if (aState.Actor == LocalActor())
        m_recoverUntil = Now() + 2000;

}
void CoopService::ObserveEnemies() noexcept
{
    if (!m_encounter || Now() - m_lastEnemyScan < 500 || m_paused)
        return;
    m_lastEnemyScan = Now();
    const auto view = m_world.view<FormIdComponent>();
    size_t sent{};
    for (auto entity : view)
    {
        const auto server = Utils::GetServerId(entity);
        if (!server)
            continue;
        const auto epoch = OwnershipSystem::GetOwnershipEpoch(m_world, entity);
        const auto reported = m_reportedEnemies.find(*server);
        if (!epoch || (reported != m_reportedEnemies.end() && reported->second == epoch))
            continue;
        auto* npc = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        if (!npc || npc->GetExtension()->IsPlayer() || npc->IsDead() || !npc->IsInCombat())
            continue;
        auto* target = npc->GetCombatTarget();
        if (!target || !target->GetExtension()->IsPlayer())
            continue;
        const auto* scene = npc->GetCurrentScene();
        const bool scripted = (scene && scene->isPlaying) || npc->IsEssential() || (npc->flags2 & (1u << 19));
        Send(Coop::EnemyContact{*server, epoch, true, false, scripted});
        m_reportedEnemies[*server] = epoch;
        if (++sent >= 32)
            break;
    }
}
void CoopService::RetargetOnce(uint32_t aDownedActor) noexcept
{
    auto* downed = Utils::GetByServerId<Actor>(aDownedActor);
    if (!downed)
        return;
    const auto npcs = m_world.view<FormIdComponent, LocalComponent>();
    const auto players = m_world.view<FormIdComponent, PlayerComponent>();
    for (auto entity : npcs)
    {
        auto* npc = Cast<Actor>(TESForm::GetById(npcs.get<FormIdComponent>(entity).Id));
        if (!npc || npc->GetExtension()->IsPlayer() || !npc->IsInCombat() || npc->GetCombatTarget() != downed)
            continue;
        Actor* next{};
        float nearest = 4096.f * 4096.f;
        auto* local = PlayerCharacter::Get();
        if (local && local != downed && !local->IsDead() && !local->GetExtension()->CoopDowned.load(std::memory_order_relaxed) &&
            local->parentCell == npc->parentCell)
        {
            const float distance = glm::length2(local->position - npc->position);
            if (distance < nearest) { nearest = distance; next = local; }
        }
        for (auto playerEntity : players)
        {
            auto* candidate = Cast<Actor>(TESForm::GetById(players.get<FormIdComponent>(playerEntity).Id));
            if (!candidate || candidate == downed || candidate->IsDead() || candidate->GetExtension()->CoopDowned.load(std::memory_order_relaxed))
                continue;
            const float distance = glm::length2(candidate->position - npc->position);
            if (candidate->parentCell == npc->parentCell && distance < nearest)
            {
                nearest = distance;
                next = candidate;
            }
        }
        npc->SetCombatTargetEx(next); // one target transition; no ownership sweep or StartCombat loop
    }
}
void CoopService::Apply(const Coop::EncounterState& aState) noexcept
{
    if (aState.Phase == Coop::EncounterPhase::Started && aState.Encounter != m_encounter)
    {
        m_encounter = aState.Encounter;
        m_reportedEnemies.clear();
    }
    if (aState.Phase == Coop::EncounterPhase::Ended)
        m_encounter = 0;
    if (aState.Phase == Coop::EncounterPhase::Interrupted)
        SetPaused(true);
    else if (aState.Phase == Coop::EncounterPhase::Started || aState.Phase == Coop::EncounterPhase::Ended)
        SetPaused(false);
    if (!aState.Reason.empty())
        Utils::ShowHudMessage(String(aState.Reason.c_str()));
    else if (aState.Phase == Coop::EncounterPhase::WipeCountdown)
        Utils::ShowHudMessage("RFAB: group down. Encounter reset preparation starts in 5 seconds.");
}
void CoopService::Apply(const Coop::ResetPlan& aPlan) noexcept
{
    const auto decision = m_reset.Accept(aPlan, LocalActor());
    if (decision == Coop::ResetParticipant::Decision::Reject)
    {
        Send(Coop::ResetReceipt{aPlan.Encounter, aPlan.Phase, false});
        SetPaused(true);
        return;
    }
    if (decision == Coop::ResetParticipant::Decision::Duplicate)
    {
        if (m_reset.LastReceipt)
            Send(*m_reset.LastReceipt);
        return; // NEVER repeat a native teleport because the server retried its command
    }
    if (decision == Coop::ResetParticipant::Decision::Complete)
    {
        SetPaused(false);
        m_recoverySession = 0;
        return;
    }
    SetPaused(true);
    bool success = true;
    for (const auto& reset : aPlan.Actors)
    {
        if (reset.IsPlayer && reset.Actor == LocalActor())
        {
            auto* cell = ResolveCell(m_world, reset.Destination);
            success &= cell != nullptr;
            if (decision == Coop::ResetParticipant::Decision::Commit && cell)
            {
                auto* player = PlayerCharacter::Get();
                player->StopCombat();
                player->MoveTo(cell, NiPoint3{glm::vec3{reset.Destination.X, reset.Destination.Y, reset.Destination.Z}});
                player->rotation.z = reset.Destination.Heading;
                ApplyHealth(reset.Actor, reset.Epoch, reset.HealthRevision, reset.Health);
                NativeDowned(player, false, reset.Health);
                SetPaused(true);
                m_recoverUntil = Now() + 2000;
            }
        }
        else if (!reset.IsPlayer && decision == Coop::ResetParticipant::Decision::Commit)
        {
            const auto entity = Utils::FindEntityByServerId(reset.Actor);
            auto* actor = Utils::GetByServerId<Actor>(reset.Actor);
            ApplyHealth(reset.Actor, reset.Epoch, reset.HealthRevision, reset.Health);
            if (entity && actor && m_world.all_of<LocalComponent>(*entity) && !actor->IsDead())
                actor->StopCombat();
        }
    }
    if (!success || decision == Coop::ResetParticipant::Decision::Prepare)
        Send(m_reset.Receipt(success));
    else
    {
        m_pendingReset = aPlan;
        m_resetReadiness.Begin(Now());
    }
}
void CoopService::UpdateResetReadiness() noexcept
{
    if (!m_reset.Pending)
        return;
    const auto presence = Observe();
    auto* player = PlayerCharacter::Get();
    // Observe intentionally ignores a residual bleedout sample for normal revive reporting.
    // That grace MUST NOT count as proof that a reset completed. Inspect raw native and graph
    // state, health, 3D/cell, combat and the destination independently, after the full grace.
    bool graphBleedout = true;
    BSFixedString variable("IsBleedingOut");
    const bool graphReady = player && player->animationGraphHolder.IsReady() &&
        player->animationGraphHolder.GetVariableBool(&variable, &graphBleedout);
    bool ready = player && player->GetNiNode() && player->parentCell && player->parentCell->IsAttached() &&
        !player->IsDead() && !player->actorState.IsBleedingOut() && ((player->actorState.flags1 >> 21) & 0xF) == 0 &&
        !player->GetExtension()->CoopDowned.load(std::memory_order_relaxed) && graphReady && !graphBleedout &&
        std::isfinite(player->GetActorValue(ActorValueInfo::kHealth)) && player->GetActorValue(ActorValueInfo::kHealth) > 0.f &&
        !(presence.Flags & (Coop::Loading | Coop::InCombat | Coop::UnsafeEncounter)) && (presence.Flags & Coop::Grounded);
    for (const auto& actor : m_pendingReset.Actors)
        if (actor.IsPlayer && actor.Actor == LocalActor())
            ready &= Coop::DistanceSquared(presence.Location, actor.Destination) <= 64.0 * 64.0;
    const auto result = m_resetReadiness.Poll(Now(), ready);
    if (result == Coop::ReadinessGate::Result::Ready)
        Send(m_reset.Receipt(true));
    else if (result == Coop::ReadinessGate::Result::Failed)
    {
        Send(m_reset.Receipt(false));
        Utils::ShowHudMessage("RFAB: reset did not reach verified native/graph standing and health. Group remains paused; no guessed fallback teleport.");
    }
}
