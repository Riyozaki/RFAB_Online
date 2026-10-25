#include <Services/ObjectService.h>

#include <GameServer.h>
#include <World.h>
#include <Components.h>

#include <Events/PlayerLeaveCellEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

ObjectService::ObjectService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_leaveCellConnection = aDispatcher.sink<PlayerLeaveCellEvent>().connect<&ObjectService::OnPlayerLeaveCellEvent>(this);
    m_assignObjectConnection = aDispatcher.sink<PacketEvent<AssignObjectsRequest>>().connect<&ObjectService::OnAssignObjectsRequest>(this);
    m_activateConnection = aDispatcher.sink<PacketEvent<ActivateRequest>>().connect<&ObjectService::OnActivate>(this);
    m_lockChangeConnection = aDispatcher.sink<PacketEvent<LockChangeRequest>>().connect<&ObjectService::OnLockChange>(this);
    m_scriptAnimationConnection = aDispatcher.sink<PacketEvent<ScriptAnimationRequest>>().connect<&ObjectService::OnScriptAnimationRequest>(this);
}

// TODO(cosideci): the cell handling of objects need to be revamped.
// We already store the location and worldspace of the mod through CellIdComponent.
// Clients need a message saying the entity was destroyed.
void ObjectService::OnPlayerLeaveCellEvent(const PlayerLeaveCellEvent& acEvent) noexcept
{
    // IMPORTANT: Keep assigned container & door ObjectComponent/InventoryComponent entities persistent
    // across cell transitions for the co-op session!
    // Destroying ObjectComponent entities when stepping across exterior grid boundaries (~58m) or between
    // dungeon sub-cells caused:
    //   1. Infinite chest/boss-chest loot respawning & duplication when re-entering a cell;
    //   2. Unlocked doors & chests re-locking for teammates;
    //   3. Stale ServerId references breaking subsequent container take/put packets.
    (void)acEvent;
}

// NOTE: this whole system kinda relies on all objects in a cell being static.
// This is fine for containers and doors, but if this system is expanded, think of temporaries.
void ObjectService::OnAssignObjectsRequest(const PacketEvent<AssignObjectsRequest>& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();

    AssignObjectsResponse response;

    for (const ObjectData& object : acMessage.Packet.Objects)
    {
        if (!object.Id || object.Id.ModId == 0xFFFFFFFF)
            continue;

        const auto iter = std::find_if(
            std::begin(view), std::end(view),
            [view, id = object.Id](auto entity)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                return formIdComponent.Id == id;
            });

        if (iter != std::end(view))
        {
            if (auto* pCellComp = m_world.try_get<CellIdComponent>(*iter))
            {
                if (!pCellComp->WorldSpaceId)
                {
                    if (object.WorldSpaceId)
                    {
                        pCellComp->WorldSpaceId = object.WorldSpaceId;
                        pCellComp->CenterCoords = object.CurrentCoords;
                    }
                    else if (acMessage.pPlayer && acMessage.pPlayer->GetCellComponent().WorldSpaceId)
                    {
                        pCellComp->WorldSpaceId = acMessage.pPlayer->GetCellComponent().WorldSpaceId;
                        pCellComp->CenterCoords = acMessage.pPlayer->GetCellComponent().CenterCoords;
                    }
                }
            }

            ObjectData objectData;
            objectData.ServerId = World::ToInteger(*iter);

            auto& formIdComponent = view.get<FormIdComponent>(*iter);
            objectData.Id = formIdComponent.Id;

            auto& objectComponent = view.get<ObjectComponent>(*iter);
            objectData.CurrentLockData = objectComponent.CurrentLockData;

            auto& inventoryComponent = view.get<InventoryComponent>(*iter);
            objectData.CurrentInventory = inventoryComponent.Content;

            objectData.IsSenderFirst = false;

            response.Objects.push_back(objectData);
        }
        else
        {
            const auto cEntity = m_world.create();

            m_world.emplace<FormIdComponent>(cEntity, object.Id);

            auto& objectComponent = m_world.emplace<ObjectComponent>(cEntity, acMessage.pPlayer);
            objectComponent.CurrentLockData = object.CurrentLockData;

            GameId worldSpaceId = object.WorldSpaceId;
            GridCellCoords coords = object.CurrentCoords;
            if (!worldSpaceId && acMessage.pPlayer && acMessage.pPlayer->GetCellComponent().WorldSpaceId)
            {
                worldSpaceId = acMessage.pPlayer->GetCellComponent().WorldSpaceId;
                coords = acMessage.pPlayer->GetCellComponent().CenterCoords;
            }

            m_world.emplace<CellIdComponent>(cEntity, object.CellId, worldSpaceId, coords);
            auto& inventoryComp = m_world.emplace<InventoryComponent>(cEntity);
            inventoryComp.Content = object.CurrentInventory;

            ObjectData objectData;
            objectData.Id = object.Id;
            objectData.ServerId = World::ToInteger(cEntity);
            objectData.IsSenderFirst = true;

            response.Objects.push_back(objectData);
        }
    }

    if (!response.Objects.empty())
        acMessage.pPlayer->Send(response);
}

