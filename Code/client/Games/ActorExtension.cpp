#include <TiltedOnlinePCH.h>

#include <Games/ActorExtension.h>

bool ActorExtension::IsRemote() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & kRemote) != 0;
}

bool ActorExtension::IsLocal() const noexcept
{
    return !IsRemote();
}

bool ActorExtension::IsPlayer() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & kPlayer) != 0;
}

bool ActorExtension::IsRemotePlayer() const noexcept
{
    const uint32_t flags = onlineFlags.load(std::memory_order_relaxed);
    return (flags & (kRemote | kPlayer)) == (kRemote | kPlayer);
}

bool ActorExtension::IsLocalPlayer() const noexcept
{
    const uint32_t flags = onlineFlags.load(std::memory_order_relaxed);
    return (flags & (kRemote | kPlayer)) == kPlayer;
}

bool ActorExtension::IsCreatedByNetwork() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & kCreatedByNetwork) != 0;
}

bool ActorExtension::IsDownedPlayer() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & kDownedPlayer) != 0;
}

void ActorExtension::SetRemote(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kRemote, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kRemote, std::memory_order_relaxed);
}

void ActorExtension::SetPlayer(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kPlayer, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kPlayer, std::memory_order_relaxed);
}

void ActorExtension::SetCreatedByNetwork(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kCreatedByNetwork, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kCreatedByNetwork, std::memory_order_relaxed);
}

void ActorExtension::SetDownedPlayer(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kDownedPlayer, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kDownedPlayer, std::memory_order_relaxed);
}
