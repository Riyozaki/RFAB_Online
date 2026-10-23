#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct SyncRfabProgressionEvent final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kSyncRfabProgressionEvent;

    enum class EventKind : uint8_t
    {
        LedgerXpGrant = 0,
        BookRead = 1,
        GlobalValueSync = 2,
    };

    SyncRfabProgressionEvent()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const SyncRfabProgressionEvent& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               Kind == acRhs.Kind &&
               EpochId == acRhs.EpochId &&
               SourceKey == acRhs.SourceKey &&
               EditorId == acRhs.EditorId &&
               BookId == acRhs.BookId &&
               XpDelta == acRhs.XpDelta &&
               GlobalValue == acRhs.GlobalValue;
    }

    EventKind Kind{EventKind::LedgerXpGrant};
    uint64_t EpochId{0};
    String SourceKey{};
    String EditorId{};
    GameId BookId{};
    int32_t XpDelta{};
    float GlobalValue{};
};
