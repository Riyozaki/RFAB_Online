#include <TiltedOnlinePCH.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <Components.h>

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

#include <misc/ActorValueOwner.h>
#include <Gameplay/HealthScaling.h>

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
    actorValuesComponent.FormId = apActor->formID;

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

void ActorValueService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // Party-size health scaling is server authoritative, so when the session
    // ends the scaled maxima have to be undone - otherwise the player keeps a
    // world of x3/x4 NPCs in single player (and saves them that way).
    //
    // Two kinds of actors need this:
    //   * ones we saw being scaled: ActorValuesComponent::BaseMaxHealth holds the
    //     exact unscaled value;
    //   * ones that arrived already scaled (late joiners receive the scaled
    //     maximum in ActorData), where only the session multiplier is known.
    //     The multiplier is read from the server settings the client already
    //     keeps (AuthenticationResponse / NotifySettingsChange).
    //
    // The entities are collected first and the component is cleared afterwards:
    // mutating the pool while iterating a view of it is exactly the kind of thing
    // that made this handler crash before.
    std::vector<entt::entity> toRestore;

    auto formView = m_world.view<FormIdComponent>();
    for (const auto entity : formView)
        toRestore.push_back(entity);

    const float serverMultiplier = m_world.GetServerSettings().HealthMultiplier;
    // Anything outside [1, 32] is either "no scaling" or a broken setting; the
    // restore path must never multiply health *up*.
    const float sessionMultiplier = (std::isfinite(serverMultiplier) && serverMultiplier > 1.0f) ? std::min(serverMultiplier, 32.0f) : 1.0f;

    Actor* pLocalPlayer = PlayerCharacter::Get();

    for (const auto entity : toRestore)
    {
        if (!m_world.valid(entity))
            continue;

        const auto& formIdComponent = m_world.get<FormIdComponent>(entity);
        if (formIdComponent.Id == GameId{})
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (pActor == nullptr || pActor == pLocalPlayer)
            continue;

        const float currentMax = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        if (!std::isfinite(currentMax) || currentMax <= 0.0f)
            continue;

        const float currentHealth = pActor->GetActorValue(ActorValueInfo::kHealth);
        const float ratio = std::clamp(currentHealth / currentMax, 0.0f, 1.0f);

        float rememberedBase = 0.0f;
        if (const auto* pActorValues = m_world.try_get<ActorValuesComponent>(entity); pActorValues != nullptr)
            rememberedBase = pActorValues->BaseMaxHealth;

        const auto restore = Gameplay::HealthScaling::ComputeRestore(currentMax, rememberedBase, sessionMultiplier);
        if (!restore.ShouldRestore)
            continue;

        const float base = restore.Base;

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, base);
        pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, ratio * base);

        spdlog::debug("Restored health of {:X} on disconnect: {:.0f} -> {:.0f}", pActor->formID, currentMax, base);
    }

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
        RequestActorMaxValueChanges requestMaxValueChanges;
        requestMaxValueChanges.Id = localComponent.Id;

        bool isPlayer = pActor->GetExtension() && pActor->GetExtension()->IsPlayer();

        for (int i = 0; i < ActorValueInfo::kActorValueCount; i++)
        {
            if (isPlayer && i == ActorValueInfo::kDragonSouls)
                continue;
            
            float newValue = pActor->GetActorValue(i);
            float oldValue = actorValuesComponent.CurrentActorValues.ActorValuesList[i];
            if (newValue != oldValue)
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

void ActorValueService::OnHealthChange(const HealthChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

    const auto hitteeIt = std::find_if(std::begin(view), std::end(view), [id = acEvent.HitteeId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (hitteeIt == std::end(view))
    {
        spdlog::warn("Health change event form id component not found, form id: {:X}", acEvent.HitteeId);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*hitteeIt);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    uint32_t serverId = serverIdRes.value();

    if (acEvent.DeltaHealth > -1.0f && acEvent.DeltaHealth < 1.0f)
    {
        if (m_smallHealthChanges.find(serverId) == m_smallHealthChanges.end())
            m_smallHealthChanges[serverId] = acEvent.DeltaHealth;
        else
            m_smallHealthChanges[serverId] += acEvent.DeltaHealth;
        return;
    }

    RequestHealthChangeBroadcast requestHealthChange;
    requestHealthChange.Id = serverId;
    requestHealthChange.DeltaHealth = acEvent.DeltaHealth;

    m_transport.Send(requestHealthChange);

    spdlog::debug("Sent out delta health through collection: {:X}:{:f}", serverId, acEvent.DeltaHealth);
}

void ActorValueService::RunSmallHealthUpdates() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 250ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    if (!m_smallHealthChanges.empty())
    {
        for (auto& value : m_smallHealthChanges)
        {
            RequestHealthChangeBroadcast requestHealthChange;
            requestHealthChange.Id = value.first;
            requestHealthChange.DeltaHealth = value.second;

            m_transport.Send(requestHealthChange);

            spdlog::debug("Sent out delta health through timer, {:X}:{:f}", value.first, value.second);
        }

        m_smallHealthChanges.clear();
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

            RequestDeathStateChange requestChange;
            requestChange.Id = localComponent.Id;
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

void ActorValueService::OnHealthChangeBroadcast(const NotifyHealthChangeBroadcast& acMessage) const noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.Id);
        return;
    }

    const float newHealth = pActor->GetActorValue(ActorValueInfo::kHealth) + acMessage.DeltaHealth;
    pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, newHealth);

    const float health = pActor->GetActorValue(ActorValueInfo::kHealth);
    if (!pActor->IsDead() && health <= 0.f)
    {
        ActorExtension* pExtension = pActor->GetExtension();
        // Players should never be killed
        if (!pExtension->IsPlayer())
            pActor->Kill();
    }

    // TODO(cosideci): find fix for player health sync so this can be used again
    /*
    if (pActor->GetExtension()->IsRemotePlayer())
        World::Get().GetOverlayService().SetPlayerHealthPercentage(pActor->formID);
    */
}

