#include "ActionReplayCache.h"
#include <CoopAnimationState.h>
#include <Game/Animation/AnimationEventLists.h>
#include <optional>
#include <vector>

ActionReplayChain ActionReplayCache::FormRefinedReplayChain() noexcept
{
    const bool shouldResetAnimGraph = RefineReplayCache();

    ActionReplayChain chain;
    chain.ResetAnimationGraph = shouldResetAnimGraph;
    chain.Actions = (shouldResetAnimGraph && !m_actions.empty())
        ? Vector<ActionEvent>{m_actions.begin() + 1, m_actions.end()} // Drop the exit action
        : m_actions;

    return chain;
}

void ActionReplayCache::AppendAll(const Vector<ActionEvent>& acActions) noexcept
{
    for (const auto& action : acActions)
    {
        if (ShouldIgnoreAction(action))
            continue;
        m_actions.push_back(action);
    }

    if (m_actions.size() > kReplayCacheMaxSize)
        m_actions.erase(m_actions.begin(), m_actions.end() - kReplayCacheMaxSize);
}

bool ActionReplayCache::RefineReplayCache() noexcept
{
    int32_t dropAllUpToIndex = -1;

    for (int32_t i = static_cast<int32_t>(m_actions.size()) - 1; i >= 0; --i)
    {
        ActionEvent& action = m_actions[i];

        // Instant counterparts of actions are highly preferred for animation replay
        if (auto instantAnimName = FindInstantCounterpartForAction(action.EventName))
        {
            action.EventName = String{*instantAnimName};
            action.TargetEventName = String{*instantAnimName};
            action.ActionId = 0;
            action.IdleId = 0;
        }

        if (IsExitAction(action))
        {
            // An "exit" action is found, meaning the actor has likely returned to its root/normal
            // state, and the clients should reset the animation state before running replay
            dropAllUpToIndex = i;
            break;
        }
    }

    if (dropAllUpToIndex != -1)
    {
        m_actions.erase(m_actions.begin(), m_actions.begin() + dropAllUpToIndex);
        return true;
    }

    // Even if Skyrim's combat AI interrupted a sitting/sleeping package without emitting a named exit event,
    // State1/State2 transitions (SitSleepState != 0 -> SitSleepState == 0 with drawn weapon / melee attack)
    // prove the actor stood up: drop all obsolete furniture actions prior to the first standing combat action
    // and insert an IdleForceDefaultState sentinel at index 0 (consumed as ResetAnimationGraph = true).
    std::vector<CoopAnimationState::ReplayEntryView> views;
    views.reserve(m_actions.size());
    for (const auto& action : m_actions)
        views.push_back({action.EventName, action.State1, action.State2});

    const int32_t standingCombatIdx = CoopAnimationState::FindImplicitFurnitureResetIndex(views);
    if (standingCombatIdx > 0)
    {
        ActionEvent sentinel = m_actions[standingCombatIdx];
        sentinel.EventName = "IdleForceDefaultState";
        sentinel.TargetEventName = "";
        sentinel.ActionId = 0;
        sentinel.IdleId = 0;
        m_actions.erase(m_actions.begin(), m_actions.begin() + standingCombatIdx);
        m_actions.insert(m_actions.begin(), std::move(sentinel));
        return true;
    }

    return false;
}

bool ActionReplayCache::IsExitAction(const ActionEvent& acAction) noexcept
{
    if (AnimationEventLists::kExitSpecial.contains(acAction.EventName))
        return true;

    return CoopAnimationState::IsFurnitureOrIdleExitEvent(acAction.EventName);
}

bool ActionReplayCache::ShouldIgnoreAction(const ActionEvent& acAction) noexcept
{
    return AnimationEventLists::kIgnore.contains(acAction.EventName);
}

std::optional<std::string_view> ActionReplayCache::FindInstantCounterpartForAction(std::string_view aAction) noexcept
{
    auto it = AnimationEventLists::kIdleToInstant.find(aAction);
    if (it == AnimationEventLists::kIdleToInstant.end())
        return std::nullopt;

    auto& [_, instantAnimationName] = *it;
    if (!instantAnimationName.empty())
    {
        return {instantAnimationName};
    }
    return std::nullopt;
}

void ActionReplayCache::Clear() noexcept
{
    m_actions.clear();
}
