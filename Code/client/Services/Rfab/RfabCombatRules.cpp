#include <TiltedOnlinePCH.h>

#include <Services/Rfab/RfabCombatRules.h>
#include <Services/Rfab/RfabFormResolver.h>

#include <Actor.h>
#include <Components.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/ActorExtension.h>
#include <Games/Misc/Lock.h>
#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <PlayerCharacter.h>
#include <Services/DownedStateService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Utils.h>
#include <World.h>

#include <cmath>
#include <optional>

void RfabCombatRules::ApplyAllyHitGameSetting(bool aProtectAllies) noexcept
{
    auto* pGmst = GameSettingCollection::Get();
    if (!pGmst)
        return;

    auto* pNonCombat = pGmst->GetSetting("iAllyHitNonCombatAllowed");
    auto* pCombat = pGmst->GetSetting("iAllyHitCombatAllowed");

    if (aProtectAllies)
    {
        if (!m_hasSavedAllyHitSettings)
        {
            if (pNonCombat)
                m_savedAllyHitNonCombatAllowed = pNonCombat->i;
            if (pCombat)
                m_savedAllyHitCombatAllowed = pCombat->i;
            m_hasSavedAllyHitSettings = true;
        }
        // Exact mirror of RFAB_AllyHitAllow.psc ("Единство в битве"): prevent accidental hits from aggroing allies.
        if (pNonCombat)
            pNonCombat->i = 1000;
        if (pCombat)
            pCombat->i = 1000;
    }
    else if (m_hasSavedAllyHitSettings)
    {
        if (pNonCombat)
            pNonCombat->i = m_savedAllyHitNonCombatAllowed;
        if (pCombat)
            pCombat->i = m_savedAllyHitCombatAllowed;
    }
}

void RfabCombatRules::BuildCombatSnapshot(
    World& aWorld,
    TransportService& aTransport,
    bool aAllyProtectionEnabled,
    RfabCombatSnapshot& aOutSnapshot) noexcept
{
    aOutSnapshot = RfabCombatSnapshot{};
    aOutSnapshot.Connected = aTransport.IsConnected();
    aOutSnapshot.AllyProtectionEnabled = aAllyProtectionEnabled;

    if (!aOutSnapshot.Connected)
        return;

    const auto& partySvc = aWorld.GetPartyService();
    aOutSnapshot.InParty = partySvc.IsInParty();
    if (aOutSnapshot.InParty)
    {
        const auto& members = partySvc.GetPartyMembers();
        aOutSnapshot.PartyMemberIds.reserve(members.size());
        for (const uint32_t pid : members)
            aOutSnapshot.PartyMemberIds.insert(pid);
    }

    const auto& downedSvc = aWorld.GetDownedStateService();
    auto tryAppendStandingPlayer = [&](Actor* apPlayerActor, bool aIsDowned)
    {
        if (!apPlayerActor || aIsDowned || apPlayerActor->VirtIsDead(false))
            return;
        if (auto* pExt = apPlayerActor->GetExtension(); pExt && pExt->IsDownedPlayer())
            return;

        RfabStandingPlayerSnapshotEntry entry{};
        entry.FormId = apPlayerActor->formID;
        entry.HandleBits = apPlayerActor->GetHandle().handle.iBits;
        entry.WorldSpaceId = apPlayerActor->GetWorldSpace() ? apPlayerActor->GetWorldSpace()->formID : 0u;
        entry.CellId = apPlayerActor->parentCell ? apPlayerActor->parentCell->formID : 0u;
        entry.Position = apPlayerActor->position;
        aOutSnapshot.StandingPlayers.push_back(entry);
    };

    auto playerView = aWorld.view<FormIdComponent, PlayerComponent>();
    bool includedLocalPlayer = false;
    for (auto entity : playerView)
    {
        const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
        const uint32_t pid = playerView.get<PlayerComponent>(entity).Id;
        if (fid != 0)
        {
            aOutSnapshot.FormIdToPlayerId[fid] = pid;
            Actor* pPlayerActor = Cast<Actor>(TESForm::GetById(fid));
            const bool isLocal = (fid == 0x14);
            if (isLocal)
                includedLocalPlayer = true;

            const bool isDowned = isLocal
                ? downedSvc.IsLocalPlayerDowned()
                : downedSvc.IsRemotePlayerDowned(pid);
            tryAppendStandingPlayer(pPlayerActor, isDowned);
        }
    }

    if (!includedLocalPlayer)
    {
        if (PlayerCharacter* pLocal = PlayerCharacter::Get())
            tryAppendStandingPlayer(pLocal, downedSvc.IsLocalPlayerDowned());
    }

    auto actorView = aWorld.view<FormIdComponent>(entt::exclude<ObjectComponent, PlayerComponent>);
    for (auto entity : actorView)
    {
        const uint32_t fid = actorView.get<FormIdComponent>(entity).Id;
        if (fid == 0 || fid == 0x14)
            continue;

        Actor* pSummon = Cast<Actor>(TESForm::GetById(fid));
        if (!pSummon || pSummon->VirtIsDead(false))
            continue;

        const auto* pExt = pSummon->GetExtension();
        if (pExt && pExt->IsPlayer())
            continue;

        const Actor* pCommander = pSummon->GetCommandingActor();
        const bool isPlayerSummon =
            pSummon->IsPlayerSummon() ||
            (pExt && pExt->IsPlayerSummonFlag()) ||
            (pCommander && (pCommander->formID == 0x14 || (pCommander->GetExtension() && pCommander->GetExtension()->IsPlayer())));
        if (!isPlayerSummon)
            continue;

        RfabStandingPlayerSnapshotEntry entry{};
        entry.FormId = pSummon->formID;
        entry.HandleBits = pSummon->GetHandle().handle.iBits;
        entry.WorldSpaceId = pSummon->GetWorldSpace() ? pSummon->GetWorldSpace()->formID : 0u;
        entry.CellId = pSummon->parentCell ? pSummon->parentCell->formID : 0u;
        entry.Position = pSummon->position;
        aOutSnapshot.StandingSummons.push_back(entry);
    }
}

