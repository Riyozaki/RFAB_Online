#pragma once

#include <cstdio>
#include <optional>
#include <Memory.hpp>
#include <Windows.h>

inline TiltedPhoques::String QueryFileVersion(const wchar_t* acpPath)
{
    // check if viable
    if (DWORD infoSize = GetFileVersionInfoSizeW(acpPath, nullptr))
    {
        auto buffer = TiltedPhoques::MakeUnique<uint8_t[]>(infoSize);
        if (GetFileVersionInfoW(acpPath, 0, infoSize, buffer.get()))
        {
            char* productVersion = nullptr;
            DWORD productVersionLen = 0;
            if (VerQueryValue(buffer.get(), "\\StringFileInfo\\040904B0\\ProductVersion", (void**)&productVersion,
                              (PUINT)&productVersionLen) &&
                productVersionLen && productVersion)
            {
                return productVersion;
            }

            if (VerQueryValueA(buffer.get(), "\\StringFileInfo\\040904B0\\FileVersion", (LPVOID*)&productVersion,
                               (PUINT)&productVersionLen) &&
                productVersionLen && productVersion && *productVersion)
            {
                return productVersion;
            }

            // RFAB Online: language/distribution independent fallback.
            // Some 1.5.97 executables ship without the 040904B0 string table
            // (downgraded or DRM-free builds). VS_FIXEDFILEINFO is always in
            // the version resource root, so derive "major.minor.revision.build"
            // from it instead of failing version detection entirely.
            VS_FIXEDFILEINFO* pFixedInfo = nullptr;
            UINT fixedInfoLen = 0;
            if (VerQueryValueA(buffer.get(), "\\", (LPVOID*)&pFixedInfo, &fixedInfoLen) && pFixedInfo && fixedInfoLen >= sizeof(VS_FIXEDFILEINFO) &&
                pFixedInfo->dwSignature == 0xFEEF04BD)
            {
                char version[64];
                std::snprintf(version, sizeof(version), "%u.%u.%u.%u", (unsigned)(pFixedInfo->dwFileVersionMS >> 16), (unsigned)(pFixedInfo->dwFileVersionMS & 0xFFFF),
                              (unsigned)(pFixedInfo->dwFileVersionLS >> 16), (unsigned)(pFixedInfo->dwFileVersionLS & 0xFFFF));
                return version;
            }
        }
    }

    return {};
}
