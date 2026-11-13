#pragma once

#include <Structs/GameId.h>
#include <Structs/Inventory.h>

#include <glm/vec3.hpp>

/**
 * One server-owned dropped item as it travels on the wire (architecture step 4,
 * common/Session/WorldItems.h). Shared by NotifyWorldItemSpawn and NotifyWorldItemSnapshot so a
 * single-item upsert and the full list can never disagree on the layout.
 */
struct WorldItemState
{
    bool operator==(const WorldItemState& acRhs) const noexcept;
    bool operator!=(const WorldItemState& acRhs) const noexcept { return !operator==(acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    // Never produces a non-finite position/rotation or a negative count, whatever the bytes say.
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    uint32_t Id{};             // WorldItems::ItemId, never 0 for a real item
    Inventory::Entry Item{};   // Item.Count is the current stack size
    GameId CellId{};
    GameId WorldSpaceId{};     // zero for interiors
    // Full floats, not Vector3_NetQuantize: truncating to whole units can sink a resting item into
    // the floor it lies on.
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Rotation{0.0f, 0.0f, 0.0f};  // radians
    uint32_t DropperPlayerId{};
    uint32_t DropperCookie{};  // echoes RequestWorldItemDrop::Cookie so the dropper adopts its own reference
};
