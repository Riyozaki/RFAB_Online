#include <TiltedOnlinePCH.h>

#include <Systems/FaceGenSystem.h>

#include <Games/References.h>
#include <PlayerCharacter.h>

#include <Forms/BGSColorForm.h>
#include <Forms/BGSHeadPart.h>
#include <Forms/TESNPC.h>

#include <Components.h>
#include <World.h>

#include <Structs/Tints.h>
#include <common/CoopHairColorSync.h>

#include <Games/Skyrim/NetImmerse/BSHairTintShaderMaterial.h>
#include <Games/Skyrim/NetImmerse/BSLightingShaderProperty.h>
#include <Games/Skyrim/NetImmerse/BSMaskedShaderMaterial.h>
#include <Games/Skyrim/NetImmerse/BSShaderProperty.h>
#include <Games/Skyrim/NetImmerse/NiNode.h>
#include <Games/Skyrim/NetImmerse/NiRenderedTexture.h>
#include <Games/Skyrim/NetImmerse/NiTriBasedGeom.h>
#include <Games/Memory.h>

#include <chrono>
#include <cstring>

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
TP_THIS_FUNCTION(TSetShaderMaterial, uint32_t, BSShaderProperty, void* apMaterial, bool aUnk1);
TP_THIS_FUNCTION(TInitializeShader, uint32_t, BSLightingShaderProperty, NiTriBasedGeom* apGeometry);

using TCreateTexture = NiRenderedTexture*(__fastcall)(BSFixedString& aName);
using TCreateTints = void(__fastcall)(const GameArray<TintMask*>& acTints, NiRenderedTexture* apTexture);

