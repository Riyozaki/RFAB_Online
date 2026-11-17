#include "CrashRecord.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
// ---- where the record goes (decided once, in Install) ----

#ifdef _WIN32
wchar_t g_logPath[1024]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
#else
char g_logPath[2048]{};
#endif

std::atomic<bool> g_writing{false};
// Not 0 only during the self-test: the process ends itself with this code once the record is written.
std::atomic<int> g_selfTestExitCode{0};

[[noreturn]] void EndProcessNow(int aExitCode) noexcept
{
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(aExitCode));
#endif
    std::_Exit(aExitCode);
}

// printf into a fixed buffer; the position never passes the end.
void Append(char* apBuffer, size_t aSize, size_t& aPosition, const char* apFormat, ...) noexcept
{
    if (aPosition + 1 >= aSize)
        return;
    va_list args;
    va_start(args, apFormat);
    const int written = std::vsnprintf(apBuffer + aPosition, aSize - aPosition, apFormat, args);
    va_end(args);
    if (written <= 0)
        return;
    const size_t advanced = static_cast<size_t>(written);
    aPosition = (aPosition + advanced < aSize) ? aPosition + advanced : aSize - 1;
}

void AppendTimestamp(char* apBuffer, size_t aSize, size_t& aPosition) noexcept
{
#ifdef _WIN32
    SYSTEMTIME now{};
    GetLocalTime(&now);
    Append(apBuffer, aSize, aPosition, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", static_cast<unsigned>(now.wYear),
           static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
           static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond), static_cast<unsigned>(now.wMilliseconds));
#else
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    Append(apBuffer, aSize, aPosition, "[%04d-%02d-%02d %02d:%02d:%02d.000] ", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           local.tm_hour, local.tm_min, local.tm_sec);
#endif
}

// Appends the text and a line end to the crash log and shows it on the console.
void WriteRecord(const char* apText, size_t aLength) noexcept
{
#ifdef _WIN32
    static const char kLineEnd[] = "\r\n";
    if (g_logPath[0] != L'\0')
    {
        const HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, apText, static_cast<DWORD>(aLength), &written, nullptr);
            WriteFile(file, kLineEnd, 2, &written, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (console != nullptr && console != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(console, apText, static_cast<DWORD>(aLength), &written, nullptr);
        WriteFile(console, kLineEnd, 2, &written, nullptr);
    }
#else
    if (g_logPath[0] != '\0')
    {
        const int file = ::open(g_logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (file >= 0)
        {
            (void)!::write(file, apText, aLength);
            (void)!::write(file, "\n", 1);
            ::close(file);
        }
    }
    (void)!::write(2, apText, aLength);
    (void)!::write(2, "\n", 1);
#endif
}

void ThrowSelfTestException()
{
    throw std::runtime_error(ServerCrash::kSelfTestTerminateText);
}

// ---- std::terminate ----

[[noreturn]] void OnTerminate() noexcept
{
    static char s_text[1536];
    if (!g_writing.exchange(true))
    {
        char what[768] = "no exception is active";
        if (const std::exception_ptr current = std::current_exception())
        {
            try
            {
                std::rethrow_exception(current);
            }
            catch (const std::exception& acError)
            {
                std::snprintf(what, sizeof(what), "%s", acError.what());
            }
            catch (...)
            {
                std::snprintf(what, sizeof(what), "an exception that is not derived from std::exception");
            }
        }

        size_t position = 0;
        AppendTimestamp(s_text, sizeof(s_text), position);
        Append(s_text, sizeof(s_text), position, "[TERMINATE] the server stopped itself: std::terminate was called; what()=[\"%s\"]", what);
        WriteRecord(s_text, position);
        g_writing.store(false);
    }
    if (g_selfTestExitCode.load() != 0)
        EndProcessNow(ServerCrash::kSelfTestTerminateExit);
    std::abort();
}

#ifdef _WIN32
// ---- unhandled structured exceptions (Windows) ----

// "STServer.dll+0x1A2B3C (0x7FF6...)": the form symbolize_crash.py reads.
void DescribeAddress(uintptr_t aAddress, char* apOut, size_t aSize) noexcept
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(aAddress), &module) &&
        module != nullptr)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH - 1);
        const wchar_t* pName = wcsrchr(path, L'\\');
        pName = pName ? pName + 1 : path;
        char name[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, pName, -1, name, static_cast<int>(sizeof(name)) - 1, nullptr, nullptr);
        std::snprintf(apOut, aSize, "%s+0x%llX (0x%llX)", name[0] ? name : "<unnamed>",
                      static_cast<unsigned long long>(aAddress - reinterpret_cast<uintptr_t>(module)),
                      static_cast<unsigned long long>(aAddress));
    }
    else
    {
        std::snprintf(apOut, aSize, "<no-module>+0x%llX (0x%llX)", static_cast<unsigned long long>(aAddress),
                      static_cast<unsigned long long>(aAddress));
    }
}

