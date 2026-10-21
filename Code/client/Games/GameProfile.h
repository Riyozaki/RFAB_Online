#pragma once

// Build profiles for the two game layouts the client can target.
//
// SkyrimSE 1.5.97 and the AE builds up to 1.6.353 share one memory layout; the
// "+0x8" shift that the AE sources of this fork assume appeared in 1.6.629.
// See docs/phase1/LAYOUTS_RU.md for the full list of differing members.
//
//   TP_GAME_PROFILE_1600 (default) - 1.6.629+ layout, what upstream targets
//   TP_GAME_PROFILE_1597          - 1.5.97 layout, what RFAB needs
//
// Structure members are always declared in game order; a profile only changes
// the explicit padding and the static_assert expectations, so both profiles
// stay readable side by side.
#if defined(TP_GAME_PROFILE_1597)
#define TP_PROFILE_IS_1597 1
#else
#define TP_PROFILE_IS_1597 0
#endif

// Selects the value that matches the active profile. Both arguments must be
// compile time constants; used mostly inside static_assert.
#define TP_LAYOUT(se, ae) (TP_PROFILE_IS_1597 ? (se) : (ae))

// Sixteen AE members have a different offset in 1.5.97. Anything not listed in
// docs/phase1/LAYOUTS_RU.md has the same offset in both profiles.
#define TP_STATIC_ASSERT_OFFSET(Type, member, se, ae)                                                                                \
    static_assert(offsetof(Type, member) == TP_LAYOUT(se, ae), "unexpected offset of " #Type "::" #member " for this game profile")

#define TP_STATIC_ASSERT_SIZE(Type, se, ae)                                                                                          \
    static_assert(sizeof(Type) == TP_LAYOUT(se, ae), "unexpected size of " #Type " for this game profile")
