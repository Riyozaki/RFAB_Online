#include <Services/ObjectService.h>

#include <World.h>
#include <Services/LootService.h>
#include <Services/StorageService.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>
#include <Forms/TESFaction.h>
#include <Games/TES.h>

#include <inttypes.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ObjectService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&ObjectService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&ObjectService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&ObjectService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&ObjectService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&ObjectService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&ObjectService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&ObjectService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&ObjectService::OnNotifyScriptAnimation>(this);

    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
}

bool IsPlayerHome(const TESObjectCELL* pCell) noexcept
{
    if (pCell && pCell->loadedCellData && pCell->loadedCellData->encounterZone)
    {
        // Only return true if cell has the NoResetZone encounter zone
        if (pCell->loadedCellData->encounterZone->formID == 0xf90b1)
        {
            switch (pCell->formID)
            {
            case 0xeec55: // one known exception: Sinderion's Field Lab
                return false;
            default: return true;
            }
        }
    }

    return false;
}

// Find each loaded faction's containers for a jailed player's belongings and stolen items.
// Do not sync them: each player's items must stay separate (#700).
// Only compare the container pointers; never read through them.
Set<const TESObjectREFR*> GetPlayerStashContainers() noexcept
{
    Set<const TESObjectREFR*> containers{};

    ModManager* pModManager = ModManager::Get();
    if (!pModManager)
        return containers;

    for (const TESFaction* pFaction : pModManager->factions)
    {
        if (!pFaction)
            continue;

        if (pFaction->crimeData.playerInventoryContainer)
            containers.insert(pFaction->crimeData.playerInventoryContainer);

        if (pFaction->crimeData.stolenGoodsContainer)
            containers.insert(pFaction->crimeData.stolenGoodsContainer);
    }

    return containers;
}

bool ShouldSyncObject(const TESObjectREFR* apObject, const Set<const TESObjectREFR*>& acPlayerStashContainers) noexcept
{
    if (!apObject)
        return false;

    if (acPlayerStashContainers.contains(apObject))
        return false;

    // Quest chests that take the player's whole inventory without going through faction crime data.
    switch (apObject->formID)
    {
    case 0x39CF1: // Don't sync the chest in the "Diplomatic Immunity" quest
        return false;
    case 0x3EF03: // ...as well as in the "No One Escapes Cidhna Mine" quest
        return false;
    default:
        return true;
    }
}

void ObjectService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_accessRequests.clear();
    const auto view = m_world.view<ObjectComponent>();
    std::vector<entt::entity> stale(view.begin(), view.end());
    for (auto entity : stale)
        m_world.destroy(entity);
}

bool ObjectService::IsSharedInventoryObject(const TESObjectREFR* object) const noexcept
{
    if (object && StorageService::IsBacking(object->formID)) return true;
    if (!object || !object->baseForm || object->baseForm->formType != FormType::Container || object->IsTemporary()) return false;
    if (!m_world.GetServerSettings().SyncPlayerHomes && IsPlayerHome(object->parentCell)) return false;
    return ShouldSyncObject(object, GetPlayerStashContainers());
}
bool ObjectService::EnsureInventoryObject(uint32_t form) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return false;
    if (StorageService::IsBacking(form)) return m_world.ctx().at<StorageService>().EnsureBacking(form);
    return RegisterInventoryObject(form, false);
}
bool ObjectService::EnsureStorageAccess(uint32_t form) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !m_world.ctx().at<StorageService>().IsAccess(form)) return false;
    return RegisterInventoryObject(form, true);
}
bool ObjectService::RegisterInventoryObject(uint32_t aForm, bool verifiedAccess) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !m_transport.IsOnline()) return false;
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();
    for (auto entity : view) if (view.get<FormIdComponent>(entity).Id == aForm) return true;
    auto* object = Cast<TESObjectREFR>(TESForm::GetById(aForm));
    if (!object || !object->baseForm || (!verifiedAccess && !IsSharedInventoryObject(object)) || !object->parentCell) return false;
    const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (const auto old = m_accessRequests.find(aForm); old != m_accessRequests.end() && now - old->second < 500) return true;
    if (m_accessRequests.size() >= 64 && !m_accessRequests.contains(aForm)) return false;
    ObjectData data;
    auto& mods = m_world.GetModSystem();
    if (!mods.GetServerModId(aForm, data.Id) || !mods.GetServerModId(object->parentCell->formID, data.CellId)) return false;
    if (auto* space = object->GetWorldSpace()) if (!mods.GetServerModId(space->formID, data.WorldSpaceId)) return false;
    data.CurrentCoords = GridCellCoords::CalculateGridCellCoords(object->position.x, object->position.y);
    data.CurrentInventory = object->GetInventory();
    if (!data.CurrentInventory.IsValid()) return false;
    AssignObjectsRequest request;
    request.Objects.push_back(std::move(data));
    m_accessRequests[aForm] = now;
    m_transport.Send(request);
    return true;
}

