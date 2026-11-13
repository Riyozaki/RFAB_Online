#pragma once

#include "Message.h"

#include <Structs/WorldItemState.h>

/**
 * Server -> one player (join, Session::Resync::Scope::kWorldItems) or every joined player (checkpoint
 * rollback): the complete list of server-owned dropped items at registry Revision. Authoritative: the
 * client removes every item that is not listed (common/Session/WorldItems.h, ClientMirror::ApplySnapshot).
 */
struct NotifyWorldItemSnapshot final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyWorldItemSnapshot;

    NotifyWorldItemSnapshot()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyWorldItemSnapshot& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && Revision == acRhs.Revision && Items == acRhs.Items;
    }

    uint32_t Revision{};
    Vector<WorldItemState> Items{};
};
