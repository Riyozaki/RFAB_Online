#include <Services/CharacterService.h>

#include <Components.h>
#include <Game/OwnerView.h>
#include <GameServer.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/DialogueRequest.h>
#include <Messages/MountRequest.h>
#include <Messages/NewPackageRequest.h>
#include <Messages/NotifyDialogue.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyMount.h>
#include <Messages/NotifyNewPackage.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyRespawn.h>
#include <Messages/NotifySubtitle.h>
#include <Messages/NotifySyncExperience.h>
#include <Messages/NotifySyncRfabExperience.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/RequestRespawn.h>
#include <Messages/SubtitleRequest.h>
#include <Messages/SyncExperienceRequest.h>
#include <Messages/SyncRfabExperienceRequest.h>
#include <Services/InventoryService.h>
#include <Setting.h>
#include <World.h>
#include <common/CoopExperienceLedger.h>
#include <common/CoopPacketSafety.h>

#include <algorithm>
#include <chrono>

namespace
{
Console::Setting bEnableXpSync{"Gameplay:bEnableXpSync", "Syncs combat XP within the party", true};
Console::Setting bEnableRfabXpSync{"Gameplay:bEnableRfabXpSync", "Syncs RFAB XP gains within the party", true};
} // namespace

void CharacterService::OnFactionsChanges(const PacketEvent<RequestFactionsChanges>& acMessage) const noexcept
{
    OwnerView<CharacterComponent> view(m_world, acMessage.GetSender());

    auto& message = acMessage.Packet;
    for (auto& [id, factions] : message.Changes)
    {
        auto it = view.find(static_cast<entt::entity>(id));
        if (it == std::end(view) || view.get<OwnerComponent>(*it).GetOwner() != acMessage.pPlayer)
            continue;

        auto& characterComponent = view.get<CharacterComponent>(*it);
        characterComponent.FactionsContent = factions;
        characterComponent.SetDirtyFactions(true);
    }
}

