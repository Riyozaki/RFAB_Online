#include <cmath>

#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/CharacterSpawnedEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Gameplay/ActorValueIds.h>
#include <Gameplay/HealthScaling.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyHealthChangeBroadcast.h>

#include <Services/HealthScalingService.h>

namespace
{
Console::Setting bEnableHealthScaling{"Gameplay:bEnableHealthScaling",
                                      "Scale NPC max health by (players + 1): 2 players -> x3, 3 players -> x4. "
                                      "The number of enemies is never changed.",
                                      true};

Console::Setting bScalePlayerSummons{"Gameplay:bScalePlayerSummons",
                                     "Also scale the max health of player summons (atronachs, familiars, ...)",
                                     false};

// Owned by GameServer.cpp, shipped to clients inside ServerSettings.
extern Console::Setting<float> fHealthMultiplier;

using namespace Gameplay;

//! Above this many tracked forms the cache is dropped: actors that are met again
//! afterwards simply recapture their base (and the session is already huge).
constexpr size_t kMaxCachedForms = 16384;

float ReadHealth(const TiltedPhoques::Map<uint32_t, float>& acValues) noexcept
{
    const auto it = acValues.find(ActorValue::kHealth);
    return it != acValues.end() ? it->second : 0.0f;
}

void WriteHealth(TiltedPhoques::Map<uint32_t, float>& aValues, float aValue) noexcept
{
    const auto it = aValues.find(ActorValue::kHealth);
    if (it != aValues.end())
        it->second = aValue;
    else
        aValues.insert({ActorValue::kHealth, aValue});
}
} // namespace

HealthScalingService::HealthScalingService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&HealthScalingService::OnUpdate>(this);
    m_playerJoinConnection = aDispatcher.sink<PlayerJoinEvent>().connect<&HealthScalingService::OnPlayerJoin>(this);
    m_playerLeaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&HealthScalingService::OnPlayerLeave>(this);
    m_characterSpawnedConnection = aDispatcher.sink<CharacterSpawnedEvent>().connect<&HealthScalingService::OnCharacterSpawned>(this);
}

void HealthScalingService::OnUpdate(const UpdateEvent&) noexcept
{
    const uint32_t playerCount = m_world.GetPlayerManager().Count();

    // The desired multiplier also changes when the setting is toggled at
    // runtime, not only when the player count changes, so both are compared.
    const float desiredMultiplier = bEnableHealthScaling.value_as<bool>() ? HealthScaling::MultiplierFor(playerCount) : 1.0f;
    const bool multiplierChanged = std::fabs(desiredMultiplier - m_multiplier) > 0.0001f;

    // Slow revalidation: an actor that was dead during the last recompute (and
    // therefore skipped) is picked up after it respawns, and any value that a
    // client changed behind our back is pulled back to the scaled one.
    const auto now = std::chrono::steady_clock::now();
    const bool validate = (now - m_lastValidation) >= kValidationInterval;
    if (validate)
        m_lastValidation = now;

    if (!m_dirty && playerCount == m_lastPlayerCount && !multiplierChanged && !validate)
        return;

    if (playerCount == 0 && m_lastPlayerCount != 0)
    {
        // Nobody left: the next player starts a fresh world, and any base value
        // captured during this one is meaningless.
        m_baseHealthByForm.clear();
    }

    m_lastPlayerCount = playerCount;
    m_dirty = false;

    Recompute();
}

void HealthScalingService::OnPlayerJoin(const PlayerJoinEvent&) noexcept
{
    // The effective count is read on the next tick; a join that never completes
    // therefore cannot inflate the multiplier.
    m_dirty = true;
}

void HealthScalingService::OnPlayerLeave(const PlayerLeaveEvent&) noexcept
{
    m_dirty = true;
}

void HealthScalingService::OnCharacterSpawned(const CharacterSpawnedEvent& acEvent) noexcept
{
    // New actors must get the current multiplier even if the player count did
    // not change since the last recompute. Only the multiplier is refreshed
    // here - iterating every character on each spawn would be quadratic.
    RefreshMultiplier();
    ApplyToCharacter(acEvent.Entity);
}

