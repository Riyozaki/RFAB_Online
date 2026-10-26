#include <Services/CombatService.h>
#include <Services/ActorValueService.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Messages/ProjectileLaunchRequest.h>
#include <Messages/NotifyProjectileLaunch.h>
#include <common/RfabModpackCompat.h>

CombatService::CombatService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_projectileLaunchConnection = aDispatcher.sink<PacketEvent<ProjectileLaunchRequest>>().connect<&CombatService::OnProjectileLaunchRequest>(this);
}

void CombatService::OnProjectileLaunchRequest(const PacketEvent<ProjectileLaunchRequest>& acMessage) const noexcept
{
    auto& packet = acMessage.Packet;
    const auto cShooterEntity = static_cast<entt::entity>(packet.ShooterID);
    if (!m_world.valid(cShooterEntity))
        return;

    if (const auto* pOwner = m_world.try_get<OwnerComponent>(cShooterEntity))
    {
        if (pOwner->GetOwner() != acMessage.pPlayer)
        {
            spdlog::debug("Ignored non-owner ProjectileLaunchRequest for {:X}", packet.ShooterID);
            return;
        }
    }

    if (!RfabCompat::AreProjectileLaunchParamsSafe(
            packet.OriginX,
            packet.OriginY,
            packet.OriginZ,
            packet.XAngle,
            packet.YAngle,
            packet.ZAngle,
            packet.Power,
            packet.Scale))
    {
        return;
    }

    if (acMessage.pPlayer)
    {
        m_world.GetActorValueService().RecordCombatClaim(
            acMessage.pPlayer->GetId(),
            0u,
            CoopDamagePipeline::ServerHitClaimKind::kProjectileLaunch);
    }

    NotifyProjectileLaunch notify{};

    notify.ShooterID = packet.ShooterID;

    notify.OriginX = packet.OriginX;
    notify.OriginY = packet.OriginY;
    notify.OriginZ = packet.OriginZ;

    notify.ProjectileBaseID = packet.ProjectileBaseID;
    notify.WeaponID = packet.WeaponID;
    notify.AmmoID = packet.AmmoID;

    notify.ZAngle = packet.ZAngle;
    notify.XAngle = packet.XAngle;
    notify.YAngle = packet.YAngle;

    notify.ParentCellID = packet.ParentCellID;

    notify.SpellID = packet.SpellID;
    notify.CastingSource = packet.CastingSource;

    notify.Area = packet.Area;
    notify.Power = packet.Power;
    notify.Scale = packet.Scale;

    notify.AlwaysHit = packet.AlwaysHit;
    notify.NoDamageOutsideCombat = packet.NoDamageOutsideCombat;
    notify.AutoAim = packet.AutoAim;
    notify.DeferInitialization = packet.DeferInitialization;
    notify.ForceConeOfFire = packet.ForceConeOfFire;

    notify.UnkBool1 = packet.UnkBool1;
    notify.UnkBool2 = packet.UnkBool2;

    if (!GameServer::Get()->SendToPlayersInRange(notify, cShooterEntity, acMessage.GetSender()))
        spdlog::error("{}: SendToPlayersInRange failed", __FUNCTION__);
}
