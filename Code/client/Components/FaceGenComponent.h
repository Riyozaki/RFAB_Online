#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <cstdint>
#include <Structs/Tints.h>

struct FaceGenComponent
{
    FaceGenComponent() noexcept {}

    bool Generated{false};
    bool HairGenerated{false};
    void* LastFaceNode{nullptr};
    void* LastHairFaceNode{nullptr};
    void* LastRootNode{nullptr};
    uint32_t LastHairGeomCount{0};
    uint64_t NextHairCheckMs{0};
    Tints FaceTints{};

    void ResetGenerationState() noexcept
    {
        Generated = false;
        HairGenerated = false;
        LastFaceNode = nullptr;
        LastHairFaceNode = nullptr;
        LastRootNode = nullptr;
        LastHairGeomCount = 0;
        NextHairCheckMs = 0;
    }
};
