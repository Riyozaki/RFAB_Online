#pragma once

#include <chrono>
#include <string>

#include <Events/PacketEvent.h>
#include <Gameplay/Progression.h>

struct World;
struct Player;
struct PlayerEnterWorldEvent;
struct PlayerLeaveEvent;
struct UpdateEvent;

/**
 * @brief Remembers what each player earned, so losing a save does not lose progress.
 *
 * RFAB keeps experience in a client side GlobalVariable: it lives in the save
 * file, not on the server. Everything else in this fork is server authoritative,
 * which leaves one very common failure mode: a player crashes, loads an older
 * save (or a fresh character) and silently loses hours of shared progress - the
 * server happily keeps handing out XP for a character that "never" did those
 * quests.
 *
 * This service keeps the highest experience total it has ever attributed to each
 * *username* in `Data/progress/progress.jsonl`. When that username connects and
 * reports a lower total, the difference is sent back as a normal
 * NotifySyncRfabExperience award (the client applies it through the same ModXP
 * path as everything else). A player who is ahead is left alone.
 *
 * The same file also stores a small "where were you" snapshot (cell, worldspace,
 * position, level, timestamp) for diagnostics. It is never used to move anyone.
 *
 * This cannot and does not replace a save file: the client's world state (quests,
 * perks, inventory) is still theirs. Only the shared experience is restored,
 * because that is the part the server actually attributed.
 */
struct ProgressionService
{
    ProgressionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~ProgressionService() noexcept;

    TP_NOCOPYMOVE(ProgressionService);

    /**
     * @brief Credits an award to every player it was fanned out to.
     *
     * The server hands the same award to everybody, so everybody's total grows
     * by it - including the initiator, who already had it applied locally. The
     * ledger must mirror that, otherwise a player who loses their save would only
     * be restored to the experience they personally initiated.
     */
    void OnAward(uint32_t aAmount) noexcept;

    //! Writes the ledger to disk (no-op when disabled or unchanged).
    void Flush(bool aForce = false) noexcept;

    [[nodiscard]] size_t TrackedPlayers() const noexcept { return m_ledger.Size(); }
    [[nodiscard]] const std::string& GetPath() const noexcept { return m_path; }

private:
    void OnPlayerEnterWorld(const PlayerEnterWorldEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;
    void OnUpdate(const UpdateEvent& acEvent) noexcept;

    void Load() noexcept;

    //! Builds the record for a player who is about to be forgotten on the server.
    Gameplay::Progression::PlayerRecord MakeRecord(const Player& acPlayer) const noexcept;

    World& m_world;
    std::string m_path{};
    Gameplay::Progression::Ledger m_ledger{};

    bool m_dirty{false};
    std::chrono::steady_clock::time_point m_lastFlush{};
    static constexpr std::chrono::seconds kFlushInterval{30};

    entt::scoped_connection m_enterWorldConnection;
    entt::scoped_connection m_leaveConnection;
    entt::scoped_connection m_updateConnection;
};
