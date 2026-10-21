#pragma once

#include <Games/GameProfile.h>

#include <Games/Primitives.h>

#include <Havok/BShkbAnimationGraph.h>

struct BSFixedString;

struct BSAnimationGraphManager
{
    virtual ~BSAnimationGraphManager();
    virtual void sub_1(void* apUnk1);

    void Release()
    {
        if (InterlockedDecrement(&refCount) == 0)
            this->~BSAnimationGraphManager();
    }

    volatile LONG refCount;
    void* pad_ptrs[6];
    BSTSmallArray<BShkbAnimationGraph> animationGraphs; // 40 - 20
    void* pad_ptrs2[TP_LAYOUT(8, 9)]; // the AE runtime block gained one pointer in 1.6.629
    BSRecursiveLock lock;  // 98 (1.5.97) / A0 (1.6.629+), 4C on 32-bit
    void* unkPtrAfterLock; // A0 - 58

#if TP_PLATFORM_32
    void* unkPtrOldrim;
#endif

    uint32_t animationGraphIndex; // A8 - 5C

    SortedMap<uint32_t, String> DumpAnimationVariables(bool aPrintVariables);
    uint64_t GetDescriptorKey(int aForceIndex = -1);
    uint32_t ReSendEvent(BSFixedString* apEventName);
};

#if TP_PLATFORM_64
static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x40);
TP_STATIC_ASSERT_OFFSET(BSAnimationGraphManager, lock, 0x98, 0xA0);
TP_STATIC_ASSERT_OFFSET(BSAnimationGraphManager, animationGraphIndex, 0xA8, 0xB0);
#else
static_assert(offsetof(BSAnimationGraphManager, animationGraphs) == 0x20);
static_assert(offsetof(BSAnimationGraphManager, lock) == 0x4C);
static_assert(offsetof(BSAnimationGraphManager, animationGraphIndex) == 0x5C);
#endif