void ObjectService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    TESObjectCELL* pCell = pPlayer->parentCell;

    // Player homes should not be synced, so that chest contents,
    // which are often used as storage, are never accidentally wiped.
    if (!World::Get().GetServerSettings().SyncPlayerHomes && IsPlayerHome(pCell))
        return;

    GameId cellId{};
    if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    GameId worldSpaceId{};
    if (TESWorldSpace* pWorldSpace = pPlayer->GetWorldSpace())
    {
        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, worldSpaceId))
        {
            spdlog::error("Server world space id not found for world space form id {:X}", pWorldSpace->formID);
            return;
        }
    }

    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    // Door seemed to be at the wrong form id (29, now 32), verify this.
    Vector<TESObjectREFR*> objects = pCell->GetRefsByFormTypes(formTypes);

    AssignObjectsRequest request{};

    const Set<const TESObjectREFR*> playerStashContainers = GetPlayerStashContainers();

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject, playerStashContainers))
        {
            spdlog::warn("Excluding sync for {:X}", pObject->formID);
            continue;
        }

        ObjectData objectData{};
        objectData.CellId = cellId;
        objectData.WorldSpaceId = worldSpaceId;
        objectData.CurrentCoords = GridCellCoords::CalculateGridCellCoords(pObject->position.x, pObject->position.y);

        if (!m_world.GetModSystem().GetServerModId(pObject->formID, objectData.Id))
        {
            spdlog::error("Server form id not found for object with form id {:X}", pObject->formID);
            continue;
        }

        if (Lock* pLock = pObject->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->IsLocked();
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        if (StorageService::IsBacking(pObject->formID)) continue;
        if (pObject->baseForm->formType == FormType::Container)
            objectData.CurrentInventory = pObject->GetInventory();

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

void ObjectService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const ObjectData& objectData : acMessage.Objects)
    {
        const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(objectData.Id);
        TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
        if (!pObject)
        {
            spdlog::error("Object not found for form id {:X}", objectData.Id);
            continue;
        }

        if (StorageService::IsBacking(pObject->formID)) continue; // only the campaign resolver may bind a hidden backing
        CreateObjectEntity(pObject->formID, objectData.ServerId);
        m_accessRequests.erase(pObject->formID);
        if (!m_world.ctx().at<LootService>().AcceptSnapshot(objectData.ServerId, 0, objectData.CurrentInventory)) continue;

        if (objectData.IsSenderFirst)
            continue;

        if (objectData.CurrentLockData != LockData{})
        {
            Lock* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
                if (!pLock)
                    continue;
            }

            pLock->lockLevel = objectData.CurrentLockData.LockLevel;
            pLock->SetLock(objectData.CurrentLockData.IsLocked);
            pObject->LockChange();
        }

        if (pObject->baseForm->formType == FormType::Container)
        {
            if (!pObject->ApplyExactInventory(objectData.CurrentInventory))
                Utils::ShowHudMessage("RFAB: container instance mismatch; contents were not blindly replaced. Shared transfer will be refused.");
        }
    }
}

