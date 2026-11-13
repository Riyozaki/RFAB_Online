#pragma once

#include "Message.h"

#include <Structs/WorldItemState.h>

/**
 * Server -> every joined player: a server-owned dropped item appeared or its stack changed (upsert by
 * State.Id). Revision is the registry revision after the change; a client that sees a jump asks for
 * a snapshot (Session::Resync::Scope::kWorldItems). Survives a world reload: it is session state the
 * server never re-sends on its own (CoopWorldStabilizer::MustSurviveWorldReload).
 */
struct NotifyWorldItemSpawn final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorldItemSpawn;

    NotifyWorldItemSpawn()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWorldItemSpawn& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Revision == acRhs.Revision && State == acRhs.State;
    }

    uint32_t Revision{};
    WorldItemState State{};
};
