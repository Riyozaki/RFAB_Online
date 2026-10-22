#include <TiltedOnlinePCH.h>

#include <algorithm>
#include <cmath>

#include <Services/ActorValueService.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Games/References.h>
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

#include <Misc/ActorValueOwner.h>

static constexpr float kMinHealthMultiplier = 0.1f;
static constexpr float kMaxHealthMultiplier = 20.0f;

static float GetHealthMultiplier(const ServerSettings& acSettings) noexcept
{
    const float value = acSettings.HealthMultiplier;
    if (!std::isfinite(value))
        return 1.0f;

    return std::clamp(value, kMinHealthMultiplier, kMaxHealthMultiplier);
}

static bool ShouldScaleActorHealth(Actor* apActor) noexcept
{
    if (!apActor)
        return false;

    auto* pExtension = apActor->GetExtension();
    if (!pExtension || pExtension->IsPlayer())
        return false;

    return !apActor->IsPlayerTeammate() && !apActor->IsPlayerSummon() && !apActor->IsCommandedActor() && !apActor->IsMount();
}

// Applies (desiredMax, desiredHealth) without ever transiently driving GetActorValue(kHealth) <= 0
// when scaling down (e.g. 3x -> 1x on disconnect while DamageMod < -unscaledMax), preventing engine instant-kill.
static void SetScaledHealthSafely(Actor* apActor, float aCurrentMax, float aDesiredMax, float aDesiredHealth) noexcept
{
    if (aDesiredMax >= aCurrentMax)
    {
        apActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, aDesiredMax);
        apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, aDesiredHealth);
    }
    else
    {
        apActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, aCurrentMax + (aDesiredHealth - aDesiredMax));
        apActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, aDesiredMax);
    }
}

ActorValueService::ActorValueService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_world.on_construct<LocalComponent>().connect<&ActorValueService::OnLocalComponentAdded>(this);
    m_world.on_destroy<LocalComponent>().connect<&ActorValueService::OnLocalComponentRemoved>(this);
    m_dispatcher.sink<DisconnectedEvent>().connect<&ActorValueService::OnDisconnected>(this);
    m_dispatcher.sink<ActorRemovedEvent>().connect<&ActorValueService::OnActorRemoved>(this);
    m_dispatcher.sink<UpdateEvent>().connect<&ActorValueService::OnUpdate>(this);
    m_dispatcher.sink<NotifyActorValueChanges>().connect<&ActorValueService::OnActorValueChanges>(this);
    m_dispatcher.sink<NotifyActorMaxValueChanges>().connect<&ActorValueService::OnActorMaxValueChanges>(this);
    m_dispatcher.sink<HealthChangeEvent>().connect<&ActorValueService::OnHealthChange>(this);
    m_dispatcher.sink<NotifyHealthChangeBroadcast>().connect<&ActorValueService::OnHealthChangeBroadcast>(this);
    m_dispatcher.sink<NotifyDeathStateChange>().connect<&ActorValueService::OnDeathStateChange>(this);
}

void ActorValueService::SeedUnscaledMaxHealthFromRemote(uint32_t aFormId, float aScaledMaxHealth) noexcept
{
    if (aFormId == 0 || aScaledMaxHealth <= 0.0f)
        return;

    if (m_unscaledMaxHealth.find(aFormId) != m_unscaledMaxHealth.end())
        return;

    const float multiplier = m_transport.IsConnected() ? GetHealthMultiplier(m_world.GetServerSettings()) : 1.0f;
    m_unscaledMaxHealth[aFormId] = aScaledMaxHealth / multiplier;
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

        auto& actorValuesComponent = aRegistry.get<ActorValuesComponent>(aEntity);
        const float healthMultiplier = m_transport.IsConnected() ? GetHealthMultiplier(m_world.GetServerSettings()) : 1.0f;
        ApplyHealthMultiplier(pActor, actorValuesComponent, healthMultiplier);
    }
}

void ActorValueService::OnLocalComponentRemoved(entt::registry& aRegistry, const entt::entity aEntity) noexcept
{
    // When ownership transfers to another connected player, keep the synced scaled max HP intact on
    // the remote representation; only unscale on ActorRemovedEvent or DisconnectedEvent.
    if (aRegistry.all_of<ActorValuesComponent>(aEntity))
        aRegistry.remove<ActorValuesComponent>(aEntity);
}

void ActorValueService::OnDisconnected(const DisconnectedEvent& acEvent) noexcept
{
    for (const auto& [formId, unscaledMax] : m_unscaledMaxHealth)
    {
        Actor* pActor = Cast<Actor>(TESForm::GetById(formId));
        if (!pActor || !ShouldScaleActorHealth(pActor) || unscaledMax <= 0.0f)
            continue;

        const float currentMax = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
        const float currentHealth = pActor->GetActorValue(ActorValueInfo::kHealth);
        if (currentMax <= 0.0f || std::abs(currentMax - unscaledMax) <= 0.01f)
            continue;

        const float healthPercentage = std::clamp(currentHealth / currentMax, 0.0f, 1.0f);
        SetScaledHealthSafely(pActor, currentMax, unscaledMax, unscaledMax * healthPercentage);
    }

    m_unscaledMaxHealth.clear();
    m_smallHealthChanges.clear();
    m_world.clear<ActorValuesComponent>();
}