// How MSVC describes a thrown C++ object to the system (x64; unchanged since Visual Studio 2005).
// The same layout is read by the client's crash record (Code/client/main.cpp).
struct MsvcTypeDescriptor
{
    const void* pVFTable;
    void* spare;
    char name[1];
};
struct MsvcPmd
{
    int mdisp;
    int pdisp;
    int vdisp;
};
struct MsvcCatchableType
{
    unsigned int properties;
    int pTypeRva;
    MsvcPmd thisDisplacement;
    int sizeOrOffset;
    int copyFunctionRva;
};
struct MsvcCatchableTypeArray
{
    int nCatchableTypes;
    int arrayOfCatchableTypesRva[1];
};
struct MsvcThrowInfo
{
    unsigned int attributes;
    int pmfnUnwindRva;
    int pForwardCompatRva;
    int pCatchableTypeArrayRva;
};

// " | Type=[...] what()=["..."]" for a C++ throw nobody caught (code 0xE06D7363): its type name and,
// when it derives from std::exception, its text. The caller holds the __try.
void AppendCppException(const EXCEPTION_RECORD* apRecord, char* apText, size_t aSize, size_t& aPosition) noexcept
{
    if (apRecord->ExceptionCode != 0xE06D7363 || apRecord->NumberParameters < 4)
        return;

    const uintptr_t object = static_cast<uintptr_t>(apRecord->ExceptionInformation[1]);
    const uintptr_t throwInfo = static_cast<uintptr_t>(apRecord->ExceptionInformation[2]);
    const uintptr_t imageBase = static_cast<uintptr_t>(apRecord->ExceptionInformation[3]);
    const char* pTypeName = "<unknown>";
    const char* pWhat = "";

    if (throwInfo && imageBase)
    {
        const auto* pInfo = reinterpret_cast<const MsvcThrowInfo*>(throwInfo);
        if (pInfo->pCatchableTypeArrayRva)
        {
            const auto* pTypes = reinterpret_cast<const MsvcCatchableTypeArray*>(imageBase + pInfo->pCatchableTypeArrayRva);
            for (int i = 0; i < pTypes->nCatchableTypes && i < 16; ++i)
            {
                const auto* pType = reinterpret_cast<const MsvcCatchableType*>(imageBase + pTypes->arrayOfCatchableTypesRva[i]);
                if (!pType->pTypeRva)
                    continue;
                const auto* pDescriptor = reinterpret_cast<const MsvcTypeDescriptor*>(imageBase + pType->pTypeRva);
                if (!pDescriptor->name[0])
                    continue;
                if (i == 0)
                    pTypeName = pDescriptor->name;  // the most derived type comes first
                if (std::strstr(pDescriptor->name, "exception@std@@"))
                {
                    const auto* pException = reinterpret_cast<const std::exception*>(object + pType->thisDisplacement.mdisp);
                    if (const char* pText = pException->what())
                        pWhat = pText;
                    break;
                }
            }
        }
    }
    Append(apText, aSize, aPosition, " | Type=[%s] what()=[\"%s\"]", pTypeName, pWhat);
}

