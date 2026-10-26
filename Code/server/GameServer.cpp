#include <Components.h>
#include <GameServer.h>
#include <Packet.hpp>

#include <algorithm>
#include <cmath>

#include <Events/AdminPacketEvent.h>
#include <Events/CharacterRemoveEvent.h>
#include <Events/OwnershipTransferEvent.h>
#include <Events/PacketEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveCellEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <steam/isteamnetworkingutils.h>

#include <AdminMessages/AdminSessionOpen.h>
#include <AdminMessages/ClientAdminMessageFactory.h>
#include <Messages/AuthenticationResponse.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifySettingsChange.h>
#include <common/RfabModpackCompat.h>
#include <console/ConsoleRegistry.h>
#include <resources/ResourceCollection.h>

constexpr size_t kMaxServerNameLength = 128u;

// -- Cvars --
Console::Setting uServerPort{"GameServer:uPort", "Which port to host the server on", 10578u};
Console::Setting uMaxPlayerCount{"GameServer:uMaxPlayerCount", "Maximum number of players allowed on the server (going over the default of 8 is not recommended)", 8u};
Console::Setting bPremiumTickrate{"GameServer:bPremiumMode", "Use premium tick rate", true};

Console::StringSetting sServerName{"GameServer:sServerName", "Name that shows up in the server list", "Dedicated Together Server"};
Console::StringSetting sAdminPassword{"GameServer:sAdminPassword", "Admin authentication password", ""};
Console::StringSetting sPassword{"GameServer:sPassword", "Server password", ""};

// Gameplay
// RFAB / Requiem requires Adept (2 = 1.0x dealt / 1.0x taken; DifficultyBalance.ini & RFAB.ini target Adept).
Console::Setting uDifficulty{"Gameplay:uDifficulty", "In game difficulty (0 to 5, RFAB default 2 = Adept 1.0x)", 2u};
Console::Setting bEnableGreetings{"Gameplay:bEnableGreetings", "Enables NPC greetings (disabled by default since they can be spammy with dialogue sync)", false};
Console::Setting bEnablePvp{"Gameplay:bEnablePvp", "Enables pvp", false};
Console::Setting bSyncPlayerHomes{"Gameplay:bSyncPlayerHomes", "Sync chests and displays in player homes and other NoResetZones", false};
Console::Setting bEnableDeathSystem{"Gameplay:bEnableDeathSystem", "Enables the custom multiplayer death system", true};
Console::Setting uTimeScale{"Gameplay:uTimeScale", "How many seconds pass ingame for every real second (0 to 1000). Changing this can make the game unstable", 20u};
Console::Setting bSyncPlayerCalendar{"Gameplay:bSyncPlayerCalendar", "Syncs up all player calendars to be the same day, month, and year. This uses the date of the player with the furthest ahead date at connection.", false};
Console::Setting bAutoPartyJoin{"Gameplay:bAutoPartyJoin", "Join parties automatically, as long as there is only one party in the server", true};
Console::Setting fHealthMultiplier{"Gameplay:fHealthMultiplier", "Base max-health multiplier for non-player enemies (0.1 to 20.0)", 1.0f};
Console::Setting bAutoScaleEnemyHealth{"Gameplay:bAutoScaleEnemyHealth", "Automatically scales enemy HP with connected player count (1p=1x, 2p=3x, 3p=4x, N>=2 -> (N+ExtraBase)x)", true};
Console::Setting fHealthExtraBase{"Gameplay:fHealthExtraBase", "Extra baseline term added to connected player count for HP scaling (1.0 => 2p=3x, 3p=4x)", 1.0f};
Console::Setting bEnableDownedSystem{"Gameplay:bEnableDownedSystem", "Enables RFAB 0-HP downed-on-knee bleedout, E-revive, strict combat leash, and party wipe checkpoint reload", true};
Console::Setting bEnableCoordinatedSaves{"Gameplay:bEnableCoordinatedSaves", "Enables host-coordinated saves/loads and automatic post-combat world checkpoints", true};
// ModPolicy Stuff
Console::Setting bEnableModCheck{"ModPolicy:bEnableModCheck", "Bypass the checking of mods on the server", false, Console::SettingsFlags::kLocked};
Console::Setting bAllowSKSE{"ModPolicy:bAllowSKSE", "Allow clients with SKSE active to join", true, Console::SettingsFlags::kLocked};
Console::Setting bAllowMO2{"ModPolicy:bAllowMO2", "Allow clients running Mod Organizer 2 to join", true, Console::SettingsFlags::kLocked};

// -- Commands --
Console::Command<> TogglePremium(
    "TogglePremium", "Toggle Premium Tickrate on/off",
    [](Console::ArgStack&)
    {
        bPremiumTickrate = !bPremiumTickrate;
        spdlog::get("ConOut")->info("Premium Tickrate has been {}.", bPremiumTickrate == true ? "enabled" : "disabled");
    });

Console::Command<> TogglePvp(
    "TogglePvp", "Toggle PvP on/off",
    [](Console::ArgStack&)
    {
        bEnablePvp = !bEnablePvp;
        spdlog::get("ConOut")->info("PvP has been {}.", bEnablePvp == true ? "enabled" : "disabled");
        GameServer::Get()->UpdateSettings();
    });

Console::Command<int64_t> SetDifficulty(
    "SetDifficulty", "Set server difficulty (0 being Novice and 5 being Legendary; default is 4)",
    [](Console::ArgStack& aStack)
    {
        auto aDiff = aStack.Pop<int64_t>();

        if (aDiff < 0 || aDiff > 5)
        {
            spdlog::warn(
                "Game difficulty is invalid (should be from 0 to 5, "
                "current value is {}), setting difficulty to 4 (master).",
                aDiff);

            aDiff = 4;
        }

        uDifficulty = (uint32_t)aDiff;

        GameServer::Get()->UpdateSettings();
        spdlog::get("ConOut")->info("Difficulty has been set to {}.", aDiff);
    });

