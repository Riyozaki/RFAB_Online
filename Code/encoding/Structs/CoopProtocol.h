#pragma once

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <Structs/GameId.h>
#include <Structs/Inventory.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace Coop
{
inline constexpr uint32_t kProtocol = 2;
inline constexpr uint32_t kReviveMs = 5000;
inline constexpr uint32_t kWipeMs = 5000;
inline constexpr uint32_t kHeartbeatTimeoutMs = 1500;
inline constexpr uint32_t kHoldTimeoutMs = 750;
inline constexpr uint32_t kSafeDwellMs = 5000;

enum Capability : uint32_t
{
    NativeDowned = 1 << 0, GroundedAnchors = 1 << 1, SemanticRewards = 1 << 2,
    TransactionalLoot = 1 << 3, PersistentStorage = 1 << 4, CheckpointReload = 1 << 5,
    EnemyScaling = 1 << 6, BizarreAdapter = 1 << 7
};
enum PresenceFlag : uint32_t
{
    Bleedout = 1 << 0, InCombat = 1 << 1, CanInteract = 1 << 2, Grounded = 1 << 3,
    Stable = 1 << 4, Loading = 1 << 5, UnsafeEncounter = 1 << 6
};

#define RFAB_COOP_FIELDS(...) template <class F> void Fields(F&& f) { f(__VA_ARGS__); }

struct Pose
{
    GameId Cell{}, World{};
    float X{}, Y{}, Z{}, Heading{};
    RFAB_COOP_FIELDS(Cell, World, X, Y, Z, Heading)
    bool Valid() const noexcept { return Cell && std::isfinite(X) && std::isfinite(Y) && std::isfinite(Z) && std::isfinite(Heading); }
};
struct Hello
{
    uint32_t Protocol{kProtocol}, Capabilities{};
    std::string Character, Adapter;
    RFAB_COOP_FIELDS(Protocol, Capabilities, Character, Adapter)
};
struct Welcome
{
    uint32_t Capabilities{};
    std::string Campaign, Error;
    RFAB_COOP_FIELDS(Capabilities, Campaign, Error)
};
struct Presence
{
    uint32_t Flags{}, HitSerial{}, DownedRevision{};
    Pose Location;
    RFAB_COOP_FIELDS(Flags, HitSerial, DownedRevision, Location)
};
struct Revive
{
    uint32_t Target{}, Revision{};
    uint64_t Hold{};
    bool Holding{};
    RFAB_COOP_FIELDS(Target, Revision, Hold, Holding)
};
struct DownedState
{
    uint32_t Actor{}, Revision{}, Reviver{}, RemainingMs{};
    bool Downed{};
    uint32_t Epoch{};
    uint64_t HealthRevision{};
    float Health{};
    RFAB_COOP_FIELDS(Actor, Revision, Reviver, RemainingMs, Downed, Epoch, HealthRevision, Health)
};
struct Anchor
{
    Pose Location;
    uint32_t DwellMs{};
    RFAB_COOP_FIELDS(Location, DwellMs)
};
enum class EncounterPhase : uint8_t { Started, WipeCountdown, Reset, Unavailable, Interrupted, Ended };
struct EncounterState
{
    uint64_t Encounter{};
    EncounterPhase Phase{};
    uint32_t RemainingMs{};
    std::string Reason;
    RFAB_COOP_FIELDS(Encounter, Phase, RemainingMs, Reason)
};
struct ResetActor
{
    uint32_t Actor{}, Epoch{};
    uint64_t HealthRevision{};
    float Health{};
    bool IsPlayer{};
    Pose Destination;
    RFAB_COOP_FIELDS(Actor, Epoch, HealthRevision, Health, IsPlayer, Destination)
};
enum class ResetPhase : uint8_t { Prepare, Commit, Complete, Blocked };
struct ResetPlan
{
    uint64_t Encounter{};
    ResetPhase Phase{};
    std::vector<ResetActor> Actors;
    RFAB_COOP_FIELDS(Encounter, Phase, Actors)
};
struct ResetReceipt
{
    uint64_t Encounter{};
    ResetPhase Phase{};
    bool Success{};
    RFAB_COOP_FIELDS(Encounter, Phase, Success)
};
struct EnemyContact
{
    uint32_t Actor{}, Epoch{};
    bool Hostile{}, Allied{}, Scripted{};
    RFAB_COOP_FIELDS(Actor, Epoch, Hostile, Allied, Scripted)
};
struct EnemyScale
{
    uint32_t Actor{}, Epoch{};
    uint64_t Revision{};
    float BaseHealth{}, MaxHealth{}, Health{};
    RFAB_COOP_FIELDS(Actor, Epoch, Revision, BaseHealth, MaxHealth, Health)
};
enum class RewardKind : uint8_t { Kill, Quest, Book, Custom };
struct Reward
{
    RewardKind Kind{};
    uint32_t Actor{};
    uint64_t Incarnation{}, Occurrence{};
    GameId Source{}, Context{};
    int32_t BaseXP{};
    RFAB_COOP_FIELDS(Kind, Actor, Incarnation, Occurrence, Source, Context, BaseXP)
};
struct RewardGrant
{
    uint64_t RewardId{};
    int32_t BaseXP{};
    RFAB_COOP_FIELDS(RewardId, BaseXP)
};
struct RewardReceipt
{
    uint64_t RewardId{};
    bool Applied{};
    RFAB_COOP_FIELDS(RewardId, Applied)
};
// A secure storage access point is NOT the identity of its backing container.
// Plugin is a normalized filename, not the current load-order or EnTT id.
struct StorageOpen
{
    std::string Plugin;
    uint32_t LocalRef{};
    GameId Backing{}, AccessPoint{};
    bool ConfirmImport{};
    std::vector<Inventory::Entry> Import;
    RFAB_COOP_FIELDS(Plugin, LocalRef, Backing, AccessPoint, ConfirmImport, Import)
};
struct StorageState
{
    std::string Key;
    uint32_t Entity{};
    uint64_t Revision{};
    bool NeedsImport{};
    std::vector<Inventory::Entry> Items;
    RFAB_COOP_FIELDS(Key, Entity, Revision, NeedsImport, Items)
};
struct Transfer
{
    uint32_t Source{}, Destination{}, SourceEpoch{}, DestinationEpoch{};
    uint64_t Nonce{}, SourceRevision{}, DestinationRevision{};
    uint8_t NativeReason{};
    Inventory::Entry Item{}; // positive exact quantity; neither endpoint has moved it yet
    RFAB_COOP_FIELDS(Source, Destination, SourceEpoch, DestinationEpoch, Nonce, SourceRevision, DestinationRevision, NativeReason, Item)
};
enum class TransferResult : uint8_t { Prepared, Committed, Rejected, Conflict, Quarantined };
struct TransferReply
{
    uint64_t Operation{}, Nonce{};
    TransferResult Result{};
    uint32_t Requester{}, Source{}, Destination{}, SourceEpoch{}, DestinationEpoch{};
    uint64_t SourceRevision{}, DestinationRevision{};
    Inventory::Entry Item{};
    std::string Reason;
    RFAB_COOP_FIELDS(Operation, Nonce, Result, Requester, Source, Destination, SourceEpoch, DestinationEpoch, SourceRevision, DestinationRevision, Item, Reason)
};
struct TransferReceipt
{
    uint64_t Operation{};
    bool Applied{}, Uncertain{};
    RFAB_COOP_FIELDS(Operation, Applied, Uncertain)
};
struct InventoryQuery
{
    uint32_t Entity{};
    RFAB_COOP_FIELDS(Entity)
};
struct InventoryState
{
    uint32_t Entity{}, Epoch{};
    uint64_t Revision{};
    bool Blocked{};
    std::vector<Inventory::Entry> Items;
    RFAB_COOP_FIELDS(Entity, Epoch, Revision, Blocked, Items)
};
enum class CheckpointPhase : uint8_t { Create, Prepare, Saved, Quiesce, TornDown, Load, Ready, Resume, Abort, Blocked };
struct Checkpoint
{
    uint64_t Id{};
    CheckpointPhase Phase{};
    std::string Manifest; // digest/identifier, never a remote filesystem path
    bool Success{};
    RFAB_COOP_FIELDS(Id, Phase, Manifest, Success)
};

using Body = std::variant<Hello, Welcome, Presence, Revive, DownedState, Anchor, EncounterState,
    ResetPlan, EnemyContact, EnemyScale, Reward, RewardGrant, RewardReceipt, StorageOpen,
    StorageState, Transfer, TransferReply, TransferReceipt, Checkpoint, ResetReceipt, InventoryQuery, InventoryState>;

struct Packet
{
    uint64_t Session{}, Generation{}, Operation{};
    Body Content{};
    bool Valid{true};
    void Serialize(TiltedPhoques::Buffer::Writer&) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader&) noexcept;
};
#undef RFAB_COOP_FIELDS
} // namespace Coop
