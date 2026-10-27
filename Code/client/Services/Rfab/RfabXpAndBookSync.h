#pragma once

#include <common/RfabModpackCompat.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct World;
struct TransportService;
struct TESGlobal;
struct NotifyRfabProgressionEvent;
class RfabFormResolver;

/**
 * Owns RFAB dual-pool experience application (RFAB_XP_Amount + PlayerCharacter::Skills::xp),
 * XPBooksReaded FormList synchronization, watched TESGlobal synchronization,
 * REQ_Combat_NoDeathHandling enforcement, and [RFAB] Autosave.ini session suppression/restore.
 */
class RfabXpAndBookSync
{
public:
    RfabXpAndBookSync() noexcept = default;
    ~RfabXpAndBookSync() noexcept { RestoreSoloRfabAutosave(); }

    TP_NOCOPYMOVE(RfabXpAndBookSync);

    [[nodiscard]] static int32_t ReadRfabXp(TESGlobal* apGlobal) noexcept;
    [[nodiscard]] static bool ApplyDualPoolExperienceDelta(
        TESGlobal* apRfabXpGlobal,
        int32_t aDelta,
        int32_t& aOutNewTotalXp) noexcept;

    void SuppressSoloRfabAutosave(const RfabCompat::RfabBindingsConfig& acBindings) noexcept;
    void RestoreSoloRfabAutosave() noexcept;

    void ApplySessionNoDeathHandling(RfabFormResolver& aResolver) noexcept;
    void RestoreSessionNoDeathHandling() noexcept;
    void EnforceNoDeathHandlingTick() noexcept;

    void SeedReadBooksOnConnect(RfabFormResolver& aResolver) noexcept;
    void ResetRuntimeWatches() noexcept;
    void ResetOnDisconnect() noexcept;

    void PollReadXpBooks(World& aWorld, TransportService& aTransport, RfabFormResolver& aResolver) noexcept;
    void PollSyncedGlobals(TransportService& aTransport, RfabFormResolver& aResolver) noexcept;

    void ApplyBookReadEvent(
        World& aWorld,
        RfabFormResolver& aResolver,
        const NotifyRfabProgressionEvent& acMessage) noexcept;
    void ApplyGlobalValueSyncEvent(
        RfabFormResolver& aResolver,
        const NotifyRfabProgressionEvent& acMessage) noexcept;

    void AdvanceWorldEpochIfNewer(uint64_t aEpochId) noexcept
    {
        if (aEpochId > m_currentWorldEpoch)
            m_currentWorldEpoch = aEpochId;
    }

    [[nodiscard]] uint64_t GetCurrentWorldEpoch() const noexcept { return m_currentWorldEpoch; }

private:
    TESGlobal* m_pNoDeathHandlingGlobal{nullptr};
    float m_savedNoDeathHandlingValue{0.0f};
    bool m_hasSavedNoDeathHandlingValue{false};

    std::filesystem::path m_autosaveIniPath{};
    std::string m_originalAutosaveIniContent{};
    bool m_autosaveSuppressed{false};

    uint64_t m_currentWorldEpoch{1};
    bool m_scannedGlobalsOnce{false};
    bool m_seededReadBooks{false};
    std::unordered_set<uint32_t> m_knownReadBookFormIds{};
    std::unordered_map<std::string, TESGlobal*> m_watchedGlobals{};
    std::unordered_map<std::string, float> m_lastGlobalValues{};
};
