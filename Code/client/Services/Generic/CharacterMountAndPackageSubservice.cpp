#include <TiltedOnlinePCH.h>

#include <Services/CharacterService.h>

#include <Actor.h>
#include <Components.h>
#include <Events/BeastFormChangeEvent.h>
#include <Events/InitPackageEvent.h>
#include <Events/MountEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Messages/MountRequest.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/RequestRespawn.h>
#include <Services/TransportService.h>
#include <Structs/ActionEvent.h>
#include <Systems/AnimationSystem.h>
#include <Systems/InterpolationSystem.h>
#include <Utils.h>
#include <World.h>

void CharacterService::OnActionEvent(const ActionEvent& acActionEvent) const noexcept
{
    const entt::entity entity = FindActorEntityByFormId(acActionEvent.ActorId);
    if (entity == entt::null)
        return;

    if (auto* pLocalAnim = m_world.try_get<LocalAnimationComponent>(entity))
    {
        pLocalAnim->Append(acActionEvent);
    }
    else if (m_transport.IsOnline())
    {
        if (auto* pEarlyAnim = m_world.try_get<EarlyAnimationBufferComponent>(entity))
            pEarlyAnim->Actions.push_back(acActionEvent);
    }
}

void CharacterService::OnFactionsChanges(const NotifyFactionsChanges& acEvent) const noexcept
{
    for (const auto& [id, factions] : acEvent.Changes)
    {
        if (auto modelIt = m_remoteModels.find(id); modelIt != m_remoteModels.end())
            modelIt.value().SpawnRequest.FactionsContent = factions;

        const entt::entity entity = FindEntityByServerId(id);
        if (entity != entt::null)
        {
            if (auto* pWaiting = m_world.try_get<WaitingFor3D>(entity))
                pWaiting->SpawnRequest.FactionsContent = factions;

            if (m_world.all_of<RemoteComponent, FormIdComponent, CacheComponent>(entity))
            {
                auto& formIdComponent = m_world.get<FormIdComponent>(entity);
                auto* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
                if (!pActor)
                    continue;

                auto& cacheComponent = m_world.get<CacheComponent>(entity);
                cacheComponent.FactionsContent = factions;
                pActor->SetFactions(cacheComponent.FactionsContent);
            }
        }
    }
}

void CharacterService::OnNotifyRespawn(const NotifyRespawn& acMessage) const noexcept
{
    const entt::entity cId = FindEntityByServerId(acMessage.ActorId);

    m_cachedRemoteSpawns.erase(acMessage.ActorId);
    m_remoteModels.erase(acMessage.ActorId);

    if (cId != entt::null && m_world.all_of<RemoteComponent>(cId))
    {
        if (const auto* pFormComp = m_world.try_get<FormIdComponent>(cId))
            CancelServerAssignment(cId, pFormComp->Id);
        else
            DeleteRemoteEntityComponents(cId);

        m_world.remove<EarlyAnimationBufferComponent, FormIdComponent>(cId);
        if (m_world.orphan(cId))
            m_world.destroy(cId);
    }

    RequestRespawn request;
    request.ActorId = acMessage.ActorId;
    m_transport.Send(request);
}

void CharacterService::OnBeastFormChange(const BeastFormChangeEvent& acEvent) const noexcept
{
    (void)acEvent;
    if (!m_transport.IsConnected())
        return;

    const entt::entity playerEntity = FindActorEntityByFormId(0x14);
    if (playerEntity == entt::null)
        return;

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(playerEntity);
    if (!serverIdRes.has_value())
    {
        spdlog::error("{}: failed to find server id", __FUNCTION__);
        return;
    }

    const uint32_t serverId = serverIdRes.value();

    RequestRespawn request;
    request.ActorId = serverId;

    Actor* pActor = Utils::GetByServerId<Actor>(serverId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor for server id {:X}", serverId);
        return;
    }

    TESNPC* pNpc = Cast<TESNPC>(pActor->baseForm);
    if (!pNpc)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor baseform for server id {:X}", serverId);
        return;
    }

    pNpc->MarkChanged(0x3000802u);
    pNpc->Serialize(&request.AppearanceBuffer);
    request.ChangeFlags = pNpc->GetChangeFlags();
    if ((pNpc->formID & 0xFF000000u) != 0xFF000000u && pActor->formID != 0x14)
        pNpc->UnsetChanged(0x3000802u);

    m_transport.Send(request);
}

