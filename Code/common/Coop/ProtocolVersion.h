#pragma once
#include <BuildInfo.h>
// Bump whenever any pre-negotiation/actor/inventory layout changes. Matching a dev semantic
// version alone is insufficient: an older dev binary must fail authentication before new codecs.
#define RFAB_NETWORK_BUILD BUILD_COMMIT "+coop-wire-5"