namespace
{
constexpr uint32_t kMaxSceneGraphTraversalDepth = 24u;

uint64_t NowFaceGenSteadyMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// RaceMenu (skee64 SKEEHooks.cpp) attaches a NiBooleanExtraData named "NO_TINT" (m_data == true at +0x18)
// to hair accessory sub-geometries (ribbons, headbands, flowers) so hair dye skips them.
bool HasShaderNoTintFlag(const BSShaderProperty* apShaderProp) noexcept
{
    if (!apShaderProp)
        return false;

    const auto* pBase = reinterpret_cast<const uint8_t*>(apShaderProp);
    auto* const* ppExtra = *reinterpret_cast<uint8_t* const* const*>(pBase + 0x20);
    const uint16_t extraCount = *reinterpret_cast<const uint16_t*>(pBase + 0x28);
    if (!ppExtra || extraCount == 0 || extraCount > 64u)
        return false;

    for (uint16_t i = 0; i < extraCount; ++i)
    {
        const uint8_t* pExtra = ppExtra[i];
        if (!pExtra)
            continue;

        const auto* pExtraName = reinterpret_cast<const BSFixedString*>(pExtra + 0x10);
        const char* pAscii = pExtraName->AsAscii();
        if (pAscii && std::strcmp(pAscii, "NO_TINT") == 0)
            return pExtra[0x18] != 0;
    }

    return false;
}

BSHairTintShaderMaterial* TryGetHairTintMaterial(
    NiTriBasedGeom* apGeom,
    NiRTTI* apLightingShaderRtti,
    BSLightingShaderProperty*& apOutLightingShader) noexcept
{
    apOutLightingShader = nullptr;
    if (!apGeom || !apLightingShaderRtti)
        return nullptr;

    BSShaderProperty* pShaderProp = niptr_cast<BSShaderProperty>(apGeom->effect);
    if (!pShaderProp || pShaderProp->GetRTTI() != apLightingShaderRtti || HasShaderNoTintFlag(pShaderProp))
        return nullptr;

    auto* pCandidate = static_cast<BSHairTintShaderMaterial*>(pShaderProp->material);
    if (!pCandidate ||
        pCandidate->GetType() != BSHairTintShaderMaterial::kTypeLighting ||
        pCandidate->GetFeature() != BSHairTintShaderMaterial::kFeatureHairTint)
    {
        return nullptr;
    }

    apOutLightingShader = static_cast<BSLightingShaderProperty*>(pShaderProp);
    return pCandidate;
}

bool FindFirstHairTintColorRecursive(
    NiAVObject* apObject,
    NiRTTI* apLightingShaderRtti,
    uint8_t& aOutRed,
    uint8_t& aOutGreen,
    uint8_t& aOutBlue,
    const uint32_t aDepth = 0) noexcept
{
    if (!apObject || aDepth > kMaxSceneGraphTraversalDepth)
        return false;

    if (NiTriBasedGeom* pGeom = apObject->AsBSGeometry())
    {
        BSLightingShaderProperty* pLightingShader = nullptr;
        if (BSHairTintShaderMaterial* pHairMat = TryGetHairTintMaterial(pGeom, apLightingShaderRtti, pLightingShader))
        {
            aOutRed = CoopHairColorSync::HairShaderTintToChannel(pHairMat->tintRed);
            aOutGreen = CoopHairColorSync::HairShaderTintToChannel(pHairMat->tintGreen);
            aOutBlue = CoopHairColorSync::HairShaderTintToChannel(pHairMat->tintBlue);
            return true;
        }
        return false;
    }

    if (NiNode* pNode = apObject->AsNiNode())
    {
        if (!pNode->children.data || pNode->children.length == 0)
            return false;

        const uint16_t bound = (pNode->children.unk1 <= pNode->children.length)
            ? pNode->children.unk1
            : pNode->children.length;
        for (uint16_t i = 0; i < bound; ++i)
        {
            if (NiAVObject* pChild = pNode->children.data[i])
            {
                if (FindFirstHairTintColorRecursive(pChild, apLightingShaderRtti, aOutRed, aOutGreen, aOutBlue, aDepth + 1u))
                    return true;
            }
        }
    }

    return false;
}

void CountHairTintGeometriesRecursive(
    NiAVObject* apObject,
    NiRTTI* apLightingShaderRtti,
    const NiAVObject* apFaceNode,
    bool& aOutVisitedFaceNode,
    uint32_t& aInOutCount,
    const uint32_t aDepth = 0) noexcept
{
    if (!apObject || aDepth > kMaxSceneGraphTraversalDepth)
        return;

    if (apObject == apFaceNode)
        aOutVisitedFaceNode = true;

    if (NiTriBasedGeom* pGeom = apObject->AsBSGeometry())
    {
        BSLightingShaderProperty* pLightingShader = nullptr;
        if (TryGetHairTintMaterial(pGeom, apLightingShaderRtti, pLightingShader))
            ++aInOutCount;
        return;
    }

    if (NiNode* pNode = apObject->AsNiNode())
    {
        if (!pNode->children.data || pNode->children.length == 0)
            return;

        const uint16_t bound = (pNode->children.unk1 <= pNode->children.length)
            ? pNode->children.unk1
            : pNode->children.length;
        for (uint16_t i = 0; i < bound; ++i)
        {
            if (NiAVObject* pChild = pNode->children.data[i])
            {
                CountHairTintGeometriesRecursive(
                    pChild, apLightingShaderRtti, apFaceNode, aOutVisitedFaceNode, aInOutCount, aDepth + 1u);
            }
        }
    }
}

void ApplyHairTintIsolationRecursive(
    NiAVObject* apObject,
    NiRTTI* apLightingShaderRtti,
    TSetShaderMaterial* apSetShaderMaterial,
    TInitializeShader* apInitializeShader,
    const float aTargetRed,
    const float aTargetGreen,
    const float aTargetBlue,
    const bool aForceIsolate,
    const uint32_t aDepth = 0) noexcept
{
    if (!apObject || aDepth > kMaxSceneGraphTraversalDepth)
        return;

    if (NiTriBasedGeom* pGeom = apObject->AsBSGeometry())
    {
        BSLightingShaderProperty* pLightingShader = nullptr;
        BSHairTintShaderMaterial* pHairMat = TryGetHairTintMaterial(pGeom, apLightingShaderRtti, pLightingShader);
        if (!pHairMat || !pLightingShader)
            return;

        const bool drifted = CoopHairColorSync::IsHairTintDrifted(
            pHairMat->tintRed, pHairMat->tintGreen, pHairMat->tintBlue,
            aTargetRed, aTargetGreen, aTargetBlue);
        if (!aForceIsolate && !drifted)
            return;

        pLightingShader->IncRef();
        BSHairTintShaderMaterial* pClonedHairMat = pHairMat->Create();
        if (pClonedHairMat)
        {
            pClonedHairMat->Copy(pHairMat);
            pClonedHairMat->tintRed = aTargetRed;
            pClonedHairMat->tintGreen = aTargetGreen;
            pClonedHairMat->tintBlue = aTargetBlue;

            TiltedPhoques::ThisCall(apSetShaderMaterial, pLightingShader, pClonedHairMat, true);
            TiltedPhoques::ThisCall(apInitializeShader, pLightingShader, pGeom);

            if (pLightingShader->material != pClonedHairMat)
            {
                using TScalarDtor = void*(__fastcall*)(BSHairTintShaderMaterial*, uint32_t);
                void** vtbl = *reinterpret_cast<void***>(pClonedHairMat);
                if (vtbl && vtbl[0])
                    reinterpret_cast<TScalarDtor>(vtbl[0])(pClonedHairMat, 1);
            }
        }
        pLightingShader->DecRef();
        return;
    }

    if (NiNode* pNode = apObject->AsNiNode())
    {
        if (!pNode->children.data || pNode->children.length == 0)
            return;

        const uint16_t bound = (pNode->children.unk1 <= pNode->children.length)
            ? pNode->children.unk1
            : pNode->children.length;
        for (uint16_t i = 0; i < bound; ++i)
        {
            if (NiAVObject* pChild = pNode->children.data[i])
            {
                ApplyHairTintIsolationRecursive(
                    pChild,
                    apLightingShaderRtti,
                    apSetShaderMaterial,
                    apInitializeShader,
                    aTargetRed,
                    aTargetGreen,
                    aTargetBlue,
                    aForceIsolate,
                    aDepth + 1u);
            }
        }
    }
}

void IsolateActorHairTintMaterials(
    Actor* apActor,
    const uint8_t aRed,
    const uint8_t aGreen,
    const uint8_t aBlue,
    bool& aInOutHairGenerated,
    void*& apInOutLastFaceNode,
    void*& apInOutLastRootNode,
    uint32_t& aInOutLastHairGeomCount,
    uint64_t& aInOutNextHairCheckMs) noexcept
{
    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 527752);
    POINTER_SKYRIMSE(TSetShaderMaterial, SetShaderMaterial, 98897);
    POINTER_SKYRIMSE(TInitializeShader, InitializeShader, 99862);