GamePtr<Actor> RfabCombatRules::SelectBestStandingTarget(
    const RfabCombatSnapshot& acSnapshot,
    Actor* apAttacker) noexcept
{
    if (!apAttacker || !acSnapshot.Connected)
        return nullptr;

    const uint32_t attackerWorldId = apAttacker->GetWorldSpace() ? apAttacker->GetWorldSpace()->formID : 0u;
    const uint32_t attackerCellId = apAttacker->parentCell ? apAttacker->parentCell->formID : 0u;
    const float maxUnits = RfabCompat::MetersToSkyrimUnits(120.0f);
    const float maxUnitsSq = maxUnits * maxUnits;

    uint32_t bestHandleBits = 0;
    uint32_t bestFormId = 0;

    auto pickNearestFrom = [&](const std::vector<RfabStandingPlayerSnapshotEntry>& aList) noexcept
    {
        float bestDistSq = maxUnitsSq;
        for (const auto& candidate : aList)
        {
            if (attackerWorldId != 0)
            {
                if (candidate.WorldSpaceId != attackerWorldId)
                    continue;
            }
            else if (attackerCellId != 0 && candidate.CellId != attackerCellId)
            {
                continue;
            }

            const float dx = candidate.Position.x - apAttacker->position.x;
            const float dy = candidate.Position.y - apAttacker->position.y;
            const float dz = candidate.Position.z - apAttacker->position.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestHandleBits = candidate.HandleBits;
                bestFormId = candidate.FormId;
            }
        }
    };

    pickNearestFrom(acSnapshot.StandingPlayers);
    if (bestHandleBits == 0 && bestFormId == 0)
        pickNearestFrom(acSnapshot.StandingSummons);

    GamePtr<Actor> spReplacement = bestHandleBits ? Actor::LookupByHandle(bestHandleBits) : nullptr;
    if (!spReplacement && bestFormId == 0x14)
        spReplacement = PlayerCharacter::Get();

    if (!spReplacement || spReplacement.Get() == apAttacker || spReplacement->VirtIsDead(false))
        return nullptr;
    if (spReplacement->actorState.IsBleedingOut())
        return nullptr;
    if (auto* pExt = spReplacement->GetExtension(); pExt && pExt->IsDownedPlayer())
        return nullptr;

    return spReplacement;
}

