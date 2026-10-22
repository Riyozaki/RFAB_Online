#include <TiltedOnlinePCH.h>

#include <fstream>
#include <system_error>

#include <Windows.h>
#include <ShlObj.h>

#include <Services/SaveGameService.h>

#include <World.h>
#include <Services/TransportService.h>

#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PreUpdateEvent.h>

#include <Games/Events.h>
#include <Forms/TESNPC.h>
#include <PlayerCharacter.h>

#include <Utils.h>

using namespace Gameplay::SaveSession;

namespace
{
constexpr wchar_t kConfigFileName[] = L"RFABOnline.ini";
constexpr char kIndexFileName[] = "RFABOnline_saves_index.jsonl";

std::filesystem::path GetGameDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return {};

    std::filesystem::path exePath(buffer);
    return exePath.parent_path();
}

std::string FormatUtcNow()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);

    char buffer[32]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  time.wYear,
                  time.wMonth,
                  time.wDay,
                  time.wHour,
                  time.wMinute,
                  time.wSecond);

    return buffer;
}

std::string FormatFileTimeUtc(const std::filesystem::file_time_type& acTime)
{
    // file_time_type -> system_clock is only well defined in C++20; use a
    // conservative approximation that is good enough for an index file.
    const auto systemTime = std::chrono::system_clock::now() +
                            std::chrono::duration_cast<std::chrono::system_clock::duration>(acTime - std::filesystem::file_time_type::clock::now());

    const std::time_t raw = std::chrono::system_clock::to_time_t(systemTime);
    std::tm utc{};
    gmtime_s(&utc, &raw);

    char buffer[32]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900,
                  utc.tm_mon + 1,
                  utc.tm_mday,
                  utc.tm_hour,
                  utc.tm_min,
                  utc.tm_sec);

    return buffer;
}
} // namespace

SaveGameService::SaveGameService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<PreUpdateEvent>().connect<&SaveGameService::OnUpdate>(this);
    m_connectedConnection = aDispatcher.sink<ConnectedEvent>().connect<&SaveGameService::OnConnected>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&SaveGameService::OnDisconnected>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

BSTEventResult SaveGameService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    if (!m_policyLoaded)
        LoadPolicy();

    spdlog::info("Save system: a save was loaded (connected: {})", m_connected);

    if (m_connected && m_policy.StopSessionOnLoad)
    {
        spdlog::warn("Save system: dropping the connection because a save was loaded during a multiplayer session");

        if (m_policy.NotifyInGame)
            Notify("Save loaded - disconnected from the server (multiplayer rule).");

        m_world.GetRunner().Queue([this]() { m_world.GetTransport().Close(); });
    }

    return BSTEventResult::kOk;
}

void SaveGameService::OnUpdate(const PreUpdateEvent&) noexcept
{
    if (!m_policyLoaded)
        LoadPolicy();

    if (!m_policy.IndexSaves)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(m_policy.PollIntervalMs);

    if (now - m_lastScan < interval)
        return;

    m_lastScan = now;

    ScanSaves(!m_scanInitialized);
    m_scanInitialized = true;
}

void SaveGameService::OnConnected(const ConnectedEvent&) noexcept
{
    m_connected = true;
    m_sessionStarted = std::chrono::steady_clock::now();
    m_sessionId = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(m_sessionStarted.time_since_epoch()).count());

    m_serverAddress = m_world.GetTransport().GetLastEndpoint();

    m_characterName.clear();
    if (const auto* pPlayer = PlayerCharacter::Get())
    {
        if (auto* pNpc = Cast<TESNPC>(pPlayer->baseForm))
            m_characterName = pNpc->fullName.value.AsAscii();
    }

    spdlog::info("Save system: session {} started on '{}' as '{}'", m_sessionId, m_serverAddress, m_characterName);
}

void SaveGameService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_connected = false;
    spdlog::info("Save system: session {} ended", m_sessionId);
}

void SaveGameService::LoadPolicy() noexcept
{
    m_policyLoaded = true;

    const auto path = GetGameDirectory() / L"Data" / L"SKSE" / L"Plugins" / kConfigFileName;

    std::ifstream file(path);
    if (!file.is_open())
    {
        spdlog::info("Save system: {} not found, using defaults", path.string());
        m_policy = Policy{};
        return;
    }

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    m_policy = ParsePolicy(text, Policy{});

    spdlog::info("Save system: stopSessionOnLoad={} indexSaves={} notify={} poll={}ms",
                 m_policy.StopSessionOnLoad,
                 m_policy.IndexSaves,
                 m_policy.NotifyInGame,
                 m_policy.PollIntervalMs);
}

std::filesystem::path SaveGameService::GetSavesDirectory() const
{
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &rawPath)))
        return {};

    std::filesystem::path documents(rawPath);
    CoTaskMemFree(rawPath);

    return documents / L"My Games" / L"Skyrim Special Edition" / L"Saves";
}

std::filesystem::path SaveGameService::GetIndexPath() const
{
    return GetSavesDirectory() / kIndexFileName;
}

void SaveGameService::ScanSaves(bool aInitial) noexcept
{
    const auto directory = GetSavesDirectory();
    if (directory.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
        return;

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    m_seenThisScan.clear();

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    {
        if (ec)
            break;

        if (!entry.is_regular_file())
            continue;

        const auto& path = entry.path();
        if (path.extension() != L".ess")
            continue;

        std::error_code statEc;
        const auto size = std::filesystem::file_size(path, statEc);
        if (statEc)
            continue;

        const auto modified = std::filesystem::last_write_time(path, statEc);
        if (statEc)
            continue;

        FileStamp stamp{static_cast<uint64_t>(size), static_cast<int64_t>(modified.time_since_epoch().count())};

        const std::string key = path.filename().string();
        m_seenThisScan.insert(key);

        // Files that were already there when the game started are primed, never
        // reported: an old single player save must not appear as a session save.
        if (aInitial)
        {
            m_settle.Prime(key, stamp);
            continue;
        }

        if (m_settle.Observe(key, stamp, nowMs))
            RecordSave(path, true);
    }

    // Forget files the player deleted, so a later save with the same name is
    // treated as new instead of "settled and already reported".
    for (const auto& key : m_settle.TrackedKeys())
    {
        if (m_seenThisScan.find(key) == m_seenThisScan.end())
            m_settle.Forget(key);
    }
}

void SaveGameService::RecordSave(const std::filesystem::path& acPath, bool aNotify) noexcept
{
    SaveIndexEntry entry;
    entry.File = acPath.filename().string();
    entry.CreatedUtc = FormatUtcNow();
    entry.InSession = m_connected;
    entry.SessionId = m_connected ? m_sessionId : 0;
    entry.Server = m_connected ? m_serverAddress : std::string{};
    entry.Character = m_connected ? m_characterName : std::string{};

    std::error_code ec;
    entry.SizeBytes = static_cast<uint64_t>(std::filesystem::file_size(acPath, ec));

    const std::string line = FormatSaveIndexLine(entry);

    std::ofstream index(GetIndexPath(), std::ios::app);
    if (index.is_open())
        index << line << '\n';
    else
        spdlog::warn("Save system: could not write {}", GetIndexPath().string());

    spdlog::info("Save system: {} ({})", entry.InSession ? "multiplayer save recorded" : "save recorded", line);

    if (aNotify && m_connected && m_policy.NotifyInGame)
        Notify("Multiplayer save created: it contains the shared world state.");
}

void SaveGameService::Notify(const std::string& acMessage) const noexcept
{
    Utils::ShowHudMessage(String(acMessage));
}
