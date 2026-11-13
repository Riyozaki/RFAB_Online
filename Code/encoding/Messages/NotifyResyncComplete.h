#pragma once

#include "Message.h"

#include <common/Session/Resync.h>

/**
 * Server -> client: terminator of one RequestResync. Sent AFTER every snapshot of the answer on the
 * same reliable ordered channel, so when the client sees it the resent state has been applied.
 */
struct NotifyResyncComplete final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyResyncComplete;

    NotifyResyncComplete()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyResyncComplete& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() && RequestId == acRhs.RequestId && RequestScope == acRhs.RequestScope &&
               ServerId == acRhs.ServerId && Result == acRhs.Result && SnapshotsSent == acRhs.SnapshotsSent;
    }

    uint32_t RequestId{};
    Session::Resync::Scope RequestScope{Session::Resync::Scope::kEntity};
    uint32_t ServerId{Session::Resync::kNoServerId};  // only meaningful for Scope::kEntity; 0 is a valid id
    Session::Resync::Outcome Result{Session::Resync::Outcome::kApplied};
    uint32_t SnapshotsSent{};  // diagnostics only
};
