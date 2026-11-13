#include <TiltedOnlinePCH.h>

#include <Services/RunnerService.h>

#include <Events/UpdateEvent.h>

RunnerService::RunnerService(entt::dispatcher& aDispatcher, Session::MainThreadQueue& aQueue, const Session::Lifecycle& acLifecycle) noexcept
    : m_dispatcher(aDispatcher)
    , m_queue(aQueue)
    , m_lifecycle(acLifecycle)
{
}

void RunnerService::Queue(std::function<void()> aFunctor) noexcept
{
    if (!aFunctor)
        return;

    if (m_queue.Post(Session::Lane::kSimulation, m_lifecycle.GetTokenAnyThread(), m_lifecycle.IsWorldLoadingAnyThread(), std::move(aFunctor)))
        return;

    // Overflow means the simulation lane has not been drained for a long time; say so a few times.
    if (m_overflowWarnings.fetch_add(1, std::memory_order_relaxed) < 8)
    {
        spdlog::warn("[RFAB MainThread] simulation lane full ({} tasks): a task was dropped",
                     m_queue.GetCapacity(Session::Lane::kSimulation));
    }
}

void RunnerService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    (void)acUpdateEvent;
    m_queue.Drain(Session::Lane::kSimulation, [this](Session::Token aPostedIn) noexcept {
        return m_lifecycle.IsStillCurrentSession(aPostedIn);
    });
}
