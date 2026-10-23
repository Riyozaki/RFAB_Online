#include <BranchInfo.h>
#include "CrashHandler.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <strsafe.h>

extern void LogUnhandledCrashToBootFile(PEXCEPTION_POINTERS apExceptionInfo) noexcept;

using time_point = std::chrono::system_clock::time_point;

std::string SerializeTimePoint(const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt); // GMT (UTC)
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

// IMPORTANT FOR RFAB / .NET SCRIPT FRAMEWORK COMPATIBILITY:
// Never register a first-chance VectoredExceptionHandler (`AddVectoredExceptionHandler(1, ...)`) that writes dumps
// and calls `spdlog::shutdown()` on `EXCEPTION_ACCESS_VIOLATION`! `.NET Script Framework` (`clr.dll`), `USVFS`,
// and DirectX routinely raise handled first-chance `0xC0000005` probes caught by their own SEH / CLR frame handlers.
// Using top-level `SetUnhandledExceptionFilter` handles ONLY genuine unhandled crashes after SEH frame unwind.
LONG WINAPI UnhandledExceptionFilterCallback(PEXCEPTION_POINTERS pExceptionInfo)
{
    static int alreadyCrashed = 0;
    auto retval = EXCEPTION_EXECUTE_HANDLER;

    static std::mutex singleThreaded;
    const std::lock_guard lock{singleThreaded};

    if (pExceptionInfo && pExceptionInfo->ExceptionRecord && alreadyCrashed++ == 0)
    {
        LogUnhandledCrashToBootFile(pExceptionInfo);

        HMODULE hMod = nullptr;
        wchar_t modPathW[MAX_PATH]{};
        uintptr_t modBase = 0;
        const uintptr_t rip = pExceptionInfo->ContextRecord ? pExceptionInfo->ContextRecord->Rip
                                                            : reinterpret_cast<uintptr_t>(pExceptionInfo->ExceptionRecord->ExceptionAddress);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &hMod) && hMod)
        {
            modBase = reinterpret_cast<uintptr_t>(hMod);
            GetModuleFileNameW(hMod, modPathW, MAX_PATH - 1);
        }
        char modUtf8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, modPathW, -1, modUtf8, sizeof(modUtf8) - 1, nullptr, nullptr);

        spdlog::critical("Unhandled crash: code=0x{:08X} RIP=0x{:X} module='{}' +0x{:X}",
                         pExceptionInfo->ExceptionRecord->ExceptionCode,
                         rip,
                         modUtf8,
                         modBase ? (rip - modBase) : rip);

#if (IS_MASTER)
        volatile static bool bMiniDump = false;
#else
        volatile static bool bMiniDump = true;
#endif
        if (bMiniDump)
        {
            HANDLE hDumpFile = NULL;
            try
            {
                MINIDUMP_EXCEPTION_INFORMATION M;
                char dumpPath[MAX_PATH];

                M.ThreadId = GetCurrentThreadId();
                M.ExceptionPointers = pExceptionInfo;
                M.ClientPointers = 0;

                std::ostringstream oss;
                oss << "crash_" << SerializeTimePoint(std::chrono::system_clock::now(), "UTC_%Y-%m-%d_%H-%M-%S")
                    << ".dmp";

                GetModuleFileNameA(NULL, dumpPath, sizeof(dumpPath));
                std::filesystem::path modulePath(dumpPath);
                auto subPath = modulePath.parent_path();

                CrashHandler::RemovePreviousDump(subPath);
                subPath /= oss.str();

                hDumpFile = CreateFileA(subPath.string().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, NULL);

                auto dumpSettings = MiniDumpWithDataSegs | MiniDumpWithProcessThreadData | MiniDumpWithHandleData |
                                    MiniDumpWithThreadInfo;

                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, (MINIDUMP_TYPE)dumpSettings,
                                  (pExceptionInfo) ? &M : NULL, NULL, NULL);
            }
            catch (...)
            {
            }

            if (hDumpFile)
                CloseHandle(hDumpFile);
        }

        if (CrashHandler::GetOriginalUnhandledExceptionFilter() &&
            CrashHandler::GetOriginalUnhandledExceptionFilter() != &UnhandledExceptionFilterCallback)
        {
            retval = (*CrashHandler::GetOriginalUnhandledExceptionFilter())(pExceptionInfo);
        }

        spdlog::default_logger()->flush();
    }
    return retval;
}

LPTOP_LEVEL_EXCEPTION_FILTER CrashHandler::m_pUnhandled;
CrashHandler::CrashHandler()
{
    m_pUnhandled = SetUnhandledExceptionFilter(&UnhandledExceptionFilterCallback);
    m_handler = nullptr;
}

CrashHandler::~CrashHandler()
{
}

void CrashHandler::RemovePreviousDump(std::filesystem::path path)
{
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        if (entry.path().string().find("crash_") != std::string::npos &&
            entry.path().extension() == ".dmp")
        {
            DeleteFileA(entry.path().string().c_str());
        }
    }
}
