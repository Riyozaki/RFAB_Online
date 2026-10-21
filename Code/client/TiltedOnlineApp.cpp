#include <TiltedOnlinePCH.h>

#include <TiltedOnlineApp.h>

#include <DInputHook.hpp>
#include <dinput.h>
#include <WindowsHook.hpp>

#include <World.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/OverlayService.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>

#include <ScriptExtender.h>
#include <NvidiaUtil.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <utility>

using TiltedPhoques::Debug;

namespace
{
// Log level comes from the environment so a tester can raise verbosity without
// rebuilding: TP_LOG_LEVEL=trace|debug|info|warn|error|critical|off.
spdlog::level::level_enum GetConfiguredLogLevel(std::string& aLevelName)
{
    static constexpr std::pair<std::string_view, spdlog::level::level_enum> kLevels[]{
        {"trace", spdlog::level::trace}, {"debug", spdlog::level::debug}, {"info", spdlog::level::info},
        {"warn", spdlog::level::warn},   {"error", spdlog::level::error}, {"critical", spdlog::level::critical},
        {"off", spdlog::level::off},
    };

    wchar_t buffer[32]{};
    const auto length = GetEnvironmentVariableW(L"TP_LOG_LEVEL", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer))
        return spdlog::level::info;

    const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string name(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), name.data(), size, nullptr, nullptr);

    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char aChar) { return static_cast<char>(std::tolower(aChar)); });

    for (const auto& [text, level] : kLevels)
    {
        if (name == text)
        {
            aLevelName = std::string(text);
            return level;
        }
    }

    aLevelName = fmt::format("info (unknown TP_LOG_LEVEL=\"{}\")", name);
    return spdlog::level::info;
}
} // namespace

TiltedOnlineApp::TiltedOnlineApp()
{
    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);

    auto logPath = TiltedPhoques::GetPath() / "logs";

    std::error_code ec;
    create_directory(logPath, ec);

    std::string levelName;
    const auto level = GetConfiguredLogLevel(levelName);

    auto rotatingLogger = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath / "tp_client.log", 1048576 * 5, 3);
    rotatingLogger->set_level(level);
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(level);
    auto logger = std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{console, rotatingLogger});
    logger->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [tid %t] %$ %v");
    logger->set_level(level);
    spdlog::flush_every(std::chrono::seconds(1));
    // Warnings and worse are on disk even if the process dies within a second.
    spdlog::flush_on(spdlog::level::warn);
    // A failing sink must not terminate the game; report it to the debugger.
    spdlog::set_error_handler([](const std::string& acMessage) { OutputDebugStringA(("spdlog: " + acMessage + "\n").c_str()); });
    set_default_logger(logger);

    if (ec)
        spdlog::warn("Unable to create the log directory {}: {}", logPath.string(), ec.message());

    spdlog::info("Logging to {} (level {}, 5 MB x 3 files)", (logPath / "tp_client.log").string(), levelName);
}

TiltedOnlineApp::~TiltedOnlineApp() = default;

void* TiltedOnlineApp::GetMainAddress() const
{
    POINTER_SKYRIMSE(void, winMain, 36544);

    return winMain.GetPtr();
}

bool TiltedOnlineApp::BeginMain()
{
    World::Create();
    World::Get().ctx().at<DiscordService>().Init();
    World::Get().ctx().emplace<RenderSystemD3D11>(World::Get().ctx().at<OverlayService>(), World::Get().ctx().at<ImguiService>());

    LoadScriptExender();

    // TODO: Figure out a way to un-blacklist NvCamera64.dll (see DllBlocklist.cpp). Then this hack can be removed
    if (IsNvidiaOverlayLoaded())
        ApplyNvidiaFix();

    return true;
}

bool TiltedOnlineApp::EndMain()
{
    UninstallHooks();
    if (m_pDevice)
        m_pDevice->Release();

    return true;
}

void TiltedOnlineApp::Update()
{
    // Reverting a change that used to be here to disable bUseFaceGenPreprocessedHeads==true (which is 
    // the default) handling. Extensive testing over months by multiple parties showed that enabling 
    // the flag introduces no issues WITH PROPERLY GENERATED CHARACTERS (in-game character generation 
    // or showracemenu). The shortcut of  "coc riverwood" from the main menu skips proper character generation.
    // 
    // Plus, having it on  has some benefits like helping with neck seams. Comment to avoid revisiting.
    // 
    // There are still some issues to track down, like hair color and maybe face tint not syncing correctly,
    // but they are unrelated and unchanged by this flag.
    // 
 
    // Make sure the window stays active
    POINTER_SKYRIMSE(uint32_t, bAlwaysActive, 380768);

    *bAlwaysActive = 1;

    World::Get().Update();
}

bool TiltedOnlineApp::Attach()
{
    TiltedPhoques::Debug::OnAttach();

    // TiltedPhoques::Nop(0x1405D3FA1, 6);
    return true;
}

bool TiltedOnlineApp::Detach()
{
    TiltedPhoques::Debug::OnDetach();
    return true;
}

void TiltedOnlineApp::InstallHooks2()
{
    TiltedPhoques::Initializer::RunAll();

    TiltedPhoques::DInputHook::Install();
    TiltedPhoques::DInputHook::Get().SetToggleKeys({DIK_F2, DIK_RCONTROL});
}

void TiltedOnlineApp::UninstallHooks()
{
}

void TiltedOnlineApp::ApplyNvidiaFix() noexcept
{
    auto d3dFeatureLevelOut = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = CreateEarlyDxDevice(&m_pDevice, &d3dFeatureLevelOut);
    if (FAILED(hr))
        spdlog::error("D3D11CreateDevice failed. Detected an NVIDIA GPU, error code={0:x}", hr);

    if (d3dFeatureLevelOut < D3D_FEATURE_LEVEL_11_0)
        spdlog::warn("Unexpected D3D11 feature level detected (< 11.0), may cause issues");
}
