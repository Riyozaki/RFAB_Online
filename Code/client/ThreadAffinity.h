#pragma once

// Thread-affinity contract of the client (architecture step 3).
//
// Engine hooks, Papyrus natives, script-event sinks and CEF callbacks run on engine threads. They may
// touch client state only through:
//   * lock-free snapshots: World::*Cached(), World::IsFeatureEnabled, Session::Lifecycle::*AnyThread();
//   * member functions declared RFAB_ANY_THREAD below;
//   * a post to the main thread: World::GetRunner().Queue/Trigger (gameplay, session-scoped) or
//     World::QueueMainThreadTask (UI/control, runs while menus pause the simulation).
// Code inside `if (World::IsMainThread() && ...) { ... }` and the rest of a function after
// `if (!World::IsMainThread()) { <post>; return; }` is main-thread code.
//
// Tools/rfab/check_hook_discipline.py enforces this on every TP_HOOK detour, PAPYRUS_FUNCTION,
// BSTEventSink::OnEvent and OverlayClient CEF handler, and checks the bodies of RFAB_ANY_THREAD
// functions too. It runs in the Linux CI job before anything is compiled.

#include <atomic>

// Declaration marker: this member function may be called from any thread. It reads atomics or state
// behind its own mutex, or marshals itself onto the main thread. Expands to nothing.
#define RFAB_ANY_THREAD

namespace ThreadAffinity
{
// Logs (once per call site, see the macro) when a synchronous engine gate that must answer on the
// spot - and therefore cannot post - runs off the main thread. Such gates are main-thread by engine
// contract (the save/load manager processes its queue in the main loop); the log turns a wrong
// assumption into a line in the playtest log instead of a silent data race. Defined in World.cpp.
void ReportIfNotMainThread(const char* apWhere, std::atomic<bool>& aReported) noexcept;
} // namespace ThreadAffinity

#define RFAB_EXPECT_MAIN_THREAD(acpWhere)                                       \
    do                                                                          \
    {                                                                           \
        static std::atomic<bool> s_rfabOffMainThreadReported{false};            \
        ::ThreadAffinity::ReportIfNotMainThread(acpWhere, s_rfabOffMainThreadReported); \
    } while (0)