    if (!apActor || apActor->IsStillLoading() || apActor->IsDeleted())
        return;

    NiNode* pRootNode = apActor->GetNiNode();
    BSFaceGenNiNode* pFaceNode = apActor->GetFaceGenNiNode();
    if (!pRootNode && !pFaceNode)
        return;

    const uint64_t nowMs = NowFaceGenSteadyMs();
    const bool nodeOrInitialChange =
        !aInOutHairGenerated || apInOutLastFaceNode != pFaceNode || apInOutLastRootNode != pRootNode;

    if (!nodeOrInitialChange && nowMs < aInOutNextHairCheckMs)
        return;

    NiRTTI* pLightingRtti = NiMaskedShaderRTTI.Get();
    bool visitedFaceNode = false;
    uint32_t curHairGeomCount = 0;
    if (pRootNode)
        CountHairTintGeometriesRecursive(pRootNode, pLightingRtti, pFaceNode, visitedFaceNode, curHairGeomCount);
    if (pFaceNode && !visitedFaceNode)
    {
        bool dummyVisited = false;
        CountHairTintGeometriesRecursive(pFaceNode, pLightingRtti, pFaceNode, dummyVisited, curHairGeomCount);
    }

    const bool forceIsolate = CoopHairColorSync::ShouldIsolateHairTintPass(
        apInOutLastFaceNode,
        pFaceNode,
        apInOutLastRootNode,
        pRootNode,
        aInOutLastHairGeomCount,
        curHairGeomCount,
        aInOutHairGenerated);

