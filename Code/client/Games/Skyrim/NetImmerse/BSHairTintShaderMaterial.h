#pragma once

#include <cstddef>
#include <cstdint>

struct BSHairTintShaderMaterial
{
    static constexpr uint32_t kFeatureHairTint = 6u;
    static constexpr uint32_t kTypeLighting = 2u;

    virtual ~BSHairTintShaderMaterial();
    virtual BSHairTintShaderMaterial* Create();
    virtual void Copy(BSHairTintShaderMaterial* apSource);
    virtual bool DoIsCopy(BSHairTintShaderMaterial* apOther);
    virtual uint32_t ComputeCRC32();
    virtual BSHairTintShaderMaterial* GetDefault();
    virtual uint32_t GetFeature() const;
    virtual uint32_t GetType() const;

    uint8_t pad08[0x24];
    uint32_t hashKey;
    uint8_t pad30[0x70];
    float tintRed;
    float tintGreen;
    float tintBlue;
    uint32_t padAC;
};

static_assert(offsetof(BSHairTintShaderMaterial, hashKey) == 0x2C);
static_assert(offsetof(BSHairTintShaderMaterial, tintRed) == 0xA0);
static_assert(sizeof(BSHairTintShaderMaterial) == 0xB0);
