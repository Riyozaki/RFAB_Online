#pragma once

#include <cstdint>

// Shared actor-value indices.
//
// The client keeps the authoritative enum in
// `Code/client/Games/Skyrim/Forms/ActorValueInfo.h`; the server cannot include
// that header (it is Skyrim-specific and pulls in game headers), so it used to
// hardcode `24` in a few places. Keep the two in sync - the client header has a
// static_assert on kHealth that points back here.
namespace Gameplay::ActorValue
{
inline constexpr uint32_t kHealth = 24;
inline constexpr uint32_t kMagicka = 25;
inline constexpr uint32_t kStamina = 26;
} // namespace Gameplay::ActorValue