RfabCompat::CoopAllyActionPolicy RfabCombatRules::EvaluateSnapshotAction(
    const RfabCombatSnapshot& acSnapshot,
    Actor* apAttacker,
    Actor* apTarget,
    bool aIsBeneficialHealOrBuff) noexcept
{
    if (!apAttacker || !apTarget || apAttacker == apTarget)
        return {};

    if (!acSnapshot.Connected || !acSnapshot.InParty)
        return {};

    struct ControllingPlayer
    {
        bool Valid{false};
        bool IsLocal{false};
        uint32_t RemotePlayerId{0};
    };

    const auto findPlayerIdByFormId = [&acSnapshot](uint32_t aFormId) -> std::optional<uint32_t>
    {
        if (aFormId == 0)
            return std::nullopt;
        const auto it = acSnapshot.FormIdToPlayerId.find(aFormId);
        if (it != acSnapshot.FormIdToPlayerId.end())
            return it->second;
        return std::nullopt;
    };

    const auto resolveController = [&findPlayerIdByFormId](Actor* apActor) -> ControllingPlayer
    {
        if (!apActor)
            return {};

        if (apActor->formID == 0x14)
            return {true, true, 0};

        const Actor* pCmd = apActor->GetCommandingActor();
        if (pCmd && pCmd->formID == 0x14)
            return {true, true, 0};

        const auto* pExt = apActor->GetExtension();
        if (pExt && !pExt->IsRemote() && apActor->IsPlayerTeammate())
            return {true, true, 0};

        if (pExt && pExt->IsRemotePlayer())
        {
            if (const auto pid = findPlayerIdByFormId(apActor->formID))
                return {true, false, *pid};
        }

        if (pCmd)
        {
            if (const auto* pCmdExt = pCmd->GetExtension(); pCmdExt && pCmdExt->IsRemotePlayer())
            {
                if (const auto pid = findPlayerIdByFormId(pCmd->formID))
                    return {true, false, *pid};
            }
        }

        return {};
    };

    const ControllingPlayer attacker = resolveController(apAttacker);
    const ControllingPlayer target = resolveController(apTarget);
    if (!attacker.Valid || !target.Valid)
        return {};

    // Never override RFAB's Speechcraft 50 «Лидерство» perk for a player's OWN solo followers or personal summons!
    if (attacker.IsLocal && target.IsLocal)
        return {};
    if (!attacker.IsLocal && !target.IsLocal && attacker.RemotePlayerId == target.RemotePlayerId)
        return {};

    const auto isMember = [&acSnapshot](uint32_t aPid) -> bool
    {
        return acSnapshot.PartyMemberIds.find(aPid) != acSnapshot.PartyMemberIds.end();
    };

    bool areSameCoopParty = false;
    if (attacker.IsLocal)
        areSameCoopParty = isMember(target.RemotePlayerId);
    else if (target.IsLocal)
        areSameCoopParty = isMember(attacker.RemotePlayerId);
    else
        areSameCoopParty = isMember(attacker.RemotePlayerId) && isMember(target.RemotePlayerId);

    return RfabCompat::EvaluateCoopAllyActionPolicy(
        areSameCoopParty, acSnapshot.AllyProtectionEnabled, aIsBeneficialHealOrBuff);
}

void RfabCombatRules::CollectPartyPeerActorFormIds(
    const RfabCombatSnapshot& acSnapshot,
    TiltedPhoques::Vector<uint32_t>& aOutFormIds) noexcept
{
    aOutFormIds.clear();
    if (!acSnapshot.Connected || !acSnapshot.InParty)
        return;

    for (const auto& [formId, playerId] : acSnapshot.FormIdToPlayerId)
    {
        if (formId == 0 || formId == 0x14)
            continue;
        if (acSnapshot.PartyMemberIds.contains(playerId))
            aOutFormIds.push_back(formId);
    }
}

bool RfabCombatRules::IsBossActor(Actor* apActor, const RfabFormResolver& acResolver) const noexcept
{
    if (!apActor)
        return false;

    const auto* pExt = apActor->GetExtension();
    if (pExt && pExt->IsPlayer())
        return false;

    if (apActor->IsDragon())
        return true;

    const float maxHp = apActor->GetActorPermanentValue(ActorValueInfo::kHealth);
    if (std::isfinite(maxHp) && maxHp >= 800.0f)
        return true;

    const auto& liveXpTable = acResolver.GetLiveXpTable();
    const auto& espEdidIndex = acResolver.GetEspEdidIndex();

    if (TESForm* pBase = apActor->baseForm)
    {
        std::string ownerPluginName;
        uint32_t localFormId = pBase->formID & 0x00FFFFFFu;
        const uint32_t topByte = (pBase->formID >> 24) & 0xFFu;
        if (topByte < 0xFFu)
        {
            if (auto* pModManager = ModManager::Get())
            {
                const bool wantLite = (topByte == 0xFEu);
                const uint16_t wantId = wantLite
                    ? static_cast<uint16_t>((pBase->formID >> 12) & 0xFFFu)
                    : static_cast<uint16_t>(topByte);
                if (wantLite)
                    localFormId = pBase->formID & 0x00000FFFu;

                for (Mod* pMod : pModManager->mods)
                {
                    if (!pMod || !pMod->IsLoaded() || pMod->IsLite() != wantLite)
                        continue;
                    if (pMod->GetId() == wantId)
                    {
                        ownerPluginName = pMod->filename;
                        break;
                    }
                }
            }
        }

        if (liveXpTable.ContainsBossForm(ownerPluginName, localFormId))
            return true;

        if (const char* baseEdid = pBase->GetFormEditorID(); baseEdid && baseEdid[0] != '\0')
        {
            if (RfabCompat::IsRfabBossEditorId(baseEdid, &liveXpTable))
                return true;
        }
        else if (!ownerPluginName.empty())
        {
            const std::string indexedEdid = espEdidIndex.LookupEditorId(ownerPluginName, localFormId);
            if (!indexedEdid.empty() && RfabCompat::IsRfabBossEditorId(indexedEdid, &liveXpTable))
                return true;
        }
    }

    if (const char* refEdid = apActor->GetFormEditorID(); refEdid && refEdid[0] != '\0')
    {
        if (RfabCompat::IsRfabBossEditorId(refEdid, &liveXpTable))
            return true;
    }

    return false;
}

