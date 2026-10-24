#include <TiltedOnlinePCH.h>

#include <Systems/AnimationSystem.h>

#include <Games/Animation/TESActionData.h>
#include <Games/Animation/ActorMediator.h>

#include <Games/References.h>

#include <Forms/BGSAction.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>

#include <Messages/ClientReferencesMoveRequest.h>

#include <Components.h>
#include <Utils.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>

extern thread_local const char* g_animErrorCode;

namespace
{
constexpr uint32_t kEncodedActorServerIdTag = 0xFF800000u;
constexpr uint32_t kEncodedActorServerIdMask = 0x007FFFFFu;
}

void AnimationSystem::Update(World& aWorld, Actor* apActor, RemoteAnimationComponent& aAnimationComponent, const uint64_t aTick) noexcept
{
    auto& actions = aAnimationComponent.TimePoints;
    if (actions.empty())
        return;

    // Check if animation graph is ready before attempting to play animations
    if (!apActor || !apActor->animationGraphHolder.IsReady())
        return;

    // Drop ancient backlog (>2s stale non-replay actions) if a lag burst queued >16 events, preventing freeze catch-up storms.
    while (actions.size() > 16 && aAnimationComponent.ReplayCount == 0 && aTick > 2000 && actions.front().Tick + 2000 < aTick)
    {
        actions.pop_front();
    }

    // Drain up to 8 causal actions per frame in order so multi-event Skyrim/RFAB combat transitions
    // (e.g. ActionDraw + weaponDraw, ActionRightAttack + attackStart + BashStart) fire together cleanly!
    uint32_t processedThisFrame = 0;
    while (!actions.empty() && actions.front().Tick <= aTick && processedThisFrame < 8)
    {
        if (aAnimationComponent.ReplayCount > 0 && aAnimationComponent.ResetAnimationGraphForReplay)
        {
            apActor->animationGraphHolder.RevertAnimationGraphManager();
            aAnimationComponent.ResetAnimationGraphForReplay = false;
        }

        const auto first = actions.front();
        actions.pop_front();
        ++processedThisFrame;

        const auto actionId = first.ActionId;
        const auto targetId = first.TargetId;

        const auto pAction = Cast<BGSAction>(TESForm::GetById(actionId));
        TESObjectREFR* pTarget = nullptr;
        if ((targetId & kEncodedActorServerIdTag) == kEncodedActorServerIdTag)
        {
            pTarget = Utils::GetByServerId<TESObjectREFR>(targetId & kEncodedActorServerIdMask);
        }
        else if (targetId != 0 && targetId != 0x14 && (targetId & 0xFF000000u) != 0xFF000000u)
        {
            pTarget = Cast<TESObjectREFR>(TESForm::GetById(targetId));
        }

        apActor->actorState.flags1 = first.State1;
        apActor->actorState.flags2 = first.State2;

        apActor->LoadAnimationVariables(first.Variables);

        aAnimationComponent.LastRanAction = first;

        // Play the animation
        TESActionData actionData(first.Type & 0x3, apActor, pAction, pTarget);
        actionData.eventName = BSFixedString(first.EventName.c_str());
        actionData.idleForm = Cast<TESIdleForm>(TESForm::GetById(first.IdleId));
        actionData.someFlag = ((first.Type & 0x4) != 0) ? 1 : 0;

        ActorMediator::Get()->ForceAction(&actionData);

        if (aAnimationComponent.ReplayCount > 0)
            aAnimationComponent.ReplayCount--;
    }
}