float HealthScalingService::GetOrCaptureBaseHealth(entt::entity aEntity, float aCurrentMax) noexcept
{
    const auto* pFormId = m_world.try_get<FormIdComponent>(aEntity);
    if (pFormId == nullptr)
    {
        // Temporary forms have no stable identity, so the entity is the only
        // thing we can key on; that is fine, they do not survive a cell unload.
        auto* pActorValues = m_world.try_get<ActorValuesComponent>(aEntity);
        if (pActorValues == nullptr)
            return aCurrentMax;

        if (pActorValues->BaseMaxHealth <= 0.0f)
            pActorValues->BaseMaxHealth = aCurrentMax;

        return pActorValues->BaseMaxHealth;
    }

    const uint64_t key = pFormId->Id.LogFormat();

    const auto it = m_baseHealthByForm.find(key);
    if (it != m_baseHealthByForm.end() && it->second > 0.0f)
        return it->second;

    if (m_baseHealthByForm.size() >= kMaxCachedForms)
    {
        spdlog::warn("Health scaling: base health cache is full ({} forms), clearing it", m_baseHealthByForm.size());
        m_baseHealthByForm.clear();
    }

    m_baseHealthByForm.insert_or_assign(key, aCurrentMax);
    return aCurrentMax;
}

void HealthScalingService::RefreshMultiplier() noexcept
{
    const float multiplier = bEnableHealthScaling.value_as<bool>() ? HealthScaling::MultiplierFor(m_lastPlayerCount) : 1.0f;

    if (std::fabs(multiplier - m_multiplier) <= 0.0001f)
        return;

    m_multiplier = multiplier;
    PublishMultiplier(multiplier);
}

void HealthScalingService::Recompute() noexcept
{
    RefreshMultiplier();

    auto view = m_world.view<ActorValuesComponent, CharacterComponent>();
    for (const auto entity : view)
        ApplyToCharacter(entity);
}

void HealthScalingService::ApplyToCharacter(entt::entity aEntity) noexcept
{
    auto* pActorValues = m_world.try_get<ActorValuesComponent>(aEntity);
    const auto* pCharacter = m_world.try_get<CharacterComponent>(aEntity);
    if (pActorValues == nullptr || pCharacter == nullptr)
        return;

    auto& values = pActorValues->CurrentActorValues.ActorValuesList;
    auto& maxValues = pActorValues->CurrentActorValues.ActorMaxValuesList;

    const auto maxIt = maxValues.find(ActorValue::kHealth);
    if (maxIt == maxValues.end())
        return;

    // Capture the unscaled maximum once, before any scaling was applied, and
    // remember it per form so that a re-created entity reuses the real base.
    pActorValues->BaseMaxHealth = GetOrCaptureBaseHealth(aEntity, maxIt->second);

    HealthScaling::ActorFlags flags{};
    flags.IsPlayer = pCharacter->IsPlayer();
    flags.IsPlayerSummon = pCharacter->IsPlayerSummon();
    flags.IsMount = pCharacter->IsMount();
    flags.IsDragon = pCharacter->IsDragon();
    flags.IsDead = pCharacter->IsDead();

    HealthScaling::Options options{};
    options.ScaleSummons = bScalePlayerSummons.value_as<bool>();

    // Never touch a dead actor: scaling must not resurrect anything, and the
    // death state is already synchronised to every client (ShouldScale covers it).
    if (!HealthScaling::ShouldScale(flags, options))
        return;

    const float oldMax = maxIt->second;
    const float oldCurrent = ReadHealth(values);

    // A freshly spawned actor may still be at 0 health for one frame; leave it
    // alone until the owning client reports something sane.
    if (oldCurrent <= 0.0f && oldMax > 0.0f)
        return;

    float newMax = oldMax;
    float newCurrent = oldCurrent;

    if (!HealthScaling::ComputeTarget(pActorValues->BaseMaxHealth, oldCurrent, oldMax, m_multiplier, newMax, newCurrent))
        return; // already at the right maximum (float tolerant)

    maxIt->second = newMax;
    WriteHealth(values, newCurrent);

    const uint32_t serverId = World::ToInteger(aEntity);

    NotifyActorMaxValueChanges maxNotify{};
    maxNotify.Id = serverId;
    maxNotify.Values.insert({ActorValue::kHealth, newMax});

    NotifyHealthChangeBroadcast healthNotify{};
    healthNotify.Id = serverId;
    // The health message carries a delta; receivers apply `current + delta`
    // (same convention as damage, see the server ActorValueService fix).
    healthNotify.DeltaHealth = newCurrent - oldCurrent;

    if (!GameServer::Get()->SendToPlayersInRange(maxNotify, aEntity, nullptr))
        spdlog::debug("{}: nobody in range for actor {:X}", __FUNCTION__, serverId);

    if (healthNotify.DeltaHealth != 0.0f)
        GameServer::Get()->SendToPlayersInRange(healthNotify, aEntity, nullptr);
}

void HealthScalingService::PublishMultiplier(float aMultiplier) noexcept
{
    fHealthMultiplier = aMultiplier;

    spdlog::info("Health scaling: {} player(s) -> NPC health x{:.2f}", m_lastPlayerCount, aMultiplier);

    if (auto* pServer = GameServer::Get())
        pServer->UpdateSettings();
}
