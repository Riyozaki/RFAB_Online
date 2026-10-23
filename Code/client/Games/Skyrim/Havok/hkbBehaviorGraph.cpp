#include <TiltedOnlinePCH.h>

#include <Havok/hkbBehaviorGraph.h>

// True if the value lives directly in the 32-bit value-set word (bool, int, real).
// Pointer/vector/quaternion variables store an index into secondary Havok arrays instead.
bool hkbBehaviorGraph::IsWordVariable(uint32_t aIndex) const noexcept
{
    if (!data || !data->variableInfos || aIndex >= static_cast<uint32_t>(data->variableInfoCount))
        return false;

    const auto cType = data->variableInfos[aIndex].type;
    return cType >= hkbVariableInfo::kBool && cType <= hkbVariableInfo::kReal;
}
