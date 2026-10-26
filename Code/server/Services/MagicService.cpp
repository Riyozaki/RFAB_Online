#include <Services/MagicService.h>

#include <Components.h>
#include <GameServer.h>
#include <World.h>
#include <common/CoopPacketSafety.h>
#include <algorithm>
#include <cmath>

#include <Messages/SpellCastRequest.h>
#include <Messages/InterruptCastRequest.h>
#include <Messages/AddTargetRequest.h>

#include <Messages/NotifySpellCast.h>
#include <Messages/NotifyInterruptCast.h>
#include <Messages/NotifyAddTarget.h>
#include <Messages/NotifyRemoveSpell.h>

MagicService::MagicService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_spellCastConnection = aDispatcher.sink<PacketEvent<SpellCastRequest>>().connect<&MagicService::OnSpellCastRequest>(this);
    m_interruptCastConnection = aDispatcher.sink<PacketEvent<InterruptCastRequest>>().connect<&MagicService::OnInterruptCastRequest>(this);
    m_addTargetConnection = aDispatcher.sink<PacketEvent<AddTargetRequest>>().connect<&MagicService::OnAddTargetRequest>(this);
    m_removeSpellConnection = aDispatcher.sink<PacketEvent<RemoveSpellRequest>>().connect<&MagicService::OnRemoveSpellRequest>(this);
}

void MagicService::OnSpellCastRequest(const PacketEvent<SpellCastRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto entity = static_cast<entt::entity>(message.CasterId);
    if (!m_world.valid(entity))
        return;

    const auto* pOwner = m_world.try_get<OwnerComponent>(entity);
    if (!pOwner || pOwner->GetOwner() != acMessage.pPlayer)
        return;

    NotifySpellCast notify{};
    notify.CasterId = message.CasterId;
    notify.SpellFormId = message.SpellFormId;
    notify.CastingSource = message.CastingSource;
    notify.IsDualCasting = message.IsDualCasting;
    notify.DesiredTarget = message.DesiredTarget;
    notify.EffectivenessMult = CoopPacketSafety::SanitizeFiniteFloat(message.EffectivenessMult, 0.05f, 10.0f, 1.0f);

    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnInterruptCastRequest(const PacketEvent<InterruptCastRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto entity = static_cast<entt::entity>(message.CasterId);
    if (!m_world.valid(entity))
        return;

    const auto* pOwner = m_world.try_get<OwnerComponent>(entity);
    if (!pOwner || pOwner->GetOwner() != acMessage.pPlayer)
        return;

    NotifyInterruptCast notify{};
    notify.CasterId = message.CasterId;
    notify.CastingSource = message.CastingSource;

    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnAddTargetRequest(const PacketEvent<AddTargetRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const auto entity = static_cast<entt::entity>(message.TargetId);
    if (!m_world.valid(entity) || !acMessage.pPlayer)
        return;

    if (!std::isfinite(message.Magnitude) || message.SpellId == GameId{} || message.EffectId == GameId{})
        return;

    const auto* pTargetChar = m_world.try_get<CharacterComponent>(entity);
    if (pTargetChar && !pTargetChar->IsPlayer() && pTargetChar->IsDead())
        return;

    if (const auto* pTargetCell = m_world.try_get<CellIdComponent>(entity))
    {
        const bool isDragon = pTargetChar && pTargetChar->IsDragon();
        if (!pTargetCell->IsInRange(acMessage.pPlayer->GetCellComponent(), isDragon))
            return;
    }

    const auto* pTargetOwner = m_world.try_get<OwnerComponent>(entity);
    bool senderAuthorized = (pTargetOwner && pTargetOwner->GetOwner() == acMessage.pPlayer);

    uint32_t sanitizedCasterId = message.CasterId;
    if (sanitizedCasterId != 0)
    {
        const auto casterEntity = static_cast<entt::entity>(sanitizedCasterId);
        if (!m_world.valid(casterEntity))
        {
            sanitizedCasterId = 0;
        }
        else if (const auto* pCasterOwner = m_world.try_get<OwnerComponent>(casterEntity))
        {
            if (pCasterOwner->GetOwner() == acMessage.pPlayer)
                senderAuthorized = true;
        }
    }

    if (!senderAuthorized)
        return;

    NotifyAddTarget notify{};
    notify.TargetId = message.TargetId;
    notify.CasterId = sanitizedCasterId;
    notify.SpellId = message.SpellId;
    notify.EffectId = message.EffectId;
    notify.Magnitude = CoopPacketSafety::SanitizeFiniteFloat(message.Magnitude, -100000.0f, 100000.0f, 0.0f);
    notify.IsDualCasting = message.IsDualCasting;
    notify.ApplyHealPerkBonus = message.ApplyHealPerkBonus;
    notify.ApplyStaminaPerkBonus = message.ApplyStaminaPerkBonus;

    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void MagicService::OnRemoveSpellRequest(const PacketEvent<RemoveSpellRequest>& acMessage) const noexcept
{
    const auto& message = acMessage.Packet;
    const auto entity = static_cast<entt::entity>(message.TargetId);
    if (!m_world.valid(entity))
        return;

    const auto* pOwner = m_world.try_get<OwnerComponent>(entity);
    if (!pOwner || pOwner->GetOwner() != acMessage.pPlayer)
        return;

    NotifyRemoveSpell notify{};
    notify.TargetId = message.TargetId;
    notify.SpellId = message.SpellId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, entity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