void ObjectService::OnActivate(const PacketEvent<ActivateRequest>& acMessage) const noexcept
{
    NotifyActivate notifyActivate;
    notifyActivate.Id = acMessage.Packet.Id;
    notifyActivate.ActivatorId = acMessage.Packet.ActivatorId;
    notifyActivate.PreActivationOpenState = acMessage.Packet.PreActivationOpenState;

    for (auto pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer || pPlayer == acMessage.pPlayer)
            continue;

        const bool inRange = acMessage.pPlayer
            ? (pPlayer->GetCellComponent().IsInRange(acMessage.pPlayer->GetCellComponent(), false) ||
               (acMessage.Packet.CellId && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId))
            : (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId);

        if (inRange)
            pPlayer->Send(notifyActivate);
    }
}

void ObjectService::OnLockChange(const PacketEvent<LockChangeRequest>& acMessage) const noexcept
{
    NotifyLockChange notifyLockChange;
    notifyLockChange.Id = acMessage.Packet.Id;
    notifyLockChange.IsLocked = acMessage.Packet.IsLocked;
    notifyLockChange.LockLevel = acMessage.Packet.LockLevel;

    auto objectView = m_world.view<FormIdComponent, ObjectComponent>();

    const auto iter = std::find_if(
        std::begin(objectView), std::end(objectView),
        [objectView, id = acMessage.Packet.Id](auto entity)
        {
            const auto& formIdComponent = objectView.get<FormIdComponent>(entity);
            return formIdComponent.Id == id;
        });

    if (iter != std::end(objectView))
    {
        auto& objectComponent = objectView.get<ObjectComponent>(*iter);
        objectComponent.CurrentLockData.IsLocked = acMessage.Packet.IsLocked;
        objectComponent.CurrentLockData.LockLevel = acMessage.Packet.LockLevel;
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer || pPlayer == acMessage.pPlayer)
            continue;

        const bool inRange = acMessage.pPlayer
            ? (pPlayer->GetCellComponent().IsInRange(acMessage.pPlayer->GetCellComponent(), false) ||
               (acMessage.Packet.CellId && pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId))
            : (pPlayer->GetCellComponent().Cell == acMessage.Packet.CellId);

        if (inRange)
            pPlayer->Send(notifyLockChange);
    }
}

void ObjectService::OnScriptAnimationRequest(const PacketEvent<ScriptAnimationRequest>& acMessage) noexcept
{
    auto& packet = acMessage.Packet;
    if (packet.FormID == 0 || (packet.Animation.empty() && packet.EventName.empty()))
        return;

    NotifyScriptAnimation message{};
    message.FormID = packet.FormID;
    message.Animation = packet.Animation;
    message.EventName = packet.EventName;

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer || pPlayer == acMessage.pPlayer)
            continue;

        if (acMessage.pPlayer && !pPlayer->GetCellComponent().IsInRange(acMessage.pPlayer->GetCellComponent(), false))
            continue;

        pPlayer->Send(message);
    }
}
