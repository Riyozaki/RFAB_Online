#pragma once

#include <Services/PlayerService.h>
#include <Services/PartyService.h>
#include <Services/CharacterService.h>
#include <Services/CalendarService.h>
#include <Services/QuestService.h>
#include <Services/ScriptService.h>

#include "Game/PlayerManager.h"

namespace ESLoader
{
struct RecordCollection;
}

namespace WorldParts
{
// Owns the event dispatcher as the FIRST base class so entt::registry (and every service in ctx()
// holding entt::scoped_connection into m_dispatcher) is destroyed BEFORE m_dispatcher is freed.
struct DispatcherOwner
{
protected:
    entt::dispatcher m_dispatcher;
};
} // namespace WorldParts

struct World : WorldParts::DispatcherOwner, entt::registry
{
    World();
    ~World() noexcept;

    TP_NOCOPYMOVE(World);

    entt::dispatcher& GetDispatcher() noexcept { return m_dispatcher; }
    const entt::dispatcher& GetDispatcher() const noexcept { return m_dispatcher; }
    CharacterService& GetCharacterService() noexcept { return ctx().at<CharacterService>(); }
    const CharacterService& GetCharacterService() const noexcept { return ctx().at<const CharacterService>(); }
    PlayerService& GetPlayerService() noexcept { return ctx().at<PlayerService>(); }
    const PlayerService& GetPlayerService() const noexcept { return ctx().at<const PlayerService>(); }
    PartyService& GetPartyService() noexcept { return ctx().at<PartyService>(); }
    const PartyService& GetPartyService() const noexcept { return ctx().at<const PartyService>(); }
    CalendarService& GetCalendarService() noexcept { return ctx().at<CalendarService>(); }
    const CalendarService& GetCalendarService() const noexcept { return ctx().at<const CalendarService>(); }
    QuestService& GetQuestService() noexcept { return ctx().at<QuestService>(); }
    const QuestService& GetQuestService() const noexcept { return ctx().at<const QuestService>(); }
    PlayerManager& GetPlayerManager() noexcept { return m_playerManager; }
    const PlayerManager& GetPlayerManager() const noexcept { return m_playerManager; }
    ScriptService& GetScriptService() const noexcept { return *m_pScriptService; }

    // Null checked at start when MoPo is on!
    ESLoader::RecordCollection* GetRecordCollection() noexcept { return m_recordCollection.get(); }

    const ESLoader::RecordCollection* GetRecordCollection() const noexcept { return m_recordCollection.get(); }

    [[nodiscard]] static uint32_t ToInteger(entt::entity aEntity) { return to_integral(aEntity); }

    // Reserve EnTT entity 0 so no real character/object/player ever receives ServerId == 0.
    [[nodiscard]] bool valid(entt::entity aEntity) const noexcept
    {
        return aEntity != m_reservedZeroEntity && entt::registry::valid(aEntity);
    }

    auto destroy(entt::entity aEntity)
    {
        if (aEntity == m_reservedZeroEntity)
            return version_type{};
        return entt::registry::destroy(aEntity);
    }

private:
    entt::entity m_reservedZeroEntity{entt::null};

    TiltedPhoques::UniquePtr<ScriptService> m_pScriptService;
    PlayerManager m_playerManager;
    UniquePtr<ESLoader::RecordCollection> m_recordCollection;
};
