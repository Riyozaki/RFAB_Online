#pragma once

#include <algorithm>
#include <array>
#include <cmath>
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

// ============================================================================
// 5. Server-Side Combat & HitClaim Authority Ledger (v2.1.0)
// ============================================================================
// Server-side counterpart to client `ScriptDamageAuthLedger`:
// - Records explicit combat engagement claims (`SpellOrTarget`, `ProjectileLaunch`, `MeleeOrDamage`) per player.
// - Tracks when a player enters the Downed-on-Knee state (`DownedSinceMs`).
// - Validates non-owner `HealthChangeBroadcast` deltas on the server:
//   1. Rejects non-finite, positive (`>= 0`), or out-of-bounds spoofed deltas (`< -100000.0f`).
//   2. Rejects hits on targets already marked dead on the server (`aIsTargetAlreadyDead`).
//   3. Rejects friendly-fire on co-op party members/summons when PvP is disabled (`!aIsPvpEnabled && aAreCoopPartyAllies`).
//   4. Rejects ambient/cloak/script damage emitted by a currently Downed player (`aIsSenderDowned`),
//      UNLESS that player recorded an in-flight projectile or spell claim BEFORE going downed within `kPostDownedInFlightGraceMs` (1500 ms)!
enum class ServerHitClaimKind : uint8_t
{
    kNone = 0,
    kSpellOrTarget = 1,
    kProjectileLaunch = 2,
    kMeleeOrDamage = 3,
};

template <size_t Capacity = 64>
class ServerCombatAuthorityLedger
{
public:
    static constexpr uint64_t kDefaultClaimDurationMs = 4000;
    static constexpr uint64_t kPostDownedInFlightGraceMs = 1500;
    static constexpr float kMaxValidSingleHitMagnitude = 100000.0f;

    void Clear() noexcept
    {
        for (auto& slot : m_claims)
            slot = {};
        for (auto& d : m_downedStates)
            d = {};
        m_claimCursor = 0;
        m_downedCursor = 0;
    }

    void ForgetPlayer(uint32_t aPlayerId) noexcept
    {
        if (aPlayerId == 0)
            return;
        for (auto& slot : m_claims)
        {
            if (slot.Active && slot.AttackerPlayerId == aPlayerId)
                slot = {};
        }
        for (auto& d : m_downedStates)
        {
            if (d.Active && d.PlayerId == aPlayerId)
                d = {};
        }
    }

    void RecordPlayerDownedState(uint32_t aPlayerId, bool aIsDowned, uint64_t aNowMs) noexcept
    {
        if (aPlayerId == 0)
            return;
        for (auto& d : m_downedStates)
        {
            if (d.Active && d.PlayerId == aPlayerId)
            {
                if (!aIsDowned)
                {
                    d = {};
                }
                else if (!d.IsDowned)
                {
                    d.IsDowned = true;
                    d.DownedSinceMs = aNowMs;
                }
                return;
            }
        }
        if (aIsDowned)
        {
            m_downedStates[m_downedCursor % Capacity] = DownedSlot{aPlayerId, aNowMs, true, true};
            ++m_downedCursor;
        }
    }

    void RecordCombatClaim(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        ServerHitClaimKind aKind,
        uint64_t aNowMs,
        uint64_t aDurationMs = kDefaultClaimDurationMs) noexcept
    {
        if (aAttackerPlayerId == 0 || aKind == ServerHitClaimKind::kNone)
            return;

        const uint64_t expiresAt = aNowMs + aDurationMs;
        for (auto& slot : m_claims)
        {
            if (slot.Active && slot.AttackerPlayerId == aAttackerPlayerId && slot.TargetServerId == aTargetServerId)
            {
                slot.Kind = aKind;
                slot.ClaimedAtMs = aNowMs;
                slot.ExpiresAtMs = expiresAt;
                return;
            }
        }

        for (auto& slot : m_claims)
        {
            if (!slot.Active || aNowMs >= slot.ExpiresAtMs)
            {
                slot = ClaimSlot{aAttackerPlayerId, aTargetServerId, aNowMs, expiresAt, aKind, true};
                return;
            }
        }

        m_claims[m_claimCursor % Capacity] =
            ClaimSlot{aAttackerPlayerId, aTargetServerId, aNowMs, expiresAt, aKind, true};
        ++m_claimCursor;
    }

