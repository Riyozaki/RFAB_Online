#pragma once

#include "Message.h"
#include <Structs/GameId.h>

struct NotifyRfabProgressionEvent final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifyRfabProgressionEvent;

    enum class EventKind : uint8_t
    {
        LedgerXpGrant = 0,
        BookRead = 1,
        GlobalValueSync = 2,
        LedgerXpRebate = 3,
        FullLedgerResync = 4,
        AllyProtectionSync = 5,
        BossEncounterSync = 6,
        TacticalPingSync = 7,
    };

    NotifyRfabProgressionEvent()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifyRfabProgressionEvent& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               Kind == acRhs.Kind &&
               EpochId == acRhs.EpochId &&
               SourceKey == acRhs.SourceKey &&
               EditorId == acRhs.EditorId &&
               BookId == acRhs.BookId &&
               XpDelta == acRhs.XpDelta &&
               AuthoritativeTotalXp == acRhs.AuthoritativeTotalXp &&
               GlobalValue == acRhs.GlobalValue;
    }

    EventKind Kind{EventKind::LedgerXpGrant};
    uint64_t EpochId{};
    String SourceKey{};
    String EditorId{};
    GameId BookId{};
    int32_t XpDelta{};
    int64_t AuthoritativeTotalXp{};
    float GlobalValue{};
};
