#pragma once

#include <Events/PacketEvent.h>
#include <Gameplay/XpShare.h>
#include <TiltedCore/Stl.hpp>

struct World;
struct PlayerJoinEvent;
struct PlayerLeaveEvent;
struct SyncRfabExperienceRequest;

/**
 * @brief Shares RFAB experience between players (server authoritative).
 *
 * Requirement: the same in-game event must give every player the same
 * experience. RFAB computes XP locally in Papyrus (RFAB_XP_Handler.ModXP(),
 * stored in the `PlayerXP` GlobalVariable of RFAB.esp), so the client
 * reports "I just gained X" and the server fans it out to everybody else. The
 * sender is always excluded: its own RFAB pipeline already awarded the XP.
 *
 * The award on the receiving side mirrors ModXP exactly
 * (Code/client/Services/RfabXp.h explains why it is a reproduction and not a
 * Papyrus call).
 *
 * The server:
 *   * sanitises the amount (a client cannot inject a million XP);
 *   * drops replayed/duplicated reports using a per-player monotonic sequence
 *     number (survives reconnects);
 *   * optionally collapses "same amount within N seconds" reports coming from
 *     different players, which is what happens when the game tells every client
 *     about the same quest completion / kill;
 *   * fans out to the whole server (configurable) instead of the party only.
 *
 * Known limitation: without per-event ids from RFAB (ModXP takes only an
 * integer; see MO2/mods/RFAB/scripts/source/RFAB_XP_Handler.psc) two *different*
 * events with the exact same XP amount inside the dedup window would be
 * collapsed into one. The window can be set to 0 to disable that layer - see
 * Gameplay:uXpShareDedupWindowMs.
 */
struct XpShareService
{
    XpShareService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~XpShareService() noexcept = default;

    TP_NOCOPYMOVE(XpShareService);

    /**
     * @brief Statistics for the admin console / diagnostics.
     */
    struct Stats
    {
        uint64_t ReportsAccepted{0};
        uint64_t ReportsRejected{0};
        uint64_t ReportsSuppressed{0};
        uint64_t TotalAwarded{0};
    };

    [[nodiscard]] const Stats& GetStats() const noexcept { return m_stats; }

private:
    void OnShareRequest(const PacketEvent<SyncRfabExperienceRequest>& acMessage) noexcept;
    void OnPlayerJoin(const PlayerJoinEvent& acEvent) noexcept;
    void OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept;

    World& m_world;

    TiltedPhoques::Map<ConnectionId_t, Gameplay::XpShare::SequenceTracker> m_trackers{};
    Gameplay::XpShare::AmountWindow m_window{};
    Stats m_stats{};

    entt::scoped_connection m_shareConnection;
    entt::scoped_connection m_playerJoinConnection;
    entt::scoped_connection m_playerLeaveConnection;
};
