#pragma once

#include <Server.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Harness
{
struct SentPacket
{
    TiltedPhoques::ConnectionId_t Connection{};
    std::vector<uint8_t> Bytes;
};

std::vector<SentPacket>& Outbox();
size_t OutboxSize();
void ClearOutbox();
} // namespace Harness