Console::Command<> ShowVersion("version", "Show the version the server was compiled with", [](Console::ArgStack&) { spdlog::get("ConOut")->info("Server " BUILD_COMMIT); });

Console::Command<> CrashServer(
    "crash", "Crashes the server, don't use!",
    [](Console::ArgStack&)
    {
        int* i = 0;
        *i = 42;
    });

Console::Command<> ShowMoPoStatus(
    "ShowMOPOStats", "Shows the status of ModPolicy",
    [](Console::ArgStack&)
    {
        auto formatStatus = [](bool aToggle)
        {
            return aToggle ? "yes" : "no";
        };

        spdlog::get("ConOut")->info("Modcheck enabled: {}\nSKSE allowed: {}\nMO2 allowed: {}", formatStatus(bEnableModCheck), formatStatus(bAllowSKSE), formatStatus(bAllowMO2));
    });

// -- Constants --
constexpr float kMinHealthMultiplier = 0.1f;
constexpr float kMaxHealthMultiplier = 20.0f;

static float GetHealthMultiplier()
{
    const float value = fHealthMultiplier.as_float();
    if (!std::isfinite(value))
        return 1.0f;

    return std::clamp(value, kMinHealthMultiplier, kMaxHealthMultiplier);
}

static float GetEffectiveHealthMultiplier()
{
    size_t playerCount = 0;
    if (auto* pServer = GameServer::Get(); pServer && pServer->HasWorld())
        playerCount = pServer->GetWorld().GetPlayerManager().Count();

    return ServerSettings::ComputeEffectiveHealthMultiplier(
        GetHealthMultiplier(),
        bAutoScaleEnemyHealth,
        fHealthExtraBase.as_float(),
        playerCount);
}

constexpr char kBypassMoPoWarning[]{"ModCheck is disabled. This can lead to desync and other oddities. Make sure you know what you are doing. We "
                                    "may not be able to assist you if ModCheck was disabled."};

constexpr char kMopoRecordsMissing[]{"Failed to start: ModPolicy's ModCheck is enabled, but no mods are installed. Players won't be able "
                                     "to join! Please create a Data/ directory, and put a \"loadorder.txt\" file in there."
                                     "Check the wiki, which can be found on skyrim-together.com, for more details."};

constexpr char kCalendarSyncWarning[]{"Calendar sync is enabled. We generally do not recommend that you use this feature."
                                      "Calendar sync can cause the calendar to jump ahead or behind, which might mess up the timing of quests."
                                      "If you disable this feature again (which is the default setting), the days will still progress, but the"
                                      "exact date will differ slightly between clients (which has no impact on gameplay)."};

static uint16_t GetUserTickRate()
{
    return bPremiumTickrate ? 60 : 30;
}

static bool IsMoPoActive()
{
    return bEnableModCheck;
}

ServerSettings GetSettings()
{
    ServerSettings settings{};
    settings.Difficulty = uDifficulty.value_as<uint8_t>();
    settings.GreetingsEnabled = bEnableGreetings;
    settings.PvpEnabled = bEnablePvp;
    settings.SyncPlayerHomes = bSyncPlayerHomes;
    settings.DeathSystemEnabled = bEnableDeathSystem;
    settings.SyncPlayerCalendar = bSyncPlayerCalendar;
    settings.AutoPartyJoin = bAutoPartyJoin;
    settings.HealthMultiplier = GetEffectiveHealthMultiplier();
    settings.HealthExtraBase = std::isfinite(fHealthExtraBase.as_float()) ? std::clamp(fHealthExtraBase.as_float(), 0.0f, 10.0f) : 1.0f;
    settings.DownedSystemEnabled = bEnableDownedSystem;
    settings.CoordinatedSavesEnabled = bEnableCoordinatedSaves;
    if (auto* pServer = GameServer::Get(); pServer && pServer->HasWorld())
    {
        const auto& encCfg = pServer->GetWorld().GetEncounterService().GetCoordinator().GetConfig();
        settings.ReviveDurationSec = encCfg.ReviveDurationSec;
        settings.ReviveHealthFraction = encCfg.ReviveHealthFraction;
        settings.PostReviveGraceSec = encCfg.PostReviveGraceSec;
        settings.CombatLeashRadiusMeters = encCfg.LeashRadiusMeters;
    }
    return settings;
}

GameServer::GameServer(Console::ConsoleRegistry& aConsole) noexcept
    : m_lastFrameTime(std::chrono::high_resolution_clock::now())
    , m_startTime(std::chrono::high_resolution_clock::now())
    , m_commands(aConsole)
    , m_requestStop(false)
{
    BASE_ASSERT(s_pInstance == nullptr, "Server instance already exists?");
    s_pInstance = this;

    auto port = uServerPort.value_as<uint16_t>();
    while (!Host(port, GetUserTickRate()))
    {
        spdlog::warn("Port {} is already in use, trying {}", port, port + 1);
        port++;
    }

    if (uDifficulty.value_as<uint8_t>() > 5)
    {
        spdlog::warn(
            "Game difficulty is invalid (should be from 0 to 5, current value is {}), setting difficulty to 4 "
            "(master).",
            uDifficulty.value_as<uint8_t>());

        uDifficulty = 4;
    }

    const float healthMultiplier = fHealthMultiplier.as_float();
    if (!std::isfinite(healthMultiplier) || healthMultiplier < kMinHealthMultiplier || healthMultiplier > kMaxHealthMultiplier)
    {
        spdlog::warn(
            "Health multiplier is invalid (should be from {} to {}, current value is {}), setting it to 1.0.",
            kMinHealthMultiplier,
            kMaxHealthMultiplier,
            healthMultiplier);

        fHealthMultiplier = 1.0f;
    }

    if (!bEnableDeathSystem)
    {
        spdlog::warn("The multiplayer death system is disabled on this server. We recommend that you ONLY do this if you have"
                     " a mod that replaces the vanilla death system. You should only disable our death system if you"
                     " absolutely know what you are doing!");
    }

    m_isPasswordProtected = strcmp(sPassword.value(), "") != 0;

    UpdateInfo();
    spdlog::info("Server {} started on port {}", BUILD_COMMIT, GetPort());
    UpdateTitle();

    m_pWorld = MakeUnique<World>();

    BindMessageHandlers();
    UpdateTimeScale();

    m_pResources = MakeUnique<Resources::ResourceCollection>();
}