void AnimationSystem::Setup(World& aWorld, const entt::entity aEntity) noexcept
{
    aWorld.emplace_or_replace<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::Clean(World& aWorld, const entt::entity aEntity) noexcept
{
    if (aWorld.all_of<RemoteAnimationComponent>(aEntity))
        aWorld.remove<RemoteAnimationComponent>(aEntity);
}

void AnimationSystem::AddActionsForReplay(RemoteAnimationComponent& aAnimationComponent,
                                          const ActionReplayChain& acReplay) noexcept
{
    aAnimationComponent.TimePoints.insert(aAnimationComponent.TimePoints.end(), acReplay.Actions.begin(),
                                          acReplay.Actions.end());
    aAnimationComponent.ReplayCount = acReplay.Actions.size();
    aAnimationComponent.ResetAnimationGraphForReplay = acReplay.ResetAnimationGraph;
}

void AnimationSystem::AddAction(RemoteAnimationComponent& aAnimationComponent, const std::string& acActionDiff) noexcept
{
    auto itor = std::begin(aAnimationComponent.TimePoints);
    const auto end = std::cend(aAnimationComponent.TimePoints);

    auto& lastProcessedAction = aAnimationComponent.LastProcessedAction;

    TiltedPhoques::ViewBuffer buffer((uint8_t*)acActionDiff.data(), acActionDiff.size());
    Buffer::Reader reader(&buffer);

    lastProcessedAction.ApplyDifferential(reader);

    aAnimationComponent.TimePoints.push_back(lastProcessedAction);
}

void AnimationSystem::Serialize(World& aWorld, ClientReferencesMoveRequest& aMovementSnapshot, LocalComponent& localComponent, LocalAnimationComponent& animationComponent, FormIdComponent& formIdComponent)
{
    const auto pForm = TESForm::GetById(formIdComponent.Id);
    const auto pActor = Cast<Actor>(pForm);
    if (!pActor)
        return;

    auto& update = aMovementSnapshot.Updates[localComponent.Id];
    auto& movement = update.UpdatedMovement;

    if (const auto pCell = pActor->parentCell)
        World::Get().GetModSystem().GetServerModId(pCell->formID, movement.CellId.ModId, movement.CellId.BaseId);

    if (const auto pWorldSpace = pActor->GetWorldSpace())
        World::Get().GetModSystem().GetServerModId(pWorldSpace->formID, movement.WorldSpaceId.ModId, movement.WorldSpaceId.BaseId);

    movement.Position = pActor->position;

    movement.Rotation.x = pActor->rotation.x;
    movement.Rotation.y = pActor->rotation.z;

    pActor->SaveAnimationVariables(movement.Variables);

    if (pActor->currentProcess && pActor->currentProcess->middleProcess)
    {
        movement.Direction = pActor->currentProcess->middleProcess->direction;
    }

    for (auto entry : animationComponent.Actions)
    {
        if (entry.TargetId != 0)
        {
            auto targetView = aWorld.view<FormIdComponent>();
            const auto targetIt = std::find_if(
                std::begin(targetView), std::end(targetView),
                [id = entry.TargetId, targetView](entt::entity e) { return targetView.get<FormIdComponent>(e).Id == id; });
            if (targetIt != std::end(targetView))
            {
                if (const auto targetServerId = Utils::GetServerId(*targetIt); targetServerId.has_value())
                    entry.TargetId = kEncodedActorServerIdTag | (targetServerId.value() & kEncodedActorServerIdMask);
                else if (entry.TargetId == 0x14 || (entry.TargetId & 0xFF000000u) == 0xFF000000u)
                    entry.TargetId = 0;
            }
            else if (entry.TargetId == 0x14 || (entry.TargetId & 0xFF000000u) == 0xFF000000u)
            {
                entry.TargetId = 0;
            }
        }
        update.ActionEvents.push_back(std::move(entry));
    }

    auto latestAction = animationComponent.GetLatestAction();

    if (latestAction)
        localComponent.CurrentAction = latestAction.MoveResult();

    animationComponent.Actions.clear();
}

bool AnimationSystem::Serialize(World& aWorld, const ActionEvent& aActionEvent, const ActionEvent& aLastProcessedAction, std::string* apData)
{
    uint32_t actionBaseId = 0;
    uint32_t actionModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.ActionId, actionModId, actionBaseId))
        return false;

    uint32_t targetBaseId = 0;
    uint32_t targetModId = 0;
    if (!aWorld.GetModSystem().GetServerModId(aActionEvent.TargetId, targetModId, targetBaseId))
        return false;

    uint8_t scratch[1 << 14];
    TiltedPhoques::ViewBuffer buffer(scratch, std::size(scratch));
    Buffer::Writer writer(&buffer);
    aActionEvent.GenerateDifferential(aLastProcessedAction, writer);

    apData->assign(buffer.GetData(), buffer.GetData() + writer.Size());

    return true;
}
