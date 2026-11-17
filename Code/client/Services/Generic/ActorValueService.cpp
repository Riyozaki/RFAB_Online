#include <TiltedOnlinePCH.h>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/References.h>
#include <Components.h>
#include <Systems/OwnershipSystem.h>

#include <Events/UpdateEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/HealthChangeEvent.h>

#include <Messages/NotifyActorValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/RequestDeathStateChange.h>

#include <Misc/ActorValueOwner.h>

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::CreateActorValuesComponent(const entt::entity aEntity, Actor* apActor) noexcept
{
    auto& actorValuesComponent = m_world.emplace_or_replace<ActorValuesComponent>(aEntity);

    for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
    {
        float value = apActor->GetActorValue(i);
        actorValuesComponent.CurrentActorValues.ActorValuesList.insert({i, value});
        float maxValue = apActor->GetActorPermanentValue(i);
        actorValuesComponent.CurrentActorValues.ActorMaxValuesList.insert({i, maxValue});
    }
}

void ActorValueService::OnLocalComponentAdded(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    const auto& formIdComponent = aRegistry.get<FormIdComponent>(aEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (pActor != NULL)
    {
        auto& localComponent = aRegistry.get<LocalComponent>(aEntity);
        localComponent.IsDead = pActor->IsDead();
        localComponent.IsWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        CreateActorValuesComponent(aEntity, pActor);
    }
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    m_health.clear();
    m_world.clear<ActorValuesComponent>();
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();
    const uint32_t formId = acEvent.FormId;

    const auto it = std::find_if(
        std::begin(view), std::end(view),
        [view, formId](auto entity)
        {
            const auto& formIdComponent = view.get<FormIdComponent>(entity);

            return formIdComponent.Id == formId;
        });

    if (it != std::end(view))
        m_world.remove<ActorValuesComponent>(*it);
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunSmallHealthUpdates();
    RunDeathStateUpdates();
    RunActorValuesUpdates();
}

void ActorValueService::BroadcastActorValues() noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent, LocalComponent, ActorValuesComponent>();

    for (auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        auto* pForm = TESForm::GetById(formIdComponent.Id);
        auto* pActor = Cast<Actor>(pForm);

        if (!pActor)
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);

        RequestActorValueChanges requestValueChanges;
        requestValueChanges.Id = localComponent.Id;
        requestValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;
        requestMaxValueChanges.OwnershipEpoch = localComponent.OwnershipEpoch;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if (i == ActorValueInfo::kInfamy || (isPlayer && i == ActorValueInfo::kDragonSouls))
                continue;

            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (i != ActorValueInfo::kHealth && newValue != oldValue)
            {
                requestValueChanges.Values.insert({i, newValue});
                actorValuesComponent.CurrentActorValues.ActorValuesList[i] = newValue;
            }

            float newMaxValue = pActor->GetActorPermanentValue(i);
            float oldMaxValue = actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i];
            if (newMaxValue != oldMaxValue)
            {
                requestMaxValueChanges.Values.insert({i, newMaxValue});
                actorValuesComponent.CurrentActorValues.ActorMaxValuesList[i] = newMaxValue;
            }
        }

        if (requestValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestValueChanges);
        }

        if (requestMaxValueChanges.Values.size() > 0)
        {
            m_transport.Send(requestMaxValueChanges);
        }
    }
}

void ActorValueService::AdoptHealth(uint32_t aServerId, uint32_t aEpoch, const ActorValues& acValues) noexcept
{
    if (!acValues.IsValid()) return;
    const auto it = acValues.ActorValuesList.find(ActorValueInfo::kHealth);
    if (it == acValues.ActorValuesList.end() || !std::isfinite(it->second))
        return;
    auto& state = m_health[aServerId];
    const auto entity = Utils::FindEntityByServerId(aServerId);
    auto* actor = Utils::GetByServerId<Actor>(aServerId);
    if (entity && actor && state.Epoch == aEpoch && m_world.all_of<LocalComponent>(*entity))
        if (auto mutation = state.Observe(actor->GetActorValue(ActorValueInfo::kHealth)))
            SendHealth(aServerId, state, *mutation);
    const auto ack = acValues.HealthSequences.find(m_transport.GetLocalPlayerId());
    const auto adopted = state.Adopt(aEpoch, acValues.HealthRevision, it->second,
        ack == acValues.HealthSequences.end() ? 0 : ack->second);
    if (adopted && actor)
        actor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, *adopted);
}

