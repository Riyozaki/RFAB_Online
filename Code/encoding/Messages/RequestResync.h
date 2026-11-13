#pragma once

#include "Message.h"

#include <common/Session/Resync.h>

/**
 * Client -> server: "I lost state, send it again" (architecture step 1, see common/Session/Resync.h).
 * The server answers with authoritative snapshots followed by NotifyResyncComplete(RequestId).
 */
struct RequestResync final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kRequestResync;

    RequestResync()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const RequestResync& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RequestId == acRhs.RequestId && RequestScope == acRhs.RequestScope &&
               ServerId == acRhs.ServerId && GapReason == acRhs.GapReason && RawScopeKnown == acRhs.RawScopeKnown;
    }

    uint32_t RequestId{};
    Session::Resync::Scope RequestScope{Session::Resync::Scope::kEntity};
    uint32_t ServerId{Session::Resync::kNoServerId};  // only meaningful for Scope::kEntity; 0 is a valid id
    Session::Resync::Reason GapReason{Session::Resync::Reason::kUnknown};
    // False when the scope byte on the wire was not a known value: the server answers kInvalid
    // instead of guessing what was meant.
    bool RawScopeKnown{true};
};
