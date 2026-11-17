#include <TiltedOnlinePCH.h>

#include <Systems/FaceGenSystem.h>

#include <Games/References.h>

#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>

#include <Components.h>
#include <World.h>

#include <Structs/Tints.h>

#include <Games/Skyrim/NetImmerse/NiTriBasedGeom.h>
#include <Games/Skyrim/NetImmerse/NiRenderedTexture.h>
#include <Games/Skyrim/NetImmerse/BSShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSLightingShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSMaskedShaderMaterial.h>
#include <Games/Memory.h>

__declspec(noinline) NiTriBasedGeom* GetHeadTriBasedGeom(Actor* apActor, uint32_t aPartType)
{
    if (!apActor)
        return nullptr;

    using TGetObjectByName = NiAVObject*(BSFaceGenNiNode*, const char**, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 74481);

    BSFaceGenNiNode* pFaceNode = apActor->GetFaceGenNiNode();
    TESNPC* pActorBase = Cast<TESNPC>(apActor->baseForm);

    if (pFaceNode && pActorBase)
    {
        BGSHeadPart* pFacePart = pActorBase->GetHeadPart(aPartType);

        if (pFacePart && pFacePart->name.data)
        {
            NiAVObject* pHeadNode = GetObjectByName(pFaceNode, &pFacePart->name.data, 1);

            if (pHeadNode)
            {
                NiTriBasedGeom* pGeometry = pHeadNode->CastToNiTriBasedGeom();

                if (pGeometry)
                {
                    return pGeometry;
                }
            }
        }
    }

    return nullptr;
}

struct TextureHolder;
TP_THIS_FUNCTION(TCreateResourceView, Ni2DBuffer*, TextureHolder, uint32_t, uint32_t);

using TCreateTexture = NiRenderedTexture*(__fastcall)(BSFixedString& aName);
using TCreateTints = void(__fastcall)(const GameArray<TintMask*>& acTints, NiRenderedTexture* apTexture);

void FaceGenSystem::Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept
{
    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 527752);
    POINTER_SKYRIMSE(TCreateTexture, CreateTexture, 69335);
    POINTER_SKYRIMSE(TCreateResourceView, CreateResourceView, 75507);
    POINTER_SKYRIMSE(TCreateTints, CreateTints, 26454);
    POINTER_SKYRIMSE(TextureHolder, s_textureHolder, 524907);

    if (aFaceGenComponent.Generated || !apActor)
        return;

    auto& tintsEntries = aFaceGenComponent.FaceTints.Entries;
    if (tintsEntries.empty())
    {
        aFaceGenComponent.Generated = true;
        return;
    }

    auto pTriBasedGeom = GetHeadTriBasedGeom(apActor, 1);

    if (!pTriBasedGeom)
        return;

    BSShaderProperty* pShaderProperty = niptr_cast<BSShaderProperty>(pTriBasedGeom->effect);

    if (!pShaderProperty)
        return;

    pShaderProperty->IncRef();

    BSLightingShaderProperty* pLightingShader = static_cast<BSLightingShaderProperty*>(pShaderProperty);

    if (pLightingShader->GetRTTI() == NiMaskedShaderRTTI.Get())
    {
        BSMaskedShaderMaterial* pMaterial = static_cast<BSMaskedShaderMaterial*>(pLightingShader->material);
        auto* pTextureHolder = s_textureHolder.Get();
        if (!pMaterial || !pTextureHolder)
        {
            pShaderProperty->DecRef();
            return;
        }

        BSFixedString name("");
        auto pTexture = CreateTexture(name);
        if (!pTexture)
        {
            pShaderProperty->DecRef();
            return;
        }

        pTexture->buffer = CreateResourceView(pTextureHolder, 512, 512);
        if (!pTexture->buffer)
        {
            pShaderProperty->DecRef();
            return;
        }

        GameArray<TintMask*> tints;
        tints.capacity = tints.length = tintsEntries.size() & 0xFFFFFFFF;
        tints.data = (TintMask**)Memory::Allocate(sizeof(TintMask*) * tints.length);
        if (!tints.data)
        {
            pShaderProperty->DecRef();
            return;
        }

        for (auto i = 0u; i < tints.length; ++i)
        {
            tints[i] = Memory::New<TintMask>();

            tints[i]->alpha = tintsEntries[i].Alpha;
            tints[i]->color = tintsEntries[i].Color;
            tints[i]->type = tintsEntries[i].Type;

            auto pNewTexture = Memory::New<TESTexture>();
            pNewTexture->Construct();
            pNewTexture->Init();

            pNewTexture->name.Set(tintsEntries[i].Name.c_str());

            tints[i]->texture = pNewTexture;
        }

        CreateTints(tints, pTexture);

        for (auto i = 0u; i < tints.length; ++i)
        {
            tints[i]->texture->~TESTexture();
            Memory::Free(tints[i]->texture);
            Memory::Free(tints[i]);
        }

        Memory::Free(tints.data);

        pMaterial->renderedTexture = pTexture;

        aFaceGenComponent.Generated = true;
    }

    pShaderProperty->DecRef();
}

void FaceGenSystem::Setup(World& aWorld, const entt::entity aEntity, const Tints& acTints) noexcept
{
    if (acTints.Entries.empty())
        return;

    auto& component = aWorld.emplace_or_replace<FaceGenComponent>(aEntity);
    component.FaceTints = acTints;
}
