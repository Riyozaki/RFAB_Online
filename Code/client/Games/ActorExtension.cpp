#include <TiltedOnlinePCH.h>

#include <Games/ActorExtension.h>

bool ActorExtension::IsRemote() const noexcept
{
    return (onlineFlags.load(std::memory_order_relaxed) & (kRemote | kLocalAvatar)) == kRemote;
}

bool ActorExtension::IsLocal() const noexcept
{
    return !IsRemote();
}

bool ActorExtension::IsPlayer() const noexcept
{
    return onlineFlags.load(std::memory_order_relaxed) & kPlayer;
}

bool ActorExtension::IsRemotePlayer() const noexcept
{
    return IsRemote() && IsPlayer();
}

bool ActorExtension::IsLocalPlayer() const noexcept
{
    return IsLocal() && IsPlayer();
}

void ActorExtension::SetRemote(bool aSet) noexcept
{
    auto flags = onlineFlags.load(std::memory_order_relaxed);
    do
    {
        if (aSet && (flags & kLocalAvatar))
            return;
        const auto desired = aSet ? flags | kRemote : flags & ~kRemote;
        if (onlineFlags.compare_exchange_weak(flags, desired, std::memory_order_relaxed))
            return;
    } while (true);
}

void ActorExtension::SetPlayer(bool aSet) noexcept
{
    if (aSet)
        onlineFlags.fetch_or(kPlayer, std::memory_order_relaxed);
    else
        onlineFlags.fetch_and(~kPlayer, std::memory_order_relaxed);
}

void ActorExtension::BindLocalAvatar() noexcept
{
    auto flags = onlineFlags.load(std::memory_order_relaxed);
    while (!onlineFlags.compare_exchange_weak(flags, (flags | kPlayer | kLocalAvatar) & ~kRemote, std::memory_order_relaxed)) {}
}

void ActorExtension::SetLatestAction(const ActionEvent& aAction) noexcept
{
    std::scoped_lock lock(m_actionMutex);
    m_latestAnimation = aAction;
}
ActionEvent ActorExtension::GetLatestAction() const noexcept
{
    std::scoped_lock lock(m_actionMutex);
    return m_latestAnimation;
}
