#include <TiltedOnlinePCH.h>

#include <Games/Memory.h>
#include <CoopDamagePipeline.h>
#include <Games/References.h>

#include <TiltedCore/MimallocAllocator.hpp>
#include <mimalloc.h>

#include <cstring>

#pragma optimize("", off)

struct GameHeap
{
    static GameHeap* Get()
    {
        POINTER_SKYRIMSE(GameHeap, s_gameHeap, 514110);

        return s_gameHeap.Get();
    }
};

TP_THIS_FUNCTION(TFormAllocate, void*, GameHeap, size_t aSize, size_t aAlignment, bool aAligned);
TP_THIS_FUNCTION(TFormFree, void, GameHeap, void* apPtr, bool aAligned);

TFormAllocate* RealFormAllocate = nullptr;
TFormFree* RealFormFree = nullptr;

static TiltedPhoques::MimallocAllocator s_allocator;

namespace
{
// Skyrim SE 1.5.97 ActorValueList singleton size:
// 0x08 header + 164 * sizeof(ActorValueInfo*) = 0x528 bytes.
// Appending two zeroed pointer slots (16 bytes) guarantees actorValues[164] == nullptr
// across EVERY allocator (mimalloc, EngineFixes tbbmalloc, vanilla) and EVERY version of RFAB.dll / CustomSkills.dll
// that scans ActorValueList::actorValues[] until a null pointer.
constexpr size_t kActorValueListByteSize = 0x528;
constexpr size_t kActorValueListSentinelTailBytes = sizeof(void*) * 2;
} // namespace

void* TP_MAKE_THISCALL(HookFormAllocate, GameHeap, size_t aSize, size_t aAlignment, bool aAligned)
{
    const size_t requestedSize = aSize;
    bool padActorValueListSentinel = false;

    switch (aSize)
    {
    case sizeof(Actor): aSize = sizeof(ExActor); break;
    case sizeof(PlayerCharacter): aSize = sizeof(ExPlayerCharacter); break;
    case kActorValueListByteSize:
        aSize = kActorValueListByteSize + kActorValueListSentinelTailBytes;
        padActorValueListSentinel = true;
        break;
    default: break;
    }

    auto* pPointer = TiltedPhoques::ThisCall(RealFormAllocate, apThis, aSize, aAlignment, aAligned);

    if (!pPointer)
        return nullptr;

    if (padActorValueListSentinel)
    {
        std::memset(static_cast<uint8_t*>(pPointer) + requestedSize, 0, kActorValueListSentinelTailBytes);
    }

    ActorExtension* pExtension = nullptr;

    switch (aSize)
    {
    case sizeof(ExActor): pExtension = static_cast<ActorExtension*>(static_cast<ExActor*>(pPointer)); break;
    case sizeof(ExPlayerCharacter): pExtension = static_cast<ActorExtension*>(static_cast<ExPlayerCharacter*>(pPointer)); break;
    default: break;
    }

    if (pExtension)
    {
        new (pExtension) ActorExtension;
    }

    return pPointer;
}

void* Memory::Allocate(const size_t aSize) noexcept
{
    return TiltedPhoques::ThisCall(HookFormAllocate, GameHeap::Get(), aSize, 0, false);
}

void Memory::Free(void* apData) noexcept
{
    if (!apData)
        return;

    TiltedPhoques::ThisCall(RealFormFree, GameHeap::Get(), apData, false);
}