GameServer::~GameServer()
{
    s_pInstance = nullptr;
}

GameServer* GameServer::Get() noexcept
{
    return s_pInstance;
}

void GameServer::Initialize()
{
    if (!CheckMoPo())
        return;

    if (bSyncPlayerCalendar)
        spdlog::warn(kCalendarSyncWarning);

    BindServerCommands();
    m_pWorld->GetScriptService().Initialize(*m_pResources);
}

void GameServer::Kill()
{
    spdlog::info("Server shutdown requested");
    m_requestStop = true;
}

bool GameServer::CheckMoPo()
{
    if (!bEnableModCheck)
    {
        // TODO: re-enable this warning when mopo has good ui and the line endings problem is fixed
        // spdlog::warn(kBypassMoPoWarning);
    }
    // Server is not aware of any installed mods.
    else if (!m_pWorld->GetRecordCollection())
    {
        spdlog::error(kMopoRecordsMissing);
        Kill();
        return false;
    }
    else
        spdlog::info("ModPolicy is active");
    return true;
}

void GameServer::BindMessageHandlers()
{
    auto handlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_messageHandlers[T::Opcode] = [this](UniquePtr<ClientMessage>& apMessage, ConnectionId_t aConnectionId)
        {
            auto* pPlayer = m_pWorld->GetPlayerManager().GetByConnectionId(aConnectionId);

            if (!pPlayer)
            {
                spdlog::error("Connection {:x} is not associated with a player.", aConnectionId);
                Kick(aConnectionId);
                return;
            }

            const auto pRealMessage = CastUnique<T>(std::move(apMessage));

            m_pWorld->GetDispatcher().trigger(PacketEvent<T>(pRealMessage.get(), pPlayer));
        };

        return false;
    };

    ClientMessageFactory::Visit(handlerGenerator);

    // Override authentication request
    m_messageHandlers[AuthenticationRequest::Opcode] = [this](UniquePtr<ClientMessage>& apMessage, ConnectionId_t aConnectionId)
    {
        const auto pRealMessage = CastUnique<AuthenticationRequest>(std::move(apMessage));
        HandleAuthenticationRequest(aConnectionId, pRealMessage);
    };

    auto adminHandlerGenerator = [this](auto& x)
    {
        using T = typename std::remove_reference_t<decltype(x)>::Type;

        m_adminMessageHandlers[T::Opcode] = [this](UniquePtr<ClientAdminMessage>& apMessage, ConnectionId_t aConnectionId)
        {
            const auto pRealMessage = CastUnique<T>(std::move(apMessage));
            m_pWorld->GetDispatcher().trigger(AdminPacketEvent<T>(pRealMessage.get(), aConnectionId));
        };

        return false;
    };

    ClientAdminMessageFactory::Visit(adminHandlerGenerator);
}

