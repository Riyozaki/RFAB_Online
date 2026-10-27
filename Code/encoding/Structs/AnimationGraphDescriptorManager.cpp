#include <Structs/AnimationGraphDescriptorManager.h>

#include <mutex>

AnimationGraphDescriptorManager& AnimationGraphDescriptorManager::Get() noexcept
{
    static AnimationGraphDescriptorManager s_manager;
    return s_manager;
}

const AnimationGraphDescriptor* AnimationGraphDescriptorManager::GetDescriptor(uint64_t aKey) const noexcept
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_descriptors.find(aKey);
    if (it != std::end(m_descriptors))
        return &it->second;

    return nullptr;
}

AnimationGraphDescriptorManager::Builder::Builder(AnimationGraphDescriptorManager& aManager, uint64_t aKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept
{
    aManager.Register(aKey, std::move(aAnimationGraphDescriptor));
}

void AnimationGraphDescriptorManager::Register(uint64_t aKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept
{
    // std::unordered_map is node-based, so concurrent insertions (e.g. from BehaviorVar::Patch on
    // Havok behavior-graph worker threads) never invalidate existing const AnimationGraphDescriptor*
    // pointers held by animation serialization/deserialization callers even across bucket rehashes.
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_descriptors.try_emplace(aKey, std::move(aAnimationGraphDescriptor));
}

void AnimationGraphDescriptorManager::UpdateKey(uint64_t aKey, uint64_t newKey) noexcept
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    if (aKey == newKey || m_descriptors.find(newKey) != std::end(m_descriptors))
        return;

    const auto it = m_descriptors.find(aKey);
    if (it != std::end(m_descriptors))
        m_descriptors.try_emplace(newKey, it->second);
}

void AnimationGraphDescriptorManager::Update(uint64_t aKey, uint64_t newKey, AnimationGraphDescriptor aAnimationGraphDescriptor) noexcept
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    (void)aKey;
    // Never overwrite an already-registered descriptor in place: worker threads hold raw
    // const AnimationGraphDescriptor* pointers returned by GetDescriptor() after releasing m_mutex,
    // and insert_or_assign would destroy and free the existing LookupTable vectors underneath them.
    m_descriptors.try_emplace(newKey, std::move(aAnimationGraphDescriptor));
}

std::unordered_map<uint64_t, AnimationGraphDescriptor> AnimationGraphDescriptorManager::GetDescriptors() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_descriptors;
}

