#pragma once

#include <Structs/AnimationGraphDescriptor.h>

#include <shared_mutex>
#include <unordered_map>

struct AnimationGraphDescriptorManager
{
    TP_NOCOPYMOVE(AnimationGraphDescriptorManager);

    static AnimationGraphDescriptorManager& Get() noexcept;
    const AnimationGraphDescriptor* GetDescriptor(uint64_t aKey) const noexcept;

    std::unordered_map<uint64_t, AnimationGraphDescriptor> GetDescriptors() const;
    void UpdateKey(uint64_t aKey, uint64_t newKey) noexcept;
    void Update(uint64_t aKey, uint64_t newKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept;

    struct Builder
    {
        Builder(AnimationGraphDescriptorManager& aManager, uint64_t aKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept;
    };

protected:
    void Register(uint64_t aKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept;

private:
    AnimationGraphDescriptorManager() noexcept;

    mutable std::shared_mutex m_mutex;
    std::unordered_map<uint64_t, AnimationGraphDescriptor> m_descriptors;
};