void GameServer::BindServerCommands()
{
    m_commands.RegisterCommand<>(
        "rfab_save", "Trigger a two-phase coordinated save checkpoint across all connected players",
        [this](Console::ArgStack&)
        {
            const bool started = m_pWorld->GetSaveSnapshotService().TriggerCoordinatedSave(false, nullptr);
            spdlog::get("ConOut")->info("[RFAB] Coordinated save {}", started ? "started" : "skipped (no players or save already in flight)");
        });

    m_commands.RegisterCommand<>(
        "rfab_load", "Roll back server world snapshot and synchronously load latest checkpoint on all clients",
        [this](Console::ArgStack&)
        {
            const bool started = m_pWorld->GetSaveSnapshotService().TriggerRollbackToLatest(false);
            spdlog::get("ConOut")->info("[RFAB] Coordinated checkpoint load {}", started ? "dispatched" : "failed");
        });

    m_commands.RegisterCommand<>(
        "rfab_snapshots", "Show current RFAB checkpoint epoch, slot name, and shared XP ledger",
        [this](Console::ArgStack&)
        {
            const auto& snaps = m_pWorld->GetSaveSnapshotService();
            const auto& ledger = m_pWorld->GetRfabProgressionService().GetLedger();
            spdlog::get("ConOut")->info(
                "[RFAB] Snapshots={} LastEpoch={} LastSlot='{}' SharedXP={} ReadBooks={} SyncedGlobals={}",
                snaps.GetSnapshotCount(),
                snaps.GetLastCommittedEpoch(),
                snaps.GetLastCommittedSlot(),
                ledger.GetTotalSharedXp(),
                ledger.GetReadBooks().size(),
                ledger.GetSyncedGlobals().size());
        });

    m_commands.RegisterCommand<>(
        "uptime", "Show how long the server has been running for",
        [this](Console::ArgStack&)
        {
            Uptime uptime = GetUptime();
            spdlog::get("ConOut")->info("Server uptime: {}w {}d {}h {}m", uptime.weeks, uptime.days, uptime.hours, uptime.minutes);
        });

    m_commands.RegisterCommand<>(
        "players", "List all players on this server",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            uint32_t count = m_pWorld->GetPlayerManager().Count();
            if (count == 0)
            {
                out->warn("No players on here. Invite some friends!");
                return;
            }

            out->info("<------Players-({})--->", count);
            for (Player* pPlayer : m_pWorld->GetPlayerManager())
            {
                out->info("{}: {}", pPlayer->GetId(), pPlayer->GetUsername().c_str());
            }
        });

    m_commands.RegisterCommand<>(
        "mods", "List all installed mods on this server",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            auto& mods = m_pWorld->ctx().at<ModsComponent>().GetServerMods();
            if (mods.size() == 0)
            {
                out->warn("No mods installed");
                return;
            }

            out->info("<------Mods-({})--->", mods.size());
            for (auto& it : mods)
            {
                out->info(it.first);
            }
        });

    m_commands.RegisterCommand<>(
        "resources", "List all loaded resources on the server",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            if (!m_pResources || m_pResources->GetManifests().size() == 0)
            {
                out->warn("No resources loaded");
                return;
            }

            out->info("<------Resources-({})--->", m_pResources->GetManifests().size());
            m_pResources->ForEachManifest([&](const auto& aManifest) { out->info("{} -> {}", aManifest.Name.c_str(), aManifest.Description.c_str()); });
        });

    m_commands.RegisterCommand<>("quit", "Stop the server", [&](Console::ArgStack&) { Kill(); });

    m_commands.RegisterCommand<int64_t, int64_t>(
        "SetTime", "Set ingame hour and minute",
        [&](Console::ArgStack& aStack)
        {
            auto out = spdlog::get("ConOut");

            auto hour = aStack.Pop<int64_t>();
            auto minute = aStack.Pop<int64_t>();
            auto timescale = m_pWorld->GetCalendarService().GetTimeScale();

            bool time_set_successfully = m_pWorld->GetCalendarService().SetTime(hour, minute, timescale);

            if (time_set_successfully)
            {
                out->info("Time set to {:02}:{:02}", hour, minute);
            }
            else
            {
                out->error("Hour must be between 0-23 and minute must be between 0-59");
            }
        });

    m_commands.RegisterCommand<int64_t, int64_t, int64_t>(
        "SetDate", "Set ingame day, month, and year",
        [&](Console::ArgStack& aStack)
        {
            auto out = spdlog::get("ConOut");

            auto day = aStack.Pop<int64_t>();
            auto month = aStack.Pop<int64_t>();
            auto year = aStack.Pop<int64_t>();

            bool time_set_successfully = m_pWorld->GetCalendarService().SetDate(day, month, year);

            if (time_set_successfully)
            {
                out->info("Time set to {:02}/{:02}:{:02}", month, day, year);
            }
            else
            {
                out->error("Day must be between 0 and 31, month must be between 0 and 11, and year must be between 0 and 999.");
            }
        });

    m_commands.RegisterCommand<std::string>(
        "AddAdmin", "Add admin privileges to player",
        [&](Console::ArgStack& aStack)
        {
            auto out = spdlog::get("ConOut");

            const auto& cUsername = aStack.Pop<String>();
            if (GetAdminByUsername(cUsername))
            {
                out->info("{} is already an admin", cUsername.c_str());
                return;
            }

            auto* pPlayer = PlayerManager::Get()->GetByUsername(cUsername);
            if (pPlayer)
            {
                AddAdminSession(pPlayer->GetConnectionId());
                out->info("{} admin privileges added", cUsername.c_str());
            }
            else
            {
                // retry after sanitizing username
                String backupUsername = SanitizeUsername(cUsername);
                pPlayer = PlayerManager::Get()->GetByUsername(backupUsername);

                if (pPlayer)
                {
                    AddAdminSession(pPlayer->GetConnectionId());
                    out->info("{} admin privileges added", cUsername.c_str());
                }
                else
                {
                    out->warn("{} is not a valid player", backupUsername.c_str());
                }
            }
        });
    m_commands.RegisterCommand<std::string>(
        "RemoveAdmin", "Remove admin privileges from player",
        [&](Console::ArgStack& aStack)
        {
            auto out = spdlog::get("ConOut");

            const auto& cUsername = aStack.Pop<String>();
            auto* pPlayer = GetAdminByUsername(cUsername);

            if (pPlayer)
            {
                RemoveAdminSession(pPlayer->GetConnectionId());
                out->info("{} admin privileges revoked", cUsername.c_str());
            }
            else
            {
                // retry after sanitizing username
                String backupUsername = SanitizeUsername(cUsername);
                pPlayer = GetAdminByUsername(backupUsername);

                if (pPlayer)
                {
                    RemoveAdminSession(pPlayer->GetConnectionId());
                    out->info("{} admin privileges revoked", cUsername.c_str());
                }
                else
                {
                    out->warn("{} is not an admin", backupUsername.c_str());
                }
            }
        });
    m_commands.RegisterCommand<>(
        "admins", "List all admins",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            if (m_adminSessions.size() == 0)
            {
                out->warn("No admins");
                return;
            }

            String output = "Admins: ";
            bool _first = true;

            for (const auto& cAdminSession : m_adminSessions)
            {
                auto* pPlayer = PlayerManager::Get()->GetByConnectionId(cAdminSession);

                if (!pPlayer)
                {
                    out->error("Admin session not found: {}", cAdminSession);
                    continue;
                }

                const auto& cUsername = pPlayer->GetUsername();

                if (_first)
                {
                    _first = false;
                }
                else
                {
                    output += ", ";
                }
                output += cUsername;
            }

            out->info("{}", output.c_str());
        });

    m_commands.RegisterCommand<>(
        "savecheckpoint", "Trigger a two-phase coordinated RFAB multiplayer checkpoint across all connected players",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            if (!m_pWorld)
                return;
            const bool started = m_pWorld->GetSaveSnapshotService().TriggerCoordinatedSave(false, nullptr);
            out->info("[RFAB] savecheckpoint started={}", started);
        });

    m_commands.RegisterCommand<>(
        "rollbackcheckpoint", "Synchronously roll back server state & all connected clients to the latest checkpoint",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            if (!m_pWorld)
                return;
            const bool ok = m_pWorld->GetSaveSnapshotService().TriggerRollbackToLatest(false);
            out->info("[RFAB] rollbackcheckpoint triggered={}", ok);
        });

    m_commands.RegisterCommand<>(
        "dumpflightstate", "Dump RFAB co-op WorldEpoch, 256-bucket state digest, and recent flight recorder events",
        [&](Console::ArgStack&)
        {
            auto out = spdlog::get("ConOut");
            if (!m_pWorld)
                return;
            const auto& snapSvc = m_pWorld->GetSaveSnapshotService();
            const auto digest = snapSvc.BuildLiveWorldDigest();
            const auto recent = snapSvc.GetCoordinator().GetFlightRing().SnapshotRecent(12);
            out->info(
                "[RFAB Flight] WorldEpoch={} LastSaveEpoch={} Slot='{}' Snapshots={} Keys={} Digest={} FlightRecords={}",
                snapSvc.GetCurrentWorldEpoch(),
                snapSvc.GetLastCommittedEpoch(),
                snapSvc.GetLastCommittedSlot().empty() ? "none" : snapSvc.GetLastCommittedSlot().c_str(),
                snapSvc.GetSnapshotCount(),
                digest.Size(),
                digest.ComputeHexDigest().c_str(),
                snapSvc.GetCoordinator().GetFlightRing().TotalRecorded());
            for (const auto& rec : recent)
            {
                out->info(
                    "  #{} ts={}ms worldEpoch={} kind={} subj={:X} aux={}",
                    rec.Sequence,
                    rec.TimestampMs,
                    rec.WorldEpoch,
                    static_cast<uint32_t>(rec.Kind),
                    rec.SubjectId,
                    rec.AuxValue);
            }
        });
}

