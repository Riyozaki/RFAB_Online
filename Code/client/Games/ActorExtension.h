#pragma once

#include <Structs/ActionEvent.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <type_traits>
#include <unordered_map>

struct ActorExtension
{
    enum : uint32_t
    {
        kRemote = 1u << 0,
        kPlayer = 1u << 1,
        kCreatedByNetwork = 1u << 2,
        kDownedPlayer = 1u << 3,
        kPlayerSummon = 1u << 4,
        kCombatTargetLocked = 1u << 5,
    };

    ActorExtension() noexcept = default;
    ~ActorExtension() noexcept = default;

    ActorExtension(const ActorExtension& aRhs) noexcept
        : GraphDescriptorHash(aRhs.GraphDescriptorHash)
        , GraphDescriptorProjectKey(aRhs.GraphDescriptorProjectKey)
        , nakedDeadline(aRhs.nakedDeadline)
        , onlineFlags(aRhs.onlineFlags.load(std::memory_order_relaxed))
    {
    }

    ActorExtension& operator=(const ActorExtension& aRhs) noexcept
    {
        if (this != &aRhs)
        {
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
    bool IsPlayerSummonFlag() const noexcept;
    bool HasCombatTargetLock() const noexcept;

    void SetRemote(bool aSet) noexcept;
    void SetPlayer(bool aSet) noexcept;
    void SetCreatedByNetwork(bool aSet) noexcept;
    void SetDownedPlayer(bool aSet) noexcept;
    void SetPlayerSummonFlag(bool aSet) noexcept;
    void SetCombatTargetLock(bool aSet) noexcept;
    void SetNakedDeadline() noexcept { nakedDeadline = std::chrono::steady_clock::now(); }

    static void SetLatestAnimation(uint32_t aActorFormId, const ActionEvent& acAction) noexcept
    {
        if (aActorFormId == 0)
            return;

        auto& shard = GetAnimationShard(aActorFormId);
        std::lock_guard<std::mutex> lock(shard.mutex);

        auto it = shard.entries.find(aActorFormId);
        if (it != shard.entries.end())
        {
            it->second = acAction;
            return;
        }

        if (shard.order.size() >= kMaxEntriesPerShard)
        {
            const uint32_t evictedId = shard.order.front();
            shard.order.pop_front();
            shard.entries.erase(evictedId);
        }

        shard.order.push_back(aActorFormId);
        shard.entries.emplace(aActorFormId, acAction);
    }

    static bool TryGetLatestAnimation(uint32_t aActorFormId, ActionEvent& aOutAction) noexcept
    {
        if (aActorFormId == 0)
            return false;

        auto& shard = GetAnimationShard(aActorFormId);
        std::lock_guard<std::mutex> lock(shard.mutex);

        const auto it = shard.entries.find(aActorFormId);
        if (it == shard.entries.end())
            return false;

        aOutAction = it->second;
        return true;
    }

    static void EraseLatestAnimation(uint32_t aActorFormId) noexcept
    {
        if (aActorFormId == 0)
            return;

        auto& shard = GetAnimationShard(aActorFormId);
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.entries.erase(aActorFormId);
    }

    static void ClearAllLatestAnimations() noexcept
    {
        for (auto& shard : GetAnimationShards())
        {
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.entries.clear();
            shard.order.clear();
        }
    }

    size_t GraphDescriptorHash = 0;
    uint64_t GraphDescriptorProjectKey = 0;
    std::chrono::steady_clock::time_point nakedDeadline{};

private:
    static constexpr size_t kShardCount = 16;
    static constexpr size_t kMaxEntriesPerShard = 256;

    struct AnimationShard
    {
        std::mutex mutex;
        std::unordered_map<uint32_t, ActionEvent> entries;
        std::deque<uint32_t> order;
    };

    static std::array<AnimationShard, kShardCount>& GetAnimationShards() noexcept
    {
        static std::array<AnimationShard, kShardCount> s_shards{};
        return s_shards;
    }

    static AnimationShard& GetAnimationShard(uint32_t aActorFormId) noexcept
    {
        const size_t idx = ((aActorFormId >> 4) ^ aActorFormId) & (kShardCount - 1);
        return GetAnimationShards()[idx];
    }

    std::atomic<uint32_t> onlineFlags{0};
};

// ActorExtension lives inside raw engine TESForm allocations (ExActor / ExPlayerCharacter)
// freed via RealFormFree without running C++ destructors; it MUST NEVER own heap pointers directly.
static_assert(std::is_trivially_destructible_v<ActorExtension>, "ActorExtension must be trivially destructible");
