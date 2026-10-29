#include "Events/CharacterInteriorCellChangeEvent.h"
#include "Events/CharacterExteriorCellChangeEvent.h"
#include "Events/PlayerLeaveCellEvent.h"

#include <algorithm>
#include <cmath>

#include <Services/PlayerService.h>
#include <Services/CharacterService.h>
#include <GameServer.h>

#include <Messages/ShiftGridCellRequest.h>
#include <Messages/EnterExteriorCellRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/PlayerRespawnRequest.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/NotifyPlayerRespawn.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/PlayerLevelRequest.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerCellChanged.h>

#include <Setting.h>
namespace
{
Console::Setting fGoldLossFactor{"Gameplay:fGoldLossFactor", "Factor of the amount of gold lost on death", 0.0f};
}

PlayerService::PlayerService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_interiorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterInteriorCellRequest>>().connect<&PlayerService::HandleInteriorCellEnter>(this))
    , m_gridCellShiftConnection(aDispatcher.sink<PacketEvent<ShiftGridCellRequest>>().connect<&PlayerService::HandleGridCellShift>(this))
    , m_exteriorCellEnterConnection(aDispatcher.sink<PacketEvent<EnterExteriorCellRequest>>().connect<&PlayerService::HandleExteriorCellEnter>(this))
    , m_playerRespawnConnection(aDispatcher.sink<PacketEvent<PlayerRespawnRequest>>().connect<&PlayerService::OnPlayerRespawnRequest>(this))
    , m_playerLevelConnection(aDispatcher.sink<PacketEvent<PlayerLevelRequest>>().connect<&PlayerService::OnPlayerLevelRequest>(this))
{
}

void SendPlayerCellChanged(const Player* apPlayer) noexcept
{
    if (!apPlayer)
        return;

    auto& cellComponent = apPlayer->GetCellComponent();

    NotifyPlayerCellChanged notify{};
    notify.PlayerId = apPlayer->GetId();
    notify.WorldSpaceId = cellComponent.WorldSpaceId;
    notify.CellId = cellComponent.Cell;

    GameServer::Get()->SendToPlayers(notify, apPlayer);
}

void PlayerService::HandleGridCellShift(const PacketEvent<ShiftGridCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    auto& message = acMessage.Packet;

    const CellIdComponent oldCellComponent = pPlayer->GetCellComponent();
    const GameId oldCell = oldCellComponent.Cell;

    const CellIdComponent cell{message.PlayerCell, message.WorldSpaceId, message.CenterCoords};
    pPlayer->SetCellComponent(cell);
    if (pPlayer->GetCharacter())
    {
        if (auto* pCharCell = m_world.try_get<CellIdComponent>(*pPlayer->GetCharacter()))
        {
            pCharCell->Cell = message.PlayerCell;
            pCharCell->WorldSpaceId = message.WorldSpaceId;
            if (!pCharCell->IsValid() || oldCellComponent.WorldSpaceId != message.WorldSpaceId)
                pCharCell->CenterCoords = message.CenterCoords;
        }
    }

    m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));

    m_world.GetCharacterService().ReconcileAllEntitiesForObserver(pPlayer);
}

void PlayerService::HandleExteriorCellEnter(const PacketEvent<EnterExteriorCellRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    auto* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    const CellIdComponent prevObserverCell = pPlayer->GetCellComponent();
    const GridCellCoords observerGridCenter =
        (prevObserverCell.IsValid() && prevObserverCell.WorldSpaceId == message.WorldSpaceId)
            ? prevObserverCell.CenterCoords
            : message.CurrentCoords;

    pPlayer->SetCellComponent(CellIdComponent{message.CellId, message.WorldSpaceId, observerGridCenter});

    if (pPlayer->GetCharacter())
    {
        const auto entity = *pPlayer->GetCharacter();
        if (auto* pCharCell = m_world.try_get<CellIdComponent>(entity))
            *pCharCell = CellIdComponent{message.CellId, message.WorldSpaceId, message.CurrentCoords};

        if (prevObserverCell.IsValid())
        {
            m_world.GetDispatcher().trigger(CharacterExteriorCellChangeEvent{pPlayer, entity, message.WorldSpaceId, message.CurrentCoords});
        }
    }

    m_world.GetCharacterService().ReconcileAllEntitiesForObserver(pPlayer);
    SendPlayerCellChanged(pPlayer);
}

