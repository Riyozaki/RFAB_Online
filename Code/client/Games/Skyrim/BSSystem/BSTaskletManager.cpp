
#include <base/threading/ThreadUtils.h>

#include <atomic>

namespace
{
struct BSTaskletManager
{
    char pad0[0x30];
    void* threadHandles[6];
};

static BSTaskletManager* (*Real_GetTaskletManager)() = nullptr;
static std::atomic_bool s_namedTaskletThreads{false};

static BSTaskletManager* Hook_GetTaskletManager()
{
    auto* pSelf = Real_GetTaskletManager();

    if (pSelf && !s_namedTaskletThreads.exchange(true))
    {
        for (int i = 0; i < 6; i++)
        {
            if (!pSelf->threadHandles[i])
                continue;

            auto name = fmt::format("TaskletThread{}", i);
            Base::SetThreadName(pSelf->threadHandles[i], name.c_str());
        }
    }

    return pSelf;
}
} // namespace

static TiltedPhoques::Initializer s_BSThreadInit(
    []()
    {
        // BSWin32TaskletManager::GetSingleton (SE 68203 @ 0xC39950):
        // Hook at function entry (+0x0) instead of mid-function SwapCall so there is zero
        // mid-function instruction offset sensitivity on Skyrim SE 1.5.97.
        const VersionDbPtr<void> getTaskletManagerInstance(68203);
        Real_GetTaskletManager = static_cast<decltype(Real_GetTaskletManager)>(getTaskletManagerInstance.GetPtr());

        TP_HOOK_IMMEDIATE(&Real_GetTaskletManager, &Hook_GetTaskletManager);
    });
