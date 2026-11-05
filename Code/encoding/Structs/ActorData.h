#pragma once

#include <Structs/ActorValues.h>
#include <Structs/GameId.h>
#include <Structs/Inventory.h>

struct ActorData
{
    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    bool operator==(const ActorData& acRhs) const noexcept
    {
        return InitialActorValues == acRhs.InitialActorValues &&
               InitialInventory == acRhs.InitialInventory && BaseId == acRhs.BaseId &&
               IsDead == acRhs.IsDead && IsWeaponDrawn == acRhs.IsWeaponDrawn;
    }

    ActorValues InitialActorValues{};
    Inventory InitialInventory{};
    GameId BaseId{};
    bool IsDead{};
    bool IsWeaponDrawn{};
};
