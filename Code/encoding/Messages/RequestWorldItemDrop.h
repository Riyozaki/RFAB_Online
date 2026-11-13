#pragma once

#include "Message.h"

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

#include <glm/vec3.hpp>

/**
 * Client -> server: an actor this client simulates dropped an item and the local engine created the
 * reference (architecture step 4, common/Session/WorldItems.h). Replaces RequestInventoryChanges
 * with Drop=true while Session::Feature::kServerOwnedWorldItems is on: the server removes the item
 * from the actor's inventory AND registers the dropped item, in one message.
 */
struct RequestWorldItemDrop final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldItemDrop;

    RequestWorldItemDrop()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWorldItemDrop& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Cookie == acRhs.Cookie && ActorServerId == acRhs.ActorServerId &&
               Item == acRhs.Item && CellId == acRhs.CellId && WorldSpaceId == acRhs.WorldSpaceId && Position == acRhs.Position &&
               Rotation == acRhs.Rotation;
    }

    uint32_t Cookie{};         // client-chosen, echoed in NotifyWorldItemSpawn::State.DropperCookie
    uint32_t ActorServerId{};  // the dropping actor (player or an NPC this client owns)
    Inventory::Entry Item{};   // Item.Count = how many were dropped (> 0)
    GameId CellId{};
    GameId WorldSpaceId{};
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation{0.0f, 0.0f, 0.0f};
};