void CharacterService::OnMountEvent(const MountEvent& acEvent) const noexcept
{
    const entt::entity cRiderEntity = FindActorEntityByFormId(acEvent.RiderID);
    const entt::entity cMountEntity = FindActorEntityByFormId(acEvent.MountID);

    if (cRiderEntity == entt::null || cMountEntity == entt::null)
    {
        m_pendingLocalMount = {acEvent.RiderID, acEvent.MountID, NowSteadyWorldViewMs() + 3000ULL};
        spdlog::debug("OnMountEvent: queued pending local mount {:X} -> {:X} while entities register", acEvent.RiderID, acEvent.MountID);
        return;
    }

    std::optional<uint32_t> riderServerIdRes = Utils::GetServerId(cRiderEntity);
    std::optional<uint32_t> mountServerIdRes = Utils::GetServerId(cMountEntity);
    if (!riderServerIdRes.has_value() || !mountServerIdRes.has_value())
    {
        m_pendingLocalMount = {acEvent.RiderID, acEvent.MountID, NowSteadyWorldViewMs() + 3000ULL};
        spdlog::debug("OnMountEvent: waiting for server assignment on rider {:X} or mount {:X}", acEvent.RiderID, acEvent.MountID);
        return;
    }

    m_pendingLocalMount = {};

    if (m_world.try_get<RemoteComponent>(cMountEntity))
        TakeOwnership(acEvent.MountID, *mountServerIdRes, cMountEntity);

    MountRequest request;
    request.MountId = mountServerIdRes.value();
    request.RiderId = riderServerIdRes.value();
    m_transport.Send(request);
}

void CharacterService::OnNotifyMount(const NotifyMount& acMessage) const noexcept
{
    Actor* pRider = nullptr;
    if (const entt::entity riderEntity = FindEntityByServerId(acMessage.RiderId);
        riderEntity != entt::null && m_world.all_of<RemoteComponent, FormIdComponent>(riderEntity))
    {
        const auto& riderFormIdComponent = m_world.get<FormIdComponent>(riderEntity);
        pRider = Cast<Actor>(TESForm::GetById(riderFormIdComponent.Id));
    }

    Actor* pMount = nullptr;
    if (const entt::entity mountEntity = FindEntityByServerId(acMessage.MountId);
        mountEntity != entt::null && m_world.all_of<FormIdComponent>(mountEntity) && !m_world.all_of<ObjectComponent>(mountEntity))
    {
        auto& mountFormIdComponent = m_world.get<FormIdComponent>(mountEntity);

        if (m_world.all_of<LocalComponent>(mountEntity))
        {
            m_world.remove<LocalAnimationComponent, LocalComponent>(mountEntity);
            m_world.emplace_or_replace<RemoteComponent>(mountEntity, acMessage.MountId, mountFormIdComponent.Id);
        }

        pMount = Cast<Actor>(TESForm::GetById(mountFormIdComponent.Id));
        if (pMount && pMount->GetExtension())
            pMount->GetExtension()->SetRemote(true);

        InterpolationSystem::Setup(m_world, mountEntity);
        AnimationSystem::Setup(m_world, mountEntity);
    }

    if (pRider && pMount && pRider->GetNiNode() && pMount->GetNiNode())
    {
        m_pendingRemoteMounts.erase(acMessage.RiderId);
        pRider->InitiateMountPackage(pMount);
    }
    else
    {
        m_pendingRemoteMounts[acMessage.RiderId] = {acMessage.MountId, NowSteadyWorldViewMs() + 5000ULL};
        spdlog::debug(
            "Queued pending remote mount attachment rider {:X} -> mount {:X} until 3D materializes",
            acMessage.RiderId,
            acMessage.MountId);
    }
}

