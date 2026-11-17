#pragma once
#include <cstdint>

// Versioned C ABI for installed RFAB/RFAB Bizarre reward adapters. No STL, engine pointers,
// wallet polling, network ids or remote paths cross this boundary. All callbacks use owned
// scalars; the host calls Apply only on the main thread under a save/native reservation.
namespace Coop::Rewards
{
inline constexpr uint32_t kVersion = 1;
enum Flag : uint32_t { PersonalMultipliers = 1, BothPools = 2, SuppressesOriginal = 4, Bizarre = 8 };
inline constexpr uint32_t kRequiredFlags = PersonalMultipliers | BothPools | SuppressesOriginal;
enum Pool : uint32_t { Wallet = 1, Engine = 2 };
enum class Mode : uint32_t { Offline, Negotiating, Active, Blocked };
enum class Submission : uint32_t { PassThrough, Queued, Blocked };
enum class Result : uint32_t { NotApplied, Applied, Pending, Uncertain };
struct Completion
{
    uint64_t WorldEpoch{}, Token{};
    Result State{Result::Uncertain};
    int32_t FinalXP{};
    uint32_t Pools{};
    bool operator==(const Completion&) const = default;
};
using Apply = Completion (*)(uint64_t aWorldEpoch, uint64_t aToken, int32_t aBaseXP);
struct Adapter
{
    uint32_t Version{kVersion}, Size{sizeof(Adapter)};
    uint64_t Signature{}; // installed script/native semantic profile digest, not personal multiplier
    uint32_t Kinds{}, Flags{};
    char Name[64]{};
    uint64_t Receiver{}; // Papyrus handle of an attached quest script, generation-bound; OR native Apply
    Apply ApplyReward{};
};
struct Source
{
    uint32_t Size{sizeof(Source)}, Kind{};
    uint32_t SourceForm{}, ContextForm{}, ActorForm{};
    int32_t BaseXP{};
    uint64_t WorldEpoch{}, Incarnation{}, Occurrence{}, InboundToken{};
    bool operator==(const Source&) const = default;
};
struct Host
{
    uint32_t Version{kVersion}, Size{sizeof(Host)};
    // Must register after each world load; changing adapters in an active generation fails closed.
    bool (*Register)(uint64_t aWorldEpoch, const Adapter*);
    uint64_t (*GetEpoch)();
    Mode (*GetMode)();
    // The original RFAB reward path MUST run only for PassThrough, never Queued or Blocked.
    Submission (*Submit)(const Source*);
    bool (*Complete)(const Completion*);
};
} // namespace Coop::Rewards
// Windows export on the launched executable: RFAB_GetRewardBridge(uint32_t version).
