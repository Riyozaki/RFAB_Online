#pragma once

#include "Message.h"
#include <Structs/GameId.h>
#include <cmath>

struct SpellCastRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kSpellCastRequest;

    SpellCastRequest()
        : ClientMessage(Opcode)
    {
    }

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const SpellCastRequest& acRhs) const noexcept
    {
        return CasterId == acRhs.CasterId &&
               SpellFormId == acRhs.SpellFormId &&
               CastingSource == acRhs.CastingSource &&
               IsDualCasting == acRhs.IsDualCasting &&
               DesiredTarget == acRhs.DesiredTarget &&
               std::abs(EffectivenessMult - acRhs.EffectivenessMult) < 0.0001f &&
               GetOpcode() == acRhs.GetOpcode();
    }

    uint32_t CasterId{};
    GameId SpellFormId{};
    int32_t CastingSource{};
    bool IsDualCasting{false};
    uint32_t DesiredTarget{};
    float EffectivenessMult{1.0f};
};
