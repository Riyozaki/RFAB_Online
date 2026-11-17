#pragma once
#include <Coop/InventoryLedger.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Coop
{
struct StorageLocation
{
    StableForm Cell;
    std::optional<StableForm> World; // absent only for an interior
    float X{}, Y{}, Z{};
    bool operator==(const StorageLocation&) const = default;
};
struct StorageRule
{
    StableForm Access, Backing;
    StorageLocation Location; // winning installed REFR DATA + enclosing CELL/WRLD, never a client claim
    bool operator==(const StorageRule&) const = default;
};
// Read-only installed-profile rules. This is neither a cell cache nor client authority.
// V2 requires trusted access positions. Six-column V1 has no spatial authority and is refused.
// TAB separation allows plugin basenames with spaces; a partial/malformed file is rejected.
class StorageCatalog
{
public:
    bool Parse(std::string_view);
    bool Read(const std::filesystem::path&);
    const std::vector<StorageRule>& Rules() const noexcept { return m_rules; }
    const StorageRule* Find(const StableForm& aAccess) const;
    const StableForm* Backing(const StableForm& aAccess) const;
    bool IsBacking(const StableForm&) const;
    // The position is the server's accepted OWN-AVATAR movement, not the point-registration
    // position or StorageOpen data. This is not server physics or an anti-teleport guarantee.
    bool InReach(const StableForm& aAccess, const StableForm& aCell, const std::optional<StableForm>& aWorld,
        float aX, float aY, float aZ, float aRadius = 320.f) const;
    // Deterministic mismatch detector over canonical forms AND locations, NOT authentication.
    uint64_t Signature() const noexcept { return m_signature; }
private:
    std::vector<StorageRule> m_rules;
    uint64_t m_signature{};
};
}