void CharacterService::RetryPendingMounts() noexcept
{
    const uint64_t nowMountMs = NowSteadyWorldViewMs();
    if (m_pendingLocalMount.RiderFormId != 0 && m_pendingLocalMount.MountFormId != 0)
    {
        if (nowMountMs >= m_pendingLocalMount.ExpiresAtMs)
        {
            m_pendingLocalMount = {};
        }
        else
        {
            const PendingLocalMountRequest pendingLocal = m_pendingLocalMount;
            OnMountEvent(MountEvent(pendingLocal.RiderFormId, pendingLocal.MountFormId));
        }
    }

    if (m_pendingRemoteMounts.empty())
        return;

    Vector<uint32_t> resolvedRemoteRiders;
    for (const auto& [riderServerId, pending] : m_pendingRemoteMounts)
    {
        if (nowMountMs >= pending.ExpiresAtMs)
        {
            resolvedRemoteRiders.push_back(riderServerId);
            continue;
        }

        Actor* pRider = nullptr;
        if (const entt::entity riderEntity = FindEntityByServerId(riderServerId);
            riderEntity != entt::null && m_world.all_of<RemoteComponent, FormIdComponent>(riderEntity))
        {
            pRider = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(riderEntity).Id));
        }

        Actor* pMount = nullptr;
        if (const entt::entity mountEntity = FindEntityByServerId(pending.MountServerId);
            mountEntity != entt::null && m_world.all_of<FormIdComponent>(mountEntity) && !m_world.all_of<ObjectComponent>(mountEntity))
        {
            pMount = Cast<Actor>(TESForm::GetById(m_world.get<FormIdComponent>(mountEntity).Id));
        }

        if (pRider && pMount && pRider->GetNiNode() && pMount->GetNiNode())
        {
            pRider->InitiateMountPackage(pMount);
            resolvedRemoteRiders.push_back(riderServerId);
        }
    }

    for (const uint32_t riderId : resolvedRemoteRiders)
        m_pendingRemoteMounts.erase(riderId);
}

void CharacterService::OnInitPackageEvent(const InitPackageEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    const entt::entity cActorEntity = FindActorEntityByFormId(acEvent.ActorId);
    if (cActorEntity == entt::null)
        return;

    if (!m_world.any_of<LocalComponent, RemoteComponent>(cActorEntity))
        return;

    std::optional<uint32_t> actorServerIdRes = Utils::GetServerId(cActorEntity);
    if (!actorServerIdRes.has_value())
    {
        spdlog::debug("{}: failed to find server id", __FUNCTION__);
        return;
    }

    NewPackageRequest request;
    request.ActorId = actorServerIdRes.value();
    if (!m_world.GetModSystem().GetServerModId(acEvent.PackageId, request.PackageId.ModId, request.PackageId.BaseId))
        return;

    m_transport.Send(request);
}

void CharacterService::OnNotifyNewPackage(const NotifyNewPackage& acMessage) const noexcept
{
    const entt::entity remoteEntity = FindEntityByServerId(acMessage.ActorId);
    if (remoteEntity == entt::null || !m_world.all_of<RemoteComponent, FormIdComponent>(remoteEntity))
    {
        spdlog::warn("Actor for package with remote id {:X} not found.", acMessage.ActorId);
        return;
    }

    const auto& formIdComponent = m_world.get<FormIdComponent>(remoteEntity);
    Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));

    const uint32_t cPackageFormId = World::Get().GetModSystem().GetGameId(acMessage.PackageId);
    const TESForm* pPackageForm = TESForm::GetById(cPackageFormId);
    if (!pPackageForm)
    {
        spdlog::warn("Actor package not found, base id: {:X}, mod id: {:X}", acMessage.PackageId.BaseId, acMessage.PackageId.ModId);
        return;
    }

    TESPackage* pPackage = Cast<TESPackage>(pPackageForm);
    if (!pActor || !pPackage)
        return;

    pActor->SetPackage(pPackage);
}

