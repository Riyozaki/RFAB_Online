#pragma once

#include <Events/PacketEvent.h>
#include <Structs/GameId.h>

struct World;
struct Player;
struct UpdateEvent;
struct PlayerEnterWorldEvent;
struct RequestQuestUpdate;
struct RequestQuestSceneUpdate;

/**
 * @brief Dispatch quest sync messages.
 */
class QuestService
{
  public:
    QuestService(World& aWorld, entt::dispatcher& aDispatcher);

    void SyncPartyQuestsForPlayer(Player* apPlayer) noexcept;

  private:
    void OnQuestChanges(const PacketEvent<RequestQuestUpdate>& aChanges) noexcept;
    void OnQuestSceneChanges(const PacketEvent<RequestQuestSceneUpdate>& aChanges) noexcept;
    void OnPlayerEnterWorld(const PlayerEnterWorldEvent& acEvent) noexcept;
    void SyncLeaderQuestLogToMember(Player* apLeader, Player* apMember) noexcept;

    World& m_world;

    entt::scoped_connection m_questUpdateConnection;
    entt::scoped_connection m_questSceneUpdateConnection;
    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_joinConnection;
};
