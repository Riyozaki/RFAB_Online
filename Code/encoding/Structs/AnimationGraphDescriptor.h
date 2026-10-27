#pragma once

enum class AnimationFloatBlendMode : uint8_t
{
    kContinuousLinear = 0,
    kCircularDegrees360 = 1,
    kDiscreteStep = 2,
};

struct AnimationGraphDescriptor
{
    AnimationGraphDescriptor() = default;

    template <std::size_t N, std::size_t O, std::size_t P> AnimationGraphDescriptor(const uint32_t (&acBooleanList)[N], const uint32_t (&acFloatList)[O], const uint32_t (&acIntegerList)[P])
    {
        static_assert(N <= 64, "Too many boolean variables!");
        static_assert((1 + O + P) <= 64, "Too many float and integer!");

        BooleanLookUpTable.assign(acBooleanList, acBooleanList + N);
        FloatLookupTable.assign(acFloatList, acFloatList + O);
        IntegerLookupTable.assign(acIntegerList, acIntegerList + P);
    }

    template <std::size_t N, std::size_t O, std::size_t P>
    AnimationGraphDescriptor(
        const uint32_t (&acBooleanList)[N],
        const uint32_t (&acFloatList)[O],
        const uint32_t (&acIntegerList)[P],
        const AnimationFloatBlendMode (&acFloatBlendModes)[O])
        : AnimationGraphDescriptor(acBooleanList, acFloatList, acIntegerList)
    {
        FloatBlendModes.assign(acFloatBlendModes, acFloatBlendModes + O);
    }

    [[nodiscard]] AnimationFloatBlendMode GetFloatBlendMode(std::size_t aSlot) const noexcept
    {
        return aSlot < FloatBlendModes.size() ? FloatBlendModes[aSlot] : AnimationFloatBlendMode::kContinuousLinear;
    }

    bool IsSynced(uint32_t aIdx) const
    {
        auto itor = std::find(std::begin(BooleanLookUpTable), std::end(BooleanLookUpTable), aIdx);
        if (itor != std::end(BooleanLookUpTable))
            return true;

        itor = std::find(std::begin(FloatLookupTable), std::end(FloatLookupTable), aIdx);
        if (itor != std::end(FloatLookupTable))
            return true;

        itor = std::find(std::begin(IntegerLookupTable), std::end(IntegerLookupTable), aIdx);
        if (itor != std::end(IntegerLookupTable))
            return true;

        return false;
    }

    TiltedPhoques::Vector<uint32_t> BooleanLookUpTable;
    TiltedPhoques::Vector<uint32_t> FloatLookupTable;
    TiltedPhoques::Vector<uint32_t> IntegerLookupTable;
    TiltedPhoques::Vector<AnimationFloatBlendMode> FloatBlendModes;
};