void ActorValueService::ApplyActorValues(Actor* apActor, uint32_t aServerId, uint32_t aEpoch, const ActorValues& acValues) noexcept
{
    if (!apActor || !acValues.IsValid())
        return;
    apActor->SetActorValues(acValues); // non-HP AVs only
    const auto state = m_health.find(aServerId);
    if (state != m_health.end() && state->second.Epoch == aEpoch)
        apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, state->second.Project());
}

void ActorValueService::SendHealth(uint32_t aServerId, Coop::HealthPrediction& aState, const Coop::HealthPrediction::Mutation& acMutation) noexcept
{
    RequestHealthChangeBroadcast request{};
    request.Id = aServerId;
    request.OwnershipEpoch = aState.Epoch;
    request.Sequence = acMutation.Sequence;
    request.DeltaHealth = acMutation.Delta;
    m_transport.Send(request);
}

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected() || !std::isfinite(acEvent.DeltaHealth))
        return;
    const auto view = m_world.view<FormIdComponent, RemoteComponent>(entt::exclude<LocalComponent>);
    for (auto entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id != acEvent.HitteeId)
            continue;
        // Local actors are sampled below; only our actual hit on a remote NPC is an external mutation.
        auto* actor = Cast<Actor>(TESForm::GetById(acEvent.HitteeId));
        if (!actor || actor->GetExtension()->IsPlayer())
            return;
        const auto& remote = view.get<RemoteComponent>(entity);
        auto state = m_health.find(remote.Id);
        if (state == m_health.end() || state->second.Epoch != remote.OwnershipEpoch)
            return; // no canonical baseline yet, never guess from the post-hit value
        if (auto mutation = state->second.Queue(acEvent.DeltaHealth))
            SendHealth(remote.Id, state->second, *mutation);
        return;
    }
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastHealthSample < 100ms)
        return;
    m_lastHealthSample = now;
    const auto view = m_world.view<FormIdComponent, LocalComponent>();
    for (const auto entity : view)
    {
        const auto& local = view.get<LocalComponent>(entity);
        auto* actor = Cast<Actor>(TESForm::GetById(view.get<FormIdComponent>(entity).Id));
        auto it = m_health.find(local.Id);
        if (!actor || it == m_health.end() || it->second.Epoch != local.OwnershipEpoch)
            continue;
        if (auto mutation = it->second.Observe(actor->GetActorValue(ActorValueInfo::kHealth)))
            SendHealth(local.Id, it->second, *mutation);
    }
}

void ActorValueService::RunDeathStateUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto localView = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : localView)
    {
        const auto& formIdComponent = localView.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto& localComponent = localView.get<LocalComponent>(entity);

        bool isDead = pActor->IsDead();
        if (isDead != localComponent.IsDead)
        {
            localComponent.IsDead = isDead;

            spdlog::info("Sending death state {} for local actor {:X}, server id: {:X}", isDead, pActor->formID, localComponent.Id);

            RequestDeathStateChange requestChange;
            requestChange.Id = localComponent.Id;
            requestChange.OwnershipEpoch = localComponent.OwnershipEpoch;
            requestChange.IsDead = isDead;

            m_transport.Send(requestChange);
        }
    }
}

