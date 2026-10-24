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
    using TGetObjectByName = NiAVObject*(BSFaceGenNiNode*, const char**, char);
    POINTER_SKYRIMSE(TGetObjectByName, GetObjectByName, 74481);

    BSFaceGenNiNode* pFaceNode = apActor->GetFaceGenNiNode();
    TESNPC* pActorBase = Cast<TESNPC>(apActor->baseForm);

    if (pFaceNode && pActorBase)
    {
        BGSHeadPart* pFacePart = pActorBase->GetHeadPart(aPartType);

        if (pFacePart)
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
TP_THIS_FUNCTION(TSetShaderMaterial, uint32_t, BSShaderProperty, BSMaskedShaderMaterial* apMaterial, bool aUnk1);
TP_THIS_FUNCTION(TInitializeShader, uint32_t, BSLightingShaderProperty, NiTriBasedGeom* apGeometry);

using TCreateTexture = NiRenderedTexture*(__fastcall)(BSFixedString& aName);
using TCreateTints = void(__fastcall)(const GameArray<TintMask*>& acTints, NiRenderedTexture* apTexture);

void FaceGenSystem::Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept
{
    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 527752);
    POINTER_SKYRIMSE(TCreateTexture, CreateTexture, 69335);
    POINTER_SKYRIMSE(TCreateResourceView, CreateResourceView, 75507);
    POINTER_SKYRIMSE(TCreateTints, CreateTints, 26454);
    POINTER_SKYRIMSE(TextureHolder, s_textureHolder, 524907);
    POINTER_SKYRIMSE(TSetShaderMaterial, SetShaderMaterial, 98897);
    POINTER_SKYRIMSE(TInitializeShader, InitializeShader, 99862);

    if (!apActor || apActor->formID == 0x14)
        return;

    BSFaceGenNiNode* pCurrentFaceNode = apActor->GetFaceGenNiNode();
    if (!pCurrentFaceNode)
        return;

    if (aFaceGenComponent.Generated && aFaceGenComponent.LastFaceNode == pCurrentFaceNode)
        return;

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
        if (!pMaterial)
        {
            pShaderProperty->DecRef();
            return;
        }

        // CRITICAL FIX (Screenshot 4):
        // Never use an empty "" texture name or mutate pMaterial->renderedTexture in-place!
        // Skyrim pools/hashes BSLightingShaderMaterialFacegen across actors (and FaceGenFixes.dll),
        // so mutating pMaterial->renderedTexture directly overwrote the local player's face tint with
        // the remote player's skin color (e.g. turning a Nord player's face bright purple/Argonian!).
        char texNameBuf[64]{};
        std::snprintf(texNameBuf, sizeof(texNameBuf), "STR_FaceTint_%08X", apActor->formID);
        BSFixedString name(texNameBuf);
        auto pTexture = CreateTexture(name);
        pTexture->buffer = CreateResourceView(s_textureHolder.Get(), 512, 512);

        auto& tintsEntries = aFaceGenComponent.FaceTints.Entries;

        GameArray<TintMask*> tints;
        tints.capacity = tints.length = aFaceGenComponent.FaceTints.Entries.size() & 0xFFFFFFFF;
        tints.data = (TintMask**)Memory::Allocate(sizeof(TintMask*) * tints.length);

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

        BSMaskedShaderMaterial* pClonedMaterial = pMaterial->Create();
        if (pClonedMaterial)
        {
            pClonedMaterial->Copy(pMaterial);
            pClonedMaterial->renderedTexture = pTexture;

            TiltedPhoques::ThisCall(SetShaderMaterial, pLightingShader, pClonedMaterial, true);
            TiltedPhoques::ThisCall(InitializeShader, pLightingShader, pTriBasedGeom);

            // CRITICAL CTD-ON-CELL-CHANGE FIX:
            // 1. Only release pClonedMaterial if SetShaderMaterial cloned a separate pooled material instance!
            //    If SetShaderMaterial (or FaceGenFixes.dll) adopted pClonedMaterial directly into
            //    pLightingShader->material, freeing it here caused an immediate Use-After-Free CTD inside
            //    ~BSLightingShaderProperty() the moment a co-op friend unloaded on location/cell change.
            // 2. When pClonedMaterial WAS cloned into a separate instance, invoke SkyrimSE.exe's own virtual
            //    scalar deleting destructor with flags = 1 (never flags = 0 + manual Memory::Free(..., false)),
            //    matching pMaterial->Create()'s exact heap & alignment under EngineFixes MemoryManager true/false.
            if (pLightingShader->material != pClonedMaterial)
            {
                using TScalarDtor = void*(__fastcall*)(BSMaskedShaderMaterial*, uint32_t);
                void** vtbl = *reinterpret_cast<void***>(pClonedMaterial);
                if (vtbl && vtbl[0])
                    reinterpret_cast<TScalarDtor>(vtbl[0])(pClonedMaterial, 1);
            }
        }

        aFaceGenComponent.Generated = true;
        aFaceGenComponent.LastFaceNode = pCurrentFaceNode;
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