// Only plain data in here: MSVC does not allow objects with destructors next to __try.
size_t FormatFault(EXCEPTION_POINTERS* apInfo, char* apText, size_t aSize) noexcept
{
    size_t position = 0;
    __try
    {
        const EXCEPTION_RECORD* pRecord = apInfo->ExceptionRecord;
        const CONTEXT* pContext = apInfo->ContextRecord;
        const uintptr_t rip = static_cast<uintptr_t>(pContext->Rip);

        HMODULE module = nullptr;
        wchar_t modulePath[MAX_PATH]{};
        uintptr_t moduleBase = 0;
        unsigned long peTimestamp = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &module) &&
            module != nullptr)
        {
            moduleBase = reinterpret_cast<uintptr_t>(module);
            GetModuleFileNameW(module, modulePath, MAX_PATH - 1);
            const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
            if (pDos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(moduleBase + static_cast<uintptr_t>(pDos->e_lfanew));
                if (pNt->Signature == IMAGE_NT_SIGNATURE)
                    peTimestamp = pNt->FileHeader.TimeDateStamp;
            }
        }
        char moduleUtf8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, modulePath, -1, moduleUtf8, static_cast<int>(sizeof(moduleUtf8)) - 1, nullptr, nullptr);

        const uintptr_t faultAddress = pRecord->NumberParameters >= 2 ? static_cast<uintptr_t>(pRecord->ExceptionInformation[1]) : 0;

        AppendTimestamp(apText, aSize, position);
        Append(apText, aSize, position,
               "[UNHANDLED_CRASH] TID=%lu Code=0x%08lX RIP=0x%llX FaultAddr=0x%llX Module='%s' Base=0x%llX RVA=+0x%llX "
               "RSP=0x%llX RBP=0x%llX RAX=0x%llX RBX=0x%llX RCX=0x%llX RDX=0x%llX PeTimestamp=0x%08lX Process=[server]",
               static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long>(pRecord->ExceptionCode),
               static_cast<unsigned long long>(rip), static_cast<unsigned long long>(faultAddress), moduleUtf8,
               static_cast<unsigned long long>(moduleBase), static_cast<unsigned long long>(moduleBase ? rip - moduleBase : rip),
               static_cast<unsigned long long>(pContext->Rsp), static_cast<unsigned long long>(pContext->Rbp),
               static_cast<unsigned long long>(pContext->Rax), static_cast<unsigned long long>(pContext->Rbx),
               static_cast<unsigned long long>(pContext->Rcx), static_cast<unsigned long long>(pContext->Rdx), peTimestamp);
        AppendCppException(pRecord, apText, aSize, position);

        // The call stack, by the unwind tables every x64 function has: up to 32 frames.
        CONTEXT unwind = *pContext;
        for (int depth = 0; depth < 32 && position + 160 < aSize; ++depth)
        {
            const DWORD64 pc = unwind.Rip;
            if (pc < 0x10000)
                break;

            char frame[384]{};
            DescribeAddress(static_cast<uintptr_t>(pc), frame, sizeof(frame));
            Append(apText, aSize, position, "\r\n    [unwind #%d] %s", depth, frame);

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION pFunction = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (!pFunction)
            {
                // A leaf function (or a jump to a bad address): the return address is on top of the stack.
                if (depth == 0 && unwind.Rsp >= 0x10000)
                {
                    unwind.Rip = *reinterpret_cast<const DWORD64*>(unwind.Rsp);
                    unwind.Rsp += 8;
                    continue;
                }
                break;
            }

            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, pFunction, &unwind, &handlerData, &establisherFrame, nullptr);
            if (unwind.Rip == 0 || unwind.Rsp == 0)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Whatever was formatted before the second fault is still written.
    }
    return position;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* apInfo) noexcept
{
    static char s_text[12288];

    // Codes below 0x80000000 are informational (debug output, thread names): not a crash.
    if (apInfo && apInfo->ExceptionRecord && apInfo->ContextRecord && apInfo->ExceptionRecord->ExceptionCode >= 0x80000000 &&
        !g_writing.exchange(true))
    {
        const size_t length = FormatFault(apInfo, s_text, sizeof(s_text));
        if (length > 0)
            WriteRecord(s_text, length);
        g_writing.store(false);
        // A C++ throw (0xE06D7363) goes on to the runtime's filter and std::terminate, self-test or not.
        if (g_selfTestExitCode.load() != 0 && apInfo->ExceptionRecord->ExceptionCode != 0xE06D7363)
            EndProcessNow(g_selfTestExitCode.load());
    }

    // The C++ runtime's own filter turns an uncaught throw into std::terminate (our handler above
    // then adds the what() text); the system's default handling follows it.
    return g_previousFilter ? g_previousFilter(apInfo) : EXCEPTION_CONTINUE_SEARCH;
}
#endif
} // namespace

namespace ServerCrash
{
void Install() noexcept
{
    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true))
        return;

#ifdef _WIN32
    wchar_t directory[512]{};
    const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(directory) / sizeof(directory[0])) - 1, directory);
    if (length > 0 && length < sizeof(directory) / sizeof(directory[0]) - 1)
    {
        wchar_t logs[768]{};
        _snwprintf_s(logs, _TRUNCATE, L"%s\\logs", directory);
        CreateDirectoryW(logs, nullptr);  // usually exists already; the logger creates it later otherwise
        _snwprintf_s(g_logPath, _TRUNCATE, L"%s\\STServer_Crash.log", logs);
    }
    g_previousFilter = SetUnhandledExceptionFilter(&OnUnhandledException);
#else
    char directory[1024]{};
    if (::getcwd(directory, sizeof(directory) - 1) != nullptr)
    {
        char logs[1536]{};
        std::snprintf(logs, sizeof(logs), "%s/logs", directory);
        ::mkdir(logs, 0755);
        std::snprintf(g_logPath, sizeof(g_logPath), "%s/STServer_Crash.log", logs);
    }
#endif
    std::set_terminate(&OnTerminate);
}

int RunSelfTest(const char* apMode, void (*apFault)()) noexcept
{
    const bool fault = apMode && std::strcmp(apMode, "fault") == 0;
    const bool terminate = apMode && std::strcmp(apMode, "terminate") == 0;
    if ((!fault && !terminate) || (fault && !apFault))
        return 2;

#ifdef _WIN32
    // No crash dialog, no error reporting window: the build machine has nobody to click them away.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    g_selfTestExitCode.store(fault ? kSelfTestFaultExit : kSelfTestTerminateExit);

    if (fault)
    {
        apFault();
        return 3;  // the fault did not happen: the caller sees a wrong exit code
    }

    // An exception leaving a noexcept function: the language calls std::terminate.
    []() noexcept { ThrowSelfTestException(); }();
    return 3;
}
} // namespace ServerCrash
