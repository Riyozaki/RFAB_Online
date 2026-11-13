#pragma once

#include "Message.h"

#include <Structs/Inventory.h>

/**
 * Client -> server: an actor this client simulates picked up a reference that shows a server-owned
 * item (architecture step 4). It is a claim: the server grants the item to the first claimer it
 * processes and rolls back the optimistic local pickup of anybody later
 * (common/Session/WorldItems.h, Registry::TryClaim).
 */
struct RequestWorldItemPickup final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestWorldItemPickup;

    RequestWorldItemPickup()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestWorldItemPickup& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && ItemId == acRhs.ItemId && PickerServerId == acRhs.PickerServerId &&
               Item == acRhs.Item;
    }

    uint32_t ItemId{};          // WorldItems::ItemId
    uint32_t PickerServerId{};  // the picking actor (player or an NPC this client owns)
    // What the local engine put into the picker's inventory; Item.Count = how many. The server grants
    // from its own record and uses this only to undo what it does not grant.
    Inventory::Entry Item{};
};
