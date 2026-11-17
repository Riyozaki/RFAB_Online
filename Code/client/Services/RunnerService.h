#pragma once

#include <Coop/GenerationQueue.h>

struct UpdateEvent;

/**
 * @brief Dispatches events.
 */
struct RunnerService
{
    RunnerService(entt::dispatcher& aDispatcher) noexcept;
    ~RunnerService() noexcept = default;

    TP_NOCOPYMOVE(RunnerService);

    /**
     * @brief Executes all queued events every frame.
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;

    /**
     * @brief Queues an event for OnUpdate() to execute.
     */
    template <class T> void Trigger(T acEvent)
    {
        m_runner.Post([event = std::move(acEvent), this]() {
            m_dispatcher.trigger(std::move(event));
        });
    }

    /**
     * @brief Queues a lambda for OnUpdate() to execute.
     */
    void QueueForThisWorld(std::function<void()> aFunctor) noexcept;
    void Invalidate() noexcept { m_runner.Invalidate(); }
    bool ConsumeOverflow() noexcept { return m_runner.ConsumeOverflow(); }

private:
    entt::dispatcher& m_dispatcher;
    Coop::GenerationQueue m_runner;
};