    const float targetR = CoopHairColorSync::ChannelToHairShaderTint(aRed);
    const float targetG = CoopHairColorSync::ChannelToHairShaderTint(aGreen);
    const float targetB = CoopHairColorSync::ChannelToHairShaderTint(aBlue);

    if (curHairGeomCount > 0)
    {
        if (pRootNode)
        {
            ApplyHairTintIsolationRecursive(
                pRootNode,
                pLightingRtti,
                SetShaderMaterial.Get(),
                InitializeShader.Get(),
                targetR,
                targetG,
                targetB,
                forceIsolate);
        }
        if (pFaceNode && !visitedFaceNode)
        {
            ApplyHairTintIsolationRecursive(
                pFaceNode,
                pLightingRtti,
                SetShaderMaterial.Get(),
                InitializeShader.Get(),
                targetR,
                targetG,
                targetB,
                forceIsolate);
        }
    }

    aInOutHairGenerated = (curHairGeomCount > 0);
    apInOutLastFaceNode = pFaceNode;
    apInOutLastRootNode = pRootNode;
    aInOutLastHairGeomCount = curHairGeomCount;
    aInOutNextHairCheckMs = nowMs + CoopHairColorSync::kSteadyHairCheckIntervalMs;
}

struct LocalPlayerHairIsolationState
{
    bool HairGenerated{false};
    void* LastFaceNode{nullptr};
    void* LastRootNode{nullptr};
    uint32_t LastHairGeomCount{0};
    uint64_t NextHairCheckMs{0};
    uint32_t LastPackedRgb{0xFFFFFFFFu};
};

LocalPlayerHairIsolationState s_localPlayerHairState{};
} // namespace

bool FaceGenSystem::TryExtractActorHairColor(
    Actor* apActor,
    uint8_t& aOutRed,
    uint8_t& aOutGreen,
    uint8_t& aOutBlue) noexcept
{
    if (!apActor)
        return false;

    if (const TESNPC* pNpc = Cast<TESNPC>(apActor->baseForm))
    {
        if (pNpc->headData && pNpc->headData->hairColor)
        {
            aOutRed = pNpc->headData->hairColor->color.red;
            aOutGreen = pNpc->headData->hairColor->color.green;
            aOutBlue = pNpc->headData->hairColor->color.blue;
            return true;
        }
    }

    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 527752);
    if (BSFaceGenNiNode* pFaceNode = apActor->GetFaceGenNiNode())
    {
        if (FindFirstHairTintColorRecursive(pFaceNode, NiMaskedShaderRTTI.Get(), aOutRed, aOutGreen, aOutBlue))
            return true;
    }
    if (NiNode* pRootNode = apActor->GetNiNode())
    {
        if (FindFirstHairTintColorRecursive(pRootNode, NiMaskedShaderRTTI.Get(), aOutRed, aOutGreen, aOutBlue))
            return true;
    }

    return false;
}

void FaceGenSystem::InvalidateLocalPlayerHairTintIsolation() noexcept
{
    s_localPlayerHairState = {};
}

void FaceGenSystem::UpdateLocalPlayerHairTintIsolation() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->IsStillLoading() || pPlayer->IsDeleted())
        return;

    uint8_t hairR = 0, hairG = 0, hairB = 0;
    if (!TryExtractActorHairColor(pPlayer, hairR, hairG, hairB))
        return;

    const uint32_t packedRgb = CoopHairColorSync::PackHairColorRgb(hairR, hairG, hairB);
    if (packedRgb != s_localPlayerHairState.LastPackedRgb)
    {
        s_localPlayerHairState.HairGenerated = false;
        s_localPlayerHairState.NextHairCheckMs = 0;
        s_localPlayerHairState.LastPackedRgb = packedRgb;
    }

    IsolateActorHairTintMaterials(
        pPlayer,
        hairR,
        hairG,
        hairB,
        s_localPlayerHairState.HairGenerated,
        s_localPlayerHairState.LastFaceNode,
        s_localPlayerHairState.LastRootNode,
        s_localPlayerHairState.LastHairGeomCount,
        s_localPlayerHairState.NextHairCheckMs);
}

