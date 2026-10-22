#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Multiplayer save policy, shared plumbing.
//
// Why this exists: a Skyrim save is a snapshot of the *whole world*, but in
// multiplayer the world belongs to the server. The two safe rules are
//   * do not load a save while connected to a server, and
//   * treat any save written during a session as "session flavoured" (NPCs that
//     died in the shared world stay dead in it).
// This module holds the pure parts (ini parsing, JSON index lines) so that they
// can be unit tested without Windows or Skyrim; the client service
// (SaveGameService) does the file watching and the actual reaction.
namespace Gameplay::SaveSession
{
struct Policy
{
    // Close the connection when a save is loaded while connected. Loading a save
    // into a live session is the classic way to desync/crash.
    bool StopSessionOnLoad{true};

    // Track saves created while connected and write them to an index file, so a
    // player can tell multiplayer saves from single player ones.
    bool IndexSaves{true};

    // Show HUD notifications for the two events above.
    bool NotifyInGame{true};

    // How often the saves folder is scanned (clamped to [1000, 60000]).
    uint32_t PollIntervalMs{5000};
};

inline constexpr uint32_t kMinPollIntervalMs = 1000;
inline constexpr uint32_t kMaxPollIntervalMs = 60000;

/**
 * @brief Parses the [Save] section of RFABOnline.ini.
 *
 * Accepted syntax is deliberately tiny:
 *   [Save]
 *   bStopSessionOnLoad = 1
 *   bIndexSaves = true
 *   uPollIntervalMs = 5000
 * Unknown keys/sections are ignored, malformed values keep the default. The
 * parser is case-insensitive for keys and accepts 0/1/true/false/yes/no.
 */
Policy ParsePolicy(std::string_view acText, Policy aDefaults = {}) noexcept;

/**
 * @brief Escapes a string for embedding into a JSON string literal.
 */
std::string EscapeJson(std::string_view acValue);

struct SaveIndexEntry
{
    std::string File;        // file name, no path
    std::string CreatedUtc;  // ISO-8601 UTC, empty when unknown
    std::string Server;      // "host:port" of the session, empty when offline
    std::string Character;   // player name, may be empty
    uint32_t SessionId{0};
    uint64_t SizeBytes{0};
    bool InSession{false};
};

/**
 * @brief One JSON object per line, for saves-index.jsonl.
 */
std::string FormatSaveIndexLine(const SaveIndexEntry& acEntry);

/**
 * @brief Identity of a file as seen by the directory scan.
 */
struct FileStamp
{
    uint64_t SizeBytes{0};
    int64_t ModifiedTicks{0}; // file time, only compared, never interpreted

    [[nodiscard]] bool operator==(const FileStamp& acRhs) const noexcept
    {
        return SizeBytes == acRhs.SizeBytes && ModifiedTicks == acRhs.ModifiedTicks;
    }
    [[nodiscard]] bool operator!=(const FileStamp& acRhs) const noexcept { return !(*this == acRhs); }
};

/**
 * @brief Decides when a save file is finished being written and can be indexed.
 *
 * Skyrim writes a save over the course of a second or two, and quicksaves and
 * manual saves reuse the same file name. Indexing on the first sighting would
 * therefore record a truncated file or record the same save several times.
 *
 * A file is reported only when its stamp has stopped changing for
 * `kSettleMs`. Files that existed before the session started are primed and
 * never reported, so single player saves are left alone.
 */
class SettleTracker
{
public:
    static constexpr int64_t kSettleMs = 1500;

    //! Remembers a file without ever reporting it (initial directory scan).
    void Prime(std::string_view acKey, FileStamp aStamp) noexcept;

    /**
     * @brief Feeds one observation of a file.
     * @return true exactly once per settled version of the file.
     */
    bool Observe(std::string_view acKey, FileStamp aStamp, int64_t aNowMs) noexcept;

    //! Drops files that disappeared from the directory.
    void Forget(std::string_view acKey) noexcept;

    //! Names currently tracked, for a caller that wants to drop missing files.
    [[nodiscard]] std::vector<std::string> TrackedKeys() const;

    [[nodiscard]] size_t TrackedCount() const noexcept;

private:
    struct Entry
    {
        FileStamp Stamp{};
        int64_t StableSinceMs{0};
        bool Reported{false};
    };

    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace Gameplay::SaveSession
