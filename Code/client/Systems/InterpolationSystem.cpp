#include <TiltedOnlinePCH.h>

#include <Systems/InterpolationSystem.h>
#include <Components.h>

#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Games/References.h>
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

    apActor->ForcePosition(position);

    // Causal + smooth animation variable blending:
    // - Use discrete Booleans & Integers from LastRanAction when an ActionEvent ran more recently than first.Tick
    //   (preventing 100ms snapshots from clobbering newly-fired ForceAction combo/attack/dodge/bleedout states!),
    //   otherwise from causal first.Variables (never future second.Variables!).
    // - Smoothly blend continuous Floats (SpeedSampled, Direction, TurnDelta) between first and second.
    const bool useActionDiscreteVars =
        apAnimComponent && apAnimComponent->LastRanAction.Tick > first.Tick &&
        !apAnimComponent->LastRanAction.Variables.Booleans.empty();

    AnimationVariables blendedVars = useActionDiscreteVars ? apAnimComponent->LastRanAction.Variables : first.Variables;
    if (first.Variables.Floats.size() == second.Variables.Floats.size() && !first.Variables.Floats.empty())
    {
        blendedVars.Floats.resize(first.Variables.Floats.size());
        for (size_t i = 0; i < first.Variables.Floats.size(); ++i)
        {
            const float a = first.Variables.Floats[i];
            const float b = second.Variables.Floats[i];
            if (std::isfinite(a) && std::isfinite(b) && std::fabs(b - a) < 500.0f)
                blendedVars.Floats[i] = a + (b - a) * delta;
            else
                blendedVars.Floats[i] = b;
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
