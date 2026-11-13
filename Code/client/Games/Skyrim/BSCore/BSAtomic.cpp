#include "BSAtomic.h"

// RFAB Online (Skyrim SE 1.5.97): backport of upstream TiltedEvolution #937 (25a0049a).
// Address Library IDs are the SE 1.5.97 ones (AE 68233/68239/68234/68240 -> SE 66976/66982/66977/66983,
// cross-checked against CommonLibSSE RELOCATION_ID and SKSE64 2.0.20 GameTypes.h RVAs
// 0xC072D0 / 0xC07590 / 0xC07350 / 0xC075A0). Never re-implement this lock in-house: a
// different algorithm on the engine's memory leaks a reader and freezes the next writer forever.

void BSSpinLock::Lock() noexcept
{
    const uint32_t cThreadId = GetCurrentThreadId();

    _mm_lfence();
    if (OwningThread == cThreadId)
    {
        InterlockedIncrement(&uiLockCount);
    }
    else
    {
        if (InterlockedCompareExchange(&uiLockCount, 1, 0))
        {
            uint32_t spinCount = 0;
            while (InterlockedCompareExchange(&uiLockCount, 1, 0))
                Sleep(++spinCount < kFastSpinThreshold ? 0 : 1);

            _mm_lfence();
        }

        OwningThread = cThreadId;
        _mm_sfence();
    }
}

void BSSpinLock::Unlock() noexcept
{
    const uint32_t cThreadId = GetCurrentThreadId();

    _mm_lfence();
    if (OwningThread == cThreadId)
    {
        if (uiLockCount == 1)
        {
            OwningThread = 0;
            _mm_mfence();
            InterlockedCompareExchange(&uiLockCount, 0, 1);
        }
        else
        {
            InterlockedDecrement(&uiLockCount);
        }
    }
}

void BSReadWriteLock::LockForRead() noexcept
{
    TP_THIS_FUNCTION(TLockForRead, void, BSReadWriteLock);
    POINTER_SKYRIMSE(TLockForRead, s_lockForRead, 66976);
    TiltedPhoques::ThisCall(s_lockForRead, this);
}

void BSReadWriteLock::UnlockRead() noexcept
{
    TP_THIS_FUNCTION(TUnlockRead, void, BSReadWriteLock);
    POINTER_SKYRIMSE(TUnlockRead, s_unlockRead, 66982);
    TiltedPhoques::ThisCall(s_unlockRead, this);
}

void BSReadWriteLock::LockForWrite() noexcept
{
    TP_THIS_FUNCTION(TLockForWrite, void, BSReadWriteLock);
    POINTER_SKYRIMSE(TLockForWrite, s_lockForWrite, 66977);
    TiltedPhoques::ThisCall(s_lockForWrite, this);
}

void BSReadWriteLock::UnlockWrite() noexcept
{
    TP_THIS_FUNCTION(TUnlockWrite, void, BSReadWriteLock);
    POINTER_SKYRIMSE(TUnlockWrite, s_unlockWrite, 66983);
    TiltedPhoques::ThisCall(s_unlockWrite, this);
}
