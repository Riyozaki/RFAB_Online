#include <TiltedOnlinePCH.h>
#include <Games/ActorExtension.h>

#include <Services/ActorValueService.h>
#include <Services/SaveStateService.h>
#include <Services/CoopService.h>
#include <Coop/EnemyHealth.h>
#include <World.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESQuest.h>
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
    m_scales.clear(); m_pendingScales.clear(); m_scaleFault.clear();
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
    {
        if (const auto id = Utils::GetServerId(*it))
        { m_scales.erase(*id); m_pendingScales.erase(*id); m_health.erase(*id); }
        m_world.remove<ActorValuesComponent>(*it);
    }
}

void ActorValueService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    RunScaleUpdates();
    if (!m_transport.IsConnected()) return;
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
            const auto scale = m_scales.find(localComponent.Id);
            if (i == ActorValueInfo::kHealth && scale != m_scales.end())
            {
                if (!std::isfinite(newMaxValue) || std::abs(newMaxValue - scale->second.Maximum) >
                    std::max(0.01f, scale->second.Maximum * 0.00001f))
                    ScaleFault("scaled permanent HP changed outside its verified life adapter (possible scripted phase/reset)");
                continue; // an old periodic max snapshot must never undo or re-multiply x2
            }
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
    if (acValues.Scale.Life)
    {
        QueueScale(aServerId, aEpoch, acValues);
        ApplyPendingScale(aServerId);
        return;
    }
    // Reset A supplies a health-only control record. Retain the already proven life/max; a
    // complete unscaled actor snapshot over a scaled life is NOT an implicit respawn/reset.
    const auto scale = m_scales.find(aServerId);
    if (scale != m_scales.end() && !acValues.ActorMaxValuesList.contains(ActorValueInfo::kHealth))
    {
        auto values = acValues; values.Scale = scale->second;
        values.ActorMaxValuesList[ActorValueInfo::kHealth] = scale->second.Maximum;
        QueueScale(aServerId, aEpoch, values); ApplyPendingScale(aServerId);
        return;
    }
    if (auto* actor = Utils::GetByServerId<Actor>(aServerId); actor && acValues.ActorMaxValuesList.contains(ActorValueInfo::kHealth))
        for (const auto& saved : m_world.ctx().at<SaveStateService>().State().Scales)
            if (saved.Form == actor->formID)
            { ScaleFault("unscaled control snapshot conflicts with a save-bound enemy life"); return; }
    ApplyHealthBaseline(aServerId, aEpoch, acValues);
}
void ActorValueService::ApplyHealthBaseline(uint32_t aServerId, uint32_t aEpoch, const ActorValues& acValues) noexcept
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
    if (adopted && actor && acValues.Scale.Life)
        actor->ForceActorValue(ActorValueOwner::ForceMode::PERMANENT, ActorValueInfo::kHealth, acValues.Scale.Maximum);
    if (adopted && actor)
        actor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, *adopted);
}