void ActorValueService::ApplyHealthScaling() noexcept
{
    const float healthMultiplier = m_transport.IsConnected() ? GetHealthMultiplier(m_world.GetServerSettings()) : 1.0f;

    auto view = m_world.view<LocalComponent, ActorValuesComponent>();
    for (auto entity : view)
    {
        auto& actorValuesComponent = view.get<ActorValuesComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(actorValuesComponent.FormId));
        if (pActor)
            ApplyHealthMultiplier(pActor, actorValuesComponent, healthMultiplier);
    }
}

void ActorValueService::ApplyHealthMultiplier(Actor* apActor, ActorValuesComponent& aComponent, float aMultiplier) noexcept
{
    if (!ShouldScaleActorHealth(apActor))
    {
        aComponent.AppliedHealthBonus = 0.0f;
        return;
    }

    const float currentMax = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    const float currentHealth = apActor->GetActorValue(ActorValueInfo::kHealth);

    if (currentMax <= 0.0f)
    {
        aComponent.AppliedHealthBonus = 0.0f;
        return;
    }

    auto baseIt = m_unscaledMaxHealth.find(apActor->formID);
    if (baseIt == m_unscaledMaxHealth.end())
        baseIt = m_unscaledMaxHealth.insert({apActor->formID, currentMax}).first;

    const float unscaledMax = baseIt->second;
    if (unscaledMax <= 0.0f)
    {
        aComponent.AppliedHealthBonus = 0.0f;
        return;
    }

    const float desiredMax = unscaledMax * aMultiplier;
    const float desiredBonus = desiredMax - unscaledMax;
    aComponent.AppliedHealthBonus = desiredBonus;

    if (std::abs(desiredMax - currentMax) <= 0.01f)
        return;

    const float healthPercentage = std::clamp(currentHealth / currentMax, 0.0f, 1.0f);
    SetScaledHealthSafely(apActor, currentMax, desiredMax, desiredMax * healthPercentage);
}

void ActorValueService::OnActorRemoved(const ActorRemovedEvent& acEvent) noexcept
{
    const uint32_t formId = acEvent.FormId;

    const auto baseIt = m_unscaledMaxHealth.find(formId);
    if (baseIt != m_unscaledMaxHealth.end())
    {
        if (Actor* pActor = Cast<Actor>(TESForm::GetById(formId)); pActor && ShouldScaleActorHealth(pActor) && baseIt->second > 0.0f)
        {
            const float currentMax = pActor->GetActorPermanentValue(ActorValueInfo::kHealth);
            const float currentHealth = pActor->GetActorValue(ActorValueInfo::kHealth);
            if (currentMax > 0.0f && std::abs(currentMax - baseIt->second) > 0.01f)
            {
                const float healthPercentage = std::clamp(currentHealth / currentMax, 0.0f, 1.0f);
                SetScaledHealthSafely(pActor, currentMax, baseIt->second, baseIt->second * healthPercentage);
            }
        }
        m_unscaledMaxHealth.erase(baseIt);
    }

    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<FormIdComponent>();

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

    ApplyHealthScaling();
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
        // Players should never be killed; mark kInfamy = -1 before non-authoritative Kill() so
        // RFAB_XP_Alias.psc's IsValidXPKill() guard never double-awards kill XP on peers!
        if (!pExtension->IsPlayer())
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
            pActor->Kill();
        }
    }
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

void ActorValueService::OnActorMaxValueChanges(const NotifyActorMaxValueChanges& acMessage) const noexcept
{
    auto view = m_world.view<FormIdComponent, RemoteComponent>();

    const auto it = std::find_if(std::begin(view), std::end(view), [id = acMessage.Id, view](entt::entity entity) { return view.get<RemoteComponent>(entity).Id == id; });

    if (it == std::end(view))
        return;

    auto& formIdComponent = view.get<FormIdComponent>(*it);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    if (!pActor)
        return;

    for (const auto& [key, value] : acMessage.Values)
    {
        if (key == ActorValueInfo::kDragonSouls)
            continue;

        if (key == ActorValueInfo::kHealth && ShouldScaleActorHealth(pActor))
            m_world.ctx().at<ActorValueService>().SeedUnscaledMaxHealthFromRemote(pActor->formID, value);

        spdlog::debug("Actor max value update, server ID: {:X}, key: {}, value: {}", acMessage.Id, key, value);

        pActor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, key, value);
    }
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
    {
        if (acMessage.IsDead)
        {
            pActor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kInfamy, -1.0f);
            pActor->Kill();
        }
        else
        {
            pActor->Respawn();
        }
    }
}
