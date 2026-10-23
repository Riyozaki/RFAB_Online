#pragma once

#include <cstdio>
#include <cctype>
#include <filesystem>
#include <optional>
#include <Memory.hpp>
#include <Windows.h>

namespace detail_filever
{
inline bool ExtractNormalizedVersion(const char* raw, TiltedPhoques::String& outVer)
{
    if (!raw || !*raw)
        return false;

    int parts[4] = {0, 0, 0, 0};
    int count = 0;
    const char* p = raw;
    while (*p && count < 4)
    {
        while (*p && !std::isdigit(static_cast<unsigned char>(*p)))
            ++p;
        if (!*p)
            break;
        int val = 0;
        while (*p && std::isdigit(static_cast<unsigned char>(*p)))
        {
            val = val * 10 + (*p - '0');
            ++p;
        }
        parts[count++] = val;
    }

    if (count < 3)
        return false;

    // Ignore placeholder 0.0.0.0 and Bethesda/CODEX FILEVERSION 1.0.0.0
    if ((parts[0] == 0 || parts[0] == 1) && parts[1] == 0 && parts[2] == 0 && parts[3] == 0)
        return false;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
    outVer = buf;
    return true;
}

inline TiltedPhoques::String QuerySinglePeVersion(const wchar_t* acpPath)
{
    DWORD infoSize = GetFileVersionInfoSizeW(acpPath, nullptr);
    if (!infoSize)
        return {};

    auto buffer = TiltedPhoques::MakeUnique<uint8_t[]>(infoSize);
    if (!GetFileVersionInfoW(acpPath, 0, infoSize, buffer.get()))
        return {};

    struct LangCodePage
    {
        WORD wLanguage;
        WORD wCodePage;
    };

    // Query both actual resource translation table (e.g. Bethesda's 040904E4) and common fallbacks
    LangCodePage candidates[12]{};
    size_t numCandidates = 0;

    LangCodePage* pTrans = nullptr;
    UINT transBytes = 0;
    if (VerQueryValueA(buffer.get(), "\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&pTrans), &transBytes) && pTrans)
    {
        const size_t count = transBytes / sizeof(LangCodePage);
        for (size_t i = 0; i < count && numCandidates < 8; ++i)
            candidates[numCandidates++] = pTrans[i];
    }
    candidates[numCandidates++] = {0x0409, 0x04B0};
    candidates[numCandidates++] = {0x0409, 0x04E4};
    candidates[numCandidates++] = {0x0409, 0x0000};
    candidates[numCandidates++] = {0x0000, 0x04B0};

    static const char* kKeys[2] = {"ProductVersion", "FileVersion"};
    TiltedPhoques::String parsed;
    for (const char* key : kKeys)
    {
        for (size_t i = 0; i < numCandidates; ++i)
        {
            char query[128];
            std::snprintf(query, sizeof(query), "\\StringFileInfo\\%04x%04x\\%s",
                          candidates[i].wLanguage, candidates[i].wCodePage, key);
            char* valStr = nullptr;
            UINT valLen = 0;
            if (VerQueryValueA(buffer.get(), query, reinterpret_cast<LPVOID*>(&valStr), &valLen) && valLen && valStr)
            {
                if (ExtractNormalizedVersion(valStr, parsed))
                    return parsed;
            }
        }
    }

    // VS_FIXEDFILEINFO root fallback: check dwProductVersion FIRST (Bethesda sets FILEVERSION 1,0,0,0
    // and PRODUCTVERSION 1,5,97,0 in SkyrimSE.exe!), then dwFileVersion.
    VS_FIXEDFILEINFO* pFixedInfo = nullptr;
    UINT fixedInfoLen = 0;
    if (VerQueryValueA(buffer.get(), "\\", reinterpret_cast<LPVOID*>(&pFixedInfo), &fixedInfoLen) &&
        pFixedInfo && fixedInfoLen >= sizeof(VS_FIXEDFILEINFO) && pFixedInfo->dwSignature == 0xFEEF04BD)
    {
        char prodBuf[64];
        std::snprintf(prodBuf, sizeof(prodBuf), "%u.%u.%u.%u",
                      static_cast<unsigned>(pFixedInfo->dwProductVersionMS >> 16),
                      static_cast<unsigned>(pFixedInfo->dwProductVersionMS & 0xFFFF),
                      static_cast<unsigned>(pFixedInfo->dwProductVersionLS >> 16),
                      static_cast<unsigned>(pFixedInfo->dwProductVersionLS & 0xFFFF));
        if (ExtractNormalizedVersion(prodBuf, parsed))
            return parsed;

        char fileBuf[64];
        std::snprintf(fileBuf, sizeof(fileBuf), "%u.%u.%u.%u",
                      static_cast<unsigned>(pFixedInfo->dwFileVersionMS >> 16),
                      static_cast<unsigned>(pFixedInfo->dwFileVersionMS & 0xFFFF),
                      static_cast<unsigned>(pFixedInfo->dwFileVersionLS >> 16),
                      static_cast<unsigned>(pFixedInfo->dwFileVersionLS & 0xFFFF));
        if (ExtractNormalizedVersion(fileBuf, parsed))
            return parsed;
    }

    return {};
}
} // namespace detail_filever

inline TiltedPhoques::String QueryFileVersion(const wchar_t* acpPath)
{
    auto ver = detail_filever::QuerySinglePeVersion(acpPath);
    if (!ver.empty())
        return ver;

    // Standalone / CODEX wrapper fallback: probe sibling SkyrimSE.cdx, then skse64_1_5_97.dll / version-1-5-97-0.bin
    std::error_code ec;
    const std::filesystem::path exePath(acpPath);
    const auto gameDir = exePath.parent_path();
    if (!gameDir.empty())
    {
        std::filesystem::path cdxPath = exePath;
        cdxPath.replace_extension(L".cdx");
        if (std::filesystem::exists(cdxPath, ec))
        {
            ver = detail_filever::QuerySinglePeVersion(cdxPath.c_str());
            if (!ver.empty())
                return ver;
        }

        if (std::filesystem::exists(gameDir / L"skse64_1_5_97.dll", ec) ||
            std::filesystem::exists(gameDir / L"Data" / L"SKSE" / L"Plugins" / L"version-1-5-97-0.bin", ec))
        {
            return "1.5.97.0";
        }
    }

    return {};
}