    [[nodiscard]] bool HasActiveClaim(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        uint64_t aNowMs) const noexcept
    {
        if (aAttackerPlayerId == 0)
            return false;
        for (const auto& slot : m_claims)
        {
            if (!slot.Active || slot.AttackerPlayerId != aAttackerPlayerId || aNowMs >= slot.ExpiresAtMs)
                continue;
            if (slot.TargetServerId == aTargetServerId || slot.TargetServerId == 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] bool ValidateNonOwnerHealthDelta(
        uint32_t aAttackerPlayerId,
        uint32_t aTargetServerId,
        float aDeltaHealth,
        bool aIsSenderInRange,
        bool aIsSenderDowned,
        bool aIsTargetAlreadyDead,
        bool aAreCoopPartyAllies,
        bool aIsPvpEnabled,
        uint64_t aNowMs) noexcept
    {
        if (aAttackerPlayerId == 0 || aTargetServerId == 0 || !aIsSenderInRange)
            return false;

        if (!std::isfinite(aDeltaHealth) || aDeltaHealth >= 0.0f || aDeltaHealth < -kMaxValidSingleHitMagnitude)
            return false;

        if (aIsTargetAlreadyDead)
            return false;

        if (aAreCoopPartyAllies && !aIsPvpEnabled)
            return false;

        if (aIsSenderDowned)
        {
            uint64_t downedSinceMs = aNowMs;
            for (const auto& d : m_downedStates)
            {
                if (d.Active && d.PlayerId == aAttackerPlayerId && d.IsDowned)
                {
                    downedSinceMs = d.DownedSinceMs;
                    break;
                }
            }

            // Only allow pre-downed in-flight projectiles or spells launched BEFORE `downedSinceMs`
            // within `kPostDownedInFlightGraceMs` (1500 ms); block all post-downed cloak/script bleed!
            bool hasPreDownedInFlightClaim = false;
            for (const auto& slot : m_claims)
            {
                if (!slot.Active || slot.AttackerPlayerId != aAttackerPlayerId || aNowMs >= slot.ExpiresAtMs)
                    continue;
                if (slot.Kind != ServerHitClaimKind::kProjectileLaunch &&
                    slot.Kind != ServerHitClaimKind::kSpellOrTarget)
                {
                    continue;
                }
                if (slot.TargetServerId != aTargetServerId && slot.TargetServerId != 0)
                    continue;
                if (slot.ClaimedAtMs <= downedSinceMs &&
                    aNowMs <= (downedSinceMs + kPostDownedInFlightGraceMs))
                {
                    hasPreDownedInFlightClaim = true;
                    break;
                }
            }

            if (!hasPreDownedInFlightClaim)
                return false;
        }
        else
        {
            RecordCombatClaim(aAttackerPlayerId, aTargetServerId, ServerHitClaimKind::kMeleeOrDamage, aNowMs);
        }

        return true;
    }

private:
    struct ClaimSlot
    {
        uint32_t AttackerPlayerId{0};
        uint32_t TargetServerId{0};
        uint64_t ClaimedAtMs{0};
        uint64_t ExpiresAtMs{0};
        ServerHitClaimKind Kind{ServerHitClaimKind::kNone};
        bool Active{false};
    };

    struct DownedSlot
    {
        uint32_t PlayerId{0};
        uint64_t DownedSinceMs{0};
        bool IsDowned{false};
        bool Active{false};
    };

    std::array<ClaimSlot, Capacity> m_claims{};
    std::array<DownedSlot, Capacity> m_downedStates{};
    size_t m_claimCursor{0};
    size_t m_downedCursor{0};
};

// ============================================================================
// 6. Native RFAB MaxHP Preservation & Incoming Enemy Damage Scaling (v3.0 Phase 4A)
// ============================================================================
// Instead of mutating client `PERMANENT kHealth` (`ForceActorValue(PERMANENT, kHealth, base * M)`),
// every NPC keeps its exact 1.0x native `RFAB.esp` `MaxHP` across Host, Guest, Server, and `.ess`
// checkpoints, while incoming enemy health damage is scaled by `1 / M` at the engine damage funnel:
//   - Boss percentage-based phase transitions (`50%`, `25%`), execute perks, and HUD bars stay 100% exact.
//   - Zero `3^K` / `1/3` permanent HP drift across ownership handoffs or `.ess` checkpoint saves.
//   - Percentage `HealRate` regeneration and boss phase percentage heals (`+0.5 * MaxHP`) stay 1:1 exact.
[[nodiscard]] inline float NormalizeEnemyHealthMultiplier(float aMultiplier) noexcept
{
    if (!std::isfinite(aMultiplier) || aMultiplier <= 0.0f)
        return 1.0f;
    return std::clamp(aMultiplier, 0.1f, 10.0f);
}

[[nodiscard]] constexpr bool ShouldScaleIncomingEnemyDamage(
    bool aIsPlayer,
    bool aIsPlayerTeammate,
    bool aIsPlayerSummon,
    bool aIsCommandedActor,
    bool aIsMount) noexcept
{
    return !aIsPlayer && !aIsPlayerTeammate && !aIsPlayerSummon && !aIsCommandedActor && !aIsMount;
}

[[nodiscard]] inline float ScaleIncomingEnemyDamageDelta(
    float aRawDelta,
    float aEffectiveMultiplier,
    bool aShouldScale) noexcept
{
    if (!std::isfinite(aRawDelta))
        return 0.0f;
    if (!aShouldScale || aRawDelta >= 0.0f)
        return aRawDelta;
    return aRawDelta / NormalizeEnemyHealthMultiplier(aEffectiveMultiplier);
}

struct AuthoritativeHealthDeltaResult
{
    float UpdatedHealth{0.0f};
    float ClampedBroadcastDelta{0.0f};
};

[[nodiscard]] inline AuthoritativeHealthDeltaResult ApplyAuthoritativeServerHealthDelta(
    float aCurrentHealth,
    float aMaxHealth,
    float aSanitizedDelta) noexcept
{
    AuthoritativeHealthDeltaResult out{};
    if (!std::isfinite(aSanitizedDelta))
    {
        out.UpdatedHealth = std::isfinite(aCurrentHealth) ? std::max(0.0f, aCurrentHealth) : 0.0f;
        return out;
    }

    const float safeCurrent = std::isfinite(aCurrentHealth) ? aCurrentHealth : 0.0f;
    const float rawUpdated = safeCurrent + aSanitizedDelta;
    if (std::isfinite(aMaxHealth) && aMaxHealth > 0.0f)
    {
        out.UpdatedHealth = std::clamp(rawUpdated, 0.0f, aMaxHealth);
        out.ClampedBroadcastDelta = std::clamp(aSanitizedDelta, -aMaxHealth, aMaxHealth);
    }
    else
    {
        out.UpdatedHealth = std::max(0.0f, rawUpdated);
        out.ClampedBroadcastDelta = aSanitizedDelta;
    }
    return out;
}

// ============================================================================
// 7. Seamless Exterior Cell-Border Motion Policy (v3.0 Phase 4B)
// ============================================================================
// Prevents visible 3D flicker, Havok `ResetTeleportPhysics` velocity resets, and 60-144 Hz `MoveTo`
// storms when remote players, summons, or NPCs walk/fight across `4096`-unit exterior cell borders:
//   - `kSmoothForcePosition`: smooth `Actor::ForcePosition` (`SetPosition`) with zero `InternalMoveTo`
//     whenever `parentCell` is attached inside the inner `3x3` loaded core (`Chebyshev <= 1`) around
//     the local player's `centerGrid`.
//   - `kSeamlessCellRehome`: lightweight `MoveToCellSeamless` (`InternalMoveTo` WITHOUT `ResetTeleportPhysics`)
//     only when `parentCell` reaches the outer `5x5` ring (`Chebyshev >= 2`) before ring-3 unload,
//     guarded by a `128`-unit border hysteresis band and `1000 ms` cooldown so border combat never ping-pongs.
//   - `kTeleportMoveTo`: full `MoveTo` (with `ResetTeleportPhysics`) only on true teleports (`> 2048` units)
//     or when `parentCell` is detached / invalid / in another worldspace.
enum class ExteriorCellMotionMode : uint8_t
{
    kSmoothForcePosition = 0,
    kSeamlessCellRehome = 1,
    kTeleportMoveTo = 2,
};

[[nodiscard]] inline float DistanceFromCellBorder4096(float aWorldCoord) noexcept
{
    if (!std::isfinite(aWorldCoord))
        return 0.0f;
    constexpr float kCellSize = 4096.0f;
    float local = std::fmod(aWorldCoord, kCellSize);
    if (local < 0.0f)
        local += kCellSize;
    return std::min(local, kCellSize - local);
}

[[nodiscard]] inline ExteriorCellMotionMode EvaluateExteriorCellMotion(
    bool aStepExceedsTeleportSnap,
    bool aHasValidAttachedParentCell,
    bool aHasLoadedDestCell,
    bool aParentEqualsDestCell,
    int32_t aParentGridX,
    int32_t aParentGridY,
    int32_t aCenterGridX,
    int32_t aCenterGridY,
    float aPosX,
    float aPosY,
    uint64_t aNowTick,
    uint64_t aLastSeamlessRehomeTick,
    float aBorderHysteresisUnits = 128.0f,
    uint64_t aRehomeCooldownMs = 1000ULL) noexcept
{
    if (!aHasLoadedDestCell)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    if (aStepExceedsTeleportSnap || !aHasValidAttachedParentCell)
        return ExteriorCellMotionMode::kTeleportMoveTo;

    if (aParentEqualsDestCell)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    const int32_t dx = (aParentGridX >= aCenterGridX) ? (aParentGridX - aCenterGridX) : (aCenterGridX - aParentGridX);
    const int32_t dy = (aParentGridY >= aCenterGridY) ? (aParentGridY - aCenterGridY) : (aCenterGridY - aParentGridY);
    const int32_t parentRingDist = std::max(dx, dy);

    // While `parentCell` sits in the inner 3x3 core (`ring <= 1`), it is at least 2 full grid shifts
    // away from unloading (`ring >= 3`), so smooth `ForcePosition` needs zero `InternalMoveTo` calls.
    if (parentRingDist <= 1)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    const float borderDist = std::min(DistanceFromCellBorder4096(aPosX), DistanceFromCellBorder4096(aPosY));
    if (borderDist < aBorderHysteresisUnits)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    if (aLastSeamlessRehomeTick != 0 && aNowTick < aLastSeamlessRehomeTick + aRehomeCooldownMs)
        return ExteriorCellMotionMode::kSmoothForcePosition;

    return ExteriorCellMotionMode::kSeamlessCellRehome;
}

} // namespace CoopDamagePipeline
