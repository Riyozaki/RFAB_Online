#pragma once

#include <NetImmerse/NiPointer.h>
#include <NetImmerse/NiTexture.h>

struct BSMaskedShaderMaterial
{
    virtual ~BSMaskedShaderMaterial();
    virtual BSMaskedShaderMaterial* Create();
    virtual void Copy(BSMaskedShaderMaterial* apSource);

    uint8_t pad08[0x98];

    NiPointer<NiTexture> renderedTexture;
};

static_assert(offsetof(BSMaskedShaderMaterial, renderedTexture) == 0xA0);
