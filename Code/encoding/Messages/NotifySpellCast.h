#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <cmath>

struct NotifySpellCast final : ServerMessage
{
    static constexpr ServerOpcode Opcode = kNotifySpellCast;

    NotifySpellCast()
        : ServerMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const NotifySpellCast& acRhs) const noexcept
    {
        return GetOpcode() == acRhs.GetOpcode() &&
               CasterId == acRhs.CasterId &&
               SpellFormId == acRhs.SpellFormId &&
               CastingSource == acRhs.CastingSource &&
               IsDualCasting == acRhs.IsDualCasting &&
               DesiredTarget == acRhs.DesiredTarget &&
               std::abs(EffectivenessMult - acRhs.EffectivenessMult) < 0.0001f;
    }

    uint32_t CasterId{};
    GameId SpellFormId{};
    int32_t CastingSource{};
    bool IsDualCasting{false};
    uint32_t DesiredTarget{};
    float EffectivenessMult{1.0f};
};
