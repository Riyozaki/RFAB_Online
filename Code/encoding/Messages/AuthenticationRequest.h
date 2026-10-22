#pragma once

#include "Message.h"
#include <Structs/Mods.h>
#include <TiltedCore/Buffer.hpp>
#include <Structs/GameId.h>
#include <Structs/TimeModel.h>

struct AuthenticationRequest final : ClientMessage
{
    static constexpr ClientOpcode Opcode = kAuthenticationRequest;

    AuthenticationRequest()
        : ClientMessage(Opcode)
    {
    }

    virtual ~AuthenticationRequest() = default;

    void SerializeRaw(TiltedPhoques::Buffer::Writer& aWriter) const noexcept override;
    void DeserializeRaw(TiltedPhoques::Buffer::Reader& aReader) noexcept override;

    bool operator==(const AuthenticationRequest& achRhs) const noexcept
    {
        return GetOpcode() == achRhs.GetOpcode() && DiscordId == achRhs.DiscordId && SKSEActive == achRhs.SKSEActive && MO2Active == achRhs.MO2Active && Token == achRhs.Token && Version == achRhs.Version && UserMods == achRhs.UserMods && Username == achRhs.Username &&
               WorldSpaceId == achRhs.WorldSpaceId && CellId == achRhs.CellId && Level == achRhs.Level && RfabXpTotal == achRhs.RfabXpTotal
            && PlayerTime == achRhs.PlayerTime;
    }

    uint64_t DiscordId{};
    bool SKSEActive{};
    bool MO2Active{};
    String Token{};
    String Version{};
    Mods UserMods{};
    String Username{};
    GameId WorldSpaceId{};
    GameId CellId{};
    uint16_t Level{};
    //! Total RFAB experience the client currently has (0 when RFAB is missing).
    //! The server uses it to top a returning player back up to what it already
    //! attributed to that username (see Gameplay::Progression).
    int32_t RfabXpTotal{};
    TimeModel PlayerTime{};
};