void CharacterService::OnNotifyActorTeleport(const NotifyActorTeleport& acMessage) noexcept
{
    if (m_engineWorldLoadInProgress || m_savePurgeActive)
        return;

    const uint32_t cActorId = m_world.GetModSystem().GetGameId(acMessage.FormId);
    if (cActorId == 0 || cActorId == 0x14)
        return;

    Actor* pActor = Cast<Actor>(TESForm::GetById(cActorId));
    if (!pActor || pActor->IsStillLoading())
    {
        spdlog::debug(__FUNCTION__ ": skipping unloaded actor {:X} for distant teleport notify.", cActorId);
        return;
    }

    if (MoveActor(pActor, acMessage.WorldSpaceId, acMessage.CellId, acMessage.Position))
    {
        spdlog::info(
            "Successfully teleported actor, form id: {:X}, world space: {:X}, cell: {:X}, position: ({}, {}, {})",
            pActor->formID,
            acMessage.WorldSpaceId.BaseId,
            acMessage.CellId.BaseId,
            acMessage.Position.x,
            acMessage.Position.y,
            acMessage.Position.z);
    }
}

void CharacterService::OnPartyJoinedEvent(const PartyJoinedEvent& acEvent) noexcept
{
    if (acEvent.IsLeader)
    {
        auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        Vector<entt::entity> entities(view.begin(), view.end());

        for (auto entity : entities)
            ProcessNewEntity(entity);
    }
}

void CharacterService::RunFactionsUpdates() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    RequestFactionsChanges message;

    auto factionedActors = m_world.view<LocalComponent, CacheComponent, FormIdComponent>();
    for (auto entity : factionedActors)
    {
        auto& formIdComponent = factionedActors.get<FormIdComponent>(entity);
        auto& localComponent = factionedActors.get<LocalComponent>(entity);
        auto& cacheComponent = factionedActors.get<CacheComponent>(entity);

        const auto* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor)
            continue;

        auto factions = pActor->GetFactions();
        if (cacheComponent.FactionsContent == factions)
            continue;

        cacheComponent.FactionsContent = factions;
        message.Changes[localComponent.Id] = factions;
    }

    if (!message.Changes.empty())
        m_transport.Send(message);
}

void CharacterService::ApplyCachedWeaponDraws(const UpdateEvent& acUpdateEvent) noexcept
{
    constexpr size_t kMaxPendingWeaponDrawUpdates = 1024;
    if (m_weaponDrawUpdates.size() > kMaxPendingWeaponDrawUpdates)
        m_weaponDrawUpdates.clear();

    std::vector<uint32_t> toRemove{};

    for (auto& [cId, _] : m_weaponDrawUpdates)
    {
        auto& data = m_weaponDrawUpdates[cId];
        data.m_timer += acUpdateEvent.Delta;

        if (data.m_timer > 5.0)
        {
            toRemove.push_back(cId);
            continue;
        }

        const double maxTime = data.m_isFirstPass ? 0.5 : 2.0;
        if (data.m_timer <= maxTime)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(cId));
        if (!pActor || pActor->IsDeleted())
        {
            toRemove.push_back(cId);
            continue;
        }

        pActor->SetWeaponDrawnEx(data.m_drawWeapon);

        if (!data.m_isFirstPass)
            toRemove.push_back(cId);

        data.m_isFirstPass = false;
    }

    for (uint32_t id : toRemove)
        m_weaponDrawUpdates.erase(id);
}
