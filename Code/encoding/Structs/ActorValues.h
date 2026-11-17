#pragma once
#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <Structs/HealthScale.h>

struct ActorValues
{
    ActorValues() = default;
    ~ActorValues() = default;

    bool operator==(const ActorValues& acRhs) const noexcept;
    bool operator!=(const ActorValues& acRhs) const noexcept;

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    bool WireValid{true};
    bool IsValid() const noexcept;
    uint64_t HealthRevision{};
    HealthScale Scale;
    // Server-only origin watermarks travel in control snapshots so an overtaken mutation reply
    // cannot make the client predict the same damage twice. Never accepted from a client.
    TiltedPhoques::Map<uint32_t, uint64_t> HealthSequences{};
    TiltedPhoques::Map<uint32_t, float> ActorValuesList{};
    TiltedPhoques::Map<uint32_t, float> ActorMaxValuesList{};
};
