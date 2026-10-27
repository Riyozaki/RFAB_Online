#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/ActorExtension.h>
#include <Games/References.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <Structs/AnimationGraphDescriptorManager.h>
#include <Structs/GridCellCoords.h>
#include <CoopAnimationState.h>
#include <CoopDamagePipeline.h>
#include <World.h>

void InterpolationSystem::Update(Actor* apActor, InterpolationComponent& aInterpolationComponent, const uint64_t aTick, const RemoteAnimationComponent* apAnimComponent) noexcept
{
    auto& movements = aInterpolationComponent.TimePoints;

    if (movements.size() < 2)
    {
        aInterpolationComponent.LastHorizontalSpeed = 0.0f;
        return;
    }

    const double renderTick = aInterpolationComponent.Clock.IsRunning()
        ? aInterpolationComponent.Clock.GetRenderTick()
        : static_cast<double>(aTick);

    while (movements.size() > 2)
    {
        const auto second = *(++movements.begin());
        if (renderTick > static_cast<double>(second.Tick))
            movements.pop_front();
        else
            break;
    }

    const auto& first = *(movements.begin());
    const auto& second = *(++movements.begin());

    // Calculate delta movement since last update
    auto delta = 0.0001f;
    const auto tickDelta = static_cast<float>(second.Tick > first.Tick ? (second.Tick - first.Tick) : 0u);
    if (tickDelta > 0.f)
    {
        delta = static_cast<float>((renderTick - static_cast<double>(first.Tick)) / static_cast<double>(tickDelta));
    }

    delta = std::clamp(delta, 0.0f, 1.0f);

    const float stepDx = second.Position.x - first.Position.x;
    const float stepDy = second.Position.y - first.Position.y;
    const float stepDz = second.Position.z - first.Position.z;
    const float stepDistSq = stepDx * stepDx + stepDy * stepDy + stepDz * stepDz;
    constexpr float kTeleportSnapThresholdSq = 2048.0f * 2048.0f;

    if (tickDelta > 0.f && stepDistSq <= kTeleportSnapThresholdSq)
    {
        const float horizDist = std::sqrt(stepDx * stepDx + stepDy * stepDy);
        aInterpolationComponent.LastHorizontalSpeed = horizDist * (1000.0f / tickDelta);
    }
    else
    {
        aInterpolationComponent.LastHorizontalSpeed = 0.0f;
    }

    const NiPoint3 position = (stepDistSq > kTeleportSnapThresholdSq)
        ? second.Position
        : NiPoint3{TiltedPhoques::Lerp(first.Position, second.Position, delta)};

    aInterpolationComponent.Position = position;

    // Don't try to move a null actor
    if (!apActor)
        return;

    bool movedViaCellTransfer = false;
    if (apActor->formID != 0x14)
    {
        PlayerCharacter* pLocalPlayer = PlayerCharacter::Get();
        TESWorldSpace* pLocalWs = pLocalPlayer ? pLocalPlayer->GetWorldSpace() : nullptr;
        const TES* pTes = TES::Get();
        ModManager* pModMgr = ModManager::Get();
        if (pLocalWs && pTes && pModMgr && apActor->GetWorldSpace() == pLocalWs)
        {
            const GridCellCoords destGrid = GridCellCoords::CalculateGridCellCoords(position.x, position.y);
            const GridCellCoords centerGrid(pTes->centerGridX, pTes->centerGridY);
            if (GridCellCoords::IsCellInGridCell(destGrid, centerGrid, false))
            {
                TESObjectCELL* pDestCell = pModMgr->GetCellFromCoordinates(destGrid.X, destGrid.Y, pLocalWs, false);
                const bool isDynamicActor = (apActor->formID & 0xFF000000u) == 0xFF000000u;
                (void)isDynamicActor;

                TESObjectCELL* pParentCell = apActor->parentCell;
                const bool hasAttachedParentCell =
                    pParentCell && pParentCell->loadedCellData && !pParentCell->cellDetached && pParentCell->worldspace == pLocalWs;
                const bool hasLoadedDestCell =
                    pDestCell && pDestCell->loadedCellData && !pDestCell->cellDetached;

                if (hasAttachedParentCell &&
                    (!aInterpolationComponent.HasTrackedParentGrid || aInterpolationComponent.TrackedParentCell != pParentCell))
                {
                    aInterpolationComponent.TrackedParentCell = pParentCell;
                    if (pParentCell == pDestCell)
                    {
                        aInterpolationComponent.TrackedParentGridX = destGrid.X;
                        aInterpolationComponent.TrackedParentGridY = destGrid.Y;
                        aInterpolationComponent.HasTrackedParentGrid = true;
                    }
                    else
                    {
                        bool foundInActiveGrid = false;
                        for (int32_t dy = -2; dy <= 2 && !foundInActiveGrid; ++dy)
                        {
                            for (int32_t dx = -2; dx <= 2; ++dx)
                            {
                                const int32_t gx = centerGrid.X + dx;
                                const int32_t gy = centerGrid.Y + dy;
                                if (pModMgr->GetCellFromCoordinates(gx, gy, pLocalWs, false) == pParentCell)
                                {
                                    aInterpolationComponent.TrackedParentGridX = gx;
                                    aInterpolationComponent.TrackedParentGridY = gy;
                                    aInterpolationComponent.HasTrackedParentGrid = true;
                                    foundInActiveGrid = true;
                                    break;
                                }
                            }
                        }

                        if (!foundInActiveGrid)
                        {
                            // `parentCell` is already outside the active 5x5 grid: mark ring distance >= 3 to re-home now.
                            aInterpolationComponent.TrackedParentGridX = centerGrid.X + 3;
                            aInterpolationComponent.TrackedParentGridY = centerGrid.Y + 3;
                            aInterpolationComponent.HasTrackedParentGrid = true;
                        }
                    }
                }

                const auto motionMode = CoopDamagePipeline::EvaluateExteriorCellMotion(
                    stepDistSq > kTeleportSnapThresholdSq,
                    hasAttachedParentCell,
                    hasLoadedDestCell,
                    pParentCell == pDestCell,
                    aInterpolationComponent.TrackedParentGridX,
                    aInterpolationComponent.TrackedParentGridY,
                    centerGrid.X,
                    centerGrid.Y,
                    position.x,
                    position.y,
                    aTick,
                    aInterpolationComponent.LastSeamlessRehomeTick);

                if (motionMode == CoopDamagePipeline::ExteriorCellMotionMode::kTeleportMoveTo)
                {
                    apActor->MoveTo(pDestCell, position);
                    aInterpolationComponent.TrackedParentCell = pDestCell;
                    aInterpolationComponent.TrackedParentGridX = destGrid.X;
                    aInterpolationComponent.TrackedParentGridY = destGrid.Y;
                    aInterpolationComponent.HasTrackedParentGrid = true;
                    aInterpolationComponent.LastSeamlessRehomeTick = aTick;
                    movedViaCellTransfer = true;
                }
                else if (motionMode == CoopDamagePipeline::ExteriorCellMotionMode::kSeamlessCellRehome)
                {
                    apActor->MoveToCellSeamless(pDestCell, position);
                    aInterpolationComponent.TrackedParentCell = pDestCell;
                    aInterpolationComponent.TrackedParentGridX = destGrid.X;
                    aInterpolationComponent.TrackedParentGridY = destGrid.Y;
                    aInterpolationComponent.HasTrackedParentGrid = true;
                    aInterpolationComponent.LastSeamlessRehomeTick = aTick;
                    movedViaCellTransfer = true;
                }
            }
        }
    }

    if (!movedViaCellTransfer)
        apActor->ForcePosition(position);

    // Causal + behavior-aware animation variable blending (v3.0 Phase 4C):
    // - Discrete Booleans, Integers (`SCAR_AttackVariants`, `iStep`, `DodgeID`), and Step Floats
    //   (`weaponSpeedMult`, `weapAdj`, `TUDM` weapon flags & i-frame windows, `playbackSpeed`) come from
    //   `LastRanAction` when an `ActionEvent` ran after `first.Tick`, otherwise from causal `first.Variables`.
    // - Circular [0, 360) angles (`Direction`) blend along shortest arc (`358 -> 2` goes through `0`, never `180`).
    // - Continuous locomotion/aiming floats (`SpeedSampled`, `TurnDelta`, `Pitch`, `TDM_Velocity*`) blend linearly.
    const bool useActionDiscreteVars =
        apAnimComponent &&
        CoopAnimationState::ShouldPreferActionDiscreteVariables(
            apAnimComponent->LastRanAction.Tick,
            first.Tick,
            apAnimComponent->LastRanAction.Variables.Booleans.size(),
            apAnimComponent->LastRanAction.Variables.Integers.size(),
            apAnimComponent->LastRanAction.Variables.Floats.size());

    const AnimationGraphDescriptor* pDescriptor = nullptr;
    if (const auto* pExt = apActor->GetExtension(); pExt && pExt->GraphDescriptorHash != 0)
        pDescriptor = AnimationGraphDescriptorManager::Get().GetDescriptor(pExt->GraphDescriptorHash);

    AnimationVariables blendedVars = useActionDiscreteVars ? apAnimComponent->LastRanAction.Variables : first.Variables;
    if (first.Variables.Floats.size() == second.Variables.Floats.size() && !first.Variables.Floats.empty())
    {
        blendedVars.Floats.resize(first.Variables.Floats.size());
        for (size_t i = 0; i < first.Variables.Floats.size(); ++i)
        {
            const float a = first.Variables.Floats[i];
            const float b = second.Variables.Floats[i];
            const bool hasActionFloat =
                useActionDiscreteVars && i < apAnimComponent->LastRanAction.Variables.Floats.size();
            const float actionFloat =
                hasActionFloat ? apAnimComponent->LastRanAction.Variables.Floats[i] : a;
            const auto blendKind = pDescriptor
                ? static_cast<CoopAnimationState::BehaviorFloatBlendKind>(pDescriptor->GetFloatBlendMode(i))
                : CoopAnimationState::BehaviorFloatBlendKind::kContinuousLinear;

            blendedVars.Floats[i] = CoopAnimationState::BlendAnimationFloatValue(
                a,
                b,
                actionFloat,
                hasActionFloat,
                delta,
                blendKind);
        }
    }
    apActor->LoadAnimationVariables(blendedVars);

    if (apActor->currentProcess && apActor->currentProcess->middleProcess)
    {
        apActor->currentProcess->middleProcess->direction = first.Direction + TiltedPhoques::DeltaAngle(first.Direction, second.Direction, false) * delta;
    }

    auto rotA = first.Rotation;
    auto rotB = second.Rotation;

    const auto deltaX = TiltedPhoques::DeltaAngle(rotA.x, rotB.x, true) * delta;
    const auto deltaY = TiltedPhoques::DeltaAngle(rotA.y, rotB.y, true) * delta;
    const auto deltaZ = TiltedPhoques::DeltaAngle(rotA.z, rotB.z, true) * delta;

    auto finalX = TiltedPhoques::Mod(rotA.x + deltaX, float(TiltedPhoques::Pi * 2));
    if (finalX > 0.f && finalX > float(TiltedPhoques::Pi / 2))
        finalX -= TiltedPhoques::Pi * 2;

    const auto finalY = TiltedPhoques::Mod(rotA.y + deltaY, float(TiltedPhoques::Pi * 2));
    const auto finalZ = TiltedPhoques::Mod(rotA.z + deltaZ, float(TiltedPhoques::Pi * 2));

    apActor->SetRotation(finalX, finalY, finalZ);
}

void InterpolationSystem::AddPoint(InterpolationComponent& aInterpolationComponent, const InterpolationComponent::TimePoint& acPoint) noexcept
{
    auto itor = std::begin(aInterpolationComponent.TimePoints);
    const auto end = std::cend(aInterpolationComponent.TimePoints);

    while (itor != end)
    {
        if (itor->Tick > acPoint.Tick)
        {
            aInterpolationComponent.TimePoints.insert(itor, acPoint);

            return;
        }

        ++itor;
    }

    aInterpolationComponent.TimePoints.push_back(acPoint);
}

InterpolationComponent& InterpolationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    return aWorld.emplace_or_replace<InterpolationComponent>(aEntity);
}

void InterpolationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<InterpolationComponent>(aEntity))
        aWorld.remove<InterpolationComponent>(aEntity);
}
