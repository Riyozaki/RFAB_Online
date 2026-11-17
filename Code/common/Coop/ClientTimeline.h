#pragma once
#include <Coop/DurableFile.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Coop
{
// Stored in the character's SKSE co-save, never inferred from a wallet or an inventory poll.
struct SavedScale
{
    uint32_t Form{}, Version{1};
    uint64_t Life{};
    float Base{}, Maximum{};
    bool operator==(const SavedScale&) const = default;
};
struct CharacterState
{
    std::string Character; // 128-bit random bearer identity, local save + private server only
    uint64_t Cursor{};
    std::vector<SavedScale> Scales;
    bool operator==(const CharacterState&) const = default;
    bool Valid() const noexcept;
    std::optional<std::vector<uint8_t>> Encode() const;
    static std::optional<CharacterState> Decode(std::span<const uint8_t>);
};
struct EffectId
{
    uint64_t Session{}, Generation{}, Operation{};
    bool operator==(const EffectId&) const = default;
    bool Valid() const noexcept { return Session && Generation && Operation; }
};
enum class EffectKind : uint8_t { Transfer = 1, Reward, Scale };
enum class EffectStage : uint8_t { Prepared = 1, Applied, Committed, Rejected, Uncertain };
struct NativeReceipt
{
    std::string Character;
    EffectId Id;
    EffectKind Kind{EffectKind::Transfer};
    EffectStage Stage{EffectStage::Prepared};
    uint64_t Before{}, After{};
    bool Valid() const noexcept;
    std::optional<std::vector<uint8_t>> Encode() const;
    static std::optional<NativeReceipt> Decode(std::span<const uint8_t>);
};

// Serial write-ahead boundary shared by every native side effect. Applied != Committed.
// The disk journal is ahead of an older ESS: that is a rollback mismatch, not a retry license.
// A process restart with Prepared/Applied/Uncertain is terminal for automatic execution.
class ClientTimeline
{
public:
    using Persist = std::function<bool(const NativeReceipt&)>;
    explicit ClientTimeline(Persist aPersist) : m_persist(std::move(aPersist)) {}
    bool Load(CharacterState, const std::optional<NativeReceipt>&);
    bool Ready() const noexcept { return m_loaded && !m_faulted; }
    bool CanSave() const noexcept;
    const CharacterState& State() const noexcept { return m_state; }
    const std::optional<NativeReceipt>& Receipt() const noexcept { return m_receipt; }
    bool Begin(EffectKind, EffectId);
    bool Applied(EffectId);
    bool Commit(EffectId);
    bool AbortBeforeNative(EffectId);
    void Uncertain(EffectId) noexcept;
    // Scaling stamps are saved with native HP, not kept in the ActorExtension/session only.
    bool StampScale(SavedScale);
    void Fault() noexcept { m_faulted = true; }
private:
    bool Publish(NativeReceipt);
    CharacterState m_state;
    std::optional<NativeReceipt> m_receipt;
    Persist m_persist;
    bool m_loaded{}, m_faulted{};
};
} // namespace Coop