static bool IsFormAllocateReplacedByEF(TFormAllocate** appOutEngineFixesAlloc) noexcept
{
    HMODULE const hEngineFixes = GetModuleHandleW(L"EngineFixes.dll");
    if (!hEngineFixes || !appOutEngineFixesAlloc)
        return false;

    POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 66859);
    TFormAllocate* pFormAllocate = s_formAllocate.Get();
    if (!pFormAllocate || !*pFormAllocate)
        return false;

    // Follow up to 4 jump hops so both direct hooks (`FF 25`, `48 B8`) AND two-stage CommonLibSSE / SKSE
    // `write_branch<5>` trampolines (`E9 rel32` -> trampoline stub -> `FF 25 [rip+0]` -> `EngineFixes.dll`)
    // are reliably detected and rehooked to allocate `sizeof(ExActor)` instead of vanilla `0x2B0`.
    uintptr_t cursor = reinterpret_cast<uintptr_t>(*pFormAllocate);
    for (int hop = 0; hop < 4 && cursor != 0; ++hop)
    {
        MEMORY_BASIC_INFORMATION codeMbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &codeMbi, sizeof(codeMbi)) == 0 ||
            codeMbi.State != MEM_COMMIT)
        {
            break;
        }

        const uintptr_t target = CoopDamagePipeline::DecodeHookJumpTargetLive(reinterpret_cast<const void*>(cursor), /*aMaxHops=*/1);
        if (target == 0 || target == cursor)
            break;

        MEMORY_BASIC_INFORMATION targetMbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(target), &targetMbi, sizeof(targetMbi)) != 0 &&
            targetMbi.State == MEM_COMMIT &&
            targetMbi.AllocationBase == hEngineFixes)
        {
            *appOutEngineFixesAlloc = reinterpret_cast<TFormAllocate*>(target);
            return true;
        }

        cursor = target;
    }

    return false;
}

static void RehookFormAllocate(TFormAllocate* apEngineFixesAllocate) noexcept
{
    RealFormAllocate = apEngineFixesAllocate;
    TP_HOOK_IMMEDIATE(&RealFormAllocate, HookFormAllocate);
}

size_t Hook_msize(void* apData)
{
    return apData ? mi_malloc_size(apData) : 0;
}

size_t Hook_aligned_msize(void* apData, size_t aAlignment, size_t aOffset)
{
    (void)aAlignment;
    (void)aOffset;
    return apData ? mi_usable_size(apData) : 0;
}

void Hookfree(void* apData)
{
    mi_free(apData);
}

void* Hookcalloc(size_t aCount, size_t aSize)
{
    return mi_calloc(aCount, aSize);
}

void* Hookmalloc(size_t aSize)
{
    return mi_malloc(aSize);
}

void* Hookrealloc(void* apData, size_t aSize)
{
    if (apData != nullptr && aSize == 0)
    {
        mi_free(apData);
        return nullptr;
    }
    return mi_realloc(apData, aSize);
}

void* Hook_recalloc(void* apData, size_t aCount, size_t aSize)
{
    if (apData != nullptr && (aCount == 0 || aSize == 0))
    {
        mi_free(apData);
        return nullptr;
    }
    return mi_recalloc(apData, aCount, aSize);
}

void* Hook_expand(void* apData, size_t aSize)
{
    return apData ? mi_expand(apData, aSize) : nullptr;
}

void Hook_aligned_free(void* apData)
{
    mi_free(apData);
}

void* Hook_aligned_malloc(size_t aSize, size_t aAlignment)
{
    return mi_malloc_aligned(aSize, aAlignment);
}

void* Hook_aligned_realloc(void* apData, size_t aSize, size_t aAlignment)
{
    if (apData != nullptr && aSize == 0)
    {
        mi_free(apData);
        return nullptr;
    }
    return mi_realloc_aligned(apData, aSize, aAlignment);
}

void* Hook_aligned_recalloc(void* apData, size_t aCount, size_t aSize, size_t aAlignment)
{
    if (apData != nullptr && (aCount == 0 || aSize == 0))
    {
        mi_free(apData);
        return nullptr;
    }
    return mi_aligned_recalloc(apData, aCount, aSize, aAlignment);
}

static TiltedPhoques::Initializer s_memoryHooks(
    []()
    {
        POINTER_SKYRIMSE(TFormAllocate, s_formAllocate, 66859);

        POINTER_SKYRIMSE(TFormFree, s_formFree, 66861);

        RealFormAllocate = s_formAllocate.Get();
        RealFormFree = s_formFree.Get();

        TP_HOOK(&RealFormAllocate, HookFormAllocate);
    });

