// The transport, replaced by an honest no-op that records what the server sent.
//
// `TiltedPhoques::Server` is the socket layer. The server talks to it through
// `GameServer::Send*`, and every decision that matters (who owns an actor, whether a hit
// counts, whether a corpse may stand up) is made *before* that call. So the harness keeps
// the real dispatch path and replaces only the last centimetre:
//
//   * nothing listens on a port and no packet leaves the process;
//   * every outbound message is copied into an outbox, which lets a test assert what the
//     server would have told the players - a corrected health value, for instance, which is
//     the half of the anti-rollback rule that is easy to forget to send.

#include <Packet.hpp>
#include <Server.hpp>
#include <SteamInterface.hpp>

#include "../TransportSpy.h"

#include <vector>

namespace Harness
{
std::vector<SentPacket>& Outbox()
{
    static std::vector<SentPacket> s_outbox;
    return s_outbox;
}

size_t OutboxSize()
{
    return Outbox().size();
}

void ClearOutbox()
{
    Outbox().clear();
}

namespace
{
void Record(TiltedPhoques::ConnectionId_t aConnection, const TiltedPhoques::Packet* apPacket)
{
    SentPacket record{};
    record.Connection = aConnection;
    if (apPacket != nullptr && apPacket->IsValid())
    {
        const auto* pData = reinterpret_cast<const uint8_t*>(apPacket->GetData());
        // Packet::GetData() already skips the 1-byte TiltedConnect framing header,
        // so [pData, pData + GetSize()) is the exact ServerMessage bitstream handed
        // to TransportService::OnConsume on the client.
        record.Bytes.assign(pData, pData + apPacket->GetSize());
    }
    Outbox().push_back(std::move(record));
}
} // namespace
} // namespace Harness

namespace TiltedPhoques
{
Server::Server() noexcept
{
    m_listenSock = 0;
    m_pollGroup = 0;
    m_pInterface = nullptr;
    m_tickRate = 60;
    m_timeBetweenUpdates = std::chrono::milliseconds(16);
}

Server::~Server() = default;

bool Server::Host(uint16_t /*aPort*/, uint32_t aTickRate, bool /*aEnableDualStackIP*/) noexcept
{
    // Always succeeds: an in-process harness has nothing to bind and nothing to fail.
    m_tickRate = aTickRate;
    return true;
}

void Server::Close() noexcept {}
void Server::Update() noexcept {}

// Ownership note: `GameServer::Send` hands over a `PacketView` that lives on its caller's
// stack and points into a scratch buffer. The real transport only reads it, so the harness
// must not delete it - doing so frees a stack address (mimalloc reports exactly that:
// "pointer does not point to a valid heap space").
void Server::SendToAll(Packet* apPacket, EPacketFlags /*aPacketFlags*/) noexcept
{
    Harness::Record(k_HSteamNetConnection_Invalid, apPacket);
}

void Server::Send(const ConnectionId_t aConnectionId, Packet* apPacket, EPacketFlags /*aPacketFlags*/) const noexcept
{
    // Recorded, never freed: see SendToAll.
    Harness::Record(aConnectionId, apPacket);
}

void Server::Kick(ConnectionId_t /*aConnectionId*/) noexcept {}

uint16_t Server::GetPort() const noexcept { return 10578; }
bool Server::IsListening() const noexcept { return true; }
uint32_t Server::GetClientCount() const noexcept { return 0; }
uint32_t Server::GetTickRate() const noexcept { return m_tickRate; }
uint64_t Server::GetTick() const noexcept { return 0; }

SteamNetConnectionInfo_t Server::GetConnectionInfo(ConnectionId_t /*aConnectionId*/) const noexcept
{
    return SteamNetConnectionInfo_t{};
}

bool Server::IsAlive(ConnectionId_t /*aConnectionId*/) const noexcept { return true; }
void Server::Remove(ConnectionId_t /*aId*/) noexcept {}

void SteamInterface::Acquire() {}
void SteamInterface::Release() {}
} // namespace TiltedPhoques