void ActorValueService::OnActorValueChanges(const NotifyActorValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acMessage.Id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (itor == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*itor);
    Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        // Syncing dragon souls triggers "Dragon soul collected" event
        if (key == ActorValueInfo::kDragonSouls || key == ActorValueInfo::kHealth)
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

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) noexcept
{
    // Server-driven maximum values apply to *every* copy of the actor, including
    // the one the local player simulates: party-size health scaling is decided
    // by the server and the owner must not keep its own idea of the maximum.
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id);
    if (pActor == nullptr)
        return;

    const auto healthIt = acMessage.Values.find(ActorValueInfo::kHealth);
    if (healthIt != acMessage.Values.end() && healthIt->second > 0.0f)
        RememberUnscaledHealth(acMessage.Id, pActor);

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
}

void ActorValueService::RememberUnscaledHealth(uint32_t aServerId, Actor* apActor) noexcept
{
    auto view = m_world.view<FormIdComponent>();
    for (const auto entity : view)
    {
        const auto serverId = Utils::GetServerId(entity);
        if (!serverId.has_value() || serverId.value() != aServerId)
            continue;

        auto* pActorValues = m_world.try_get<ActorValuesComponent>(entity);
        if (pActorValues == nullptr || pActorValues->BaseMaxHealth > 0.0f)
            return;

        pActorValues->FormId = view.get<FormIdComponent>(entity).Id;
        pActorValues->BaseMaxHealth = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        return;
    }
}

void ActorValueService::RememberScaledMaxHealth(entt::entity aEntity, float aBaseMaxHealth) noexcept
{
    if (!(aBaseMaxHealth > 0.0f))
        return;

    auto* pActorValues = m_world.try_get<ActorValuesComponent>(aEntity);
    if (pActorValues == nullptr)
        pActorValues = &m_world.emplace_or_replace<ActorValuesComponent>(aEntity);

    // Whatever was captured before is the truth (it comes from an exact value).
    if (pActorValues->BaseMaxHealth > 0.0f)
        return;

    if (const auto* pFormId = m_world.try_get<FormIdComponent>(aEntity))
        pActorValues->FormId = pFormId->Id;

    pActorValues->BaseMaxHealth = aBaseMaxHealth;
}

void ActorValueService::OnDeathStateChange(const NotifyDeathStateChange& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto it = std::find_if(std::begin(view), std::end(view), [id = acMessage.Id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (it == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    ActorExtension* pExtension = pActor->GetExtension();
    // Players should never be killed
    if (pExtension->IsPlayer())
        return;

    if (pActor->IsDead() != acMessage.IsDead)
        acMessage.IsDead ? pActor->Kill() : pActor->Respawn();
}
