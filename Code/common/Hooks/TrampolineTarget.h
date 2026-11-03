#pragma once

/**
 * Reading where a hook actually goes.
 *
 * Several mods (SSE Engine Fixes, SKSE's BranchTrampoline, MinHook, Xbyak) overwrite the
 * entry of a game function with a jump. To call the original, RFAB has to decode that
 * jump and follow it. That is x86-64 instruction decoding and has nothing to do with
 * combat, which is where it used to live.
 */

#include <cstdint>
#include <cstring>

namespace Hooks
{

// Decodes common x86-64 hook trampolines installed at the entry of `FormAllocate` (`0x190D50` SE 1.5.97)
// by SSE Engine Fixes (`MemoryManager = true`), MinHook, SKSE `BranchTrampoline`, or Xbyak:
//   1. `E9 cd`                   -> 5-byte RIP-relative `jmp rel32`
//   2. `FF 25 cd`                -> 6-byte RIP-indirect `jmp qword ptr [rip + disp32]` (arbitrary signed disp32)
//   3. `48 B8 io FF E0`          -> 12-byte `mov rax, imm64; jmp rax`
//   4. `49 BB io 41 FF E3`       -> 13-byte `mov r11, imm64; jmp r11`
// `aReadPtrAt` reads an indirect 64-bit pointer from `aIndirectSlotAddress` (defaults to live memory read).
template <class TReadIndirectPtr>
[[nodiscard]] inline uintptr_t DecodeHookJumpTarget(
    const uint8_t* apBytes,
    uintptr_t aInstructionAddress,
    TReadIndirectPtr&& aReadPtrAt) noexcept
{
    if (!apBytes || aInstructionAddress == 0)
        return 0;

    // 0. `EB rel8` (2 bytes): target = instruction + 2 + int8(rel8)
    if (apBytes[0] == 0xEBu)
    {
        const auto rel8 = static_cast<int8_t>(apBytes[1]);
        return static_cast<uintptr_t>(static_cast<int64_t>(aInstructionAddress) + 2 + static_cast<int64_t>(rel8));
    }

    // 1. `E9 rel32` (5 bytes): target = instruction + 5 + int32(rel32)
    if (apBytes[0] == 0xE9u)
    {
        int32_t rel32 = 0;
        std::memcpy(&rel32, apBytes + 1, sizeof(rel32));
        return static_cast<uintptr_t>(static_cast<int64_t>(aInstructionAddress) + 5 + static_cast<int64_t>(rel32));
    }

    // 2. `FF 25 disp32` (6 bytes): target = *(uint64_t*)(instruction + 6 + int32(disp32))
    if (apBytes[0] == 0xFFu && apBytes[1] == 0x25u)
    {
        int32_t disp32 = 0;
        std::memcpy(&disp32, apBytes + 2, sizeof(disp32));
        const uintptr_t slotAddr =
            static_cast<uintptr_t>(static_cast<int64_t>(aInstructionAddress) + 6 + static_cast<int64_t>(disp32));
        return aReadPtrAt(slotAddr);
    }

    // 3. `48 B8 imm64 FF E0` (12 bytes): `mov rax, imm64; jmp rax`
    if (apBytes[0] == 0x48u && apBytes[1] == 0xB8u && apBytes[10] == 0xFFu && apBytes[11] == 0xE0u)
    {
        uint64_t abs64 = 0;
        std::memcpy(&abs64, apBytes + 2, sizeof(abs64));
        return static_cast<uintptr_t>(abs64);
    }

    // 4. `49 BB imm64 41 FF E3` (13 bytes): `mov r11, imm64; jmp r11`
    if (apBytes[0] == 0x49u && apBytes[1] == 0xBBu &&
        apBytes[10] == 0x41u && apBytes[11] == 0xFFu && apBytes[12] == 0xE3u)
    {
        uint64_t abs64 = 0;
        std::memcpy(&abs64, apBytes + 2, sizeof(abs64));
        return static_cast<uintptr_t>(abs64);
    }

    return 0;
}

[[nodiscard]] inline uintptr_t DecodeHookJumpTargetLive(const void* apFuncEntry, int aMaxHops = 4) noexcept
{
    if (!apFuncEntry || aMaxHops <= 0)
        return 0;

    uintptr_t cursor = reinterpret_cast<uintptr_t>(apFuncEntry);
    uintptr_t lastResolved = 0;

    for (int hop = 0; hop < aMaxHops && cursor != 0; ++hop)
    {
        const auto* pBytes = reinterpret_cast<const uint8_t*>(cursor);
        const uintptr_t next = DecodeHookJumpTarget(
            pBytes,
            cursor,
            [](uintptr_t aSlotAddr) noexcept -> uintptr_t
            {
                if (aSlotAddr == 0)
                    return 0;
                uintptr_t target = 0;
                std::memcpy(&target, reinterpret_cast<const void*>(aSlotAddr), sizeof(target));
                return target;
            });

        if (next == 0)
            break;

        lastResolved = next;
        if (next == cursor)
            break;
        cursor = next;
    }

    return lastResolved;
}

} // namespace Hooks
