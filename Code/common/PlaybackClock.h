#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

/**
 * @brief Adaptive remote-actor playback clock (TiltedEvolution #921 + post-combat slowdown guard).
 *
 * Replaces the fixed 300 ms remote delay with per-entity adaptive jitter/interval estimation:
 *   renderTick = now - minTransit - targetDelay
 * where ArrivalTime uses network packet receive timestamps (not frame-processing time) so local frame
 * hitches never inflate remote actor lag.
 */
struct PlaybackClock
{
    inline bool AddSample(const double aArrivalTime, const uint64_t aCaptureTick) noexcept
    {
        for (size_t i = 0; i < m_sampleCount; ++i)
        {
            if (m_samples[i].CaptureTick == aCaptureTick)
                return false;
        }

        const bool cIsFirstSample = !IsRunning();

        m_samples[m_nextSample] = {aArrivalTime, aCaptureTick};
        m_nextSample = (m_nextSample + 1) % cMaxSamples;
        m_sampleCount = std::min(m_sampleCount + 1, cMaxSamples);

        UpdateEstimates(aArrivalTime);

        if (cIsFirstSample)
        {
            m_lastAdvanceTime = aArrivalTime;
            m_renderTick = GetDesiredRenderTick(aArrivalTime);
        }

        return true;
    }

    inline double Advance(const double aNow) noexcept
    {
        if (!IsRunning())
            return 0.0;

        const double delta = std::max(aNow - m_lastAdvanceTime, 0.0);
        m_lastAdvanceTime = aNow;

        const double cCorrection = std::clamp(delta * (m_playbackRate - 1.0), std::min(m_error, 0.0), std::max(m_error, 0.0));
        m_renderTick += delta + cCorrection;

        const double desiredRenderTick = GetDesiredRenderTick(aNow);
        m_error = desiredRenderTick - m_renderTick;

        if (std::abs(m_error) > cSnapThreshold)
        {
            m_renderTick = desiredRenderTick;
            m_playbackRate = 1.0;
            m_error = 0.0;
        }
        else
        {
            m_playbackRate = 1.0 + std::clamp(m_error / cFullRateAdjustmentError * cMaxRateAdjustment, -cMaxRateAdjustment, cMaxRateAdjustment);
        }

        return delta;
    }

    inline void Reset() noexcept
    {
        m_sampleCount = 0;
        m_nextSample = 0;
        m_minTransit = 0.0;
        m_jitter = 0.0;
        m_snapshotInterval = cDefaultSnapshotInterval;
        m_targetDelay = cDefaultSnapshotInterval + cSafetyMargin;
        m_renderTick = 0.0;
        m_lastAdvanceTime = 0.0;
        m_playbackRate = 1.0;
        m_error = 0.0;
    }

    [[nodiscard]] bool IsRunning() const noexcept { return m_sampleCount > 0; }
    [[nodiscard]] double GetRenderTick() const noexcept { return m_renderTick; }
    [[nodiscard]] double GetTargetDelay() const noexcept { return m_targetDelay; }
    [[nodiscard]] double GetJitter() const noexcept { return m_jitter; }
    [[nodiscard]] double GetSnapshotInterval() const noexcept { return m_snapshotInterval; }
    [[nodiscard]] double GetPlaybackRate() const noexcept { return m_playbackRate; }

    static constexpr double cSampleWindow = 3000.0;
    static constexpr size_t cMaxSamples = 128;
    static constexpr double cDefaultSnapshotInterval = 100.0;
    static constexpr size_t cRecentGaps = 4;
    static constexpr double cJitterPercentile = 0.95;
    static constexpr double cSafetyMargin = 8.0;
    static constexpr double cMinTargetDelay = 16.0;
    static constexpr double cMaxTargetDelay = 250.0;
    static constexpr double cMaxRateAdjustment = 0.1;
    static constexpr double cFullRateAdjustmentError = 50.0;
    static constexpr double cSnapThreshold = 250.0;

private:
    struct Sample
    {
        double ArrivalTime;
        uint64_t CaptureTick;
    };

    inline void UpdateEstimates(const double aNewestArrivalTime) noexcept
    {
        std::array<double, cMaxSamples> transits;
        std::array<uint64_t, cMaxSamples> captureTicks;
        size_t count = 0;
        uint64_t oldestCaptureTick = std::numeric_limits<uint64_t>::max();
        uint64_t newestCaptureTick = 0;

        for (size_t i = 0; i < m_sampleCount; ++i)
        {
            const auto& sample = m_samples[i];
            if (sample.ArrivalTime < aNewestArrivalTime - cSampleWindow)
                continue;

            captureTicks[count] = sample.CaptureTick;
            transits[count++] = sample.ArrivalTime - static_cast<double>(sample.CaptureTick);
            oldestCaptureTick = std::min(oldestCaptureTick, sample.CaptureTick);
            newestCaptureTick = std::max(newestCaptureTick, sample.CaptureTick);
        }

        if (count == 0)
            return;

        const auto end = transits.begin() + count;
        m_minTransit = *std::min_element(transits.begin(), end);

        const auto percentile = transits.begin() + static_cast<size_t>(std::ceil(cJitterPercentile * static_cast<double>(count - 1)));
        std::nth_element(transits.begin(), percentile, end);
        m_jitter = *percentile - m_minTransit;

        if (count >= 2)
            m_snapshotInterval = static_cast<double>(newestCaptureTick - oldestCaptureTick) / static_cast<double>(count - 1);

        if (count > cRecentGaps)
        {
            const auto recentCaptureTick = captureTicks.begin() + cRecentGaps;
            std::nth_element(captureTicks.begin(), recentCaptureTick, captureTicks.begin() + count, std::greater<>{});
            m_snapshotInterval = std::max(
                m_snapshotInterval,
                static_cast<double>(newestCaptureTick - *recentCaptureTick) / static_cast<double>(cRecentGaps));
        }

        m_targetDelay = std::clamp(m_snapshotInterval + m_jitter + cSafetyMargin, cMinTargetDelay, cMaxTargetDelay);
    }

    [[nodiscard]] double GetDesiredRenderTick(const double aNow) const noexcept
    {
        return aNow - m_minTransit - m_targetDelay;
    }

    std::array<Sample, cMaxSamples> m_samples{};
    size_t m_sampleCount{0};
    size_t m_nextSample{0};

    double m_minTransit{0.0};
    double m_jitter{0.0};
    double m_snapshotInterval{cDefaultSnapshotInterval};
    double m_targetDelay{cDefaultSnapshotInterval + cSafetyMargin};

    double m_renderTick{0.0};
    double m_lastAdvanceTime{0.0};
    double m_playbackRate{1.0};
    double m_error{0.0};
};
