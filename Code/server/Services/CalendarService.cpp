#include <Services/CalendarService.h>

#include <GameServer.h>
#include <World.h>
#include <common/CoopPacketSafety.h>
#include <common/RfabModpackCompat.h>

#include <Events/PlayerJoinEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ServerTimeSettings.h>

#include "Game/Player.h"

CalendarService::CalendarService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CalendarService::OnUpdate>(this);
    m_joinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&CalendarService::OnPlayerJoin>(this);
}

void CalendarService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_lastTick)
        m_lastTick = GameServer::Get()->GetTick();

    auto now = GameServer::Get()->GetTick();

    // client got ahead, we wait
    if (now < m_lastTick)
        return;

    auto delta = now - m_lastTick;
    m_lastTick = now;
    m_dateTime.Update(delta);
}

void CalendarService::OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept
{
    ServerTimeSettings timeMsg;
    const bool initializedFromHost = !m_timeSetFromFirstPlayer;

    // Seed the authoritative server calendar from the first player (Host), or keep the
    // authoritative checkpoint calendar restored by SaveSnapshotService::ApplyWorldSnapshot.
    if (initializedFromHost)
    {
        TimeModel hostTime = acEvent.PlayerTime.m_timeModel;
        CoopPacketSafety::SanitizeSkyrimCalendarValues(
            hostTime.TimeScale,
            hostTime.Time,
            hostTime.Day,
            hostTime.Month,
            hostTime.Year);
        m_dateTime.m_timeModel.Time = hostTime.Time;
        m_dateTime.m_timeModel.Day = hostTime.Day;
        m_dateTime.m_timeModel.Month = hostTime.Month;
        m_dateTime.m_timeModel.Year = hostTime.Year;
        m_timeSetFromFirstPlayer = true;
    }

    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;

    if (initializedFromHost)
        GameServer::Get()->SendToPlayers(timeMsg);
    else if (acEvent.pPlayer)
        acEvent.pPlayer->Send(timeMsg);
}

bool CalendarService::SetTime(int aHours, int aMinutes, float aScale) noexcept
{
    if (!std::isfinite(aScale) || aScale < 0.f || aScale > 1000.f)
        return false;

    if (aHours >= 0 && aHours <= 23 && aMinutes >= 0 && aMinutes <= 59)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;
        m_dateTime.m_timeModel.Time = RfabCompat::EncodeSkyrimGameHour(aHours, aMinutes);
        m_timeSetFromFirstPlayer = true;

        SendTimeResync();

        GameServer::Get()->GetWorld().GetScriptService().HandleSetTime(aHours, aMinutes, aScale);
        return true;
    }
    return false;
}

bool CalendarService::SetDate(int aDay, int aMonth, float aYear) noexcept
{
    if (std::isfinite(aYear) && aMonth >= 0 && aMonth < 12 && aYear >= 0 && aYear <= 999)
    {
        auto maxDays = m_dateTime.GetNumberOfDaysByMonthIndex(aMonth);
        if (aDay >= 0 && aDay < maxDays)
        {
            m_dateTime.m_timeModel.Day = aDay;
            m_dateTime.m_timeModel.Month = aMonth;
            m_dateTime.m_timeModel.Year = static_cast<uint32_t>(aYear);
            m_timeSetFromFirstPlayer = true;
            SendTimeResync();
            return true;
        }
    }
    return false;
}

void CalendarService::SendTimeResync() noexcept
{
    ServerTimeSettings timeMsg;
    timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
    timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
    timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
    timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
    timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
    GameServer::Get()->SendToLoaded(timeMsg);
}

CalendarService::TTime CalendarService::GetTime() const noexcept
{
    int hours = 8;
    int minutes = 0;
    RfabCompat::DecodeSkyrimGameHour(m_dateTime.m_timeModel.Time, hours, minutes);
    return {hours, minutes};
}

CalendarService::TTime CalendarService::GetRealTime() noexcept
{
    const auto t = std::time(nullptr);
    int h = (t / 3600) % 24;
    int m = (t / 60) % 60;
    return {h, m};
}

CalendarService::TDate CalendarService::GetDate() const noexcept
{
    return {m_dateTime.m_timeModel.Day, m_dateTime.m_timeModel.Month, m_dateTime.m_timeModel.Year};
}

bool CalendarService::SetTimeScale(float aScale) noexcept
{
    if (std::isfinite(aScale) && aScale >= 0.f && aScale <= 1000.f)
    {
        m_dateTime.m_timeModel.TimeScale = aScale;

        ServerTimeSettings timeMsg;
        timeMsg.timeModel.TimeScale = m_dateTime.m_timeModel.TimeScale;
        timeMsg.timeModel.Time = m_dateTime.m_timeModel.Time;
        timeMsg.timeModel.Day = m_dateTime.m_timeModel.Day;
        timeMsg.timeModel.Month = m_dateTime.m_timeModel.Month;
        timeMsg.timeModel.Year = m_dateTime.m_timeModel.Year;
        GameServer::Get()->SendToPlayers(timeMsg);
        return true;
    }

    return false;
}
