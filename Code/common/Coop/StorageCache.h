#pragma once
#include <Coop/ClientTimeline.h>
#include <string_view>

namespace Coop
{
enum class StorageCacheDecision { AwaitImport, Blocked, AwaitNewRevision, Current, Adopt, Reconcile };
inline StorageCacheDecision StorageCachePolicy(const SavedStorage* saved, std::string_view campaign,
    uint64_t revision, bool needsImport, bool localEmpty, bool exact, bool confirmed) noexcept
{
    if (!revision || needsImport) return StorageCacheDecision::AwaitImport;
    if (saved && (saved->Campaign != campaign || saved->Revision > revision)) return StorageCacheDecision::Blocked;
    if (!saved)
        return confirmed && (localEmpty || exact) ? StorageCacheDecision::Adopt : StorageCacheDecision::Blocked;
    if (saved->Revision == revision)
        // A local paired native move may be Applied while the corresponding newer bank snapshot
        // is still in flight. An old SAME-revision snapshot is NOT permission to clone it back.
        return exact ? StorageCacheDecision::Current : StorageCacheDecision::AwaitNewRevision;
    return StorageCacheDecision::Reconcile;
}
}