entt::entity ObjectService::CreateObjectEntity(const uint32_t acFormId, const uint32_t acServerId) noexcept
{
    const auto view = m_world.view<FormIdComponent, ObjectComponent>();

    auto it = std::find_if(view.begin(), view.end(), [acServerId, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == acServerId; });

    if (it != view.end())
        return *it;

    entt::entity entity = m_world.create();
    spdlog::info("Created object entity, server id: {:X}, form id {:X}", acServerId, acFormId);

    m_world.emplace<FormIdComponent>(entity, acFormId);
    m_world.emplace<ObjectComponent>(entity, acServerId);

    return entity;
}

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    auto* object = Cast<TESObjectREFR>(TESForm::GetById(acEvent.ObjectId));
    auto* activator = Cast<Actor>(TESForm::GetById(acEvent.ActivatorId));
    auto* objectToGet = Cast<TESBoundObject>(TESForm::GetById(acEvent.ObjectToGetId));
    if (!object || !activator)
        return;
    if (m_world.ctx().at<StorageService>().IsAccess(acEvent.ObjectId)) return; // local script redirects; don't open a second client's bank by legacy Activate

    if (acEvent.ActivateFlag)
    {
        object->Activate(activator, acEvent.Unk1, objectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    if (!m_transport.IsConnected())
        return;

    if (Lock* pLock = object->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(object->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", object->formID);
        return;
    }

    TESObjectCELL* pCell = object->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", object->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", pCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = activator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        // spdlog::error("Activator entity not found for form id {:X}", activator->formID);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(*pEntity);
    if (!serverIdRes.has_value())
        return;

    request.ActivatorId = serverIdRes.value();
    request.PreActivationOpenState = acEvent.PreActivationOpenState;

    m_transport.Send(request);
}

void ObjectService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ActivatorId);
    if (!pActor)
    {
        spdlog::error("{}: could not find actor server id {:X}", __FUNCTION__, acMessage.ActivatorId);
        return;
    }

    const uint32_t cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to activate.");
        return;
    }

    if (pObject->baseForm->formType == FormType::Door)
    {
        auto remotePreActivationState = static_cast<TESObjectREFR::OpenState>(acMessage.PreActivationOpenState);
        TESObjectREFR::OpenState localState = pObject->GetOpenState();

        if (remotePreActivationState != localState)
        {
            // The doors are unsynced at this point. If we'll Activate the one on our side
            // it'll just continue to be unsynced (open remotely, closed locally and vice versa)
            return;
        }
    }

    // unsure if these flags are the best, but these are passed with the papyrus Activate fn
    // might be an idea to have the client send the flags through NotifyActivate
    pObject->Activate(pActor, 0, nullptr, 1, 0);
}

void ObjectService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.FormId, request.Id))
    {
        spdlog::error("Server form id for lock object not found, form id: {:X}", acEvent.FormId);
        return;
    }

    const auto* const pObject = Cast<TESObjectREFR>(TESForm::GetById(acEvent.FormId));

    TESObjectCELL* pCell = pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pCell->formID);
        return;
    }

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void ObjectService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId));
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    if(!acMessage.IsLocked)
    {
        if (!pLock || !pLock->IsLocked())
            return;
    }

    if (!pLock && acMessage.IsLocked)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
        {
            spdlog::error("Failed to create lock for object form id {:X}", pObject->formID);
            return;
        }
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void ObjectService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request{};
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;

    m_transport.Send(request);
}

void ObjectService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = Cast<TESObjectREFR>(pForm);

    if (!pObject)
    {
        spdlog::error("Failed to fetch notify script animation object, form id: {:X}", acMessage.FormID);
        return;
    }

    BSFixedString eventName(acMessage.EventName.c_str());
    if (acMessage.Animation == String{})
    {
        pObject->PlayAnimation(&eventName);
    }
    else
    {
        BSFixedString animation(acMessage.Animation.c_str());
        pObject->PlayAnimationAndWait(&animation, &eventName);
    }
}

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>*)
{
#if ENVIRONMENT_DEBUG
    if (acEvent && acEvent->object)
        m_world.GetRunner().QueueForThisWorld([this, formId = acEvent->object->formID] {
            const auto view = m_world.view<ObjectComponent>();
            const auto it = std::find_if(view.begin(), view.end(), [view, formId](auto entity) {
                return view.get<ObjectComponent>(entity).Id == formId;
            });
            if (it == view.end())
                if (auto* object = Cast<TESObjectREFR>(TESForm::GetById(formId)))
                    AddObjectComponent(object);
        });
#endif
    return BSTEventResult::kOk;
}
