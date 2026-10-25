#include "InventoryService.h"

#include <Components.h>
#include <World.h>
#include <GameServer.h>
#include <CoopPacketSafety.h>

#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>

#include <Setting.h>
namespace
{
Console::Setting bEnableItemDrops{"Gameplay:bEnableItemDrops", "Syncs dropped items in the 3D world across players", true};
}

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_inventoryChangeConnection = aDispatcher.sink<PacketEvent<RequestInventoryChanges>>().connect<&InventoryService::OnInventoryChanges>(this);
    m_equipmentChangeConnection = aDispatcher.sink<PacketEvent<RequestEquipmentChanges>>().connect<&InventoryService::OnEquipmentChanges>(this);
    m_drawWeaponConnection = aDispatcher.sink<PacketEvent<DrawWeaponRequest>>().connect<&InventoryService::OnWeaponDrawnRequest>(this);
}

void InventoryService::OnInventoryChanges(const PacketEvent<RequestInventoryChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cOrigin))
        return;

    if (!message.Item.BaseId || message.Item.Count == 0)
        return;

    // Strict player inventory ownership isolation: a peer client's remote puppet scripts must NEVER
    // add or remove items from another player's character inventory!
    if (const auto* pChar = m_world.try_get<CharacterComponent>(cOrigin))
    {
        if (pChar->IsPlayer())
        {
            const auto* pOwner = m_world.try_get<OwnerComponent>(cOrigin);
            if (!pOwner || pOwner->GetOwner() != acMessage.pPlayer)
            {
                spdlog::warn(
                    "Rejected cross-client inventory mutation on player entity {:X} from sender {:X}",
                    message.ServerId,
                    acMessage.pPlayer ? acMessage.pPlayer->GetId() : 0);
                return;
            }
        }
    }

    Inventory::Entry sanitizedItem = message.Item;
    sanitizedItem.Count = CoopPacketSafety::SanitizeItemCountDelta(sanitizedItem.Count);
    sanitizedItem.ExtraCharge = CoopPacketSafety::SanitizeFiniteFloat(sanitizedItem.ExtraCharge, 0.0f, 100000.0f, 0.0f);
    sanitizedItem.ExtraHealth = CoopPacketSafety::SanitizeFiniteFloat(sanitizedItem.ExtraHealth, 0.0f, 100000.0f, 0.0f);

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(cOrigin);

    if (it != view.end())
    {
        auto& inventoryComponent = view.get<InventoryComponent>(*it);
        inventoryComponent.Content.AddOrRemoveEntry(sanitizedItem);
    }

    if (!message.UpdateClients)
        return;

    NotifyInventoryChanges notify{};
    notify.ServerId = message.ServerId;
    notify.Item = std::move(sanitizedItem);

    notify.Drop = bEnableItemDrops ? message.Drop : false;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnEquipmentChanges(const PacketEvent<RequestEquipmentChanges>& acMessage) noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cOrigin = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cOrigin))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cOrigin))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
        {
            spdlog::debug("Ignored non-owner RequestEquipmentChanges for {:X}", message.ServerId);
            return;
        }
    }

    auto view = m_world.view<InventoryComponent>();

    const auto it = view.find(cOrigin);

    if (it != view.end())
    {
        auto& inventoryComponent = view.get<InventoryComponent>(*it);
        inventoryComponent.Content.UpdateEquipment(message.CurrentInventory);
    }

    NotifyEquipmentChanges notify;
    notify.ServerId = message.ServerId;
    notify.ItemId = message.ItemId;
    notify.EquipSlotId = message.EquipSlotId;
    notify.Count = message.Count;
    notify.Unequip = message.Unequip;
    notify.IsSpell = message.IsSpell;
    notify.IsShout = message.IsShout;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cOrigin, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void InventoryService::OnWeaponDrawnRequest(const PacketEvent<DrawWeaponRequest>& acMessage) noexcept
{
    auto& message = acMessage.Packet;

    auto characterView = m_world.view<CharacterComponent, OwnerComponent>();
    const auto it = characterView.find(static_cast<entt::entity>(message.Id));

    if (it != std::end(characterView) && characterView.get<OwnerComponent>(*it).GetOwner() == acMessage.pPlayer)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(*it);
        characterComponent.SetWeaponDrawn(message.IsWeaponDrawn);
        spdlog::debug("Updating weapon drawn state {:x}:{}", message.Id, message.IsWeaponDrawn);
    }
}
