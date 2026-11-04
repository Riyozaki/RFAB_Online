#pragma once
// A hand-written stand-in for Valve's GameNetworkingSockets header.
//
// The harness never opens a socket: packets are dispatched in-process through the server's
// own event dispatcher, which is the code path a real connection takes once the transport
// has handed the bytes over. Only what TiltedConnect's *headers* name has to exist for the
// server to compile, so the surface here is deliberately tiny and inert - there is no
// implementation of ISteamNetworkingSockets at all.
//
// Keeping the transport out is what makes the harness runnable: GameNetworkingSockets drags
// in protobuf, abseil and OpenSSL, none of which know anything about who owns an actor.
#include <cstddef>
#include <cstdint>

using HSteamNetConnection = uint32_t;
using HSteamListenSocket = uint32_t;
using HSteamNetPollGroup = uint32_t;
using SteamNetworkingMicroseconds = int64_t;

static constexpr HSteamNetConnection k_HSteamNetConnection_Invalid = 0;

enum ESteamNetworkingConnectionState : int
{
    k_ESteamNetworkingConnectionState_None = 0,
    k_ESteamNetworkingConnectionState_Connecting = 1,
    k_ESteamNetworkingConnectionState_FindingRoute = 2,
    k_ESteamNetworkingConnectionState_Connected = 3,
    k_ESteamNetworkingConnectionState_ClosedByPeer = 4,
    k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,
};

enum EResult : int
{
    k_EResultOK = 1,
    k_EResultFail = 2,
};

enum ESteamNetworkingSendType : int
{
    k_nSteamNetworkingSend_Unreliable = 0,
    k_nSteamNetworkingSend_NoNagle = 1,
    k_nSteamNetworkingSend_UnreliableNoNagle = k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoNagle,
    k_nSteamNetworkingSend_NoDelay = 4,
    k_nSteamNetworkingSend_Reliable = 8,
};

enum ESteamNetworkingConfigValue : int
{
    k_ESteamNetworkingConfig_Invalid = 0,
    k_ESteamNetworkingConfig_SendBufferSize = 9,
    k_ESteamNetworkingConfig_TimeoutConnected = 25,
    k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged = 201,
};

enum ESteamNetworkingConfigDataType : int
{
    k_ESteamNetworkingConfig_Int32 = 1,
    k_ESteamNetworkingConfig_FunctionPtr = 9,
};

struct SteamNetworkingIPAddr
{
    uint8_t m_ipv6[16]{};
    uint16_t m_port{0};
    void Clear() { *this = SteamNetworkingIPAddr{}; }
    bool IsIPv6AllZeros() const { return true; }
    const char* ToString(char* apBuffer, size_t aBufferSize, bool /*aWithPort*/ = false) const
    {
        if (apBuffer != nullptr && aBufferSize > 0)
            apBuffer[0] = '\0';
        return apBuffer;
    }
};

struct SteamNetworkingConfigValue_t
{
    ESteamNetworkingConfigValue m_eValue{};
    ESteamNetworkingConfigDataType m_eDataType{};
    union
    {
        int32_t m_int32;
        int64_t m_int64;
        float m_float;
        void* m_ptr;
        void (*m_fnPtr)();
    } m_val{};
};

struct SteamNetConnectionInfo_t
{
    SteamNetworkingIPAddr m_addrRemote{};
    int m_eState{};
    int m_nEndReason{};
};

struct SteamNetConnectionStatusChangedCallback_t
{
    HSteamNetConnection m_hConn{k_HSteamNetConnection_Invalid};
    SteamNetConnectionInfo_t m_info{};
    ESteamNetworkingConnectionState m_eOldState{};
};

struct SteamNetworkingMessage_t
{
    void* m_pData{nullptr};
    int m_cbSize{0};
    HSteamNetConnection m_conn{k_HSteamNetConnection_Invalid};
    void Release() {}
};

struct ISteamNetworkingSockets
{
    virtual ~ISteamNetworkingSockets() = default;
};
