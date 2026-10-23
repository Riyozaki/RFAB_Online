#include <TiltedOnlinePCH.h>
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
extern "C" const wchar_t* GetTiltedClientDirW() noexcept;

using time_point = std::chrono::system_clock::time_point;

std::string SerializeTimePoint(const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt);
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

// Uses AddVectoredContinueHandler + SetUnhandledExceptionFilter (NEVER first-chance AddVectoredExceptionHandler):
// - Handled first-chance probes in .NET Script Framework (clr.dll), USVFS, and DirectX are caught cleanly by
//   their own SEH/CLR frames without ever invoking this callback.
// - Genuine unhandled exceptions (including unhandled C++ 0xE06D7363 across dynamic RtlAddFunctionTable / noexcept
//   boundaries) ALWAYS reach this handler, decoding the exact throwing DLL + RTTI class + what() + stack trace!
static void RecordFatalCrashOnce(PEXCEPTION_POINTERS pExceptionInfo)
{
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord)
        return;

    // Ignore benign Win32 informational codes (e.g. OutputDebugStringA 0x40010006, SetThreadName 0x406D1388)
    if (pExceptionInfo->ExceptionRecord->ExceptionCode < 0x80000000)
        return;

    static std::mutex singleThreaded;
    const std::lock_guard lock{singleThreaded};

    static int alreadyCrashed = 0;
    if (alreadyCrashed++ != 0)
        return;

    LogUnhandledCrashToBootFile(pExceptionInfo);

    HMODULE hMod = nullptr;
    wchar_t modPathW[MAX_PATH]{};
    uintptr_t modBase = 0;
    const uintptr_t rip = pExceptionInfo->ContextRecord
                              ? pExceptionInfo->ContextRecord->Rip
                              : reinterpret_cast<uintptr_t>(pExceptionInfo->ExceptionRecord->ExceptionAddress);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &hMod) &&
        hMod)
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
        HANDLE hDumpFile = INVALID_HANDLE_VALUE;
        MINIDUMP_EXCEPTION_INFORMATION M{};
        M.ThreadId = GetCurrentThreadId();
        M.ExceptionPointers = pExceptionInfo;
        M.ClientPointers = 0;

        try
        {
            const wchar_t* pClientDir = GetTiltedClientDirW();
            std::filesystem::path subPath = (pClientDir && *pClientDir)
                                                ? std::filesystem::path(pClientDir)
                                                : TiltedPhoques::GetPath();

            CrashHandler::RemovePreviousDump(subPath);

            std::ostringstream oss;
            const time_point input = std::chrono::system_clock::now();
            oss << "crash_" << SerializeTimePoint(input, "%Y-%m-%d_%H-%M-%S") << ".dmp";
            subPath /= oss.str();

            hDumpFile = CreateFileW(subPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);

            if (hDumpFile != INVALID_HANDLE_VALUE)
            {
                auto dumpSettings = MiniDumpWithDataSegs | MiniDumpWithProcessThreadData | MiniDumpWithHandleData |
                                    MiniDumpWithThreadInfo;
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, (MINIDUMP_TYPE)dumpSettings,
                                  (pExceptionInfo) ? &M : NULL, NULL, NULL);
            }
        }
        catch (...)
        {
        }

        if (hDumpFile != INVALID_HANDLE_VALUE && hDumpFile != nullptr)
            CloseHandle(hDumpFile);
    }

    if (auto logger = spdlog::default_logger())
        logger->flush();
}

LONG WINAPI VectoredContinueCrashLogger(PEXCEPTION_POINTERS pExceptionInfo)
{
    RecordFatalCrashOnce(pExceptionInfo);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledExceptionFilterCallback(PEXCEPTION_POINTERS pExceptionInfo)
{
    RecordFatalCrashOnce(pExceptionInfo);

    if (CrashHandler::GetOriginalUnhandledExceptionFilter() &&
        CrashHandler::GetOriginalUnhandledExceptionFilter() != &UnhandledExceptionFilterCallback)
    {
        return (*CrashHandler::GetOriginalUnhandledExceptionFilter())(pExceptionInfo);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

LPTOP_LEVEL_EXCEPTION_FILTER CrashHandler::m_pUnhandled;

CrashHandler::CrashHandler()
{
    m_pUnhandled = SetUnhandledExceptionFilter(&UnhandledExceptionFilterCallback);
    m_handler = AddVectoredContinueHandler(1, &VectoredContinueCrashLogger);
}

CrashHandler::~CrashHandler()
{
    if (m_handler)
    {
        RemoveVectoredContinueHandler(m_handler);
        m_handler = nullptr;
    }
}

void CrashHandler::RemovePreviousDump(std::filesystem::path path)
{
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        const auto ext = entry.path().extension().wstring();
        const auto stem = entry.path().filename().wstring();
        if (stem.find(L"crash_") != std::wstring::npos && _wcsicmp(ext.c_str(), L".dmp") == 0)
        {
            DeleteFileW(entry.path().c_str());
        }
    }
}
