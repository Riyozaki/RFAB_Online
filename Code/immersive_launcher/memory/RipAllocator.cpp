
// Deliberately in no namespace...

#include <Windows.h>
#include <cstdint>
#include <cstring>

extern uint8_t highrip[];
static uint8_t* globalPointer = &highrip[0];

void* RipAllocateN(size_t blockLength)
{
    // Typical alignment size for function calls on x64 systems
    const size_t alignment = 16;

    // Calculate the next aligned address for the start of the block
    const uintptr_t currentPtr = reinterpret_cast<uintptr_t>(globalPointer);
    const uintptr_t alignedPtr = (currentPtr + (alignment - 1)) & ~(alignment - 1);
    const size_t paddingSize = alignedPtr - currentPtr;

    // Align the block length as well
    const size_t alignedBlockLength = (blockLength + (alignment - 1)) & ~(alignment - 1);

    // Ensure highrip[] pages in .game BSS are marked executable (PAGE_EXECUTE_READWRITE)
    // so Xbyak JIT stubs (such as FireEntryStub on spell cast) NEVER fault on hardware DEP / NX!
    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<void*>(currentPtr), paddingSize + alignedBlockLength + 16, PAGE_EXECUTE_READWRITE, &oldProtect);

    // Pad the unused space with 0xCC
    memset(reinterpret_cast<void*>(currentPtr), 0xCC, paddingSize);

    // Advance the global pointer by the aligned block length plus the padding
    globalPointer += alignedBlockLength + paddingSize;

    // Pad the end of the block if there is any space remaining due to alignment
    if (alignedBlockLength > blockLength)
    {
        const size_t endPaddingSize = alignedBlockLength - blockLength;
        memset(reinterpret_cast<void*>(alignedPtr + blockLength), 0xCC, endPaddingSize);
    }

    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(alignedPtr), alignedBlockLength);

    // Return the aligned pointer
    return reinterpret_cast<void*>(alignedPtr);
}

// TBD
void RipFree(void* apBlock)
{
}