/* Update Info fields from user facing CVARS.*/
void GameServer::UpdateInfo()
{
    const String cServerName = sServerName.c_str();

    if (cServerName.length() > kMaxServerNameLength)
    {
        spdlog::error("sServerName is longer than the limit of {} characters/bytes, and has been cut short", kMaxServerNameLength);
        m_info.name = cServerName.substr(0U, kMaxServerNameLength);
    }
    else
    {
        m_info.name = cServerName;
    }

    m_info.desc = "";
    m_info.icon_url = "";
    m_info.tagList = "";
    m_info.tick_rate = GetUserTickRate();
}

void GameServer::UpdateTimeScale()
{
    auto timescale = uTimeScale.value_as<float>();

    bool timescale_set_successfully = m_pWorld->GetCalendarService().SetTimeScale(timescale);

    if (!timescale_set_successfully)
    {
        spdlog::warn("TimeScale is invalid (should be from 0 to 1000, current value is {}), setting TimeScale to 20 (default)", timescale);

        uTimeScale = 20u;
    }
}

void GameServer::OnUpdate()
{
    const auto cNow = std::chrono::high_resolution_clock::now();
    const auto cDelta = cNow - m_lastFrameTime;
    m_lastFrameTime = cNow;

    const auto cDeltaSeconds = std::chrono::duration_cast<std::chrono::duration<float>>(cDelta).count();

    auto& dispatcher = m_pWorld->GetDispatcher();

    dispatcher.trigger(UpdateEvent{cDeltaSeconds});

    if (m_requestStop)
        Close();
}

void GameServer::OnConsume(const void* apData, const uint32_t aSize, const ConnectionId_t aConnectionId)
{
    ViewBuffer buf((uint8_t*)apData, aSize);
    Buffer::Reader reader(&buf);

    // TODO: ClientAdminMessageFactory
    /*if (m_adminSessions.contains(aConnectionId)) [[unlikely]]
    {
        const ClientAdminMessageFactory factory;
        auto pMessage = factory.Extract(reader);
        if (!pMessage)
        {
            spdlog::error("Couldn't parse packet from {:x}", aConnectionId);
            return;
        }

        m_adminMessageHandlers[pMessage->GetOpcode()](pMessage, aConnectionId);
    }
    else
    {*/
    const ClientMessageFactory factory;
    auto pMessage = factory.Extract(reader);
    if (!pMessage)
    {
        spdlog::error("Couldn't parse packet from {:x}", aConnectionId);
        return;
    }

    const auto opcode = pMessage->GetOpcode();
    if (opcode >= std::size(m_messageHandlers) || !m_messageHandlers[opcode])
    {
        spdlog::error("Unhandled or out-of-bounds opcode {} from {:x}", opcode, aConnectionId);
        return;
    }

    m_messageHandlers[opcode](pMessage, aConnectionId);
    //}
}

void GameServer::OnConnection(const ConnectionId_t aHandle)
{
    spdlog::info("Connection received {:x}", aHandle);
    UpdateTitle();
}

void GameServer::OnDisconnection(const ConnectionId_t aConnectionId, EDisconnectReason aReason)
{
    m_adminSessions.erase(aConnectionId);

    auto* pPlayer = m_pWorld->GetPlayerManager().GetByConnectionId(aConnectionId);

    spdlog::info("Connection ended {:x} - '{}' disconnected", aConnectionId, (pPlayer != NULL ? pPlayer->GetUsername().c_str() : "NULL"));

    m_pWorld->GetScriptService().HandlePlayerQuit(aConnectionId, aReason);

    if (pPlayer)
    {
        if (const auto& cell = pPlayer->GetCellComponent())
        {
            const auto oldCell = cell.Cell;
            pPlayer->SetCellComponent(CellIdComponent{{}, {}, {}});
            m_pWorld->GetDispatcher().trigger(PlayerLeaveCellEvent(oldCell));
        }

        m_pWorld->GetDispatcher().trigger(PlayerLeaveEvent(pPlayer));

        NotifyPlayerLeft notify{};
        notify.PlayerId = pPlayer->GetId();
        notify.Username = pPlayer->GetUsername();
        SendToPlayers(notify);

        entt::entity playerCharacter = pPlayer->GetCharacter().value_or(static_cast<entt::entity>(0));

        // Cleanup all entities that we own
        auto ownerView = m_pWorld->view<OwnerComponent>();
        for (auto entity : ownerView)
        {
            if (entity == playerCharacter)
            {
                m_pWorld->GetDispatcher().enqueue(CharacterRemoveEvent(World::ToInteger(entity)));
                continue;
            }

            const auto& [ownerComponent] = ownerView.get(entity);
            if (ownerComponent.GetOwner() == pPlayer)
            {
                const auto* pCharComp = m_pWorld->try_get<CharacterComponent>(entity);
                if (pCharComp && (pCharComp->IsPlayer() || pCharComp->IsPlayerSummon()))
                {
                    m_pWorld->GetDispatcher().enqueue(CharacterRemoveEvent(World::ToInteger(entity)));
                }
                else
                {
                    m_pWorld->GetDispatcher().enqueue(OwnershipTransferEvent(entity));
                }
            }
        }

        m_pWorld->GetDispatcher().update();

        m_pWorld->GetPlayerManager().Remove(pPlayer);
        UpdateSettings();
    }

    UpdateTitle();
}