void PlayerService::HandleInteriorCellEnter(const PacketEvent<EnterInteriorCellRequest>& acMessage) const noexcept
{
    auto* pPlayer = acMessage.pPlayer;
    if (!pPlayer)
        return;

    auto& message = acMessage.Packet;

    const auto oldCell = pPlayer->GetCellComponent().Cell;

    const CellIdComponent cell{message.CellId, {}, {}};
    pPlayer->SetCellComponent(cell);

    m_world.GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));

    if (pPlayer->GetCharacter())
    {
        const auto entity = *pPlayer->GetCharacter();

        if (auto* pCellIdComponent = m_world.try_get<CellIdComponent>(entity))
        {
            *pCellIdComponent = cell;
            m_world.GetDispatcher().trigger(CharacterInteriorCellChangeEvent{pPlayer, entity, message.CellId});
        }
    }

    m_world.GetCharacterService().ReconcileAllEntitiesForObserver(pPlayer);
    SendPlayerCellChanged(pPlayer);
}

void PlayerService::OnPlayerRespawnRequest(const PacketEvent<PlayerRespawnRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    const float rawGoldLossFactor = fGoldLossFactor.as_float();
    const float goldLossFactor = std::isfinite(rawGoldLossFactor)
        ? std::clamp(rawGoldLossFactor, 0.0f, 1.0f)
        : 0.0f;

    auto character = acMessage.pPlayer->GetCharacter();
    if (!character || !m_world.valid(*character))
        return;

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(static_cast<entt::entity>(*character));

    if (it != view.end())
    {
        if (goldLossFactor > 0.0f)
        {
            auto& inventoryComponent = view.get<InventoryComponent>(*it);

            GameId goldId(0, 0xF);
            int32_t goldCount = std::max<int32_t>(0, inventoryComponent.Content.GetEntryCountById(goldId));
            int32_t goldToRemove = std::clamp<int32_t>(
                static_cast<int32_t>(static_cast<double>(goldCount) * static_cast<double>(goldLossFactor)),
                0,
                goldCount);

            if (goldToRemove > 0)
            {
                Inventory::Entry entry{};
                entry.BaseId = goldId;
                entry.Count = -goldToRemove;

                inventoryComponent.Content.AddOrRemoveEntry(entry);

                NotifyInventoryChanges notifyInventoryChanges{};
                notifyInventoryChanges.ServerId = World::ToInteger(*character);
                notifyInventoryChanges.Item = entry;
                notifyInventoryChanges.Drop = false;

                // Exclude respawned player from inventory changes notification...
                if (!GameServer::Get()->SendToPlayersInRange(notifyInventoryChanges, *character, acMessage.GetSender()))
                    spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
            }

            // ...and instead, send NotifyPlayerRespawn so that the client can print a message.
            NotifyPlayerRespawn notifyPlayerRespawn{};
            notifyPlayerRespawn.GoldLost = goldToRemove;

            acMessage.pPlayer->Send(notifyPlayerRespawn);
        }

        // Let all other players in cell respawn this player, since the body state seems to be bugged otherwise
        NotifyRespawn notifyRespawn{};
        notifyRespawn.ActorId = World::ToInteger(*character);

        if (!GameServer::Get()->SendToPlayersInRange(notifyRespawn, *character, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
}

void PlayerService::OnPlayerLevelRequest(const PacketEvent<PlayerLevelRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer || acMessage.Packet.NewLevel == 0 || acMessage.Packet.NewLevel > 10000)
        return;

    acMessage.pPlayer->SetLevel(acMessage.Packet.NewLevel);

    NotifyPlayerLevel notify{};
    notify.PlayerId = acMessage.pPlayer->GetId();
    notify.NewLevel = acMessage.Packet.NewLevel;

    GameServer::Get()->SendToPlayers(notify, acMessage.pPlayer);
}