void ActorValueService::RunActorValuesUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    BroadcastActorValues();
}

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) noexcept
{
    if (!std::isfinite(acMessage.Health))
        return;
    const auto entity = Utils::FindEntityByServerId(acMessage.Id);
    auto it = m_health.find(acMessage.Id);
    if (!entity || it == m_health.end() || it->second.Epoch != acMessage.OwnershipEpoch ||
        OwnershipSystem::GetOwnershipEpoch(m_world, *entity) != acMessage.OwnershipEpoch)
        return;
    auto* actor = Utils::GetByServerId<Actor>(acMessage.Id);
    auto& state = it->second;
    if (actor && m_world.all_of<LocalComponent>(*entity))
    {
        if (auto mutation = state.Observe(actor->GetActorValue(ActorValueInfo::kHealth)))
            SendHealth(acMessage.Id, state, *mutation);
    }
    const auto ownAck = acMessage.SourcePlayerId == m_transport.GetLocalPlayerId() ? acMessage.Sequence : 0;
    const float health = state.Accept(acMessage.Revision, acMessage.Health, ownAck);
    if (auto* waiting = m_world.try_get<WaitingFor3D>(*entity))
    {
        waiting->SpawnRequest.InitialActorValues.ActorValuesList[ActorValueInfo::kHealth] = health;
        waiting->SpawnRequest.InitialActorValues.HealthRevision = state.Revision;
    }
    if (actor)
    {
        actor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, health);
        if (!actor->GetExtension()->IsPlayer() && !actor->IsDead() && health <= 0.f)
            actor->Kill();
    }
    if (auto* cache = m_world.try_get<ActorValuesComponent>(*entity))
    {
        cache->CurrentActorValues.HealthRevision = state.Revision;
        cache->CurrentActorValues.ActorValuesList[ActorValueInfo::kHealth] = health;
    }
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept
{
    const auto entity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.Id, acMessage.OwnershipEpoch);
    const auto* pFormIdComponent = entity ? m_world.try_get<FormIdComponent>(*entity) : nullptr;
    if (!pFormIdComponent)
        return;

    Actor* const pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event
        if (key >= ActorValueInfo::kActorValueCount || key == ActorValueInfo::kInfamy || !std::isfinite(value) || key == ActorValueInfo::kDragonSouls || key == ActorValueInfo::kHealth)
            continue;

        spdlog::debug("Actor value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        if (key == ActorValueInfo::kStamina || key == ActorValueInfo::kMagicka || key == ActorValueInfo::kHealth)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, key, value);
            continue;
        }
        pActor->SetActorValue(key, value);
    }
}

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    const auto entity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.Id, acMessage.OwnershipEpoch);
    const auto* pFormIdComponent = entity ? m_world.try_get<FormIdComponent>(*entity) : nullptr;
    if (!pFormIdComponent)
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key >= ActorValueInfo::kActorValueCount || key == ActorValueInfo::kInfamy || !std::isfinite(value) || key == ActorValueInfo::kDragonSouls)
            continue;

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) const noexcept
{
    const auto entity = OwnershipSystem::AcceptRemoteUpdate(m_world, acMessage.Id, acMessage.OwnershipEpoch);
    const auto* pFormIdComponent = entity ? m_world.try_get<FormIdComponent>(*entity) : nullptr;
    if (!pFormIdComponent)
    {
        const auto cEntity = Utils::FindEntityByServerId(acMessage.Id);
        spdlog::info(
            "Dropped death state {} for server id {:X} at epoch {}, entity found: {}, has form id: {}, waiting for 3D: {}", acMessage.IsDead, acMessage.Id, acMessage.OwnershipEpoch,
            cEntity.has_value(), cEntity && m_world.all_of<FormIdComponent>(*cEntity), cEntity && m_world.all_of<WaitingFor3D>(*cEntity));
        return;
    }

    Actor* pActor = Cast<Actor>(TESForm::GetById(pFormIdComponent->Id));

    if (!pActor)
        return;

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (pExtension->IsPlayer())
        return;

    if (pActor->IsDead() != acMessage.IsDead)
    {
        spdlog::info("Applying death state {} to remote actor {:X}, server id: {:X}", acMessage.IsDead, pActor->formID, acMessage.Id);
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
    }
}
