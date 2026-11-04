#pragma once
// See steam/steamnetworkingsockets.h. `GameServer`'s constructor tunes two socket timeouts
// through this interface; with no interface the tuning is skipped, which is what a build
// that never opens a socket wants.
#include "steamnetworkingsockets.h"

struct ISteamNetworkingUtils
{
    virtual ~ISteamNetworkingUtils() = default;
    virtual bool SetGlobalConfigValueInt32(ESteamNetworkingConfigValue, int32_t) { return false; }
    virtual bool SetGlobalConfigValuePtr(ESteamNetworkingConfigValue, void*) { return false; }
};

inline ISteamNetworkingUtils* SteamNetworkingUtils() { return nullptr; }