using T_initterm_e = decltype(&_initterm_e);
T_initterm_e Real_initterm_e = nullptr;

extern void EnsureTiltedInitialized();

// If EngineFixes loaded, and it changed our FormAllocate hook,
// reset it. Our hook works just fine chaining to theirs.
int __cdecl Hook_initterm_e(_PIFV* apFirst, _PIFV* apLast)
{
    // Pre-chain first so skse64_plugin_preloader (EngineFixes PreLoad) runs before we install .text detours.
    auto retval = Real_initterm_e(apFirst, apLast);

    // Now both standalone/CODEX EntryPoint unpackers AND EngineFixes PreLoad have finished:
    // load Address Library (version-1-5-97-0.bin) and commit all .text detours before C++ global ctors (_initterm).
    EnsureTiltedInitialized();

    // Check if EngineFixes messed with STR's modified alloc hook; if it did, treat EF as truth and rehook
    TFormAllocate* pEngineFixesAllocate = nullptr;
    if (GetModuleHandleW(L"EngineFixes.dll") && IsFormAllocateReplacedByEF(&pEngineFixesAllocate))
        RehookFormAllocate(pEngineFixesAllocate);

    return retval;
}

void HookFormAllocateSentinelInit()
{
    // Install the COMPLETE CRT heap IAT family BEFORE LC->gameMain() executes instruction #1,
    // ensuring malloc/calloc/realloc/_recalloc/_expand/_aligned_* and free/_msize never split
    // allocations across mimalloc and Windows UCRT RtlReAllocateHeap.
    using T_msize = decltype(&Hook_msize);
    using T_aligned_msize = decltype(&Hook_aligned_msize);
    using Tfree = decltype(&Hookfree);
    using Tcalloc = decltype(&Hookcalloc);
    using Tmalloc = decltype(&Hookmalloc);
    using Trealloc = decltype(&Hookrealloc);
    using T_recalloc = decltype(&Hook_recalloc);
    using T_expand = decltype(&Hook_expand);
    using T_aligned_malloc = decltype(&Hook_aligned_malloc);
    using T_aligned_free = decltype(&Hook_aligned_free);
    using T_aligned_realloc = decltype(&Hook_aligned_realloc);
    using T_aligned_recalloc = decltype(&Hook_aligned_recalloc);

    T_msize Real_msize = nullptr;
    T_aligned_msize Real_aligned_msize = nullptr;
    Tfree Realfree = nullptr;
    Tcalloc Realcalloc = nullptr;
    Tmalloc Realmalloc = nullptr;
    Trealloc Realrealloc = nullptr;
    T_recalloc Real_recalloc = nullptr;
    T_expand Real_expand = nullptr;
    T_aligned_malloc Real_aligned_malloc = nullptr;
    T_aligned_free Real_aligned_free = nullptr;
    T_aligned_realloc Real_aligned_realloc = nullptr;
    T_aligned_recalloc Real_aligned_recalloc = nullptr;

    const char* cHeapModule = "api-ms-win-crt-heap-l1-1-0.dll";
    TP_HOOK_IAT(_msize, cHeapModule);
    TP_HOOK_IAT(_aligned_msize, cHeapModule);
    TP_HOOK_IAT(free, cHeapModule);
    TP_HOOK_IAT(calloc, cHeapModule);
    TP_HOOK_IAT(malloc, cHeapModule);
    TP_HOOK_IAT(realloc, cHeapModule);
    TP_HOOK_IAT(_recalloc, cHeapModule);
    TP_HOOK_IAT(_expand, cHeapModule);
    TP_HOOK_IAT(_aligned_malloc, cHeapModule);
    TP_HOOK_IAT(_aligned_free, cHeapModule);
    TP_HOOK_IAT(_aligned_realloc, cHeapModule);
    TP_HOOK_IAT(_aligned_recalloc, cHeapModule);

    TP_HOOK_IAT(_initterm_e, "api-ms-win-crt-runtime-l1-1-0.dll");
}
#pragma optimize("", on)
