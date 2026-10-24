#pragma once

#include <Structs/ActionEvent.h>
#include <atomic>

struct ActorExtension
{
    enum : uint32_t
    {
        kRemote = 1u << 0,
        kPlayer = 1u << 1,
        kCreatedByNetwork = 1u << 2,
        kDownedPlayer = 1u << 3,
    };

    ActorExtension() noexcept = default;
    ActorExtension(const ActorExtension& aRhs) noexcept
        : LatestAnimation(aRhs.LatestAnimation)
        , GraphDescriptorHash(aRhs.GraphDescriptorHash)
        , GraphDescriptorProjectKey(aRhs.GraphDescriptorProjectKey)
        , nakedDeadline(aRhs.nakedDeadline)
        , onlineFlags(aRhs.onlineFlags.load(std::memory_order_relaxed))
    {
    }
    ActorExtension& operator=(const ActorExtension& aRhs) noexcept
    {
        if (this != &aRhs)
        {
            LatestAnimation = aRhs.LatestAnimation;
            GraphDescriptorHash = aRhs.GraphDescriptorHash;
            GraphDescriptorProjectKey = aRhs.GraphDescriptorProjectKey;
            nakedDeadline = aRhs.nakedDeadline;
            onlineFlags.store(aRhs.onlineFlags.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    bool IsRemote() const noexcept;
    bool IsLocal() const noexcept;
    bool IsPlayer() const noexcept;
    bool IsRemotePlayer() const noexcept;
    bool IsLocalPlayer() const noexcept;
    bool IsCreatedByNetwork() const noexcept;
    bool IsDownedPlayer() const noexcept;
    void SetRemote(bool aSet) noexcept;
    void SetPlayer(bool aSet) noexcept;
    void SetCreatedByNetwork(bool aSet) noexcept;
    void SetDownedPlayer(bool aSet) noexcept;
    void SetNakedDeadline() noexcept { nakedDeadline = std::chrono::steady_clock::now();  }

    ActionEvent LatestAnimation{};
    size_t GraphDescriptorHash = 0;
    uint64_t GraphDescriptorProjectKey = 0;
    std::chrono::steady_clock::time_point nakedDeadline{};

  private:
    std::atomic<uint32_t> onlineFlags{0};
};
