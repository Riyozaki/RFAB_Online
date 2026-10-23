
#include <Games/Skyrim/BSSystem/BSThread.h>
#include <base/threading/ThreadUtils.h>

namespace
{
void (*BSThread_Initialize)(BSThread*, int, const char*){nullptr};

void Hook_BSThread_Initialize(BSThread* apThis, int aStackSize, const char* apName)
{
    BSThread_Initialize(apThis, aStackSize, apName);

    if (!apName && apThis->m_ThreadHandle)
        spdlog::warn("Unnamed thread started: {}", apThis->m_ThreadID);

    // this means the thread was successfully created
    if (apThis->m_ThreadHandle)
    {
        bool result = Base::SetThreadName(apThis->m_ThreadHandle, apName);
        if (!result)
        {
            spdlog::warn("Failed to set thread name for tid {} to {}", apThis->m_ThreadID, apName);
        }
    }
}

// bsthreadutils
// hook this in order to redirect the game to use the new naming apis (windows 10+)
void Hook_SetThreadName(uint32_t aThreadId, const char* apThreadName)
{
    // query thread handle
    if (auto hThread = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, aThreadId))
    {
        Base::SetThreadName(hThread, apThreadName);

        ::CloseHandle(hThread);
    }
    else
        spdlog::warn("Hook_SetThreadName(): Unable to query thread handle :(");
}
} // namespace

static TiltedPhoques::Initializer s_BSThreadInit(
    []()
    {
        const VersionDbPtr<uint8_t> threadInit(67003);
        BSThread_Initialize = static_cast<decltype(BSThread_Initialize)>(threadInit.GetPtr());
        TP_HOOK_IMMEDIATE(&BSThread_Initialize, &Hook_BSThread_Initialize);

        const VersionDbPtr<uint8_t> setThreadName(67740);
        TiltedPhoques::Jump(setThreadName.Get(), &Hook_SetThreadName);
    });
