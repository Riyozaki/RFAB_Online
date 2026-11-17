
#include "DediRunner.h"

#include <console/CommandSettingsProvider.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_set>
#include <vector>
#include <base/threading/ThreadUtils.h>
#include <common/RfabModpackCompat.h>
#include <common/Compat/SafeFs.h>

namespace
{
constexpr char kSettingsFileName[] = "STServer.ini";
constexpr char kGenerateConfigArg[] = "--generate-config";

DediRunner* s_pRunner{nullptr};

void EnsureServerLoadOrderReady()
{
    std::error_code ec;
    const fs::path workDir = Compat::SafeFs::CurrentPathOr(fs::path("."));
    const fs::path dataDir = workDir / "Data";
    fs::create_directories(dataDir, ec);
    const fs::path targetLoadOrder = dataDir / "loadorder.txt";

    if (fs::exists(targetLoadOrder, ec) && fs::file_size(targetLoadOrder, ec) > 0 && !ec)
    {
        std::ifstream check(targetLoadOrder);
        std::string firstLine;
        std::getline(check, firstLine);
        if (firstLine.rfind("version https://git-lfs.github.com/spec/v1", 0) != 0)
            return;
    }

    std::vector<fs::path> candidateDirs;
    fs::path probe = workDir;
    for (int depth = 0; depth < 4 && !probe.empty(); ++depth)
    {
        candidateDirs.push_back(probe / "MO2" / "profiles");
        candidateDirs.push_back(probe / "profiles");
        if (probe == probe.parent_path())
            break;
        probe = probe.parent_path();
    }
#ifdef _WIN32
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData)
        candidateDirs.push_back(fs::path(localAppData) / "Skyrim Special Edition");
#endif

    auto writeCleanOrder = [&](const fs::path& profDir) -> bool
    {
        const fs::path pluginsTxt = profDir / "plugins.txt";
        const fs::path loadOrderTxt = profDir / "loadorder.txt";
        if (!fs::exists(pluginsTxt, ec) && !fs::exists(loadOrderTxt, ec))
            return false;

        std::unordered_set<std::string> enabledLowerSet = {
            "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm"
        };
        std::vector<std::string> ordered;
        std::unordered_set<std::string> seenLower;

        auto appendOrdered = [&](const std::string& name)
        {
            const std::string lower = RfabCompat::Detail::ToLowerAscii(name);
            if (!lower.empty() && seenLower.insert(lower).second)
                ordered.push_back(name);
        };

        const bool hasPluginsTxt = fs::exists(pluginsTxt, ec) && !ec;
        const bool hasLoadOrderTxt = fs::exists(loadOrderTxt, ec) && !ec;

        if (hasPluginsTxt)
        {
            std::string rawPlugins;
            if (RfabCompat::Detail::ReadTextFile(pluginsTxt, rawPlugins))
            {
                const auto activeFromPlugins = RfabCompat::ParseActivePluginList(rawPlugins, true);
                for (const auto& name : activeFromPlugins)
                {
                    enabledLowerSet.insert(RfabCompat::Detail::ToLowerAscii(name));
                    if (!hasLoadOrderTxt)
                        appendOrdered(name);
                }
            }
        }

        if (hasLoadOrderTxt)
        {
            std::string rawLoadOrder;
            if (RfabCompat::Detail::ReadTextFile(loadOrderTxt, rawLoadOrder))
            {
                const auto allInOrder = RfabCompat::ParseActivePluginList(rawLoadOrder, false);
                for (const auto& name : allInOrder)
                {
                    const std::string lower = RfabCompat::Detail::ToLowerAscii(name);
                    if (!hasPluginsTxt || enabledLowerSet.count(lower) > 0)
                        appendOrdered(name);
                }
            }
        }

        if (ordered.empty())
            return false;

        if (seenLower.count("skyrimtogether.esp") == 0)
            appendOrdered("SkyrimTogether.esp");

        std::ofstream out(targetLoadOrder, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        for (const auto& entry : ordered)
            out << entry << "\n";
        spdlog::info("[RFAB Server] Auto-synced {} active plugins from '{}' -> Data/loadorder.txt", ordered.size(),
                     Compat::SafeFs::ToUtf8Lossy(profDir));
        return true;
    };

    for (const auto& base : candidateDirs)
    {
        if (!fs::exists(base, ec))
            continue;
        if (writeCleanOrder(base))
            return;
        bool written = false;
        Compat::SafeFs::ForEachEntry(base, [&](const fs::directory_entry& acSub) {
            written = Compat::SafeFs::IsDirectory(acSub) && writeCleanOrder(acSub.path());
            return !written;
        });
        if (written)
            return;
    }
}
} // namespace

// imports
GS_IMPORT void RegisterServerSettings(Console::ConsoleRegistry& conReg);
GS_IMPORT TiltedPhoques::UniquePtr<IGameServerInstance> CreateGameServer(Console::ConsoleRegistry& conReg);
// needs to be global
Console::Setting bConsole{"bConsole", "Enable the console", true};

DediRunner* GetDediRunner() noexcept
{
    return s_pRunner;
}