bool RfabCombatRules::IsBossKnownEthereal(uint32_t aServerId) const noexcept
{
    if (aServerId == 0)
        return false;
    const auto it = m_lastBossGhostByServerId.find(aServerId);
    return (it != m_lastBossGhostByServerId.end()) && it->second;
}

void RfabCombatRules::BroadcastArenaBarrierUnlock(
    TransportService& aTransport,
    const GameId& acObjectId,
    uint64_t aWorldEpoch) noexcept
{
    if (!aTransport.IsConnected() || !acObjectId)
        return;

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
    ev.EpochId = aWorldEpoch;
    ev.SourceKey = String("arena_unlock");
    ev.BookId = acObjectId;
    ev.GlobalValue = 1.0f;
    aTransport.Send(ev);
}

void RfabCombatRules::PollBossEncounterState(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    uint64_t aWorldEpoch) noexcept
{
    if (!aTransport.IsConnected())
        return;

    auto view = aWorld.view<LocalComponent, FormIdComponent>();
    for (auto entity : view)
    {
        const auto& localComp = view.get<LocalComponent>(entity);
        const auto& formIdComp = view.get<FormIdComponent>(entity);
        if (formIdComp.Id == 0x14 || localComp.Id == 0)
            continue;

        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComp.Id));
        if (!pActor || pActor->IsDead())
            continue;

        const uint32_t serverId = localComp.Id;
        const auto prevIt = m_lastBossGhostByServerId.find(serverId);
        const bool wasTracked = (prevIt != m_lastBossGhostByServerId.end());

        if (!wasTracked && !IsBossActor(pActor, acResolver) && !pActor->IsGhost())
            continue;

        const bool isGhost = pActor->IsGhost();
        const bool prevGhost = wasTracked ? prevIt->second : false;
        if (isGhost == prevGhost)
            continue;

        m_lastBossGhostByServerId[serverId] = isGhost;

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
        ev.EpochId = aWorldEpoch;
        ev.SourceKey = String("boss_ghost");
        ev.XpDelta = static_cast<int32_t>(serverId);
        ev.GlobalValue = isGhost ? 1.0f : 0.0f;
        aTransport.Send(ev);
    }
}

void RfabCombatRules::ApplyBossEncounterSyncEvent(
    World& aWorld,
    const NotifyRfabProgressionEvent& acMessage) noexcept
{
    const std::string sourceKey(acMessage.SourceKey.c_str(), acMessage.SourceKey.size());
    if (sourceKey == "boss_ghost" && acMessage.XpDelta > 0)
    {
        const uint32_t bossServerId = static_cast<uint32_t>(acMessage.XpDelta);
        const bool isGhost = (acMessage.GlobalValue >= 0.5f);
        m_lastBossGhostByServerId[bossServerId] = isGhost;

        if (Actor* pBoss = Utils::GetByServerId<Actor>(bossServerId))
            pBoss->SetGhost(isGhost);
    }
    else if (sourceKey == "arena_unlock" && acMessage.BookId)
    {
        const uint32_t cObjectId = aWorld.GetModSystem().GetGameId(acMessage.BookId);
        if (TESObjectREFR* pObject = Cast<TESObjectREFR>(TESForm::GetById(cObjectId)))
        {
            if (auto* pLock = pObject->GetLock(); pLock && pLock->IsLocked())
            {
                pLock->SetLock(false);
                pObject->LockChange();
            }

            if (pObject->baseForm && pObject->baseForm->formType == FormType::Door)
            {
                const uint8_t openState = RfabCompat::NormalizeDoorOpenState(pObject->GetOpenState());
                if (openState == 3u) // kClosed -> open for co-op party
                {
                    ScopedActivateOverride recursionGuard;
                    pObject->Activate(PlayerCharacter::Get(), 0, 0, 1, 0);
                }
            }
        }
    }
}
