#pragma once

#include <DateTime.h>
#include <Events/EventDispatcher.h>
#include <Games/Events.h>
#include <common/Session/Lifecycle.h>

struct ServerTimeSettings;
struct World;
struct UpdateEvent;
struct TransportService;

/**
 * @brief Handles time sync.
 */
class CalendarService final : public BSTEventSink<TESActivateEvent>, public Session::IParticipant
{
public:
    CalendarService(World&, entt::dispatcher&, TransportService&);

    static bool AllowGameTick() noexcept;

    // Session::ParticipantId::kCalendar: the server clock drives the game clock only during a session.
    void OnSessionEnd(const Session::Transition& acTransition) noexcept override;

private:
    void OnTimeUpdate(const ServerTimeSettings&) noexcept;
    void HandleUpdate(const UpdateEvent&) noexcept;

    void ToggleGameClock(bool aEnable);
    float TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const;

    entt::scoped_connection m_timeUpdateConnection;
    entt::scoped_connection m_updateConnection;

    DateTime m_onlineTime;
    DateTime m_offlineTime;
    float m_fadeTimer = 0.f;
    static bool s_gameClockLocked;

    uint64_t m_lastTick = 0;
    uint64_t m_lastLogTick = 0;
    World& m_world;
    TransportService& m_transport;
};