DediRunner::DediRunner(int argc, char** argv)
    : m_console(KCompilerStopThisBullshit)
{
    s_pRunner = this;

    EnsureServerLoadOrderReady();

    uv_loop_init(&m_loop);

    // settings must be registered before they can be loaded
    RegisterServerSettings(m_console);
    if (!LoadSettings(argc, argv))
        return;

    m_pServerInstance = CreateGameServer(m_console);

    // it is here for now..
    m_pServerInstance->Initialize();
    if (m_useIni && !m_SettingsPath.empty())
        SaveSettingsToIni(m_console, m_SettingsPath);
}

DediRunner::~DediRunner()
{
    if (m_useIni && !m_SettingsPath.empty())
        SaveSettingsToIni(m_console, m_SettingsPath);
    uv_loop_close(&m_loop);
}

bool DediRunner::LoadSettings(int argc, char** argv)
{
    std::error_code ec;
    const fs::path workDir = Compat::SafeFs::CurrentPathOr(fs::path("."));

    // --generate-config [path]: write the default settings and don't start the server
    if (argc > 1 && std::strcmp(argv[1], kGenerateConfigArg) == 0)
    {
        const fs::path path = argc > 2 ? fs::path(argv[2]) : workDir / kConfigPathName / kSettingsFileName;
        if (path.has_parent_path())
            create_directories(path.parent_path(), ec);
        SaveSettingsToIni(m_console, path);
        spdlog::info("Generated {}", Compat::SafeFs::ToUtf8Lossy(path));
        return false;
    }

    // If we have line args load, don't use the ini
    if (argc > 1)
    {
        LoadSettingsFromCommand(m_console, argc, argv);
    }
    else
    {
        m_useIni = true;
        m_SettingsPath = workDir / kConfigPathName / kSettingsFileName;
        bool needsFreshIni = !exists(m_SettingsPath, ec);
        if (!needsFreshIni)
        {
            std::ifstream check(m_SettingsPath);
            std::string firstLine;
            std::getline(check, firstLine);
            if (firstLine.rfind("version https://git-lfs.github.com/spec/v1", 0) == 0)
                needsFreshIni = true;
        }
        if (needsFreshIni)
        {
            create_directories(workDir / kConfigPathName, ec);
            SaveSettingsToIni(m_console, m_SettingsPath);
            return true;
        }
        LoadSettingsFromIni(m_console, m_SettingsPath);
    }

    return true;
}

struct Context
{
    TiltedPhoques::String data;
    DediRunner* instance;
};

void DediRunner::ReadStdin(uv_stream_t* apStream, ssize_t aRead, const uv_buf_t* acpBuffer)
{
    auto* ctx = static_cast<Context*>(apStream->data);

    if (aRead < 0)
    {
        if (aRead == UV_EOF)
            uv_close(reinterpret_cast<uv_handle_t*>(&apStream), nullptr);
    }
    else if (aRead > 0)
    {
        for (auto i = 0; i < aRead; ++i)
        {
            if (acpBuffer->base[i] == '\n')
            {
                ctx->instance->HandleConsole(ctx->data);
                ctx->data = "";
            }
            else
                ctx->data += acpBuffer->base[i];
        }
    }

    // OK to free buffer as write_data copies it.
    if (acpBuffer->base)
        TiltedPhoques::Allocator::GetDefault()->Free(acpBuffer->base);
}

void DediRunner::AllocateBuffer(uv_handle_t* apHandle, size_t aSuggestedSize, uv_buf_t* apBuffer)
{
    *apBuffer = uv_buf_init(static_cast<char*>(TiltedPhoques::Allocator::GetDefault()->Allocate(aSuggestedSize)), static_cast<uint32_t>(aSuggestedSize));
}

void DediRunner::PrintExecutorArrowHack()
{
}

void DediRunner::RunGSThread()
{
    while (m_pServerInstance && m_pServerInstance->IsListening())
    {
        m_pServerInstance->Update();
        if (bConsole)
        {
            uv_run(&m_loop, UV_RUN_NOWAIT);
            if (m_console.Update())
                PrintExecutorArrowHack();
        }
    }
}

void DediRunner::StartTerminalIO()
{
    spdlog::get("ConOut")->info("Server started, type /help for a list of commands.");
    PrintExecutorArrowHack();

    uv_tty_init(&m_loop, &m_tty, 0, 1);
    uv_tty_set_mode(&m_tty, UV_TTY_MODE_NORMAL);

    static Context ctx;
    ctx.instance = this;

    m_tty.data = &ctx;

    uv_read_start(reinterpret_cast<uv_stream_t*>(&m_tty), AllocateBuffer, ReadStdin);
}

void DediRunner::RequestKill()
{
    if (m_pServerInstance)
        m_pServerInstance->Shutdown();

#if defined(_WIN32)
    if (IsDebuggerPresent())
    {
        using namespace std::chrono_literals;
        std::this_thread::sleep_for(300ms);
    }
#endif
}

void DediRunner::HandleConsole(const TiltedPhoques::String& acCommand)
{
    using exr = Console::ConsoleRegistry::ExecutionResult;

    exr r = m_console.TryExecuteCommand(acCommand);

    PrintExecutorArrowHack();

    if (r == exr::kDirty && m_useIni && !m_SettingsPath.empty())
        SaveSettingsToIni(m_console, m_SettingsPath);
}
