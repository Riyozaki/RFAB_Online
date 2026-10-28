#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace CoopTeleportGuard
{
    inline uint64_t NowSteadyNs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    /**
     * Latched suppression state machine for legacy `PlayerService::RunRespawnUpdates` (`RespawnPlayer()` ->
     * `GetCOCPlacementInfo` -> spontaneous teleport to the nearest cell COC marker):
     * - Once `DownedSystemEnabled` is active in a session (or while disconnected / within a 15-second
     *   transient-disconnect / reconnect grace window), `ShouldSuppressLegacyRespawnTeleport` ALWAYS
     *   returns `true`, preventing `GetCOCPlacementInfo` from ever firing across brief disconnects
     *   or before `NotifySettings` arrives on reconnect.
     */
    class LatchedDownedRespawnGuard
    {
    public:
        static constexpr uint64_t kPostDisconnectCooldownMs = 15000ULL;

        void NotifyConnected(uint64_t aNowMs) noexcept
        {
            m_isConnected = true;
            m_lastTransitionMs = aNowMs;
        }

        void NotifyDisconnected(uint64_t aNowMs) noexcept
        {
            m_isConnected = false;
            m_lastDisconnectMs = aNowMs;
            m_lastTransitionMs = aNowMs;
        }

        void NotifyServerSettings(bool aDeathSystemEnabled, bool aDownedSystemEnabled) noexcept
        {
            m_deathSystemEnabled = aDeathSystemEnabled;
            m_downedSystemEnabled = aDownedSystemEnabled;
            if (aDownedSystemEnabled)
                m_downedLatchedInSession = true;
            else if (aDeathSystemEnabled)
                m_downedLatchedInSession = false;
        }

        [[nodiscard]] bool ShouldSuppressLegacyRespawnTeleport(uint64_t aNowMs) const noexcept
        {
            // Never allow multiplayer COC respawn teleport while offline!
            if (!m_isConnected)
                return true;

            // Suppress whenever Downed-on-Knee is active OR latched from earlier in this session.
            if (m_downedSystemEnabled || m_downedLatchedInSession)
                return true;

            if (!m_deathSystemEnabled)
                return true;

            // Also suppress during the 15-second cooldown following any transient disconnect.
            if (m_lastDisconnectMs > 0 && aNowMs >= m_lastDisconnectMs &&
                (aNowMs - m_lastDisconnectMs) < kPostDisconnectCooldownMs)
            {
                return true;
            }

            return false;
        }

        [[nodiscard]] bool IsDownedLatched() const noexcept { return m_downedLatchedInSession; }

    private:
        bool m_isConnected{false};
        bool m_deathSystemEnabled{false};
        bool m_downedSystemEnabled{true};
        bool m_downedLatchedInSession{true};
        uint64_t m_lastDisconnectMs{0};
        uint64_t m_lastTransitionMs{0};
    };

    /**
     * Cross-thread guard preventing remote peer quest stage updates (`QuestService::OnQuestUpdate` ->
     * `TESQuest::SetStage`) and their asynchronous Papyrus stage fragments (`QF_*` calling
     * `Game.GetPlayer().MoveTo(MarkerRef)` on Papyrus VM worker threads) from spontaneously yanking
     * the local `PlayerCharacter` (`0x14`) across the map to another player's quest marker.
     *
     * Authorized multiplayer teleports (`CoordinatedSaveService::ApplyInSessionRollback`, `PartyService`,
     * `TESObjectREFR::MoveTo` under `ScopedReferencesOverride`) bypass this guard cleanly.
     */
    class RemoteQuestMoveToGuard
    {
    public:
        static constexpr uint64_t kDefaultAsyncPapyrusHoldNs = 750'000'000ULL; // 750 ms post-SetStage async VM window
        static constexpr uint64_t kExtendedLateSessionPapyrusHoldNs = 4'500'000'000ULL; // 4.5 s late-session VM lag guard
        static constexpr uint64_t kLocalPlayerTravelGraceNs = 8'000'000'000ULL; // 8.0 s carriage fade-out / scripted travel grace

        static void EnterScope(uint64_t aNowNs = NowSteadyNs(), uint64_t aAsyncHoldNs = kDefaultAsyncPapyrusHoldNs) noexcept
        {
            s_activeDepth.fetch_add(1, std::memory_order_acq_rel);
            ExtendHoldWindow(aNowNs + aAsyncHoldNs);
        }

        static void ExitScope(uint64_t aNowNs = NowSteadyNs(), uint64_t aAsyncHoldNs = kDefaultAsyncPapyrusHoldNs) noexcept
        {
            ExtendHoldWindow(aNowNs + aAsyncHoldNs);
            const uint32_t prev = s_activeDepth.load(std::memory_order_acquire);
            if (prev > 0)
                s_activeDepth.fetch_sub(1, std::memory_order_acq_rel);
        }

        static void ClearAsyncHoldForLocalInteraction() noexcept
        {
            if (s_activeDepth.load(std::memory_order_acquire) == 0)
                s_holdUntilNs.store(0, std::memory_order_release);
        }

        static void NoteLocalDirectPlayerTravelInteraction(uint64_t aNowNs = NowSteadyNs()) noexcept
        {
            ClearAsyncHoldForLocalInteraction();
            const uint64_t candidateUntil = aNowNs + kLocalPlayerTravelGraceNs;
            uint64_t cur = s_localTravelGraceUntilNs.load(std::memory_order_relaxed);
            while (candidateUntil > cur &&
                   !s_localTravelGraceUntilNs.compare_exchange_weak(
                       cur, candidateUntil, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }

        [[nodiscard]] static bool ShouldBlockUnauthorizedPlayerMoveTo(uint64_t aNowNs = NowSteadyNs()) noexcept
        {
            if (s_activeDepth.load(std::memory_order_acquire) > 0)
                return true;

            const uint64_t localGraceUntil = s_localTravelGraceUntilNs.load(std::memory_order_acquire);
            if (localGraceUntil > 0 && aNowNs < localGraceUntil)
                return false;

            const uint64_t holdUntil = s_holdUntilNs.load(std::memory_order_acquire);
            return holdUntil > 0 && aNowNs < holdUntil;
        }

        static void ResetForTesting() noexcept
        {
            s_activeDepth.store(0, std::memory_order_release);
            s_holdUntilNs.store(0, std::memory_order_release);
            s_localTravelGraceUntilNs.store(0, std::memory_order_release);
        }

    private:
        static void ExtendHoldWindow(uint64_t aCandidateUntilNs) noexcept
        {
            uint64_t cur = s_holdUntilNs.load(std::memory_order_relaxed);
            while (aCandidateUntilNs > cur &&
                   !s_holdUntilNs.compare_exchange_weak(cur, aCandidateUntilNs, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }

        inline static std::atomic<uint32_t> s_activeDepth{0};
        inline static std::atomic<uint64_t> s_holdUntilNs{0};
        inline static std::atomic<uint64_t> s_localTravelGraceUntilNs{0};
    };

    struct ScopedRemoteQuestMoveToSuppression
    {
        explicit ScopedRemoteQuestMoveToSuppression(
            uint64_t aNowNs = NowSteadyNs(),
            uint64_t aAsyncHoldNs = RemoteQuestMoveToGuard::kDefaultAsyncPapyrusHoldNs) noexcept
            : m_holdNs(aAsyncHoldNs)
        {
            RemoteQuestMoveToGuard::EnterScope(aNowNs, m_holdNs);
        }

        ~ScopedRemoteQuestMoveToSuppression() noexcept
        {
            RemoteQuestMoveToGuard::ExitScope(NowSteadyNs(), m_holdNs);
        }

        ScopedRemoteQuestMoveToSuppression(const ScopedRemoteQuestMoveToSuppression&) = delete;
        ScopedRemoteQuestMoveToSuppression& operator=(const ScopedRemoteQuestMoveToSuppression&) = delete;

    private:
        uint64_t m_holdNs;
    };
} // namespace CoopTeleportGuard
