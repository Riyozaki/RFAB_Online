#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace Coop
{
// Engine/VM/CEF producers, one main-thread consumer. A drain detaches one bounded batch;
// callbacks and captured-object destruction NEVER run under the producer mutex. Tasks posted
// by a callback wait for the next frame instead of recursively starving the game loop.
class GenerationQueue
{
public:
    static constexpr size_t kMaxPending = 8192;
    bool Post(std::function<void()> aTask) noexcept
    {
        const auto generation = m_generation.load(std::memory_order_acquire);
        std::scoped_lock lock(m_mutex);
        if (m_pending.size() >= kMaxPending)
        {
            m_overflow.store(true, std::memory_order_release);
            return false;
        }
        m_pending.push_back({generation, std::move(aTask)});
        return true;
    }
    void Invalidate() noexcept { m_generation.fetch_add(1, std::memory_order_acq_rel); }
    bool ConsumeOverflow() noexcept { return m_overflow.exchange(false, std::memory_order_acq_rel); }
    size_t Drain() noexcept
    {
        std::deque<Entry> batch;
        {
            std::scoped_lock lock(m_mutex);
            batch.swap(m_pending);
        }
        size_t executed{};
        for (auto& entry : batch)
            if (entry.Generation == m_generation.load(std::memory_order_acquire))
            {
                entry.Task();
                ++executed;
            }
        return executed;
    }
private:
    struct Entry { uint64_t Generation; std::function<void()> Task; };
    std::mutex m_mutex;
    std::deque<Entry> m_pending;
    std::atomic<uint64_t> m_generation{1};
    std::atomic_bool m_overflow{false};
};
} // namespace Coop
