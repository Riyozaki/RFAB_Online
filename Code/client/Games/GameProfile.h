#pragma once

// Build profiles for the two game layouts the client can target.
//
// SkyrimSE 1.5.97 and the AE builds up to 1.6.353 share one memory layout; the
// "+0x8" shift that the AE sources of this fork assume appeared in 1.6.629.
// See docs/phase1/LAYOUTS_RU.md for the full list of differing members.
//
//   TP_GAME_PROFILE_1597 (default) - 1.5.97 layout, what RFAB needs
//   TP_GAME_PROFILE_1600           - 1.6.629+ layout, what upstream targets
//
// The fork targets 1.5.97 only (docs/BUILD_RU.md); the 1600 profile is kept so
// the upstream layout stays compilable and the difference between the two stays
// visible in one file, but it is not a supported configuration.
//
// Structure members are always declared in game order; a profile only changes
// the explicit padding and the static_assert expectations, so both profiles
// stay readable side by side.
#if defined(TP_GAME_PROFILE_1597) && defined(TP_GAME_PROFILE_1600)
#error "select exactly one game profile: TP_GAME_PROFILE_1597 or TP_GAME_PROFILE_1600"
#elif !defined(TP_GAME_PROFILE_1597) && !defined(TP_GAME_PROFILE_1600)
#error "no game profile selected: define TP_GAME_PROFILE_1597 (1.5.97) or TP_GAME_PROFILE_1600 (1.6.629+)"
#endif

#if defined(TP_GAME_PROFILE_1597)
#define TP_PROFILE_IS_1597 1
// Checked against the real executable at startup (Code/client/main.cpp); the
// build refuses to run on a game whose version does not match this profile.
#define TP_TARGET_GAME_VERSION_MAJOR 1
#define TP_TARGET_GAME_VERSION_MINOR 5
#define TP_TARGET_GAME_VERSION_REVISION 97
#define TP_TARGET_GAME_VERSION "1.5.97.0"
#define TP_TARGET_GAME_VERSION_W L"1.5.97.0"
#define TP_TARGET_PROFILE_NAME "1.5.97"
#else
#define TP_PROFILE_IS_1597 0
#define TP_TARGET_GAME_VERSION_MAJOR 1
#define TP_TARGET_GAME_VERSION_MINOR 6
#define TP_TARGET_GAME_VERSION_REVISION 629
#define TP_TARGET_GAME_VERSION "1.6.629+"
#define TP_TARGET_GAME_VERSION_W L"1.6.629+"
#define TP_TARGET_PROFILE_NAME "1.6.629+"
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
