#pragma once
#include <cstdint>
#include <filesystem>

namespace Coop
{
// Advisory process ownership of one journal. The lock file is not removed: unlinking it while
// another process holds the inode can give two writers independent locks on the same campaign.
class DurableLease
{
public:
    explicit DurableLease(const std::filesystem::path&) noexcept;
    ~DurableLease() noexcept;
    DurableLease(const DurableLease&) = delete;
    DurableLease& operator=(const DurableLease&) = delete;
    bool Acquired() const noexcept { return m_handle != -1; }
private:
    intptr_t m_handle{-1};
};
}
