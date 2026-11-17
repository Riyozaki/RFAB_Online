#pragma once
#include <atomic>
#include <windows.h>
#include <spdlog/spdlog.h>

// Annotation inspected by Tools/rfab/check_hook_discipline.py. It is a contract, not a lock.
#define RFAB_ANY_THREAD
namespace ThreadAffinity
{
inline std::atomic<DWORD> MainThread{};
inline void BindMainThread() noexcept
{
    DWORD empty{};
    MainThread.compare_exchange_strong(empty, GetCurrentThreadId(), std::memory_order_release);
}
inline bool IsMainThread() noexcept
{
    const auto id = MainThread.load(std::memory_order_acquire);
    return id && id == GetCurrentThreadId();
}
inline bool ExpectMainThread(const char* aOperation) noexcept
{
    if (IsMainThread())
        return true;
    spdlog::error("[RFAB] refused off-main-thread engine/ECS operation: {}", aOperation);
    return false;
}
} // namespace ThreadAffinity
#define RFAB_EXPECT_MAIN_THREAD() ThreadAffinity::ExpectMainThread(__func__)
