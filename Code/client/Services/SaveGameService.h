#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Gameplay/SaveSession.h>

#include <Events/EventDispatcher.h>
#include <Games/Events.h>

struct World;
struct ConnectedEvent;
struct DisconnectedEvent;
struct PreUpdateEvent;
struct TESLoadGameEvent;

/**
 * @brief Multiplayer save hygiene.
 *
 * A Skyrim save contains the entire world; in multiplayer the world belongs to
 * the server. This service implements the two rules that keep saves usable
 * without adding any new game hooks:
 *
 *  1. Loading a save while connected to a server closes the connection. Loading
 *     a world into a live session is the classic way to get a desync or a
 *     crash; the server would otherwise keep sending state for a world that no
 *     longer exists.
 *
 *  2. Every save written while connected is recorded in
 *     `saves-index.jsonl` next to the save files, together with the session
 *     (server address, character, session id). The player then knows which of
 *     their saves contain shared-world state, and diagnostics can be matched to
 *     a session afterwards.
 *
 * Everything is driven by the engine's existing TESLoadGameEvent sink plus a
 * directory scan, so no address needs to be reversed here. Configuration lives
 * in `Data/SKSE/Plugins/RFABOnline.ini`, section [Save].
 */
struct SaveGameService final : BSTEventSink<TESLoadGameEvent>
{
    SaveGameService(World& aWorld, entt::dispatcher& aDispatcher) noexcept;
    ~SaveGameService() noexcept override = default;

    TP_NOCOPYMOVE(SaveGameService);

protected:
    BSTEventResult OnEvent(const TESLoadGameEvent* apEvent, const EventDispatcher<TESLoadGameEvent>* apDispatcher) override;

private:
    void OnUpdate(const PreUpdateEvent& acEvent) noexcept;
    void OnConnected(const ConnectedEvent& acEvent) noexcept;
    void OnDisconnected(const DisconnectedEvent& acEvent) noexcept;

    void LoadPolicy() noexcept;
    void ScanSaves(bool aInitial) noexcept;
    void RecordSave(const std::filesystem::path& acPath, bool aNotify) noexcept;
    void Notify(const std::string& acMessage) const noexcept;

    [[nodiscard]] std::filesystem::path GetSavesDirectory() const;
    [[nodiscard]] std::filesystem::path GetIndexPath() const;

    World& m_world;

    Gameplay::SaveSession::Policy m_policy{};
    bool m_policyLoaded{false};
    bool m_scanInitialized{false};

    std::chrono::steady_clock::time_point m_lastScan{};
    Gameplay::SaveSession::SettleTracker m_settle{};
    std::unordered_set<std::string> m_seenThisScan{};
    std::chrono::steady_clock::time_point m_sessionStarted{};

    bool m_connected{false};
    uint32_t m_sessionId{0};
    uint32_t m_playerCountAtStart{0};
    std::string m_serverAddress{};
    std::string m_characterName{};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
};
