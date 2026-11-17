#pragma once
#include <Coop/InventoryLedger.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Coop
{
struct StorageRule
{
    StableForm Access, Backing;
    bool operator==(const StorageRule&) const = default;
};
// Read-only installed-profile rules. This is neither a cell cache nor client authority.
// TAB separation allows plugin basenames with spaces; a partial/malformed file is rejected.
class StorageCatalog
{
public:
    bool Parse(std::string_view);
    bool Read(const std::filesystem::path&);
    const std::vector<StorageRule>& Rules() const noexcept { return m_rules; }
    const StableForm* Backing(const StableForm& aAccess) const;
    bool IsBacking(const StableForm&) const;
    // Deterministic mismatch detector over canonical rules, NOT an authentication/hash promise.
    uint64_t Signature() const noexcept { return m_signature; }
private:
    std::vector<StorageRule> m_rules;
    uint64_t m_signature{};
};
}