void GameServer::Send(const ConnectionId_t aConnectionId, const ServerMessage& acServerMessage, const TiltedPhoques::EPacketFlags aPacketFlags) const
{
    static thread_local TiltedPhoques::ScratchAllocator s_allocator{1 << 18};

    Buffer buffer(1 << 20);
    Buffer::Writer writer(&buffer);
    writer.WriteBits(0, 8); // Skip the first byte as it is used by packet

    acServerMessage.Serialize(writer);

    TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), static_cast<uint32_t>(writer.Size()));
    Server::Send(aConnectionId, &packet, aPacketFlags);

    s_allocator.Reset();
}

void GameServer::Send(ConnectionId_t aConnectionId, const ServerAdminMessage& acServerMessage) const
{
    static thread_local TiltedPhoques::ScratchAllocator s_allocator{1 << 18};

    Buffer buffer(1 << 20);
    Buffer::Writer writer(&buffer);
    writer.WriteBits(0, 8); // Skip the first byte as it is used by packet

    acServerMessage.Serialize(writer);

    TiltedPhoques::PacketView packet(reinterpret_cast<char*>(buffer.GetWriteData()), static_cast<uint32_t>(writer.Size()));
    Server::Send(aConnectionId, &packet);

    s_allocator.Reset();
}

void GameServer::SendToLoaded(const ServerMessage& acServerMessage) const
{
    for (Player* pPlayer : m_pWorld->GetPlayerManager())
    {
        if (pPlayer->GetCellComponent())
            pPlayer->Send(acServerMessage);
    }
}

void GameServer::SendToPlayers(const ServerMessage& acServerMessage, const Player* apExcludedPlayer) const
{
    for (Player* pPlayer : m_pWorld->GetPlayerManager())
    {
        if (pPlayer != apExcludedPlayer)
            pPlayer->Send(acServerMessage);
    }
}

// NOTE: this doesn't check objects in range, only characters in range.
bool GameServer::SendToPlayersInRange(const ServerMessage& acServerMessage, const entt::entity acOrigin, const Player* apExcludedPlayer) const
{
    if (!m_pWorld->valid(acOrigin))
    {
        spdlog::error("Entity is invalid: {:X}", World::ToInteger(acOrigin));
        return false;
    }

    const auto view = m_pWorld->view<CellIdComponent>();
    const auto it = view.find(acOrigin);

    if (it == view.end())
    {
        spdlog::warn("Cell component not found for entity {:X}", World::ToInteger(acOrigin));
        return false;
    }

    const auto& cellComponent = view.get<CellIdComponent>(*it);

    bool isDragon = false;
    if (const auto* characterComponent = m_pWorld->try_get<CharacterComponent>(acOrigin))
        isDragon = characterComponent->IsDragon();

    for (Player* pPlayer : m_pWorld->GetPlayerManager())
    {
        if (cellComponent.IsInRange(pPlayer->GetCellComponent(), isDragon) && pPlayer != apExcludedPlayer)
            pPlayer->Send(acServerMessage);
    }

    return true;
}

void GameServer::SendToLeader(const ServerMessage& acServerMessage, const PartyComponent& acPartyComponent, const Player* apLeader) const
{
    if (!acPartyComponent.JoinedPartyId.has_value())
    {
        spdlog::warn("Party does not exist, canceling broadcast.");
        return;
    }

    if (const_cast<Player*>(apLeader)->GetParty().JoinedPartyId != acPartyComponent.JoinedPartyId)
    {
        spdlog::warn("Specified party leader belongs to different party? Canceling send");
        return;
    }

    apLeader->Send(acServerMessage);
}

void GameServer::SendToParty(const ServerMessage& acServerMessage, const PartyComponent& acPartyComponent, const Player* apExcludeSender) const
{
    if (!acPartyComponent.JoinedPartyId.has_value())
    {
        spdlog::warn("Party does not exist, canceling broadcast.");
        return;
    }

    for (Player* pPlayer : m_pWorld->GetPlayerManager())
    {
        if (pPlayer == apExcludeSender)
            continue;

        const auto& partyComponent = pPlayer->GetParty();
        if (partyComponent.JoinedPartyId == acPartyComponent.JoinedPartyId)
        {
            pPlayer->Send(acServerMessage);
        }
    }
}

void GameServer::SendToPartyInRange(const ServerMessage& acServerMessage, const PartyComponent& acPartyComponent, const entt::entity acOrigin, const Player* apExcludeSender) const
{
    if (!acPartyComponent.JoinedPartyId.has_value())
    {
        spdlog::warn("Party does not exist, canceling broadcast.");
        return;
    }

    const auto view = m_pWorld->view<CellIdComponent>();
    const auto it = view.find(acOrigin);

    if (it == view.end())
    {
        spdlog::warn("Cell component not found for entity {:X}", World::ToInteger(acOrigin));
        return;
    }

    const auto& cellComponent = view.get<CellIdComponent>(*it);

    for (Player* pPlayer : m_pWorld->GetPlayerManager())
    {
        if (pPlayer == apExcludeSender)
            continue;

        if (!cellComponent.IsInRange(pPlayer->GetCellComponent(), false))
            continue;

        if (pPlayer->GetParty().JoinedPartyId != acPartyComponent.JoinedPartyId)
            continue;

        pPlayer->Send(acServerMessage);
    }
}

static String PrettyPrintModList(const Vector<Mods::Entry>& acMods)
{
    String text;
    for (size_t i = 0; i < acMods.size(); i++)
    {
        text += acMods[i].Filename;
        if (i != (acMods.size() - 1))
            text += ", ";
    }

    return text;
}