void FaceGenSystem::Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept
{
    (void)aWorld;
    POINTER_SKYRIMSE(NiRTTI, NiMaskedShaderRTTI, 527752);
    POINTER_SKYRIMSE(TCreateTexture, CreateTexture, 69335);
    POINTER_SKYRIMSE(TCreateResourceView, CreateResourceView, 75507);
    POINTER_SKYRIMSE(TCreateTints, CreateTints, 26454);
    POINTER_SKYRIMSE(TextureHolder, s_textureHolder, 524907);
    POINTER_SKYRIMSE(TSetShaderMaterial, SetShaderMaterial, 98897);
    POINTER_SKYRIMSE(TInitializeShader, InitializeShader, 99862);

    if (!apActor || apActor->formID == 0x14 || apActor->IsStillLoading() || apActor->IsDeleted())
        return;

    BSFaceGenNiNode* pCurrentFaceNode = apActor->GetFaceGenNiNode();
    const bool needFaceTints = pCurrentFaceNode && (!aFaceGenComponent.Generated || aFaceGenComponent.LastFaceNode != pCurrentFaceNode);
    if (needFaceTints)
    {
        const size_t faceTintCount = aFaceGenComponent.FaceTints.CountFaceTintEntries();
        if (faceTintCount == 0)
        {
            aFaceGenComponent.Generated = true;
            aFaceGenComponent.LastFaceNode = pCurrentFaceNode;
        }
        else if (auto pTriBasedGeom = GetHeadTriBasedGeom(apActor, 1))
        {
            if (BSShaderProperty* pShaderProperty = niptr_cast<BSShaderProperty>(pTriBasedGeom->effect))
            {
                pShaderProperty->IncRef();

                BSLightingShaderProperty* pLightingShader = static_cast<BSLightingShaderProperty*>(pShaderProperty);

                if (pLightingShader->GetRTTI() == NiMaskedShaderRTTI.Get())
                {
                    BSMaskedShaderMaterial* pMaterial = static_cast<BSMaskedShaderMaterial*>(pLightingShader->material);
                    if (pMaterial)
                    {
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
                        tints.capacity = tints.length = static_cast<uint32_t>(faceTintCount & 0xFFFFFFFFu);
                        tints.data = (TintMask**)Memory::Allocate(sizeof(TintMask*) * tints.length);

                        uint32_t dstIdx = 0;
                        for (size_t srcIdx = 0; srcIdx < tintsEntries.size() && dstIdx < tints.length; ++srcIdx)
                        {
                            if (Tints::IsHairColorEntry(tintsEntries[srcIdx]))
                                continue;

                            tints[dstIdx] = Memory::New<TintMask>();

                            tints[dstIdx]->alpha = tintsEntries[srcIdx].Alpha;
                            tints[dstIdx]->color = tintsEntries[srcIdx].Color;
                            tints[dstIdx]->type = tintsEntries[srcIdx].Type;

                            auto pNewTexture = Memory::New<TESTexture>();
                            pNewTexture->Construct();
                            pNewTexture->Init();

                            pNewTexture->name.Set(tintsEntries[srcIdx].Name.c_str());

                            tints[dstIdx]->texture = pNewTexture;
                            ++dstIdx;
                        }
                        tints.length = dstIdx;

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
                }

                pShaderProperty->DecRef();
            }
        }
    }

    uint8_t hairR = 0, hairG = 0, hairB = 0;
    if (aFaceGenComponent.FaceTints.TryGetHairColor(hairR, hairG, hairB))
    {
        IsolateActorHairTintMaterials(
            apActor,
            hairR,
            hairG,
            hairB,
            aFaceGenComponent.HairGenerated,
            aFaceGenComponent.LastHairFaceNode,
            aFaceGenComponent.LastRootNode,
            aFaceGenComponent.LastHairGeomCount,
            aFaceGenComponent.NextHairCheckMs);
    }
}

void FaceGenSystem::Setup(World& aWorld, const entt::entity aEntity, const Tints& acTints) noexcept
{
    if (acTints.Entries.empty())
        return;

    auto& component = aWorld.emplace_or_replace<FaceGenComponent>(aEntity);
    component.ResetGenerationState();
    component.FaceTints = acTints;
}
