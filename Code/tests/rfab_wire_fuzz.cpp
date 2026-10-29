/*
 * Wire-format robustness suite.
 *
 * Why this file exists: the previous "audit" suites only asserted that certain
 * strings still exist in the sources, so no test ever exercised the deserializers
 * with hostile input. Every message here is fed through the real factory with
 * truncated / pseudo-random / maximal-varint payloads.
 *
 * Contract verified:
 *   1. For every opcode in [0, kClientOpcodeMax) the factory must either return a
 *      message whose GetOpcode() matches, or nullptr - it must never crash,
 *      terminate, throw or exhaust memory.
 *   2. Out-of-range opcodes (>= kOpcodeMax, 0xFF) must return nullptr.
 *   3. Truncating a valid serialized message at any byte boundary must be safe.
 */

#include <catch2/catch.hpp>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <TiltedCore/ViewBuffer.hpp>

#include <Messages/ClientMessageFactory.h>
#include <Messages/ServerMessageFactory.h>
#include <Opcodes.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using TiltedPhoques::Buffer;
using TiltedPhoques::ViewBuffer;

namespace
{
// Deterministic pseudo random source: failures must be reproducible from the log alone.
uint32_t NextXorShift(uint32_t& aState) noexcept
{
    aState ^= aState << 13;
    aState ^= aState >> 17;
    aState ^= aState << 5;
    return aState;
}

// Keep the byte alphabet tiny (0..7) so random varints stay small; the maximal
// varint case is covered separately below. This avoids multi-gigabyte allocations
// while still exercising every length/index branch.
void FillSmallAlphabet(uint8_t* apData, size_t aSize, uint32_t aSeed) noexcept
{
    uint32_t state = aSeed | 1u;
    for (size_t i = 0; i < aSize; ++i)
        apData[i] = static_cast<uint8_t>(NextXorShift(state) & 0x7u);
}

std::string Describe(const char* apKind, size_t aOpcode, size_t aSize, const char* apCase)
{
    return std::string(apKind) + " opcode " + std::to_string(aOpcode) + ", payload " + std::to_string(aSize) +
           " bytes, case '" + apCase + "'";
}

template <class TFactory> void ExpectSafeExtract(const TFactory& aFactory,
                                                              uint32_t aOpcode,
                                                              const std::vector<uint8_t>& acPayload,
                                                              const char* apKind,
                                                              const char* apCase)
{
    const auto ctx = Describe(apKind, aOpcode, acPayload.empty() ? 0 : acPayload.size() - 1, apCase);

    std::vector<uint8_t> raw(1 + acPayload.size());
    raw[0] = static_cast<uint8_t>(aOpcode);
    if (!acPayload.empty())
        std::copy(acPayload.begin(), acPayload.end(), raw.begin() + 1);

    bool threw = false;
    std::string what;
    bool opcodeMismatch = false;
    uint32_t reportedOpcode = 0;

    try
    {
        ViewBuffer readBuf(raw.data(), raw.size());
        Buffer::Reader reader(&readBuf);
        auto pMessage = aFactory.Extract(reader);
        if (pMessage)
        {
            reportedOpcode = static_cast<uint32_t>(pMessage->GetOpcode());
            opcodeMismatch = (reportedOpcode != aOpcode);
        }
    }
    catch (const std::exception& e)
    {
        threw = true;
        what = e.what();
    }
    catch (...)
    {
        threw = true;
        what = "unknown exception";
    }

    INFO(ctx);
    if (threw)
        UNSCOPED_INFO("exception: " << what);
    if (opcodeMismatch)
        UNSCOPED_INFO("factory returned opcode " << reportedOpcode);
    CHECK_FALSE(threw);
    CHECK_FALSE(opcodeMismatch);
}
} // namespace

TEST_CASE("Wire: every ClientOpcode survives truncated and random payloads", "[wire][fuzz][client]")
{
    const ClientMessageFactory factory;
    const size_t opcodeCount = static_cast<size_t>(kClientOpcodeMax);

    static constexpr size_t kPayloadSizes[] = {0, 1, 2, 3, 5, 8, 16, 32, 64};

    for (size_t opcode = 0; opcode < opcodeCount; ++opcode)
    {
        for (size_t size : kPayloadSizes)
        {
            std::vector<uint8_t> payload(size);
            FillSmallAlphabet(payload.data(), payload.size(), static_cast<uint32_t>(opcode * 131u + size * 17u + 1u));
            ExpectSafeExtract<ClientMessageFactory>(factory, static_cast<uint32_t>(opcode), payload,
                                                                  "client", "small-alphabet");
        }

        // Maximal varint: 8 x 0xFF decodes to a huge unsigned value.
        std::vector<uint8_t> maximal(8, 0xFF);
        ExpectSafeExtract<ClientMessageFactory>(factory, static_cast<uint32_t>(opcode), maximal, "client",
                                                              "max-varint");
    }
}