bool GameServer::ValidateAuthParams(ConnectionId_t aConnectionId, const UniquePtr<AuthenticationRequest>& acRequest)
{
    return false;
}

void GameServer::HandleAuthenticationRequest(const ConnectionId_t aConnectionId, const UniquePtr<AuthenticationRequest>& acRequest)
{
    if (m_pWorld->GetPlayerManager().GetByConnectionId(aConnectionId) != nullptr)
    {
        spdlog::warn("Duplicate AuthenticationRequest from already authenticated connection {:x}; kicking.", aConnectionId);
        Kick(aConnectionId);
        return;
    }

    const auto info = GetConnectionInfo(aConnectionId);

    char remoteAddress[48]{};
    info.m_addrRemote.ToString(remoteAddress, 48, false);

    AuthenticationResponse serverResponse;
    serverResponse.Version = BUILD_COMMIT;

    using RT = AuthenticationResponse::ResponseType;
    auto sendKick = [&](const RT type)
    {
        serverResponse.Type = type;
        Send(aConnectionId, serverResponse);
        // the previous message is a lingering kick, it still gets delivered.
        Kick(aConnectionId);
    };
#if 1
    // Accept exact commit matches AND any builds within the same vMAJOR.MINOR protocol family
    // (e.g. v1.0.0-6-g383738a6 <-> v1.0.0-8-g..., preventing client hotfixes from breaking co-op connections).
    if (!RfabCompat::AreBuildVersionsProtocolCompatible(BUILD_COMMIT, acRequest->Version.c_str()))
    {
        spdlog::info("New player {:x} '{}' tried to connect with incompatible protocol version {} (server {})",
                     aConnectionId, remoteAddress, acRequest->Version.c_str(), BUILD_COMMIT);
        sendKick(RT::kWrongVersion);
        return;
    }
    if (acRequest->Version != BUILD_COMMIT)
    {
        spdlog::info("Player {:x} '{}' connected with protocol-compatible build {} (server {})",
                     aConnectionId, remoteAddress, acRequest->Version.c_str(), BUILD_COMMIT);
    }
#endif

    if (m_pWorld->GetPlayerManager().Count() >= uMaxPlayerCount.value_as<uint32_t>())
    {
        sendKick(RT::kServerFull);
        return;
    }

    bool skseProblem = !bAllowSKSE && acRequest->SKSEActive;
    bool mo2Problem = !bAllowMO2 && acRequest->MO2Active;

    if (skseProblem || mo2Problem)
    {
        TiltedPhoques::String response;
        if (skseProblem)
            response += "SKSE ";
        if (mo2Problem)
            response += "MO2 ";

        spdlog::info("New player {:x} '{}' tried to connect, but {}{} disallowed - Kicked.", aConnectionId, remoteAddress, response.c_str(), skseProblem && mo2Problem ? "are" : "is");

        serverResponse.SKSEActive = acRequest->SKSEActive;
        serverResponse.MO2Active = acRequest->MO2Active;
        sendKick(RT::kClientModsDisallowed);
        return;
    }

    bool adminPasswordUsed = acRequest->Token == sAdminPassword.value() && !sAdminPassword.empty();

    // check if the proper server password was supplied.
    if (acRequest->Token == sPassword.value() || adminPasswordUsed)
    {
        if (adminPasswordUsed)
        {
            m_adminSessions.insert(aConnectionId);
            spdlog::warn("New admin session for {:x} '{}'", aConnectionId, remoteAddress);
        }

        Mods& responseList = serverResponse.UserMods;
        auto& modsComponent = m_pWorld->ctx().at<ModsComponent>();

        if (IsMoPoActive())
        {
            // mods that exist on the client, but not on the server
            // modscomponent contains a list filled in by the recordcollection
            Mods modsToRemove;

            const auto& userMods = acRequest->UserMods.ModList;
            for (const Mods::Entry& mod : userMods)
            {
                // if the client has more mods than the server..
                if (!modsComponent.IsInstalled(mod.Filename))
                {
                    modsToRemove.ModList.push_back(mod);
                }
            }

            // TODO(Vince): if you have a better to do this than two for loops
            // let me know!
            // Also, for the future, lets think about a mode that allows more than the server installed mods
            // but requires essential mods?

            // mods that may exist on the server, but not on the client
            auto equalsAsciiIgnoreCase = [](const String& aLhs, const String& aRhs) noexcept
            {
                if (aLhs.size() != aRhs.size())
                    return false;
                for (size_t idx = 0; idx < aLhs.size(); ++idx)
                {
                    char c1 = aLhs[idx];
                    char c2 = aRhs[idx];
                    if (c1 >= 'A' && c1 <= 'Z')
                        c1 = static_cast<char>(c1 - 'A' + 'a');
                    if (c2 >= 'A' && c2 <= 'Z')
                        c2 = static_cast<char>(c2 - 'A' + 'a');
                    if (c1 != c2)
                        return false;
                }
                return true;
            };

            for (const auto& entry : modsComponent.GetServerMods())
            {
                const auto it = std::find_if(userMods.begin(), userMods.end(), [&](const Mods::Entry& it) {
                    return equalsAsciiIgnoreCase(it.Filename, entry.first);
                });

                if (it == userMods.end())
                {
                    Mods::Entry removeEntry;
                    removeEntry.Filename = entry.first;
                    removeEntry.Id = 0;
                    modsToRemove.ModList.push_back(removeEntry);
                }
            }

            if (modsToRemove.ModList.size() > 0)
            {
                String text = PrettyPrintModList(modsToRemove.ModList);
                // "ModPolicy: refusing connection {:x} because essential mods are missing: {}"
                // for future reference ^
                spdlog::info("ModPolicy: refusing connection {:x} because the following mods are installed on the client: {}", aConnectionId, text.c_str());

                serverResponse.UserMods.ModList = std::move(modsToRemove.ModList);
                sendKick(RT::kModsMismatch);
                return;
            }
        }

        // Note: to lower traffic we only send the mod ids the user can fix in order as other ids will lead to a
        // null form id anyway
        Vector<String> playerMods;
        Vector<uint16_t> playerModsIds;

        size_t i = 0;
        for (auto& mod : acRequest->UserMods.ModList)
        {
            const uint32_t id = mod.IsLite ? modsComponent.AddLite(mod.Filename) : modsComponent.AddStandard(mod.Filename);

            Mods::Entry entry;
            entry.Filename = mod.Filename;
            entry.Id = static_cast<uint16_t>(id);
            entry.IsLite = mod.IsLite;

            playerMods.push_back(mod.Filename);
            playerModsIds.push_back(entry.Id);
            responseList.ModList.push_back(entry);
        }

        Player* pPlayer = m_pWorld->GetPlayerManager().Create(aConnectionId);
        pPlayer->SetEndpoint(remoteAddress);
        pPlayer->SetDiscordId(acRequest->DiscordId);
        pPlayer->SetUsername(std::move(acRequest->Username));
        pPlayer->SetMods(playerMods);
        pPlayer->SetModIds(playerModsIds);
        pPlayer->SetLevel(acRequest->Level);

        // this event is shit, needs to be fixed, i know
        auto [canceled, reason] = m_pWorld->GetScriptService().HandlePlayerJoin(aConnectionId);
        if (canceled)
        {
            spdlog::info("New player {:x} has a been rejected because \"{}\".", aConnectionId, reason.c_str());
            Kick(aConnectionId);
            m_pWorld->GetPlayerManager().Remove(pPlayer);
            return;
        }

        serverResponse.PlayerId = pPlayer->GetId();

        auto modList = PrettyPrintModList(acRequest->UserMods.ModList);
        spdlog::info("New player '{}' [{:x}] connected with {} mods\n\t: {}", pPlayer->GetUsername().c_str(), aConnectionId, acRequest->UserMods.ModList.size(), modList.c_str());

        serverResponse.Settings = GetSettings();

        serverResponse.Type = AuthenticationResponse::ResponseType::kAccepted;
        Send(aConnectionId, serverResponse);

        // Broadcast updated player-count health scaling to all already-connected peers as well
        for (auto* pExisting : m_pWorld->GetPlayerManager())
        {
            if (pExisting != pPlayer)
            {
                NotifySettingsChange settingsNotify{};
                settingsNotify.Settings = serverResponse.Settings;
                pExisting->Send(settingsNotify);
            }
        }

        uint32_t startId = 0;
        auto initStringCache = StringCache::Get().Serialize(startId);

        pPlayer->SetStringCacheId(startId);

        Send(aConnectionId, initStringCache);

        for (auto* pOtherPlayer : m_pWorld->GetPlayerManager())
        {
            if (pOtherPlayer == pPlayer)
                continue;

            NotifyPlayerJoined notify{};
            notify.PlayerId = pOtherPlayer->GetId();
            notify.Username = pOtherPlayer->GetUsername();

            auto& cellComponent = pOtherPlayer->GetCellComponent();
            notify.WorldSpaceId = cellComponent.WorldSpaceId;
            notify.CellId = cellComponent.Cell;

            notify.Level = pOtherPlayer->GetLevel();

            spdlog::debug("[GameServer] New notify player {:x} {}", notify.PlayerId, notify.Username.c_str());

            Send(pPlayer->GetConnectionId(), notify);
        }

        m_pWorld->GetDispatcher().trigger(PlayerJoinEvent(pPlayer, acRequest->WorldSpaceId, acRequest->CellId, acRequest->PlayerTime));
    }
    /*else if (acRequest->Token == sAdminPassword.value() && !sAdminPassword.empty())
    {
        AdminSessionOpen response;
        Send(aConnectionId, response);

        m_adminSessions.insert(aConnectionId);
        spdlog::warn("New admin session for {:x} '{}'", aConnectionId, remoteAddress);
    } */
    else
    {
        spdlog::info("New player {:x} '{}' has a bad password, kicking.", aConnectionId, remoteAddress);
        sendKick(RT::kWrongPassword);
    }
}

