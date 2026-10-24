#include <Services/ObjectService.h>

#include <common/RfabModpackCompat.h>
#include <World.h>
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
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/BGSEncounterZone.h>

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
    if (pCell)
    {
        auto* pEncounterZone = static_cast<ExtraEncounterZone*>(pCell->extraData.GetByType(ExtraDataType::EncounterZone));

        // Only return true if cell has the NoResetZone encounter zone
        if (pEncounterZone && pEncounterZone->zone && pEncounterZone->zone->formID == 0xf90b1)
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

bool ShouldSyncObject(const TESObjectREFR* apObject) noexcept
{
    if (!apObject)
        return false;

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
    // TODO(cosideci): clear object components
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

    for (TESObjectREFR* pObject : objects)
    {
        if (!ShouldSyncObject(pObject))
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

        CreateObjectEntity(pObject->formID, objectData.ServerId);

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
            Inventory currentInventory = pObject->GetInventory();

            if (currentInventory.ContainsQuestItems())
                pObject->SetInventoryRetainingQuestItems(currentInventory, objectData.CurrentInventory);
            else
                pObject->SetInventory(objectData.CurrentInventory);
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

namespace
{
bool IsPersonalBlessingActivator(const TESObjectREFR* apObject) noexcept
{
    if (!apObject || !apObject->baseForm || apObject->baseForm->formType != FormType::Activator)
        return false;

    if (const char* rawEdid = apObject->baseForm->GetFormEditorID(); rawEdid && rawEdid[0] != '\0')
    {
        const std::string edid = RfabCompat::Detail::ToLowerAscii(rawEdid);
        if (RfabCompat::Detail::StartsWithIgnoreCase(edid, "rfab_shrine_") ||
            RfabCompat::Detail::StartsWithIgnoreCase(edid, "doomstone") ||
            RfabCompat::Detail::StartsWithIgnoreCase(edid, "dlc2standingstone") ||
            edid == "rfab_ba_universalaltar" ||
            edid == "t01dibellasaltaractivator")
        {
            return true;
        }
    }

    const uint32_t fullFormId = apObject->baseForm->formID;
    const uint32_t modIdx = (fullFormId >> 24) & 0xFFu;
    const uint32_t baseLocalId = fullFormId & 0x00FFFFFFu;

    // Vanilla Skyrim.esm (0x00) & Dragonborn.esm (0x04) personal shrines & standing stones
    if (modIdx == 0x00u &&
        ((baseLocalId >= 0x0D232Du && baseLocalId <= 0x0D2339u) || // Vanilla DoomstoneWarrior..DoomstoneSerpent
         (baseLocalId >= 0x0D987Bu && baseLocalId <= 0x0D9887u) || // Vanilla Shrine Divines
         baseLocalId == 0x06C9E1u))                                // T01DibellasAltarActivator
    {
        return true;
    }

    if (modIdx == 0x04u && (baseLocalId >= 0x01EDA5u && baseLocalId <= 0x01EDB1u)) // Dragonborn All-Maker Stones
        return true;

    // RFAB.esp personal shrines (dynamically resolved for RFAB Respect Edition 0x07, classic 0x06, or any load-order slot)
    if (auto* pModManager = ModManager::Get())
    {
        if (auto* pRfabMod = pModManager->GetByName("RFAB.esp"); pRfabMod && pRfabMod->GetFormId(baseLocalId) == fullFormId)
        {
            switch (baseLocalId)
            {
            case 0x071854u: // RFAB_Shrine_Arkay
            case 0x100780u: // RFAB_Shrine_Talos
            case 0x10E8B0u: // RFAB_Shrine_Nocturnal
            case 0x00C86Bu: // RFAB_Shrine_Auriel
            case 0x039E34u: // RFAB_Shrine_Boethiah
            case 0x03A481u: // RFAB_Shrine_Mephala
            case 0x03A484u: // RFAB_Shrine_Azura
            case 0x0031EDu: // RFAB_Shrine_Malacath
            case 0x0031EEu: // RFAB_Shrine_Peryite
            case 0x0031EFu: // RFAB_Shrine_Sanguine
            case 0x0CE38Fu: // RFAB_Shrine_Dagon
            case 0x239FC5u: // RFAB_BA_UniversalAltar
                return true;
            default:
                break;
            }
        }
    }

    return false;
}
} // namespace

void ObjectService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ActivateFlag)
    {
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
    }

    if (!m_transport.IsConnected())
        return;

    // Personal RFAB Aedra/Daedra shrines, Universal Altars, and Standing Stones are strictly per-character:
    // forwarding Activate to peers triggers RFAB_BA_UniversalShrine.psc / TempleBlessingScript on Game.GetPlayer()
    // or locks the shrine in its cooldown state on peer clients before they can pray!
    if (IsPersonalBlessingActivator(acEvent.pObject))
        return;

    if (Lock* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
    {
        spdlog::error("Server form id not found for object form id {:X}", acEvent.pObject->formID);
        return;
    }

    TESObjectCELL* pCell = acEvent.pObject->GetParentCellEx();
    if (!pCell)
    {
        spdlog::error("Activated object has no parent cell: {:X}", acEvent.pObject->formID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(pCell->formID, request.CellId))
    {
        spdlog::error("Server cell id not found for cell form id {:X}", acEvent.pObject->parentCell->formID);
        return;
    }

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity = std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == id; });

    if (pEntity == std::end(view))
    {
        // spdlog::error("Activator entity not found for form id {:X}", acEvent.pActivator->formID);
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

    if (IsPersonalBlessingActivator(pObject))
        return;

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
        spdlog::error("Server cell id for cell not found, cell form id: {:X}", pObject->parentCell->formID);
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

BSTEventResult ObjectService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if ENVIRONMENT_DEBUG
    auto view = m_world.view<ObjectComponent>();

    const auto itor = std::find_if(std::begin(view), std::end(view), [id = acEvent->object->formID, view](entt::entity entity) { return view.get<ObjectComponent>(entity).Id == id; });

    if (itor == std::end(view))
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}
