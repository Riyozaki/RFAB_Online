// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.

#include <Windows.h>

#include <optional>
#include <shobjidl.h>

#include "TargetConfig.h"
#include "launcher.h"
#include <common/RfabModpackCompat.h>

#include "utils/ComUtils.h"
#include "utils/Registry.h"

namespace oobe
{
using namespace TiltedPhoques;

namespace
{
constexpr wchar_t kBethesdaRegistryPath[] = LR"(Software\Wow6432Node\Bethesda Softworks\)" SHORT_NAME;
constexpr wchar_t kTiltedRegistryPath[] = LR"(Software\TiltedPhoques\TiltedEvolution\)" SHORT_NAME;

std::wstring SuggestTitlePath()
{
    return Registry::ReadString<wchar_t>(HKEY_LOCAL_MACHINE, kBethesdaRegistryPath, L"installed path");
}

std::optional<std::wstring> OpenPathSelectionDialog2(const std::wstring& aPathSuggestion)
{
    // in case some other stuff already tried to unregister the global instance
    ComScope _;

    ComPtr<IFileOpenDialog> pFileDialog;
    auto hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(pFileDialog.GetAddressOf()));
    if (FAILED(hr))
        return std::nullopt;

    pFileDialog->SetTitle(L"Select the game executable (" TARGET_NAME L".exe)");

    static constinit COMDLG_FILTERSPEC rgSpec[] = {{L"Executables", TARGET_NAME L".exe"}};
    pFileDialog->SetFileTypes(1, &rgSpec[0]);

    // pre select suggested folder & exe
    if (!aPathSuggestion.empty())
    {
        ComPtr<IShellItem> pItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(aPathSuggestion.c_str(), nullptr, IID_PPV_ARGS(&pItem))))
        {
            pFileDialog->SetFolder(pItem.Get());
            pFileDialog->SetFileName(TARGET_NAME L".exe");
        }
    }

    hr = pFileDialog->Show(nullptr);
    if (FAILED(hr))
        return std::nullopt;

    ComPtr<IShellItem> pItem;
    hr = pFileDialog->GetResult(pItem.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        PWSTR filePath;
        hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);

        if (SUCCEEDED(hr))
        {
            auto rval = filePath;
            CoTaskMemFree(filePath);
            return rval;
        }
    }

    return std::nullopt;
}
} // namespace

bool SelectInstall(bool aForceSelect)
{
    auto titlePath = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath");
    auto exePath = Registry::ReadString<wchar_t>(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe");

    const std::filesystem::path selfDir = TiltedPhoques::GetPath();

    // Priority 1: If launched from inside a Mod Organizer 2 tree (<MO2>/mods/RFAB_Online/SkyrimTogetherReborn),
    // read MO2's active `ModOrganizer.ini` (`gamePath=` / `%BASE_DIR%`) FIRST so LC->gamePath and CWD ALWAYS match
    // MO2's USVFS-virtualized Data directory even after reinstalling clean Skyrim to a new path or copying a friend's mod archive.
    if (!aForceSelect)
    {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);

        std::filesystem::path mo2GameRoot = RfabCompat::ResolveMo2ConfiguredGamePath(selfDir, selfDir, TARGET_NAME_A ".exe");
        if (mo2GameRoot.empty() && !cwd.empty())
            mo2GameRoot = RfabCompat::ResolveMo2ConfiguredGamePath(cwd, selfDir, TARGET_NAME_A ".exe");

        if (!mo2GameRoot.empty())
        {
            const std::filesystem::path mo2Exe = mo2GameRoot / (TARGET_NAME L".exe");
            titlePath = mo2GameRoot.wstring();
            exePath = mo2Exe.wstring();
            Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath", titlePath);
            Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe", exePath);
        }
        else
        {
            // Priority 2: Standalone portable RFAB / MO2 root relative to working directory or launcher folder
            const std::filesystem::path p1 = selfDir.parent_path();
            const std::filesystem::path p2 = p1.parent_path();
            const std::filesystem::path p3 = p2.parent_path();
            const std::filesystem::path p4 = p3.parent_path();

            for (const auto& candidateRoot : {cwd, p1, p2, p3, p4})
            {
                if (candidateRoot.empty())
                    continue;

                ec.clear();
                if (std::filesystem::exists(selfDir, ec) &&
                    std::filesystem::exists(candidateRoot, ec) &&
                    std::filesystem::equivalent(candidateRoot, selfDir, ec))
                {
                    continue;
                }

                const std::filesystem::path candidateExe = candidateRoot / (TARGET_NAME L".exe");
                const std::filesystem::path candidateData = candidateRoot / L"Data";
                ec.clear();
                if (std::filesystem::exists(candidateExe, ec) && std::filesystem::exists(candidateData, ec))
                {
                    titlePath = candidateRoot.wstring();
                    exePath = candidateExe.wstring();
                    Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath", titlePath);
                    Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe", exePath);
                    break;
                }
            }
        }
    }

    bool result = true;
    std::error_code existsEc1;
    std::error_code existsEc2;
    if (titlePath.empty() || exePath.empty() || !std::filesystem::exists(titlePath, existsEc1) || !std::filesystem::exists(exePath, existsEc2) || aForceSelect)
    {
        constexpr int kSelectionAttempts = 3;

        for (int i = 0; i < kSelectionAttempts; i++)
        {
            if (auto path = OpenPathSelectionDialog2(SuggestTitlePath()))
            {
                size_t pos = path->find_last_of(L'\\');
                if (pos == std::string::npos)
                    return false;

                titlePath = path->substr(0, pos);
                exePath = (*path);

                // game is installed into our directory, or otherwise.
                std::error_code ec;
                if (titlePath == selfDir || std::filesystem::equivalent(titlePath, selfDir, ec))
                {
                    continue;
                }

                // if this fails, we try again
                result = Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitlePath", titlePath) && Registry::WriteString(HKEY_CURRENT_USER, kTiltedRegistryPath, L"TitleExe", exePath);

                if (result)
                    break;
            }
            else
                break;
        }
    }

    if (result)
    {
        auto* apContext = launcher::GetLaunchContext();
        apContext->gamePath = titlePath;
        apContext->exePath = exePath;
    }

    return result;
}
} // namespace oobe
