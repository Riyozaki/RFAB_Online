#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Server side progression ledger.
//
// Why: RFAB keeps experience in a client side GlobalVariable, so everything a
// player earns lives in *their* save file. In multiplayer that is fragile in a
// very specific way: crash, "load an older save", reinstall - and the progress
// that the server already attributed to the player is gone, while the server
// (the source of truth for every other system) has no idea who that player is
// beyond a username.
//
// The ledger is the smallest thing that fixes it without touching the save
// format: the server remembers the highest experience it has ever attributed to
// each username and, when that player returns, tops their client up to that
// value. Nothing is invented, nothing is rolled back, and a player who is ahead
// (played single player in between) is never punished.
//
// Everything in this file is dependency free and unit tested on the host
// (Code/tests/pure/, tools/run_pure_tests.sh).
namespace Gameplay::Progression
{
//! Same ceiling as the client award path (a float GlobalVariable).
inline constexpr int32_t kMaxTotalXp = 10'000'000;

//! How much experience the client is missing, so that the server total is
//! reached. 0 when the client is at or above the server total, or when the
//! values are garbage (negative / absurd).
int32_t ComputeCatchUp(int32_t aServerTotal, int32_t aClientTotal) noexcept;

/**
 * @brief One player's persisted progression plus a diagnostic snapshot.
 *
 * Only Username, RfabXp and Level are used to restore anything; the rest is
 * written so that a support request ("where was I when the server crashed?")
 * can be answered from a file.
 */
struct PlayerRecord
{
    std::string Username{};
    int32_t RfabXp{0};
    uint16_t Level{0};
    uint64_t UpdatedUnixMs{0};

    // Diagnostic snapshot (never used to move a player around).
    uint32_t CellBaseId{0};
    uint32_t CellModId{0};
    uint32_t WorldSpaceBaseId{0};
    uint32_t WorldSpaceModId{0};
    float PositionX{0.0f};
    float PositionY{0.0f};
    float PositionZ{0.0f};

    [[nodiscard]] bool IsValid() const noexcept { return !Username.empty(); }
};

//! One JSON object per line.
std::string FormatRecord(const PlayerRecord& acRecord);

//! Parses one line produced by FormatRecord. Returns false on malformed input.
bool ParseRecord(std::string_view acLine, PlayerRecord& aOut) noexcept;

/**
 * @brief Merges a stored record with what we know now.
 *
 * Experience only ever grows on the server side (max of both), the snapshot is
 * overwritten by the newest observation, and an empty stored record is replaced
 * entirely.
 */
PlayerRecord Merge(const PlayerRecord& acStored, const PlayerRecord& acCurrent) noexcept;

/**
 * @brief In-memory ledger with a stable order, ready to be written out.
 *
 * Kept separate from the file so that the whole policy (who wins a merge, when a
 * record is dropped) is testable.
 */
class Ledger
{
public:
    static constexpr size_t kMaxPlayers = 4096;

    //! Inserts or merges a record. Returns the stored result.
    const PlayerRecord& Upsert(const PlayerRecord& acRecord) noexcept;

    [[nodiscard]] const PlayerRecord* Find(std::string_view acUsername) const noexcept;

    //! Serialises the whole ledger, one line per player, in insertion order.
    [[nodiscard]] std::string Serialize() const;

    //! Replaces the contents from text produced by Serialize(). Bad lines are skipped.
    size_t Deserialize(std::string_view acText);

    [[nodiscard]] size_t Size() const noexcept { return m_records.size(); }
    void Clear() noexcept { m_records.clear(); }

private:
    std::vector<PlayerRecord> m_records{};
};

} // namespace Gameplay::Progression