void GameServer::UpdateSettings()
{
    NotifySettingsChange notify{};
    notify.Settings = GetSettings();

    SendToPlayers(notify);
}

GameServer::Uptime GameServer::GetUptime() const noexcept
{
    auto duration = std::chrono::high_resolution_clock::now() - m_startTime;
    auto weeks = std::chrono::duration_cast<std::chrono::weeks>(duration);
    duration -= weeks;
    auto days = std::chrono::duration_cast<std::chrono::days>(duration);
    duration -= days;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
    duration -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
    return {weeks.count(), days.count(), hours.count(), minutes.count()};
}

void GameServer::UpdateTitle() const
{
    const auto name = m_info.name.empty() ? "Private server" : m_info.name;
    const char* playerText = GetClientCount() <= 1 ? " player" : " players";

    const auto title = fmt::format("{} - {} {} - {} Ticks - " BUILD_BRANCH "@" BUILD_COMMIT, name.c_str(), GetClientCount(), playerText, GetTickRate());

#if TP_PLATFORM_WINDOWS
    SetConsoleTitleA(title.c_str());
#else
    std::cout << "\033]0;" << title << "\007";
#endif
}

Player* GameServer::GetAdminByUsername(const String& acUsername) const noexcept
{
    for (auto session : m_adminSessions)
    {
        if (auto* pPlayer = PlayerManager::Get()->GetByConnectionId(session))
        {
            if (pPlayer->GetUsername() == acUsername)
                return pPlayer;
        }
    }

    return nullptr;
}

Player const* GameServer::GetAdminByUsername(const String& acUsername) noexcept
{
    for (auto session : m_adminSessions)
    {
        if (auto const* pPlayer = PlayerManager::Get()->GetByConnectionId(session))
        {
            if (pPlayer->GetUsername() == acUsername)
                return pPlayer;
        }
    }

    return nullptr;
}

String GameServer::SanitizeUsername(const String& acUsername) const noexcept
{
    String username = acUsername;

    // space in username handling | "_" -> space
    std::ranges::replace(username, '_', ' ');

    return username;
}