void CharacterService::OnMountRequest(const PacketEvent<MountRequest>& acMessage) const noexcept
{
    if (!acMessage.pPlayer)
        return;

    auto& message = acMessage.Packet;
    const entt::entity riderEntity = static_cast<entt::entity>(message.RiderId);
    const entt::entity mountEntity = static_cast<entt::entity>(message.MountId);
    if (!m_world.valid(riderEntity) || !m_world.valid(mountEntity))
        return;

    // Verify that the sender actually owns the rider entity, then atomically transfer mount ownership
    // to the rider so horse movement updates belong to the player physically riding the mount!
    const auto* pRiderOwner = m_world.try_get<OwnerComponent>(riderEntity);
    if (pRiderOwner && pRiderOwner->GetOwner() != acMessage.pPlayer)
        return;

    if (auto* pMountOwner = m_world.try_get<OwnerComponent>(mountEntity))
    {
        const auto* pMountChar = m_world.try_get<CharacterComponent>(mountEntity);
        if (pMountChar && !pMountChar->IsPlayer() && pMountOwner->GetOwner() != acMessage.pPlayer)
        {
            const ActorData mountData = BuildActorData(mountEntity);
            TransferOwnership(acMessage.pPlayer, message.MountId, mountData);

            NotifyOwnershipTransfer grant{};
            grant.ServerId = message.MountId;
            grant.OwnershipEpoch = pMountOwner->Epoch;
            acMessage.pPlayer->Send(grant);
        }
    }

    NotifyMount notify;
    notify.RiderId = message.RiderId;
    notify.MountId = message.MountId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, mountEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnNewPackageRequest(const PacketEvent<NewPackageRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ActorId);
    if (!m_world.valid(cEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
            return;
    }

    NotifyNewPackage notify;
    notify.ActorId = message.ActorId;
    notify.PackageId = message.PackageId;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnRequestRespawn(const PacketEvent<RequestRespawn>& acMessage) const noexcept
{
    auto view = m_world.view<OwnerComponent, CharacterComponent>();
    auto it = view.find(static_cast<entt::entity>(acMessage.Packet.ActorId));
    if (it == view.end())
    {
        spdlog::warn("No OwnerComponent found for actor id {:X}", acMessage.Packet.ActorId);
        return;
    }

    auto& ownerComponent = view.get<OwnerComponent>(*it);
    if (ownerComponent.GetOwner() == acMessage.pPlayer)
    {
        m_world.GetInventoryService().ForgetContainerForEntity(*it, acMessage.Packet.ActorId);

        if (auto* pAnimationComponent = m_world.try_get<AnimationComponent>(*it))
            pAnimationComponent->ActionsReplayCache.Clear();

        if (!acMessage.Packet.AppearanceBuffer.empty())
        {
            auto& characterComponent = view.get<CharacterComponent>(*it);
            characterComponent.SaveBuffer = acMessage.Packet.AppearanceBuffer;
            characterComponent.ChangeFlags = acMessage.Packet.ChangeFlags;
        }

        NotifyRespawn notify;
        notify.ActorId = acMessage.Packet.ActorId;

        if (!GameServer::Get()->SendToPlayersInRange(notify, *it, acMessage.GetSender()))
            spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
    }
    else if (acMessage.GetSender())
    {
        CharacterSpawnRequest message{};
        Serialize(m_world, *it, &message);
        acMessage.GetSender()->Send(message);
    }
}

void CharacterService::OnSyncExperienceRequest(const PacketEvent<SyncExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableXpSync)
        return;

    const float sanitizedXp = CoopPacketSafety::SanitizeFiniteFloat(acMessage.Packet.Experience, 0.0f, 1'000'000.0f, 0.0f);
    if (sanitizedXp <= 0.0f)
        return;

    NotifySyncExperience notify{};
    notify.Experience = sanitizedXp;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (partyComponent.JoinedPartyId.has_value())
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    else
        GameServer::Get()->SendToPlayers(notify, acMessage.GetSender());
}

void CharacterService::OnSyncRfabExperienceRequest(const PacketEvent<SyncRfabExperienceRequest>& acMessage) const noexcept
{
    if (!bEnableRfabXpSync)
        return;

    const uint32_t experience = std::min(acMessage.Packet.Experience, CoopExperienceLedger::kMaxRfabXpDelta);
    if (experience == 0)
        return;

    NotifySyncRfabExperience notify;
    notify.Experience = experience;

    const auto& partyComponent = acMessage.pPlayer->GetParty();
    if (partyComponent.JoinedPartyId.has_value())
        GameServer::Get()->SendToParty(notify, partyComponent, acMessage.GetSender());
    else
        GameServer::Get()->SendToPlayers(notify, acMessage.GetSender());
}

void CharacterService::OnDialogueRequest(const PacketEvent<DialogueRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
            return;
    }

    NotifyDialogue notify{};
    notify.ServerId = message.ServerId;
    notify.SoundFilename = message.SoundFilename;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::OnSubtitleRequest(const PacketEvent<SubtitleRequest>& acMessage) const noexcept
{
    auto& message = acMessage.Packet;
    const entt::entity cEntity = static_cast<entt::entity>(message.ServerId);
    if (!m_world.valid(cEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
            return;
    }

    NotifySubtitle notify{};
    notify.ServerId = message.ServerId;
    notify.Text = message.Text;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}

void CharacterService::ProcessFactionsChanges() const noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenSnapshots = 2000ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenSnapshots)
        return;

    lastSendTimePoint = now;

    const auto characterView = m_world.view<CellIdComponent, CharacterComponent, OwnerComponent>();
    TiltedPhoques::Map<Player*, NotifyFactionsChanges> messages;

    for (auto entity : characterView)
    {
        auto& characterComponent = characterView.get<CharacterComponent>(entity);
        auto& cellIdComponent = characterView.get<CellIdComponent>(entity);
        auto& ownerComponent = characterView.get<OwnerComponent>(entity);

        if (!characterComponent.IsDirtyFactions())
            continue;

        for (auto pPlayer : m_world.GetPlayerManager())
        {
            if (pPlayer == ownerComponent.GetOwner())
                continue;

            if (!cellIdComponent.IsInRange(pPlayer->GetCellComponent(), characterComponent.IsDragon()))
                continue;

            auto& message = messages[pPlayer];
            message.Changes[World::ToInteger(entity)] = characterComponent.FactionsContent;
        }

        characterComponent.SetDirtyFactions(false);
    }

    for (auto [pPlayer, message] : messages)
    {
        if (!message.Changes.empty())
            pPlayer->Send(message);
    }
}