void ActorValueService::ApplyActorValues(Actor* apActor, uint32_t aServerId, uint32_t aEpoch, const ActorValues& acValues) noexcept
{
    if (!apActor || !acValues.IsValid())
        return;
    if (acValues.Scale.Life)
    {
        QueueScale(aServerId, aEpoch, acValues); ApplyPendingScale(aServerId);
        return;
    }
    if (m_scales.contains(aServerId))
    { ScaleFault("complete unscaled control snapshot over a saved scaled life"); return; }
    for (const auto& saved : m_world.ctx().at<SaveStateService>().State().Scales)
        if (saved.Form == apActor->formID)
        { ScaleFault("unscaled control snapshot conflicts with this character's saved enemy life"); return; }
    apActor->SetActorValues(acValues); // health remains exclusively canonical/predicted
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
    if (!std::isfinite(acMessage.Health) || !acMessage.Scale.IsValid()) return;
    if (acMessage.Scale.Life)
    {
        ActorValues values;
        values.Scale = acMessage.Scale; values.HealthRevision = acMessage.Revision;
        values.ActorValuesList[ActorValueInfo::kHealth] = acMessage.Health;
        values.ActorMaxValuesList[ActorValueInfo::kHealth] = acMessage.Scale.Maximum;
        if (acMessage.SourcePlayerId == m_transport.GetLocalPlayerId() && acMessage.Sequence)
            values.HealthSequences[acMessage.SourcePlayerId] = acMessage.Sequence;
        QueueScale(acMessage.Id, acMessage.OwnershipEpoch, values); ApplyPendingScale(acMessage.Id);
        return;
    }
    const auto entity = Utils::FindEntityByServerId(acMessage.Id);
    if (!entity || OwnershipSystem::GetOwnershipEpoch(m_world, *entity) != acMessage.OwnershipEpoch) return;
    if (m_scales.contains(acMessage.Id) || m_pendingScales.contains(acMessage.Id))
    {
        const auto baseline = m_health.find(acMessage.Id);
        const auto pending = m_pendingScales.find(acMessage.Id);
        // A health reply sent BEFORE scaling may arrive while the first stamped snapshot is
        // waiting for the native/save boundary. Only a newer/equal omission is contradictory.
        const auto revision = pending != m_pendingScales.end() ? pending->second.Values.HealthRevision :
            (baseline != m_health.end() ? baseline->second.Revision : 0);
        if (acMessage.Revision >= revision)
            ScaleFault("health update omitted an already negotiated life stamp");
        return;
    }
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
        if ((key == ActorValueInfo::kHealth && (m_scales.contains(acMessage.Id) || m_pendingScales.contains(acMessage.Id))) || key >= ActorValueInfo::kActorValueCount || key == ActorValueInfo::kInfamy || !std::isfinite(value) || key == ActorValueInfo::kDragonSouls)
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

namespace
{
uint64_t ScaleNow() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool ScaleNear(float actual, float expected) noexcept
{
    return std::isfinite(actual) && std::isfinite(expected) &&
        std::abs(actual - expected) <= std::max(0.01f, std::abs(expected) * 0.00001f);
}
}
void ActorValueService::ScaleFault(const char* reason) noexcept
{
    if (m_scaleFault.empty()) m_scaleFault = reason;
    m_world.ctx().at<SaveStateService>().Quarantine();
}
void ActorValueService::DescribeScale(Actor* actor, ActorValues& values) noexcept
{
    if (!actor || actor->GetExtension()->IsPlayer()) return;
    for (const auto& saved : m_world.ctx().at<SaveStateService>().State().Scales)
        if (saved.Form == actor->formID)
        {
            const auto maximum = values.ActorMaxValuesList.find(ActorValueInfo::kHealth);
            HealthScale scale{saved.Life, saved.Base, saved.Maximum};
            if (saved.Version != 1 || !scale.IsValid() || maximum == values.ActorMaxValuesList.end() ||
                !ScaleNear(maximum->second, saved.Maximum))
            {
                values.WireValid = false; // an unknown reset/phase is not permission to divide or multiply HP again
                ScaleFault("saved enemy life does not match native permanent HP; a validated reset/phase adapter is required");
                return;
            }
            values.Scale = scale;
            values.ActorMaxValuesList[ActorValueInfo::kHealth] = saved.Maximum;
            return;
        }
}
void ActorValueService::QueueScale(uint32_t server, uint32_t epoch, const ActorValues& values) noexcept
{
    if (!server || !epoch || !values.IsValid() || !values.Scale.Life) return;
    if (const auto entity = Utils::FindEntityByServerId(server); entity &&
        OwnershipSystem::GetOwnershipEpoch(m_world, *entity) > epoch) return;
    if (m_pendingScales.size() >= 512 && !m_pendingScales.contains(server))
    { ScaleFault("enemy scale inbox exceeds its bounded capacity"); return; }
    const auto known = m_scales.find(server);
    if (known != m_scales.end() && known->second != values.Scale)
    { ScaleFault("enemy life changed without a coordinated native reset"); return; }
    auto& pending = m_pendingScales[server];
    if (epoch < pending.Epoch) return;
    if (epoch > pending.Epoch)
    {
        pending = {epoch, values, ScaleNow()};
        return;
    }
    if (!Coop::MergeScaleSnapshot(pending.Values, values))
        ScaleFault("conflicting or over-limit scale snapshots in one ownership epoch");
}
bool ActorValueService::ApplyPendingScale(uint32_t server) noexcept
{
    const auto it = m_pendingScales.find(server);
    if (it == m_pendingScales.end() || !m_scaleFault.empty()) return false;
    const auto pending = it->second; // owned data, never retain a native pointer across a frame
    auto& coop = m_world.ctx().at<CoopService>();
    auto& saves = m_world.ctx().at<SaveStateService>();
    if (!coop.Active()) return false; // spawn/control can precede Welcome
    if (!coop.Supports(Coop::EnemyScaling) || !saves.Ready())
    { ScaleFault("scaled actor requires negotiated enemy-health and a matching loaded co-save"); return false; }
    if (ScaleNow() - pending.Since > 5000)
    { ScaleFault("enemy scale could not acquire its native/receipt boundary within five seconds"); return false; }
    const auto entity = Utils::FindEntityByServerId(server);
    auto* actor = Utils::GetByServerId<Actor>(server);
    if (!entity || !actor || !actor->GetNiNode()) return false;
    const auto epoch = OwnershipSystem::GetOwnershipEpoch(m_world, *entity);
    if (epoch > pending.Epoch) { m_pendingScales.erase(server); return false; }
    if (epoch != pending.Epoch) return false;
    if (actor->GetExtension()->IsPlayer() || actor->IsPlayerSummon() || actor->IsCommandedActor() || actor->IsMount() ||
        (actor->flags1 & (1u << 26)))
    { ScaleFault("enemy scale targets a player, ally, commanded actor or mount"); return false; }
    if (const auto* scene = actor->GetCurrentScene(); (scene && scene->isPlaying) || actor->IsEssential() || (actor->flags2 & (1u << 19)))
    { ScaleFault("scripted/protected actor requires an explicit health-phase adapter"); return false; }
    const auto& values = pending.Values;
    const auto& scale = values.Scale;
    const Coop::SavedScale* saved{};
    for (const auto& stamp : saves.State().Scales) if (stamp.Form == actor->formID) { saved = &stamp; break; }
    if (saved && (saved->Version != 1 || saved->Life != scale.Life || saved->Base != scale.Base || saved->Maximum != scale.Maximum))
    { ScaleFault("enemy life conflicts with this character's saved scale stamp"); return false; }
    if (saved && !ScaleNear(actor->GetActorPermanentValue(ActorValueInfo::kHealth), scale.Maximum))
    { ScaleFault("a script/reset changed scaled permanent HP outside its negotiated life adapter"); return false; }
    const auto effect = Coop::NativeEffect(Coop::EffectKind::Scale, coop.Session(), coop.Generation(), scale.Life);
    const bool first = saved == nullptr;
    if (first && m_world.all_of<LocalComponent>(*entity) &&
        !ScaleNear(actor->GetActorPermanentValue(ActorValueInfo::kHealth), scale.Base))
    { ScaleFault("native owner maximum changed before scale application; no guessed script-phase override"); return false; }
    if (first)
    {
        if (!saves.Begin(Coop::EffectKind::Scale, effect)) return false; // no blocking main-thread wait
        if (!saves.StampScale({actor->formID, 1, scale.Life, scale.Base, scale.Maximum}))
        { saves.AbortBeforeNative(effect); ScaleFault("cannot stamp enemy scale in this bounded character co-save"); return false; }
    }
    // Observe unsent local damage BEFORE changing permanent HP (which can itself change current
    // HP), then adopt the canonical revision+ACK and restore its prediction once, not x2 again.
    ApplyHealthBaseline(server, pending.Epoch, values);
    actor->SetActorValues(values);
    const auto health = m_health.find(server);
    if (health != m_health.end())
        actor->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, health->second.Project());
    bool applied = health != m_health.end() && health->second.Epoch == pending.Epoch &&
        ScaleNear(actor->GetActorPermanentValue(ActorValueInfo::kHealth), scale.Maximum) &&
        ScaleNear(actor->GetActorValue(ActorValueInfo::kHealth), health->second.Project());
    if (first) applied = applied && saves.Applied(effect) && saves.Commit(effect);
    if (!applied)
    {
        if (first) saves.Uncertain(effect);
        ScaleFault("enemy HP/native receipt application is uncertain; no guessed rollback or repeat multiplier");
        return false;
    }
    m_scales[server] = scale;
    if (auto* cache = m_world.try_get<ActorValuesComponent>(*entity))
    {
        cache->CurrentActorValues.Scale = scale;
        cache->CurrentActorValues.ActorMaxValuesList[ActorValueInfo::kHealth] = scale.Maximum;
        cache->CurrentActorValues.HealthRevision = health->second.Revision;
        cache->CurrentActorValues.ActorValuesList[ActorValueInfo::kHealth] = health->second.Project();
    }
    if (auto* waiting = m_world.try_get<WaitingFor3D>(*entity))
    {
        waiting->SpawnRequest.InitialActorValues.Scale = scale;
        waiting->SpawnRequest.InitialActorValues.ActorMaxValuesList[ActorValueInfo::kHealth] = scale.Maximum;
        waiting->SpawnRequest.InitialActorValues.ActorValuesList[ActorValueInfo::kHealth] = health->second.Project();
        waiting->SpawnRequest.InitialActorValues.HealthRevision = health->second.Revision;
    }
    m_pendingScales.erase(server);
    if (!actor->IsDead() && health->second.Project() <= 0.f) actor->Kill();
    return true;
}
void ActorValueService::RunScaleUpdates() noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    std::vector<uint32_t> pending;
    for (const auto& [server, _] : m_pendingScales) pending.push_back(server);
    for (auto server : pending) ApplyPendingScale(server);
    if (m_scaleFault.empty()) return;
    const auto reason = m_scaleFault; // Close() synchronously clears this service's network caches
    m_world.ctx().at<SaveStateService>().Quarantine();
    spdlog::error("[RFAB] enemy-health adapter quarantined: {}", reason);
    Utils::ShowHudMessage("RFAB: enemy life/HP adapter mismatch. Session disconnected and shared effects locked; load a matching save. No automatic x4 scaling or base-HP guess.");
    m_transport.Close();
}
