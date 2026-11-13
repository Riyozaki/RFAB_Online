#pragma once

#include "Message.h"

#include <common/Session/WorldItems.h>

/**
 * Server -> every joined player: a server-owned dropped item left the world; every client deletes the
 * reference bound to ItemId - by id, nobody guesses. Also sent to a single claimer whose item the
 * server does not have (Reason kClaimRejected, Revision not advanced): ids are never reused, so a
 * despawn is applied whatever its revision.
 */
struct NotifyWorldItemDespawn final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorldItemDespawn;

    NotifyWorldItemDespawn()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWorldItemDespawn& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Revision == acRhs.Revision && ItemId == acRhs.ItemId &&
               Reason == acRhs.Reason && PickerPlayerId == acRhs.PickerPlayerId;
    }

    uint32_t Revision{};
    uint32_t ItemId{};
    WorldItems::DespawnReason Reason{WorldItems::DespawnReason::kPickedUp};
    uint32_t PickerPlayerId{};  // who took it (diagnostics); 0 when not picked up
};
