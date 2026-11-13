#pragma once

#include <common/Session/MainThreadQueue.h>

struct UpdateEvent;

/**
 * @brief Gameplay work for the next simulated frame: the simulation lane of World's MainThreadQueue.
 *
 * Callable from any thread (engine hooks post their events here). Every task carries the session it
 * was posted in and is dropped if that session ended first; tasks run outside any lock, so posting
 * never waits for the main thread and a task may post again (it runs on the next frame).
 * See common/Session/MainThreadQueue.h for the contract.
 */
struct RunnerService
{
    RunnerService(entt::dispatcher& aDispatcher, Session::MainThreadQueue& aQueue, const Session::Lifecycle& acLifecycle) noexcept;
    ~RunnerService() noexcept = default;

    TP_NOCOPYMOVE(RunnerService);

    /**
     * @brief Executes the work queued before this frame (World::Update, simulated frames only).
     */
    void OnUpdate(const UpdateEvent& acUpdateEvent) noexcept;

    /**
     * @brief Queues an event to be dispatched on the main thread during the next simulated frame.
     */
    template <class T> void Trigger(T acEvent)
    {
        Queue([event = std::move(acEvent), this]() { m_dispatcher.trigger(std::move(event)); });
    }

    /**
     * @brief Queues a lambda to run on the main thread during the next simulated frame.
     */
    void Queue(std::function<void()> aFunctor) noexcept;

private:
    entt::dispatcher& m_dispatcher;
    Session::MainThreadQueue& m_queue;
    const Session::Lifecycle& m_lifecycle;
    std::atomic<uint32_t> m_overflowWarnings{0};
};
