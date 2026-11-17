#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace Coop
{
// One bounded, checksummed, atomic snapshot. Success is reported only after the new file and
// its rename are flushed. A damaged existing file is NEVER interpreted as a new empty campaign.
// This is not a distributed/Skyrim-save transaction; unknown native application stays quarantined.
class DurableFile
{
public:
    enum class ReadResult { Missing, Loaded, Failed };
    enum class Step { BeforeWrite, AfterWrite, AfterFlush, AfterReplace };
    using Fault = std::function<bool(Step)>; // deterministic failure injection; empty in production
    static constexpr uint32_t kMaxBytes = 32 * 1024 * 1024;

    explicit DurableFile(std::filesystem::path aPath) : m_path(std::move(aPath)) {}
    bool Write(std::span<const uint8_t> aBytes, const Fault& aFault = {}) const noexcept;
    ReadResult Read(std::vector<uint8_t>& aBytes) const noexcept;

private:
    std::filesystem::path m_path;
};
} // namespace Coop
