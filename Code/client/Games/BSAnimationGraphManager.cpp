#include <TiltedOnlinePCH.h>

#include <BSAnimationGraphManager.h>
#include <Havok/BShkbAnimationGraph.h>
#include <Havok/BShkbHkxDB.h>
#include <Havok/hkbBehaviorGraph.h>

#include <TiltedCore/Hash.hpp>

SortedMap<uint32_t, String> BSAnimationGraphManager::DumpAnimationVariables(bool aPrintVariables)
{
    BSScopedLock<BSRecursiveLock> _{lock};
    SortedMap<uint32_t, String> variables;

    if (animationGraphIndex < animationGraphs.size)
    {
        const auto pGraph = animationGraphs.Get(animationGraphIndex);
        if (pGraph && pGraph->hkxDB && pGraph->behaviorGraph)
        {
            const auto pDb = pGraph->hkxDB;
            const auto pBuckets = pDb->animationVariables.buckets;
            const auto pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (pBuckets && pVariableSet)
            {
                for (decltype(pDb->animationVariables.bucketCount) i = 0; i < pDb->animationVariables.bucketCount; ++i)
                {
                    auto pBucket = &pBuckets[i];
                    if (!pBucket->next)
                        continue;

                    uint32_t stepGuard = 0;
                    while (pBucket && pBucket != pDb->animationVariables.end && stepGuard++ < 16384u)
                    {
                        const auto variableIndex = pBucket->value;
                        if (pVariableSet->size > static_cast<uint32_t>(variableIndex) && pBucket->key.AsAscii())
                        {
                            variables[variableIndex] = pBucket->key.AsAscii();
                        }

                        pBucket = pBucket->next;
                    }
                }

                if (aPrintVariables)
                {
                    for (auto& [id, name] : variables)
                    {
                        std::cout << "k" << name << " = " << id << "," << std::endl;
                    }
                }
            }
        }
    }

    return variables;
}

uint64_t BSAnimationGraphManager::GetDescriptorKey(int aForceIndex)
{
    using TiltedPhoques::FHash::Crc64;
    BSScopedLock<BSRecursiveLock> _{lock};

    String variableNames{};
    variableNames.reserve(8192);
    std::map<uint32_t, const char*> variables;

    const uint32_t targetIdx = aForceIndex == -1 ? animationGraphIndex : static_cast<uint32_t>(aForceIndex);
    if (targetIdx < animationGraphs.size)
    {
        const auto pGraph = animationGraphs.Get(targetIdx);

        if (pGraph && pGraph->hkxDB && pGraph->behaviorGraph)
        {
            const auto pDb = pGraph->hkxDB;
            const auto pBuckets = pDb->animationVariables.buckets;
            const auto pVariableSet = pGraph->behaviorGraph->animationVariables;

            if (pBuckets && pVariableSet)
            {
                for (decltype(pDb->animationVariables.bucketCount) i = 0; i < pDb->animationVariables.bucketCount; ++i)
                {
                    auto pBucket = &pBuckets[i];
                    if (!pBucket->next)
                        continue;

                    uint32_t stepGuard = 0;
                    while (pBucket && pBucket != pDb->animationVariables.end && stepGuard++ < 16384u)
                    {
                        const auto variableIndex = pBucket->value;
                        if (pVariableSet->size > static_cast<uint32_t>(variableIndex) && pBucket->key.AsAscii())
                        {
                            variables[variableIndex] = pBucket->key.AsAscii();
                        }

                        pBucket = pBucket->next;
                    }
                }

                for (auto& [id, name] : variables)
                {
                    if (name)
                        variableNames += name;
                }
            }
        }
    }

    std::transform(variableNames.begin(), variableNames.end(), variableNames.begin(), [](unsigned char c) { return std::tolower(c); });

    return Crc64(reinterpret_cast<const unsigned char*>(variableNames.c_str()), variableNames.size());
}

uint64_t BSAnimationGraphManager::GetProjectKey(int aForceIndex)
{
    using TiltedPhoques::FHash::Crc64;
    BSScopedLock<BSRecursiveLock> _{lock};

    const uint32_t targetIdx = aForceIndex == -1 ? animationGraphIndex : static_cast<uint32_t>(aForceIndex);
    if (targetIdx < animationGraphs.size)
    {
        const auto pGraph = animationGraphs.Get(targetIdx);

        if (pGraph && pGraph->projectName.AsAscii())
        {
            const char* cpProjectName = pGraph->projectName.AsAscii();
            return Crc64(reinterpret_cast<const unsigned char*>(cpProjectName), std::strlen(cpProjectName));
        }
    }

    return 0;
}
