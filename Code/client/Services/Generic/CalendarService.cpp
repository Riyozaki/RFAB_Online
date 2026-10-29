#include <Services/CalendarService.h>

#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Messages/ServerTimeSettings.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <PlayerCharacter.h>
#include <TimeManager.h>
#include <common/CoopPacketSafety.h>

constexpr float kTransitionSpeed = 5.f;

bool CalendarService::s_gameClockLocked = false;

bool CalendarService::AllowGameTick() noexcept
{
    return !s_gameClockLocked;
}

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport)
    : m_world(aWorld), m_transport(aTransport)
{
    m_timeUpdateConnection = aDispatcher.sink<ServerTimeSettings>().connect<&CalendarService::OnTimeUpdate>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::HandleUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&CalendarService::OnDisconnected>(this);
}

void CalendarService::OnTimeUpdate(const ServerTimeSettings& acMessage) noexcept
{
    TimeModel sanitized = acMessage.timeModel;
    CoopPacketSafety::SanitizeSkyrimCalendarValues(
        sanitized.TimeScale,
        sanitized.Time,
        sanitized.Day,
        sanitized.Month,
        sanitized.Year);

    // disable the game clock and reset tick delta baseline for fresh/resynced server clock
    ToggleGameClock(false);
    m_lastTick = 0;
    m_onlineTime.m_timeModel.TimeScale = sanitized.TimeScale;
    m_onlineTime.m_timeModel.Time = sanitized.Time;

    if (m_world.GetServerSettings().SyncPlayerCalendar)
    {
        m_onlineTime.m_timeModel.Day = sanitized.Day;
        m_onlineTime.m_timeModel.Month = sanitized.Month;
        m_onlineTime.m_timeModel.Year = sanitized.Year;
    }
    else
    {
        m_onlineTime.m_timeModel.Day = m_offlineTime.m_timeModel.Day;
        m_onlineTime.m_timeModel.Month = m_offlineTime.m_timeModel.Month;
        m_onlineTime.m_timeModel.Year = m_offlineTime.m_timeModel.Year;
    }
}

void CalendarService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // signal a time transition and clear server tick baseline so reconnecting to a restarted server never stalls
    m_fadeTimer = 0.f;
    m_lastTick = 0;
    ToggleGameClock(true);
}

float CalendarService::TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const
{
    const auto t = aTo.Time - aFrom.Time;
    if (t < 0.f)
    {
        const auto v = t + 24.f;
        // interpolate on the time difference, not the time
        const auto x = TiltedPhoques::Lerp(0.f, v, m_fadeTimer / kTransitionSpeed) + aFrom.Time;

        return TiltedPhoques::Mod(x, 24.f);
    }

    return TiltedPhoques::Lerp(aFrom.Time, aTo.Time, m_fadeTimer / kTransitionSpeed);
}

void CalendarService::ToggleGameClock(bool aEnable)
{
    if (auto* pGameTime = TimeData::Get())
    {
        if (pGameTime->GameDay && std::isfinite(pGameTime->GameDay->f))
            m_offlineTime.m_timeModel.Day = static_cast<uint32_t>(std::clamp(pGameTime->GameDay->f, 0.f, 31.f));
        if (pGameTime->GameMonth && std::isfinite(pGameTime->GameMonth->f))
            m_offlineTime.m_timeModel.Month = static_cast<uint32_t>(std::clamp(pGameTime->GameMonth->f, 0.f, 11.f));
        if (pGameTime->GameYear && std::isfinite(pGameTime->GameYear->f))
            m_offlineTime.m_timeModel.Year = static_cast<uint32_t>(std::clamp(pGameTime->GameYear->f, 0.f, 9999.f));
        if (pGameTime->GameHour && std::isfinite(pGameTime->GameHour->f))
            m_offlineTime.m_timeModel.Time = pGameTime->GameHour->f;
        if (pGameTime->TimeScale && std::isfinite(pGameTime->TimeScale->f))
            m_offlineTime.m_timeModel.TimeScale = pGameTime->TimeScale->f;
    }

    s_gameClockLocked = !aEnable;
}

void CalendarService::HandleUpdate(const UpdateEvent& aEvent) noexcept
{
    if (s_gameClockLocked)
    {
        auto* pGameTime = TimeData::Get();
        if (!pGameTime || !pGameTime->TimeScale || !pGameTime->GameDay || !pGameTime->GameMonth ||
            !pGameTime->GameYear || !pGameTime->GameDaysPassed || !pGameTime->GameHour)
        {
            return;
        }

        const auto updateDelta = static_cast<float>(aEvent.Delta);

        if (!m_lastTick)
            m_lastTick = m_world.GetTick();

        const auto now = m_world.GetTick();

        // Resync tick baseline cleanly if server clock restarted at a lower tick value
        if (now < m_lastTick)
        {
            m_lastTick = now;
            return;
        }

        const auto delta = now - m_lastTick;
        m_lastTick = now;

        m_onlineTime.Update(delta);
        pGameTime->TimeScale->f = m_onlineTime.m_timeModel.TimeScale;
        pGameTime->GameDay->f = static_cast<float>(m_onlineTime.m_timeModel.Day);
        pGameTime->GameMonth->f = static_cast<float>(m_onlineTime.m_timeModel.Month);
        pGameTime->GameYear->f = static_cast<float>(m_onlineTime.m_timeModel.Year);
        const float deltaHours = m_onlineTime.GetDeltaTime(delta);
        if (std::isfinite(deltaHours))
            pGameTime->GameDaysPassed->f += deltaHours * (1.f / 24.f);

        // time transition in
        if (m_fadeTimer < kTransitionSpeed)
        {
            const float interpolatedHour = TimeInterpolate(m_offlineTime.m_timeModel, m_onlineTime.m_timeModel);
            pGameTime->GameHour->f = std::isfinite(interpolatedHour) ? interpolatedHour : m_onlineTime.m_timeModel.Time;
            if (std::isfinite(updateDelta) && updateDelta > 0.f)
                m_fadeTimer += updateDelta;
        }
        else
            pGameTime->GameHour->f = m_onlineTime.m_timeModel.Time;
    }
}
