#pragma once

// Client side of common/Diag/Breadcrumbs.h: stamps a crumb with the tick count and the thread id.
// Every client translation unit has <windows.h> through TiltedOnlinePCH.h.
//
//   RFAB_HOOK_CRUMB("HookDamageActor", apThis ? apThis->formID : 0);   // a hook: subject, other party
//   RFAB_EVENT_CRUMB("remove-character", serverId);                    // session, world, remote actors
//   RFAB_PACKET_CRUMB("packet-in", opcode, size);                      // transport only
//
// Repeats with the same name and first value collapse into one row, so a hook that runs for every
// actor every frame passes the actor as the SECOND value (first value 0).
//
// The first argument MUST be a string literal: the crash writer reads it after the fact.

#include <common/Diag/Breadcrumbs.h>

namespace Diag
{
inline void LeaveHookCrumb(const char* apLiteral, uint32_t aA = 0, uint32_t aB = 0) noexcept
{
    g_hookCrumbs.Note(apLiteral, aA, aB, ::GetTickCount64(), ::GetCurrentThreadId());
}

inline void LeaveEventCrumb(const char* apLiteral, uint32_t aA = 0, uint32_t aB = 0) noexcept
{
    g_eventCrumbs.Note(apLiteral, aA, aB, ::GetTickCount64(), ::GetCurrentThreadId());
}

inline void LeavePacketCrumb(const char* apLiteral, uint32_t aOpcode, uint32_t aB = 0) noexcept
{
    g_packetCrumbs.Note(apLiteral, aOpcode, aB, ::GetTickCount64(), ::GetCurrentThreadId());
}
} // namespace Diag

#define RFAB_HOOK_CRUMB(...) ::Diag::LeaveHookCrumb(__VA_ARGS__)
#define RFAB_EVENT_CRUMB(...) ::Diag::LeaveEventCrumb(__VA_ARGS__)
#define RFAB_PACKET_CRUMB(...) ::Diag::LeavePacketCrumb(__VA_ARGS__)
