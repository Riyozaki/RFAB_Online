#include <TiltedOnlinePCH.h>

#include <Services/CharacterService.h>

#include <Actor.h>
#include <Components.h>
#include <World.h>

void CharacterService::OnFormIdIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity);
    if (!pForm || pForm->Id == 0)
    {
        m_entityIndex.EraseFormId(aEntity);
        return;
    }

    m_entityIndex.UpsertFormId(aEntity, pForm->Id, aRegistry.all_of<ObjectComponent>(aEntity));
}

void CharacterService::OnFormIdIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    (void)aRegistry;
    m_entityIndex.EraseFormId(aEntity);
}

void CharacterService::OnLocalIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pLocal = aRegistry.try_get<LocalComponent>(aEntity))
        m_entityIndex.BindServerId(aEntity, decltype(m_entityIndex)::kLocal, pLocal->Id);
}

void CharacterService::OnLocalIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    (void)aRegistry;
    m_entityIndex.UnbindServerId(aEntity, decltype(m_entityIndex)::kLocal);
}

void CharacterService::OnRemoteIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pRemote = aRegistry.try_get<RemoteComponent>(aEntity))
        m_entityIndex.BindServerId(aEntity, decltype(m_entityIndex)::kRemote, pRemote->Id);
}

void CharacterService::OnRemoteIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    (void)aRegistry;
    m_entityIndex.UnbindServerId(aEntity, decltype(m_entityIndex)::kRemote);
}

void CharacterService::OnObjectIndexUpdated(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    if (const auto* pObj = aRegistry.try_get<ObjectComponent>(aEntity))
        m_entityIndex.BindServerId(aEntity, decltype(m_entityIndex)::kObject, pObj->Id);

    if (const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity); pForm && pForm->Id != 0)
        m_entityIndex.PromoteFormToObject(aEntity, pForm->Id);
}

void CharacterService::OnObjectIndexDestroyed(entt::registry& aRegistry, entt::entity aEntity) noexcept
{
    m_entityIndex.UnbindServerId(aEntity, decltype(m_entityIndex)::kObject);
    if (const auto* pForm = aRegistry.try_get<FormIdComponent>(aEntity); pForm && pForm->Id != 0)
        m_entityIndex.UpsertFormId(aEntity, pForm->Id, false);
    else
        m_entityIndex.EraseFormId(aEntity);
}

entt::entity CharacterService::FindActorEntityByFormId(uint32_t aFormId) const noexcept
{
    if (aFormId == 0)
        return entt::null;

    if (const auto candidate = m_entityIndex.FindActorByFormId(aFormId); candidate.has_value())
    {
        const entt::entity cached = *candidate;
        if (m_world.valid(cached) && !m_world.all_of<ObjectComponent>(cached))
        {
            if (const auto* pForm = m_world.try_get<FormIdComponent>(cached); pForm && pForm->Id == aFormId)
                return cached;
        }
    }

    auto view = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
    for (entt::entity entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == aFormId)
        {
            m_entityIndex.UpsertFormId(entity, aFormId, false);
            return entity;
        }
    }

    return entt::null;
}

entt::entity CharacterService::FindAnyEntityByFormId(uint32_t aFormId) const noexcept
{
    if (aFormId == 0)
        return entt::null;

    if (const entt::entity actorEntity = FindActorEntityByFormId(aFormId); actorEntity != entt::null)
        return actorEntity;

    if (const auto objCandidate = m_entityIndex.FindObjectByFormId(aFormId); objCandidate.has_value())
    {
        const entt::entity cached = *objCandidate;
        if (m_world.valid(cached))
        {
            if (const auto* pForm = m_world.try_get<FormIdComponent>(cached); pForm && pForm->Id == aFormId)
                return cached;
        }
    }

    auto view = m_world.view<FormIdComponent>();
    for (entt::entity entity : view)
    {
        if (view.get<FormIdComponent>(entity).Id == aFormId)
        {
            m_entityIndex.UpsertFormId(entity, aFormId, m_world.all_of<ObjectComponent>(entity));
            return entity;
        }
    }

    return entt::null;
}

entt::entity CharacterService::FindEntityByServerId(uint32_t aServerId) const noexcept
{
    if (const auto candidate = m_entityIndex.FindByServerId(aServerId); candidate.has_value())
    {
        const entt::entity cached = *candidate;
        if (m_world.valid(cached))
        {
            if (const auto* pRemote = m_world.try_get<RemoteComponent>(cached); pRemote && pRemote->Id == aServerId)
                return cached;
            if (const auto* pLocal = m_world.try_get<LocalComponent>(cached); pLocal && pLocal->Id == aServerId)
                return cached;
            if (const auto* pObj = m_world.try_get<ObjectComponent>(cached); pObj && pObj->Id == aServerId)
                return cached;
        }
    }

    auto remoteView = m_world.view<RemoteComponent>();
    for (entt::entity entity : remoteView)
    {
        if (remoteView.get<RemoteComponent>(entity).Id == aServerId)
        {
            m_entityIndex.BindServerId(entity, decltype(m_entityIndex)::kRemote, aServerId);
            return entity;
        }
    }

    auto localView = m_world.view<LocalComponent>();
    for (entt::entity entity : localView)
    {
        if (localView.get<LocalComponent>(entity).Id == aServerId)
        {
            m_entityIndex.BindServerId(entity, decltype(m_entityIndex)::kLocal, aServerId);
            return entity;
        }
    }

    auto objectView = m_world.view<ObjectComponent>();
    for (entt::entity entity : objectView)
    {
        if (objectView.get<ObjectComponent>(entity).Id == aServerId)
        {
            m_entityIndex.BindServerId(entity, decltype(m_entityIndex)::kObject, aServerId);
            return entity;
        }
    }

    return entt::null;
}

Actor* CharacterService::CreateCharacterForEntity(entt::entity aEntity) const noexcept
{
    if (!m_world.valid(aEntity) || !m_world.all_of<RemoteComponent>(aEntity))
        return nullptr;

    const uint32_t serverId = m_world.get<RemoteComponent>(aEntity).Id;
    auto modelIt = m_remoteModels.find(serverId);
    if (modelIt == m_remoteModels.end())
        return nullptr;

    TESWorldSpace* pTargetWs = nullptr;
    TESObjectCELL* pTargetCell = ResolveCompatibleTargetCell(
        modelIt->second.Location,
        pTargetWs,
        modelIt->second.SpawnRequest.IsDragon);
    if (!pTargetCell)
        return nullptr;

    return MaterializeRemoteView(aEntity, modelIt.value(), pTargetCell, pTargetWs);
}

ActorData CharacterService::BuildActorData(Actor* apActor) const noexcept
{
    ActorData actorData{};
    if (!apActor)
        return actorData;

    actorData.InitialActorValues = apActor->GetEssentialActorValues();
    actorData.InitialInventory = apActor->GetActorInventory();
    actorData.IsDead = apActor->IsDead();
    actorData.IsWeaponDrawn = apActor->actorState.IsWeaponFullyDrawn();

    return actorData;
}
