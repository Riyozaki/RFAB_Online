#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace CoopDamagePipeline
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

// Zero-heap, thread-safe authorization window ledger for local-player-authored `kScript` /
// `RFAB_PapyrusFunctions.ScriptDamage` / Perk `OnHit` Papyrus health deltas targeting remote NPCs.
// Ensures:
//   - Non-owner Mages, Rogues, and Arcane Archers deal 100% of their exact RFAB perk/skill scripted damage.
//   - Shared world/quest scripts or passive world effects never double-fire across peers (`IsAuthorized == false`).
template <size_t Capacity = 64>
class ScriptDamageAuthLedger
{
public:
    static constexpr uint64_t kMinWindowMs = 2500ULL;
    static constexpr uint64_t kMaxWindowMs = 120000ULL;

    void Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_entries.fill(Entry{});
        m_cursor = 0;
    }

    void ArmWindow(uint32_t aTargetFormId, uint64_t aNowMs, uint64_t aDurationMs) noexcept
    {
        if (aTargetFormId == 0)
            return;

        const uint64_t clampedDuration = std::clamp(aDurationMs, kMinWindowMs, kMaxWindowMs);
        const uint64_t expiresAt = aNowMs + clampedDuration;

        std::scoped_lock lock(m_mutex);
        for (auto& entry : m_entries)
        {
            if (entry.TargetFormId == aTargetFormId)
            {
                if (expiresAt > entry.ExpiresAtMs)
                    entry.ExpiresAtMs = expiresAt;
                return;
            }
        }

        for (auto& entry : m_entries)
        {
            if (entry.TargetFormId == 0 || aNowMs >= entry.ExpiresAtMs)
            {
                entry.TargetFormId = aTargetFormId;
                entry.ExpiresAtMs = expiresAt;
                return;
            }
        }

        auto& victim = m_entries[m_cursor % Capacity];
        ++m_cursor;
        victim.TargetFormId = aTargetFormId;
        victim.ExpiresAtMs = expiresAt;
    }

    [[nodiscard]] bool IsAuthorized(uint32_t aTargetFormId, uint64_t aNowMs) const noexcept
    {
        if (aTargetFormId == 0)
            return false;

        std::scoped_lock lock(m_mutex);
        for (const auto& entry : m_entries)
        {
            if (entry.TargetFormId == aTargetFormId && aNowMs < entry.ExpiresAtMs)
                return true;
        }
        return false;
    }

private:
    struct Entry
    {
        uint32_t TargetFormId{0};
        uint64_t ExpiresAtMs{0};
    };

    mutable std::mutex m_mutex{};
    std::array<Entry, Capacity> m_entries{};
    size_t m_cursor{0};
};

// Zero-heap, thread-safe packet rate-limiter for hostile concentration `AddTargetEvent` beams
// (Flames, Sparks, Frostbite, Lightning Ray), coalescing 60 Hz Havok beam evaluations to Skyrim's
// ~5 Hz magic tick cadence without dropping caster-authoritative RFAB Destruction perk scaling.
template <size_t Capacity = 64>
class ConcentrationSyncLimiter
{
public:
    static constexpr uint64_t kDefaultCadenceMs = 180ULL;

    void Clear() noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_slots.fill(Slot{});
        m_cursor = 0;
    }

    [[nodiscard]] bool ShouldAllowSend(
        uint32_t aCasterFormId,
        uint32_t aTargetFormId,
        uint32_t aEffectFormId,
        uint64_t aNowMs,
        uint64_t aCadenceMs = kDefaultCadenceMs) noexcept
    {
        if (aTargetFormId == 0 || aEffectFormId == 0)
            return true;

        std::scoped_lock lock(m_mutex);
        for (auto& slot : m_slots)
        {
            if (slot.Active &&
                slot.CasterFormId == aCasterFormId &&
                slot.TargetFormId == aTargetFormId &&
                slot.EffectFormId == aEffectFormId)
            {
                if (aNowMs >= slot.LastSentAtMs && (aNowMs - slot.LastSentAtMs) < aCadenceMs)
                    return false;
                slot.LastSentAtMs = aNowMs;
                return true;
            }
        }

        for (auto& slot : m_slots)
        {
            if (!slot.Active || (aNowMs >= slot.LastSentAtMs && (aNowMs - slot.LastSentAtMs) >= aCadenceMs * 8))
            {
                slot = Slot{aCasterFormId, aTargetFormId, aEffectFormId, aNowMs, true};
                return true;
            }
        }

        auto& victim = m_slots[m_cursor % Capacity];
        ++m_cursor;
        victim = Slot{aCasterFormId, aTargetFormId, aEffectFormId, aNowMs, true};
        return true;
    }

private:
    struct Slot
    {
        uint32_t CasterFormId{0};
        uint32_t TargetFormId{0};
        uint32_t EffectFormId{0};
        uint64_t LastSentAtMs{0};
        bool Active{false};
    };

    std::mutex m_mutex{};
    std::array<Slot, Capacity> m_slots{};
    size_t m_cursor{0};
};

} // namespace CoopDamagePipeline