TEST_CASE("Wire: every ServerOpcode survives truncated and random payloads", "[wire][fuzz][server]")
{
    const ServerMessageFactory factory;
    const size_t opcodeCount = static_cast<size_t>(kServerOpcodeMax);

    static constexpr size_t kPayloadSizes[] = {0, 1, 2, 3, 5, 8, 16, 32, 64};

    for (size_t opcode = 0; opcode < opcodeCount; ++opcode)
    {
        for (size_t size : kPayloadSizes)
        {
            std::vector<uint8_t> payload(size);
            FillSmallAlphabet(payload.data(), payload.size(), static_cast<uint32_t>(opcode * 197u + size * 13u + 7u));
            ExpectSafeExtract<ServerMessageFactory>(factory, static_cast<uint32_t>(opcode), payload,
                                                                  "server", "small-alphabet");
        }

        std::vector<uint8_t> maximal(8, 0xFF);
        ExpectSafeExtract<ServerMessageFactory>(factory, static_cast<uint32_t>(opcode), maximal, "server",
                                                              "max-varint");
    }
}

TEST_CASE("Wire: out-of-range opcodes are rejected without touching a handler", "[wire][bounds]")
{
    const ClientMessageFactory clientFactory;
    const ServerMessageFactory serverFactory;

    static constexpr uint8_t kBadOpcodes[] = {kClientOpcodeMax, static_cast<uint8_t>(kClientOpcodeMax + 1), 0x7F, 0xFF};
    for (uint8_t opcode : kBadOpcodes)
    {
        uint8_t raw[16]{};
        raw[0] = opcode;
        ViewBuffer readBuf(raw, sizeof(raw));
        Buffer::Reader reader(&readBuf);
        bool threw = false;
        try
        {
            auto pMessage = clientFactory.Extract(reader);
            CHECK(!pMessage);
        }
        catch (...)
        {
            threw = true;
        }
        INFO("client bad opcode " << static_cast<int>(opcode));
        CHECK_FALSE(threw);
    }

    for (uint16_t opcode : {static_cast<uint16_t>(kServerOpcodeMax), static_cast<uint16_t>(kServerOpcodeMax + 1),
                            static_cast<uint16_t>(0x7F), static_cast<uint16_t>(0xFF)})
    {
        uint8_t raw[16]{};
        raw[0] = static_cast<uint8_t>(opcode);
        ViewBuffer readBuf(raw, sizeof(raw));
        Buffer::Reader reader(&readBuf);
        bool threw = false;
        try
        {
            auto pMessage = serverFactory.Extract(reader);
            CHECK(!pMessage);
        }
        catch (...)
        {
            threw = true;
        }
        INFO("server bad opcode " << opcode);
        CHECK_FALSE(threw);
    }
}

TEST_CASE("Wire: a valid message truncated at every byte boundary stays safe", "[wire][truncation]")
{
    // AuthenticationRequest is the very first packet a client sends, so every
    // prefix of it can be produced by a hostile or half-dead peer.
    AuthenticationRequest request;
    request.Token = "0123456789abcdef";

    uint8_t raw[256]{};
    ViewBuffer writeBuf(raw, sizeof(raw));
    Buffer::Writer writer(&writeBuf);
    request.Serialize(writer);

    const size_t fullSize = writer.Size();
    REQUIRE(fullSize > 2);

    const ClientMessageFactory factory;
    for (size_t prefix = 1; prefix <= fullSize; ++prefix)
    {
        ViewBuffer readBuf(raw, prefix);
        Buffer::Reader reader(&readBuf);

        bool threw = false;
        bool opcodeMismatch = false;
        try
        {
            auto pMessage = factory.Extract(reader);
            if (pMessage)
                opcodeMismatch = (pMessage->GetOpcode() != kAuthenticationRequest);
        }
        catch (...)
        {
            threw = true;
        }

        INFO("authentication prefix " << prefix << " of " << fullSize);
        CHECK_FALSE(threw);
        CHECK_FALSE(opcodeMismatch);
    }
}
