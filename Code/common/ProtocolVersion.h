#pragma once

#include <cstdint>

// Wire-protocol revision of RFAB Online.
//
// Bump this whenever an opcode is added/removed/reordered or a message layout
// changes in a way that cannot be parsed by the other side. It is *not* a
// substitute for the build-commit check (which still rejects any mismatch), it
// exists so both sides can state what they speak and so the rejection message
// is actionable.
//
// History:
//   1 - initial RFAB Online fork: ServerSettings gained HealthMultiplier,
//       client opcode  SyncRfabExperienceRequest  added,
//       server opcode  NotifySyncRfabExperience    added.
//   2 - SyncRfabExperienceRequest gained Sequence (replay/reconnect protection)
//       and shares experience server-wide instead of party-only.
//
// A wrong number is never silently accepted: the server still rejects a client
// whose BUILD_COMMIT differs (GameServer::HandleAuthenticationRequest), and both
// sides log the protocol version at startup so a mismatched pair is obvious.
namespace Protocol
{
inline constexpr uint32_t kVersion = 2;

} // namespace Protocol
