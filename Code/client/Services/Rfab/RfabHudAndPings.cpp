#include <TiltedOnlinePCH.h>

#include <Services/Rfab/RfabHudAndPings.h>
#include <Services/Rfab/RfabCombatRules.h>
#include <Services/Rfab/RfabFormResolver.h>

#include <Actor.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Camera/PlayerCamera.h>
#include <Combat/CombatController.h>
#include <Components.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/ActorExtension.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Interface/MenuControls.h>
#include <Games/Skyrim/Interface/Menus/HUDMenuUtils.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <PlayerCharacter.h>
#include <Services/DownedStateService.h>
#include <Services/OverlayService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <Utils.h>
#include <World.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace
{
std::string ResolveCellDisplayName(World& aWorld, const GameId& acWorldSpaceId, const GameId& acCellId) noexcept
{
    auto& modSystem = aWorld.GetModSystem();
    if (acWorldSpaceId)
    {
        const uint32_t worldFormId = modSystem.GetGameId(acWorldSpaceId);
        if (TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldFormId)))
        {
            if (const char* name = pWorldSpace->GetName(); name && name[0] != '\0')
                return std::string(name);
        }
    }

    if (acCellId)
    {
        const uint32_t cellFormId = modSystem.GetGameId(acCellId);
        if (TESObjectCELL* pCell = Cast<TESObjectCELL>(TESForm::GetById(cellFormId)))
        {
            if (const char* name = pCell->GetName(); name && name[0] != '\0')
                return std::string(name);
        }
    }

    return "Скайрим";
}

bool IsTacticalHudBlockedByModalMenu() noexcept
{
    auto* pUi = UI::Get();
    if (!pUi)
        return true;

    // Must ONLY be called on the Main Game Thread (inside BuildHudSnapshot), NEVER on D3D11 Render Thread!
    struct CachedHiddenHudMenus
    {
        std::array<BSFixedString, 22> Names{
            BSFixedString("Main Menu"),
            BSFixedString("Loading Menu"),
            BSFixedString("Fader Menu"),
            BSFixedString("Mist Menu"),
            BSFixedString("MapMenu"),
            BSFixedString("StatsMenu"),
            BSFixedString("CustomMenu"),
            BSFixedString("TweenMenu"),
            BSFixedString("Journal Menu"),
            BSFixedString("InventoryMenu"),
            BSFixedString("MagicMenu"),
            BSFixedString("FavoritesMenu"),
            BSFixedString("ContainerMenu"),
            BSFixedString("BarterMenu"),
            BSFixedString("GiftMenu"),
            BSFixedString("Training Menu"),
            BSFixedString("Crafting Menu"),
            BSFixedString("Book Menu"),
            BSFixedString("Lockpicking Menu"),
            BSFixedString("Sleep/Wait Menu"),
            BSFixedString("RaceSex Menu"),
            BSFixedString("Dialogue Menu"),
        };
    };

    static const CachedHiddenHudMenus* s_pMenus = new CachedHiddenHudMenus();
    for (const auto& menuName : s_pMenus->Names)
    {
        if (pUi->GetMenuOpen(menuName))
            return true;
    }

    return false;
}

bool IsHotkeyInputBlockedByTextMenu(World& aWorld) noexcept
{
    if (aWorld.GetOverlayService().GetActive())
        return true;

    if (auto* pUi = UI::Get())
    {
        struct CachedTextMenus
        {
            std::array<BSFixedString, 2> Names{
                BSFixedString("Console"),
                BSFixedString("MessageBoxMenu"),
            };
        };
        static const CachedTextMenus* s_pTextMenus = new CachedTextMenus();
        for (const auto& menuName : s_pTextMenus->Names)
        {
            if (pUi->GetMenuOpen(menuName))
                return true;
        }
    }

    return IsTacticalHudBlockedByModalMenu();
}

void DrawShadowedText(ImDrawList* apDrawList, const ImVec2& acPos, ImU32 aTextColor, const char* apText) noexcept
{
    if (!apDrawList || !apText || apText[0] == '\0')
        return;

    apDrawList->AddText(ImVec2(acPos.x + 1.0f, acPos.y + 1.0f), IM_COL32(0, 0, 0, 225), apText);
    apDrawList->AddText(acPos, aTextColor, apText);
}

ImU32 GetPingPrimaryColor(RfabCompat::TacticalPingKind aKind, uint8_t aAlpha = 240) noexcept
{
    switch (aKind)
    {
    case RfabCompat::TacticalPingKind::Enemy:
        return IM_COL32(255, 76, 76, aAlpha);
    case RfabCompat::TacticalPingKind::EnemyBoss:
        return IM_COL32(255, 158, 44, aAlpha);
    case RfabCompat::TacticalPingKind::Danger:
        return IM_COL32(255, 42, 85, aAlpha);
    case RfabCompat::TacticalPingKind::Loot:
        return IM_COL32(255, 212, 71, aAlpha);
    case RfabCompat::TacticalPingKind::Objective:
        return IM_COL32(71, 209, 255, aAlpha);
    case RfabCompat::TacticalPingKind::AssistAlly:
        return IM_COL32(61, 220, 132, aAlpha);
    case RfabCompat::TacticalPingKind::Rally:
    default:
        return IM_COL32(92, 168, 255, aAlpha);
    }
}
} // namespace

void RfabHudAndPings::ApplyDefaultModeFromBindings(const RfabCompat::RfabBindingsConfig& acBindings) noexcept
{
    if (acBindings.TacticalHudDefaultMode <= 3)
        m_tacticalHudMode = static_cast<RfabCompat::TacticalHudMode>(acBindings.TacticalHudDefaultMode);
    else
        m_tacticalHudMode = RfabCompat::TacticalHudMode::Full;
}

void RfabHudAndPings::SetMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice) noexcept
{
    m_tacticalHudMode = aMode;
    if (aShowNotice)
    {
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "Тактический HUD: %s", RfabCompat::GetTacticalHudModeNameRu(m_tacticalHudMode));
        Utils::ShowHudMessage(String(buf));
    }
}

void RfabHudAndPings::ResetOnDisconnect() noexcept
{
    m_partyHudPeers.clear();
    m_activeTacticalPings.clear();
    m_pingCooldownRemainingSec = 0.0;
    m_pingKeyWasDown = false;
    m_hudKeyWasDown = false;
}

void RfabHudAndPings::OnPlayerJoined(World& aWorld, const NotifyPlayerJoined& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    state.Username = std::string(acMessage.Username.c_str(), acMessage.Username.size());
    if (acMessage.Level > 0)
        state.Level = acMessage.Level;
    state.CellName = ResolveCellDisplayName(aWorld, acMessage.WorldSpaceId, acMessage.CellId);
}

void RfabHudAndPings::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_partyHudPeers.erase(acMessage.PlayerId);
    m_activeTacticalPings.erase(
        std::remove_if(
            m_activeTacticalPings.begin(),
            m_activeTacticalPings.end(),
            [pid = acMessage.PlayerId](const ActiveTacticalPing& p) { return p.Payload.SenderPlayerId == pid; }),
        m_activeTacticalPings.end());
}

void RfabHudAndPings::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    if (acMessage.NewLevel > 0)
        state.Level = acMessage.NewLevel;
}

void RfabHudAndPings::OnPlayerCellChanged(World& aWorld, const NotifyPlayerCellChanged& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    state.CellName = ResolveCellDisplayName(aWorld, acMessage.WorldSpaceId, acMessage.CellId);
}

void RfabHudAndPings::OnPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    if (std::isfinite(acMessage.Percentage))
        state.HealthPercent = std::clamp(acMessage.Percentage, 0.0f, 100.0f);
}

void RfabHudAndPings::OnPlayerList(const NotifyPlayerList& acMessage) noexcept
{
    for (const auto& [playerId, username] : acMessage.Players)
    {
        auto& state = m_partyHudPeers[playerId];
        state.PlayerId = playerId;
        state.Username = std::string(username.c_str(), username.size());
    }
}

void RfabHudAndPings::UpsertActivePing(
    const RfabCompat::TacticalPingPayload& acPayload,
    const RfabCompat::RfabBindingsConfig& acBindings) noexcept
{
    const double baseDuration = std::clamp(static_cast<double>(acBindings.TacticalPingDurationSec), 3.0, 60.0);
    const double durationSec = (acPayload.Kind == RfabCompat::TacticalPingKind::EnemyBoss)
        ? std::max(baseDuration, 18.0)
        : baseDuration;

    const float mergeRadiusUnits = RfabCompat::MetersToSkyrimUnits(2.5f);
    const float mergeRadiusSq = mergeRadiusUnits * mergeRadiusUnits;

    for (auto& existing : m_activeTacticalPings)
    {
        if (existing.Payload.SenderPlayerId != acPayload.SenderPlayerId)
            continue;

        const bool sameTargetEntity =
            (acPayload.TargetServerId != 0 && existing.Payload.TargetServerId == acPayload.TargetServerId);
        const float dx = existing.Payload.PosX - acPayload.PosX;
        const float dy = existing.Payload.PosY - acPayload.PosY;
        const float dz = existing.Payload.PosZ - acPayload.PosZ;
        const bool sameSpot = (dx * dx + dy * dy + dz * dz) <= mergeRadiusSq;

        if (sameTargetEntity || sameSpot)
        {
            existing.Payload = acPayload;
            existing.TotalDurationSec = durationSec;
            existing.RemainingSec = durationSec;
            return;
        }
    }

    size_t senderCount = 0;
    size_t oldestSenderIdx = m_activeTacticalPings.size();
    double smallestRemaining = 1e9;
    for (size_t i = 0; i < m_activeTacticalPings.size(); ++i)
    {
        if (m_activeTacticalPings[i].Payload.SenderPlayerId == acPayload.SenderPlayerId)
        {
            ++senderCount;
            if (m_activeTacticalPings[i].RemainingSec < smallestRemaining)
            {
                smallestRemaining = m_activeTacticalPings[i].RemainingSec;
                oldestSenderIdx = i;
            }
        }
    }

    if (senderCount >= 3 && oldestSenderIdx < m_activeTacticalPings.size())
        m_activeTacticalPings.erase(m_activeTacticalPings.begin() + static_cast<ptrdiff_t>(oldestSenderIdx));

    ActiveTacticalPing entry{};
    entry.Payload = acPayload;
    entry.TotalDurationSec = durationSec;
    entry.RemainingSec = durationSec;
    m_activeTacticalPings.push_back(std::move(entry));
}

void RfabHudAndPings::ApplyIncomingPingSyncEvent(
    const NotifyRfabProgressionEvent& acMessage,
    const RfabCompat::RfabBindingsConfig& acBindings) noexcept
{
    const std::string encoded(acMessage.SourceKey.c_str(), acMessage.SourceKey.size());
    RfabCompat::TacticalPingPayload payload{};
    if (!RfabCompat::DeserializeTacticalPingPayload(encoded, payload))
        return;

    if (payload.Kind == RfabCompat::TacticalPingKind::ClearSenderPings)
    {
        m_activeTacticalPings.erase(
            std::remove_if(
                m_activeTacticalPings.begin(),
                m_activeTacticalPings.end(),
                [senderId = payload.SenderPlayerId](const ActiveTacticalPing& p)
                { return p.Payload.SenderPlayerId == senderId; }),
            m_activeTacticalPings.end());
        return;
    }

    UpsertActivePing(payload, acBindings);

    char noticeBuf[256]{};
    const char* sender = payload.SenderName.empty() ? "Союзник" : payload.SenderName.c_str();
    const char* title = RfabCompat::GetTacticalPingTitleRu(payload.Kind);
    if (!payload.TargetLabel.empty())
        std::snprintf(noticeBuf, sizeof(noticeBuf), "[Метка от %s]: %s — %s", sender, title, payload.TargetLabel.c_str());
    else
        std::snprintf(noticeBuf, sizeof(noticeBuf), "[Метка от %s]: %s", sender, title);
    Utils::ShowHudMessage(String(noticeBuf));
}

void RfabHudAndPings::ClearLocalPings(
    TransportService& aTransport,
    uint32_t aLocalPlayerId,
    uint64_t aWorldEpoch) noexcept
{
    m_activeTacticalPings.erase(
        std::remove_if(
            m_activeTacticalPings.begin(),
            m_activeTacticalPings.end(),
            [aLocalPlayerId](const ActiveTacticalPing& p) { return p.Payload.SenderPlayerId == aLocalPlayerId; }),
        m_activeTacticalPings.end());

    if (aTransport.IsConnected())
    {
        RfabCompat::TacticalPingPayload clearPayload{};
        clearPayload.Kind = RfabCompat::TacticalPingKind::ClearSenderPings;
        clearPayload.SenderPlayerId = aLocalPlayerId;

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
        ev.EpochId = aWorldEpoch;
        ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(clearPayload).c_str());
        aTransport.Send(ev);
    }

    Utils::ShowHudMessage(String("Ваши тактические метки сняты"));
}

void RfabHudAndPings::TriggerContextualPing(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    const RfabCombatRules& acCombatRules,
    uint32_t aLocalPlayerId,
    uint64_t aWorldEpoch,
    bool aForceDanger,
    const std::string& acCustomLabel) noexcept
{
    if (!aTransport.IsConnected() || m_pingCooldownRemainingSec > 0.0)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const auto& bindings = acResolver.GetBindings();
    m_pingCooldownRemainingSec = std::clamp(static_cast<double>(bindings.TacticalPingCooldownSec), 0.15, 5.0);

    const uint32_t localWorldId = pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0u;
    const uint32_t localCellId = pPlayer->parentCell ? pPlayer->parentCell->formID : 0u;

    TESObjectREFR* pBestTarget = nullptr;
    uint32_t bestServerId = 0;
    uint32_t bestRemotePlayerId = 0;
    bool bestIsPlayer = false;
    bool bestIsDownedPlayer = false;
    bool bestIsBoss = false;
    bool bestIsHostile = false;
    bool bestIsDead = false;
    float bestScore = 1.0f;

    const float maxScanUnits = RfabCompat::MetersToSkyrimUnits(85.0f);
    const float maxScanUnitsSq = maxScanUnits * maxScanUnits;

    auto view = aWorld.view<FormIdComponent>();
    for (auto entity : view)
    {
        const uint32_t formId = view.get<FormIdComponent>(entity).Id;
        if (formId == 0 || formId == 0x14)
            continue;

        TESObjectREFR* pRef = Cast<TESObjectREFR>(TESForm::GetById(formId));
        if (!pRef || pRef == pPlayer || pRef->IsDisabled())
            continue;

        const uint32_t refWorldId = pRef->GetWorldSpace() ? pRef->GetWorldSpace()->formID : 0u;
        const uint32_t refCellId = pRef->parentCell ? pRef->parentCell->formID : 0u;
        if (localWorldId != 0)
        {
            if (refWorldId != localWorldId)
                continue;
        }
        else if (localCellId != 0 && refCellId != localCellId)
        {
            continue;
        }

        const float dx = pRef->position.x - pPlayer->position.x;
        const float dy = pRef->position.y - pPlayer->position.y;
        const float dz = pRef->position.z - pPlayer->position.z;
        const float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq > maxScanUnitsSq)
            continue;

        NiPoint3 aimPt = pRef->position;
        Actor* pActor = Cast<Actor>(pRef);
        if (pActor)
            aimPt.z += std::max(pActor->GetHeight() * 0.65f, 45.0f);
        else
            aimPt.z += 25.0f;

        NiPoint3 screenPt{};
        if (!HUDMenuUtils::WorldPtToScreenPt3(aimPt, screenPt) || screenPt.z < 0.0f)
            continue;

        const float offCenterX = screenPt.x - 0.5f;
        const float offCenterY = screenPt.y - 0.5f;
        const float screenDist = std::sqrt(offCenterX * offCenterX + offCenterY * offCenterY);
        if (screenDist > 0.075f)
            continue;

        float score = screenDist + (std::sqrt(distSq) / maxScanUnits) * 0.015f;
        bool isPlayer = false;
        bool isDowned = false;
        bool isBoss = false;
        bool isHostile = false;
        bool isDead = false;
        uint32_t remotePid = 0;

        if (const auto* pPlayerComp = aWorld.try_get<PlayerComponent>(entity))
        {
            isPlayer = true;
            remotePid = pPlayerComp->Id;
            isDowned = aWorld.GetDownedStateService().IsRemotePlayerDowned(remotePid);
            if (isDowned)
                score -= 0.025f;
        }
        else if (pActor)
        {
            isDead = pActor->IsDead();
            if (!isDead)
            {
                isBoss = acCombatRules.IsBossActor(pActor, acResolver);
                isHostile = isBoss || pActor->IsInCombat();
                if (isBoss)
                    score -= 0.02f;
                else if (isHostile)
                    score -= 0.01f;
            }
        }

        if (score < bestScore)
        {
            bestScore = score;
            pBestTarget = pRef;
            bestIsPlayer = isPlayer;
            bestIsDownedPlayer = isDowned;
            bestIsBoss = isBoss;
            bestIsHostile = isHostile;
            bestIsDead = isDead;
            bestRemotePlayerId = remotePid;
            bestServerId = 0;
            if (const auto* pLocal = aWorld.try_get<LocalComponent>(entity))
                bestServerId = pLocal->Id;
            else if (const auto* pRemote = aWorld.try_get<RemoteComponent>(entity))
                bestServerId = pRemote->Id;
            else if (const auto* pObj = aWorld.try_get<ObjectComponent>(entity))
                bestServerId = pObj->Id;
        }
    }

    RfabCompat::TacticalPingPayload payload{};
    payload.SenderPlayerId = aLocalPlayerId;
    if (pPlayer->baseForm)
    {
        if (const char* localName = pPlayer->baseForm->GetName(); localName && localName[0] != '\0')
            payload.SenderName = localName;
    }
    if (payload.SenderName.empty())
        payload.SenderName = "Игрок";

    payload.WorldSpaceBaseId = localWorldId;
    payload.CellBaseId = localCellId;

    if (pBestTarget)
    {
        const uint8_t formType = pBestTarget->baseForm ? static_cast<uint8_t>(pBestTarget->baseForm->formType) : 0u;
        payload.Kind = RfabCompat::ClassifyCrosshairTacticalPingKind(
            aForceDanger,
            bestIsPlayer,
            bestIsDownedPlayer,
            bestIsBoss,
            bestIsHostile,
            bestIsDead,
            formType);
        payload.TargetServerId = bestServerId;
        payload.PosX = pBestTarget->position.x;
        payload.PosY = pBestTarget->position.y;
        payload.PosZ = pBestTarget->position.z + (Cast<Actor>(pBestTarget) ? std::max(pBestTarget->GetHeight(), 70.0f) : 35.0f);

        if (!acCustomLabel.empty())
        {
            payload.TargetLabel = acCustomLabel;
        }
        else if (bestIsPlayer && bestRemotePlayerId != 0)
        {
            if (const auto it = m_partyHudPeers.find(bestRemotePlayerId);
                it != m_partyHudPeers.end() && !it->second.Username.empty())
            {
                payload.TargetLabel = it->second.Username;
            }
        }

        if (payload.TargetLabel.empty() && pBestTarget->baseForm)
        {
            if (const char* refName = pBestTarget->baseForm->GetName(); refName && refName[0] != '\0')
                payload.TargetLabel = refName;
        }
    }
    else
    {
        payload.Kind = aForceDanger ? RfabCompat::TacticalPingKind::Danger : RfabCompat::TacticalPingKind::Rally;
        payload.TargetServerId = 0;
        payload.TargetLabel = acCustomLabel;

        float yaw = pPlayer->rotation.z;
        float pitch = pPlayer->rotation.x;
        NiPoint3 origin = pPlayer->position;
        origin.z += 110.0f;

        if (auto* pCam = PlayerCamera::Get())
        {
            yaw = pCam->rotZ;
            pitch = pCam->rotX;
        }

        const float forwardDistUnits = RfabCompat::MetersToSkyrimUnits(16.0f);
        const float cosPitch = std::cos(pitch);
        payload.PosX = origin.x + std::sin(yaw) * cosPitch * forwardDistUnits;
        payload.PosY = origin.y + std::cos(yaw) * cosPitch * forwardDistUnits;
        payload.PosZ = std::max(pPlayer->position.z + 25.0f, origin.z - std::sin(pitch) * forwardDistUnits);
    }

    UpsertActivePing(payload, bindings);

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
    ev.EpochId = aWorldEpoch;
    ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(payload).c_str());
    aTransport.Send(ev);

    char localMsg[256]{};
    const char* title = RfabCompat::GetTacticalPingTitleRu(payload.Kind);
    if (!payload.TargetLabel.empty())
        std::snprintf(localMsg, sizeof(localMsg), "Тактическая метка: %s — %s", title, payload.TargetLabel.c_str());
    else
        std::snprintf(localMsg, sizeof(localMsg), "Тактическая метка: %s", title);
    Utils::ShowHudMessage(String(localMsg));
}

void RfabHudAndPings::TriggerExplicitPing(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    const RfabCombatRules& acCombatRules,
    uint32_t aLocalPlayerId,
    uint64_t aWorldEpoch,
    RfabCompat::TacticalPingKind aKind,
    const std::string& acCustomLabel) noexcept
{
    if (aKind == RfabCompat::TacticalPingKind::ClearSenderPings)
    {
        ClearLocalPings(aTransport, aLocalPlayerId, aWorldEpoch);
        return;
    }

    if (!aTransport.IsConnected() || !PlayerCharacter::Get())
        return;

    m_pingCooldownRemainingSec = 0.0;
    TriggerContextualPing(
        aWorld,
        aTransport,
        acResolver,
        acCombatRules,
        aLocalPlayerId,
        aWorldEpoch,
        aKind == RfabCompat::TacticalPingKind::Danger,
        acCustomLabel);

    if (!m_activeTacticalPings.empty())
    {
        auto& newest = m_activeTacticalPings.back();
        if (newest.Payload.Kind != aKind)
        {
            newest.Payload.Kind = aKind;
            SyncRfabProgressionEvent ev{};
            ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
            ev.EpochId = aWorldEpoch;
            ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(newest.Payload).c_str());
            aTransport.Send(ev);
        }
    }
}

bool RfabHudAndPings::ExecuteChatCommand(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    const RfabCombatRules& acCombatRules,
    uint32_t aLocalPlayerId,
    uint64_t aWorldEpoch,
    const RfabCompat::TacticalChatCommand& acCmd,
    bool& aOutShouldClearSnapshot) noexcept
{
    aOutShouldClearSnapshot = false;
    switch (acCmd.Kind)
    {
    case RfabCompat::TacticalChatCommand::Action::TriggerContextualPing:
        TriggerContextualPing(
            aWorld, aTransport, acResolver, acCombatRules, aLocalPlayerId, aWorldEpoch, false, acCmd.CustomLabel);
        return true;
    case RfabCompat::TacticalChatCommand::Action::TriggerExplicitPing:
        TriggerExplicitPing(
            aWorld,
            aTransport,
            acResolver,
            acCombatRules,
            aLocalPlayerId,
            aWorldEpoch,
            acCmd.PingKind,
            acCmd.CustomLabel);
        return true;
    case RfabCompat::TacticalChatCommand::Action::ClearPings:
        ClearLocalPings(aTransport, aLocalPlayerId, aWorldEpoch);
        return true;
    case RfabCompat::TacticalChatCommand::Action::CycleHudMode:
        SetMode(RfabCompat::CycleTacticalHudMode(m_tacticalHudMode), true);
        aOutShouldClearSnapshot = (m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden);
        return true;
    case RfabCompat::TacticalChatCommand::Action::SetHudMode:
        SetMode(acCmd.HudMode, true);
        aOutShouldClearSnapshot = (m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden);
        return true;
    case RfabCompat::TacticalChatCommand::Action::None:
    default:
        return false;
    }
}

bool RfabHudAndPings::UpdatePingsAndInput(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    const RfabCombatRules& acCombatRules,
    uint32_t aLocalPlayerId,
    uint64_t aWorldEpoch,
    double aDeltaSec) noexcept
{
    bool hidHudMode = false;
    if (m_pingCooldownRemainingSec > 0.0)
        m_pingCooldownRemainingSec = std::max(0.0, m_pingCooldownRemainingSec - aDeltaSec);

    for (auto it = m_activeTacticalPings.begin(); it != m_activeTacticalPings.end();)
    {
        it->RemainingSec -= aDeltaSec;
        if (it->RemainingSec <= 0.0)
        {
            it = m_activeTacticalPings.erase(it);
            continue;
        }

        if (it->Payload.TargetServerId != 0)
        {
            if (TESObjectREFR* pTarget = Utils::GetByServerId<TESObjectREFR>(it->Payload.TargetServerId))
            {
                it->Payload.PosX = pTarget->position.x;
                it->Payload.PosY = pTarget->position.y;
                if (Actor* pActor = Cast<Actor>(pTarget))
                {
                    it->Payload.PosZ = pTarget->position.z + std::max(pActor->GetHeight(), 70.0f);
                    if (pActor->IsDead() &&
                        (it->Payload.Kind == RfabCompat::TacticalPingKind::Enemy ||
                         it->Payload.Kind == RfabCompat::TacticalPingKind::EnemyBoss))
                    {
                        it->RemainingSec = std::min(it->RemainingSec, 2.0);
                    }
                }
                else
                {
                    it->Payload.PosZ = pTarget->position.z + 35.0f;
                }
            }
        }

        ++it;
    }

#ifdef _WIN32
    auto* pWindow = BSGraphics::GetMainWindow();
    if (!pWindow || !pWindow->IsForeground() || IsHotkeyInputBlockedByTextMenu(aWorld))
    {
        m_pingKeyWasDown = false;
        m_hudKeyWasDown = false;
        return false;
    }

    const auto& bindings = acResolver.GetBindings();
    const int hudVk = static_cast<int>(bindings.TacticalHudToggleVirtualKey & 0xFFu);
    const bool hudKeyDown = (hudVk > 0) && ((GetAsyncKeyState(hudVk) & 0x8000) != 0);
    if (hudKeyDown && !m_hudKeyWasDown)
    {
        SetMode(RfabCompat::CycleTacticalHudMode(m_tacticalHudMode), true);
        hidHudMode = (m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden);
    }
    m_hudKeyWasDown = hudKeyDown;

    const int pingVk = static_cast<int>(bindings.TacticalPingVirtualKey & 0xFFu);
    const bool primaryPingDown = (pingVk > 0) && ((GetAsyncKeyState(pingVk) & 0x8000) != 0);
    const bool altMmbPingDown =
        ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) && ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    const bool pingKeyDown = primaryPingDown || altMmbPingDown;

    if (pingKeyDown && !m_pingKeyWasDown)
    {
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrlHeld)
        {
            ClearLocalPings(aTransport, aLocalPlayerId, aWorldEpoch);
        }
        else
        {
            TriggerContextualPing(
                aWorld, aTransport, acResolver, acCombatRules, aLocalPlayerId, aWorldEpoch, shiftHeld, "");
        }
    }
    m_pingKeyWasDown = pingKeyDown;
#else
    (void)aWorld;
    (void)aTransport;
    (void)acResolver;
    (void)acCombatRules;
    (void)aLocalPlayerId;
    (void)aWorldEpoch;
#endif
    return hidHudMode;
}

void RfabHudAndPings::SelfHealFreeRoamMenuControls(World& aWorld) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || pPlayer->VirtIsDead(false))
        return;

    if (aWorld.GetDownedStateService().IsLocalPlayerDowned())
        return;
    if (auto* pExt = pPlayer->GetExtension(); pExt && pExt->IsDownedPlayer())
        return;

    if (auto* pMenuControls = MenuControls::GetInstance())
    {
        if (pMenuControls->InBeastForm() || pMenuControls->InRemapMode())
            return;
    }

    if (auto* pUi = UI::Get())
    {
        struct CachedControlHealMenus
        {
            std::array<BSFixedString, 18> Names{
                BSFixedString("Loading Menu"),   BSFixedString("Main Menu"),
                BSFixedString("Dialogue Menu"),  BSFixedString("BarterMenu"),
                BSFixedString("Crafting Menu"),  BSFixedString("Lockpicking Menu"),
                BSFixedString("Book Menu"),      BSFixedString("Sleep/Wait Menu"),
                BSFixedString("RaceSex Menu"),   BSFixedString("MapMenu"),
                BSFixedString("StatsMenu"),      BSFixedString("Journal Menu"),
                BSFixedString("MessageBoxMenu"), BSFixedString("Console"),
                BSFixedString("TweenMenu"),      BSFixedString("CustomMenu"),
                BSFixedString("Fader Menu"),     BSFixedString("Mist Menu")};
        };
        static const CachedControlHealMenus* s_pMenus = new CachedControlHealMenus();
        for (const auto& menuName : s_pMenus->Names)
        {
            if (pUi->GetMenuOpen(menuName))
                return;
        }
    }

    auto* pInputEnable = BSInputEnableManager::Get();
    if (!pInputEnable)
        return;

    const uint32_t mask = pInputEnable->GetEnabledControlsMask();
    constexpr uint32_t kFreeRoamActiveMask =
        BSInputEnableManager::kMovement | BSInputEnableManager::kLooking | BSInputEnableManager::kFighting;
    constexpr uint32_t kMenuAndMainFourMask =
        BSInputEnableManager::kMenu | BSInputEnableManager::kMainFour;

    if ((mask & kFreeRoamActiveMask) == kFreeRoamActiveMask &&
        (mask & kMenuAndMainFourMask) != kMenuAndMainFourMask)
    {
        pInputEnable->EnableOtherEvent(static_cast<int32_t>(kMenuAndMainFourMask), true, true);
        spdlog::info("[RFAB Input] Self-healed stuck ControlMap kMenu/kMainFour flags (mask was 0x{:X})", mask);
    }
}

void RfabHudAndPings::BuildHudSnapshot(
    World& aWorld,
    TransportService& aTransport,
    const RfabFormResolver& acResolver,
    const RfabCombatRules& acCombatRules,
    uint32_t aLocalPlayerId,
    bool aAllyProtectionEnabled,
    bool aLocalIsAllyProtectionHost,
    RfabTacticalHudRenderSnapshot& aOutSnapshot) const noexcept
{
    aOutSnapshot = RfabTacticalHudRenderSnapshot{};

    if (!aTransport.IsConnected() || m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
        return;

    if (IsTacticalHudBlockedByModalMenu())
        return;

    const auto& bindings = acResolver.GetBindings();
    aOutSnapshot.Visible = true;
    aOutSnapshot.Mode = m_tacticalHudMode;
    aOutSnapshot.AllyProtectionEnabled = aAllyProtectionEnabled;

    const uint32_t localWorldId = pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0u;
    const uint32_t localCellId = pPlayer->parentCell ? pPlayer->parentCell->formID : 0u;
    const auto& downedService = aWorld.GetDownedStateService();
    const auto& partyService = aWorld.GetPartyService();

    aOutSnapshot.IsChannelingRevive = downedService.IsChannelingRevive();
    aOutSnapshot.ReviveChannelProgress = downedService.GetReviveChannelProgress();
    aOutSnapshot.IsLocalPlayerDowned = downedService.IsLocalPlayerDowned();
    aOutSnapshot.IsLocalPlayerBeingRevived = downedService.IsPlayerBeingRevived(aLocalPlayerId);
    aOutSnapshot.IsLocalPlayerInvulnerable = downedService.IsLocalPlayerInvulnerable();
    aOutSnapshot.GraceRemainingSec = downedService.GetGraceRemainingSec();

    std::unordered_map<uint32_t, Actor*> loadedActorByPlayerId;
    std::unordered_map<uint32_t, std::string> playerNameByFormId;
    if (pPlayer->baseForm && pPlayer->baseForm->GetName())
        playerNameByFormId[pPlayer->formID] = pPlayer->baseForm->GetName();

    auto playerView = aWorld.view<FormIdComponent, PlayerComponent>();
    for (auto entity : playerView)
    {
        const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
        const uint32_t pid = playerView.get<PlayerComponent>(entity).Id;
        if (fid == 0 || fid == 0x14)
            continue;
        if (Actor* pRemoteActor = Cast<Actor>(TESForm::GetById(fid)))
        {
            loadedActorByPlayerId[pid] = pRemoteActor;
            auto itPeer = m_partyHudPeers.find(pid);
            if (itPeer != m_partyHudPeers.end() && !itPeer->second.Username.empty())
                playerNameByFormId[fid] = itPeer->second.Username;
            else if (const auto& partyPlayers = partyService.GetPlayers(); partyPlayers.contains(pid))
                playerNameByFormId[fid] = std::string(partyPlayers.at(pid).c_str(), partyPlayers.at(pid).size());
        }
    }

    if (bindings.ShowDowned3dBeacons)
    {
        for (const auto& [pid, pAlly] : loadedActorByPlayerId)
        {
            if (!pAlly || !downedService.IsRemotePlayerDowned(pid))
                continue;

            const uint32_t allyWorldId = pAlly->GetWorldSpace() ? pAlly->GetWorldSpace()->formID : 0u;
            const uint32_t allyCellId = pAlly->parentCell ? pAlly->parentCell->formID : 0u;
            if (localWorldId != 0 ? (allyWorldId != localWorldId) : (localCellId != 0 && allyCellId != localCellId))
                continue;

            const float dx = pAlly->position.x - pPlayer->position.x;
            const float dy = pAlly->position.y - pPlayer->position.y;
            const float dz = pAlly->position.z - pPlayer->position.z;
            const float distMeters = RfabCompat::SkyrimUnitsToMeters(std::sqrt(dx * dx + dy * dy + dz * dz));

            NiPoint3 worldHead = pAlly->position;
            worldHead.z += std::max(pAlly->GetHeight() + 24.0f, 95.0f);

            NiPoint3 normScreen{};
            HUDMenuUtils::WorldPtToScreenPt3(worldHead, normScreen);

            RfabDownedBeaconSnapshotEntry entry{};
            entry.AllyName = "Союзник";
            if (auto it = m_partyHudPeers.find(pid); it != m_partyHudPeers.end() && !it->second.Username.empty())
                entry.AllyName = it->second.Username;
            entry.DistMeters = distMeters;
            entry.NormScreenX = normScreen.x;
            entry.NormScreenY = normScreen.y;
            entry.NormScreenZ = normScreen.z;
            entry.BeingRevived = downedService.IsPlayerBeingRevived(pid);
            aOutSnapshot.DownedBeacons.push_back(std::move(entry));
        }
    }

    for (const auto& pingEntry : m_activeTacticalPings)
    {
        const auto& ping = pingEntry.Payload;
        if (localWorldId != 0)
        {
            if (ping.WorldSpaceBaseId != 0 && ping.WorldSpaceBaseId != localWorldId)
                continue;
        }
        else if (localCellId != 0 && ping.CellBaseId != 0 && ping.CellBaseId != localCellId)
        {
            continue;
        }

        const float dx = ping.PosX - pPlayer->position.x;
        const float dy = ping.PosY - pPlayer->position.y;
        const float dz = ping.PosZ - pPlayer->position.z;
        const float distMeters = RfabCompat::SkyrimUnitsToMeters(std::sqrt(dx * dx + dy * dy + dz * dz));

        NiPoint3 worldPt{};
        worldPt.x = ping.PosX;
        worldPt.y = ping.PosY;
        worldPt.z = ping.PosZ;
        NiPoint3 normScreen{};
        HUDMenuUtils::WorldPtToScreenPt3(worldPt, normScreen);

        RfabTacticalPingSnapshotEntry snapPing{};
        snapPing.Kind = ping.Kind;
        snapPing.TargetLabel = ping.TargetLabel;
        snapPing.DistMeters = distMeters;
        snapPing.NormScreenX = normScreen.x;
        snapPing.NormScreenY = normScreen.y;
        snapPing.NormScreenZ = normScreen.z;
        snapPing.RemainingSec = pingEntry.RemainingSec;
        snapPing.TotalDurationSec = pingEntry.TotalDurationSec;
        aOutSnapshot.ActivePings.push_back(std::move(snapPing));
    }

    if (m_tacticalHudMode != RfabCompat::TacticalHudMode::MarkersOnly)
    {
        if (bindings.ShowBossEncounterBar)
        {
            Actor* pBestBoss = nullptr;
            uint32_t bestBossServerId = 0;
            float bestBossDistSq = RfabCompat::MetersToSkyrimUnits(65.0f) * RfabCompat::MetersToSkyrimUnits(65.0f);

            auto actorView = aWorld.view<FormIdComponent>();
            for (auto entity : actorView)
            {
                const uint32_t fid = actorView.get<FormIdComponent>(entity).Id;
                if (fid == 0 || fid == 0x14)
                    continue;
                Actor* pNpc = Cast<Actor>(TESForm::GetById(fid));
                if (!pNpc || pNpc->IsDead() || !pNpc->IsInCombat() || !acCombatRules.IsBossActor(pNpc, acResolver))
                    continue;

                const float dx = pNpc->position.x - pPlayer->position.x;
                const float dy = pNpc->position.y - pPlayer->position.y;
                const float dz = pNpc->position.z - pPlayer->position.z;
                const float dSq = dx * dx + dy * dy + dz * dz;
                if (dSq <= bestBossDistSq)
                {
                    bestBossDistSq = dSq;
                    pBestBoss = pNpc;
                    bestBossServerId = 0;
                    if (const auto* pLocal = aWorld.try_get<LocalComponent>(entity))
                        bestBossServerId = pLocal->Id;
                    else if (const auto* pRemote = aWorld.try_get<RemoteComponent>(entity))
                        bestBossServerId = pRemote->Id;
                }
            }

            if (pBestBoss)
            {
                const float maxHp = std::max(
                    pBestBoss->GetActorPermanentValue(ActorValueInfo::kHealth) + pBestBoss->healthModifiers.temporaryModifier,
                    1.0f);
                const float curHp = std::clamp(pBestBoss->GetActorValue(ActorValueInfo::kHealth), 0.0f, maxHp);
                const float hpRatio = std::clamp(curHp / maxHp, 0.0f, 1.0f);
                const bool isEtherealPhase =
                    pBestBoss->IsGhost() || acCombatRules.IsBossKnownEthereal(bestBossServerId);

                const char* bossName =
                    (pBestBoss->baseForm && pBestBoss->baseForm->GetName() && pBestBoss->baseForm->GetName()[0] != '\0')
                    ? pBestBoss->baseForm->GetName()
                    : "Босс RFAB";

                std::string targetPlayerName;
                Actor* pTarget = pBestBoss->GetCombatTarget();
                if (!pTarget && pBestBoss->pCombatController && pBestBoss->pCombatController->targetHandle != 0)
                    pTarget = Cast<Actor>(TESObjectREFR::GetByHandle(pBestBoss->pCombatController->targetHandle));
                if (pTarget)
                {
                    if (const auto itName = playerNameByFormId.find(pTarget->formID); itName != playerNameByFormId.end())
                        targetPlayerName = itName->second;
                    else if (pTarget->baseForm && pTarget->baseForm->GetName())
                        targetPlayerName = pTarget->baseForm->GetName();
                }

                aOutSnapshot.Boss.Active = true;
                aOutSnapshot.Boss.Name = bossName;
                aOutSnapshot.Boss.TargetPlayerName = std::move(targetPlayerName);
                aOutSnapshot.Boss.Level = pBestBoss->GetLevel();
                aOutSnapshot.Boss.CurHp = curHp;
                aOutSnapshot.Boss.MaxHp = maxHp;
                aOutSnapshot.Boss.HpRatio = hpRatio;
                aOutSnapshot.Boss.IsEtherealPhase = isEtherealPhase;
            }
        }

        auto computeActorVitalPct = [](Actor* apActor, int aAvIndex, float aTempMod) noexcept -> float
        {
            if (!apActor)
                return 0.0f;
            const float maxVal = apActor->GetActorPermanentValue(aAvIndex) + aTempMod;
            if (!std::isfinite(maxVal) || maxVal <= 0.001f)
                return 0.0f;
            const float curVal = apActor->GetActorValue(aAvIndex);
            if (!std::isfinite(curVal))
                return 0.0f;
            return std::clamp((curVal / maxVal) * 100.0f, 0.0f, 100.0f);
        };

        std::vector<uint32_t> displayPeerIds;
        const auto& partyMemberIds = partyService.GetPartyMembers();
        if (!partyMemberIds.empty())
        {
            for (uint32_t pid : partyMemberIds)
            {
                if (pid != aLocalPlayerId)
                    displayPeerIds.push_back(pid);
            }
        }
        else
        {
            for (const auto& [pid, peerState] : m_partyHudPeers)
            {
                (void)peerState;
                if (pid != aLocalPlayerId)
                    displayPeerIds.push_back(pid);
            }
            std::sort(displayPeerIds.begin(), displayPeerIds.end());
        }

        aOutSnapshot.PartyRows.reserve(1 + displayPeerIds.size());

        RfabPartyMemberHudSnapshotEntry selfRow{};
        selfRow.Name = (pPlayer->baseForm && pPlayer->baseForm->GetName() && pPlayer->baseForm->GetName()[0] != '\0')
            ? pPlayer->baseForm->GetName()
            : "Вы";
        selfRow.Level = pPlayer->GetLevel();
        selfRow.IsLeader = aLocalIsAllyProtectionHost;
        selfRow.IsDowned = aOutSnapshot.IsLocalPlayerDowned;
        selfRow.IsBeingRevived = aOutSnapshot.IsLocalPlayerBeingRevived;
        selfRow.IsGraceActive = aOutSnapshot.IsLocalPlayerInvulnerable;
        selfRow.IsInCombat = pPlayer->IsInCombat();
        selfRow.IsLoaded3D = true;
        selfRow.DistMeters = 0.0f;
        selfRow.HpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kHealth, pPlayer->healthModifiers.temporaryModifier);
        selfRow.MpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kMagicka, pPlayer->magickaModifiers.temporaryModifier);
        selfRow.SpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kStamina, pPlayer->staminaModifiers.temporaryModifier);
        aOutSnapshot.PartyRows.push_back(std::move(selfRow));

        for (uint32_t pid : displayPeerIds)
        {
            RfabPartyMemberHudSnapshotEntry row{};
            row.Name = "Союзник";
            row.CellName = "Скайрим";
            row.Level = 1;
            row.HpPct = 100.0f;
            row.MpPct = 100.0f;
            row.SpPct = 100.0f;

            if (const auto it = m_partyHudPeers.find(pid); it != m_partyHudPeers.end())
            {
                if (!it->second.Username.empty())
                    row.Name = it->second.Username;
                row.Level = it->second.Level;
                row.HpPct = it->second.HealthPercent;
                if (!it->second.CellName.empty())
                    row.CellName = it->second.CellName;
            }
            else if (const auto& partyPlayers = partyService.GetPlayers(); partyPlayers.contains(pid))
            {
                row.Name = std::string(partyPlayers.at(pid).c_str(), partyPlayers.at(pid).size());
            }

            row.IsLeader = partyService.IsInParty() && partyService.GetLeaderPlayerId() == pid;
            row.IsDowned = downedService.IsRemotePlayerDowned(pid);
            row.IsBeingRevived = downedService.IsPlayerBeingRevived(pid);
            row.IsGraceActive = false;

            if (const auto itAct = loadedActorByPlayerId.find(pid); itAct != loadedActorByPlayerId.end() && itAct->second)
            {
                Actor* pPeerActor = itAct->second;
                row.IsLoaded3D = true;
                const uint16_t liveLvl = pPeerActor->GetLevel();
                if (liveLvl > row.Level)
                    row.Level = liveLvl;
                row.HpPct = computeActorVitalPct(pPeerActor, ActorValueInfo::kHealth, pPeerActor->healthModifiers.temporaryModifier);
                row.MpPct = computeActorVitalPct(pPeerActor, ActorValueInfo::kMagicka, pPeerActor->magickaModifiers.temporaryModifier);
                row.SpPct = computeActorVitalPct(pPeerActor, ActorValueInfo::kStamina, pPeerActor->staminaModifiers.temporaryModifier);
                row.IsInCombat = pPeerActor->IsInCombat();

                const float dx = pPeerActor->position.x - pPlayer->position.x;
                const float dy = pPeerActor->position.y - pPlayer->position.y;
                const float dz = pPeerActor->position.z - pPlayer->position.z;
                row.DistMeters = RfabCompat::SkyrimUnitsToMeters(std::sqrt(dx * dx + dy * dy + dz * dz));
            }

            aOutSnapshot.PartyRows.push_back(std::move(row));
        }
    }
}

void RfabHudAndPings::DrawDownedBeacon(
    ImDrawList* apDrawList,
    const RfabDownedBeaconSnapshotEntry& acBeacon,
    const RfabCompat::ScreenMarkerProjection& acProj) noexcept
{
    if (!apDrawList)
        return;

    const ImU32 beaconCol = acBeacon.BeingRevived ? IM_COL32(61, 220, 132, 245) : IM_COL32(255, 58, 76, 245);
    const ImVec2 center(acProj.ScreenX, acProj.ScreenY);

    if (acProj.IsOnScreen)
    {
        const float r = 11.0f;
        const ImVec2 pts[4] = {
            ImVec2(center.x, center.y - r),
            ImVec2(center.x + r, center.y),
            ImVec2(center.x, center.y + r),
            ImVec2(center.x - r, center.y),
        };
        apDrawList->AddConvexPolyFilled(pts, 4, beaconCol);
        apDrawList->AddPolyline(pts, 4, IM_COL32(255, 235, 190, 240), ImDrawFlags_Closed, 2.0f);

        char line1[128]{};
        if (acBeacon.BeingRevived)
            std::snprintf(line1, sizeof(line1), "[+] РЕАНИМАЦИЯ: %s (%.0f м)", acBeacon.AllyName.c_str(), acBeacon.DistMeters);
        else if (acBeacon.DistMeters <= 3.2f)
            std::snprintf(line1, sizeof(line1), "[E] ПОДНЯТЬ: %s (%.1f м)", acBeacon.AllyName.c_str(), acBeacon.DistMeters);
        else
            std::snprintf(line1, sizeof(line1), "[!] В НОКАУТЕ: %s (%.0f м)", acBeacon.AllyName.c_str(), acBeacon.DistMeters);

        const ImVec2 textSz = ImGui::CalcTextSize(line1);
        const ImVec2 boxMin(center.x - textSz.x * 0.5f - 7.0f, center.y - r - textSz.y - 10.0f);
        const ImVec2 boxMax(center.x + textSz.x * 0.5f + 7.0f, center.y - r - 2.0f);
        apDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 12, 16, 215), 4.0f);
        apDrawList->AddRect(boxMin, boxMax, beaconCol, 4.0f, 0, 1.5f);
        DrawShadowedText(apDrawList, ImVec2(boxMin.x + 7.0f, boxMin.y + 3.0f), beaconCol, line1);
    }
    else
    {
        const ImVec2 tip(center.x + acProj.DirX * 14.0f, center.y + acProj.DirY * 14.0f);
        const ImVec2 perp(-acProj.DirY * 9.0f, acProj.DirX * 9.0f);
        const ImVec2 baseCenter(center.x - acProj.DirX * 6.0f, center.y - acProj.DirY * 6.0f);
        apDrawList->AddTriangleFilled(
            tip,
            ImVec2(baseCenter.x + perp.x, baseCenter.y + perp.y),
            ImVec2(baseCenter.x - perp.x, baseCenter.y - perp.y),
            beaconCol);

        char edgeLabel[96]{};
        std::snprintf(edgeLabel, sizeof(edgeLabel), "[!] %s (%.0fм)", acBeacon.AllyName.c_str(), acBeacon.DistMeters);
        const ImVec2 textSz = ImGui::CalcTextSize(edgeLabel);
        DrawShadowedText(apDrawList, ImVec2(center.x - textSz.x * 0.5f, center.y + 16.0f), beaconCol, edgeLabel);
    }
}

void RfabHudAndPings::DrawTacticalPing(
    ImDrawList* apDrawList,
    const RfabTacticalPingSnapshotEntry& acPing,
    const RfabCompat::ScreenMarkerProjection& acProj) noexcept
{
    if (!apDrawList)
        return;

    const float alphaFade = static_cast<float>(std::clamp(acPing.RemainingSec / 1.5, 0.25, 1.0));
    const uint8_t alphaByte = static_cast<uint8_t>(240.0f * alphaFade);
    const ImU32 pingCol = GetPingPrimaryColor(acPing.Kind, alphaByte);
    const ImVec2 center(acProj.ScreenX, acProj.ScreenY);

    if (acProj.IsOnScreen)
    {
        const float r = 9.5f;
        const ImVec2 pts[4] = {
            ImVec2(center.x, center.y - r),
            ImVec2(center.x + r, center.y),
            ImVec2(center.x, center.y + r),
            ImVec2(center.x - r, center.y),
        };
        apDrawList->AddConvexPolyFilled(pts, 4, pingCol);
        apDrawList->AddPolyline(pts, 4, IM_COL32(255, 255, 255, alphaByte), ImDrawFlags_Closed, 1.5f);

        char headerBuf[160]{};
        const char* badge = RfabCompat::GetTacticalPingBadgeAscii(acPing.Kind);
        const char* title = RfabCompat::GetTacticalPingTitleRu(acPing.Kind);
        if (!acPing.TargetLabel.empty())
            std::snprintf(headerBuf, sizeof(headerBuf), "%s %s: %s (%.0f м)", badge, title, acPing.TargetLabel.c_str(), acPing.DistMeters);
        else
            std::snprintf(headerBuf, sizeof(headerBuf), "%s %s (%.0f м)", badge, title, acPing.DistMeters);

        const ImVec2 textSz = ImGui::CalcTextSize(headerBuf);
        const ImVec2 boxMin(center.x - textSz.x * 0.5f - 7.0f, center.y - r - textSz.y - 12.0f);
        const ImVec2 boxMax(center.x + textSz.x * 0.5f + 7.0f, center.y - r - 2.0f);
        apDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 17, 24, static_cast<uint8_t>(210 * alphaFade)), 4.0f);
        apDrawList->AddRect(boxMin, boxMax, pingCol, 4.0f, 0, 1.2f);
        DrawShadowedText(apDrawList, ImVec2(boxMin.x + 7.0f, boxMin.y + 2.0f), pingCol, headerBuf);

        const float lifeFrac = (acPing.TotalDurationSec > 0.1)
            ? static_cast<float>(std::clamp(acPing.RemainingSec / acPing.TotalDurationSec, 0.0, 1.0))
            : 0.0f;
        const float barW = boxMax.x - boxMin.x - 4.0f;
        apDrawList->AddRectFilled(
            ImVec2(boxMin.x + 2.0f, boxMax.y - 3.0f),
            ImVec2(boxMin.x + 2.0f + barW * lifeFrac, boxMax.y - 1.0f),
            pingCol);
    }
    else
    {
        const ImVec2 tip(center.x + acProj.DirX * 12.0f, center.y + acProj.DirY * 12.0f);
        const ImVec2 perp(-acProj.DirY * 7.5f, acProj.DirX * 7.5f);
        const ImVec2 baseCenter(center.x - acProj.DirX * 5.0f, center.y - acProj.DirY * 5.0f);
        apDrawList->AddTriangleFilled(
            tip,
            ImVec2(baseCenter.x + perp.x, baseCenter.y + perp.y),
            ImVec2(baseCenter.x - perp.x, baseCenter.y - perp.y),
            pingCol);

        char edgeBuf[64]{};
        std::snprintf(edgeBuf, sizeof(edgeBuf), "%s %.0fм", RfabCompat::GetTacticalPingBadgeAscii(acPing.Kind), acPing.DistMeters);
        const ImVec2 textSz = ImGui::CalcTextSize(edgeBuf);
        DrawShadowedText(apDrawList, ImVec2(center.x - textSz.x * 0.5f, center.y + 13.0f), pingCol, edgeBuf);
    }
}

void RfabHudAndPings::DrawOverlayPanels(
    ImDrawList* apDrawList,
    const RfabTacticalHudRenderSnapshot& acSnapshot,
    float aViewW,
    float aViewH) noexcept
{
    if (!apDrawList)
        return;

    // 1. Center-Screen Revive Channeling / Downed / Grace Progress Bar
    if (acSnapshot.IsChannelingRevive)
    {
        const float progress = acSnapshot.ReviveChannelProgress;
        const float barW = 300.0f;
        const float barH = 22.0f;
        const ImVec2 barMin((aViewW - barW) * 0.5f, aViewH * 0.62f);
        const ImVec2 barMax(barMin.x + barW, barMin.y + barH);

        apDrawList->AddRectFilled(barMin, barMax, IM_COL32(14, 18, 22, 225), 5.0f);
        apDrawList->AddRectFilled(
            ImVec2(barMin.x + 2.0f, barMin.y + 2.0f),
            ImVec2(barMin.x + 2.0f + (barW - 4.0f) * progress, barMax.y - 2.0f),
            IM_COL32(61, 220, 132, 230),
            4.0f);
        apDrawList->AddRect(barMin, barMax, IM_COL32(196, 235, 200, 220), 5.0f, 0, 1.5f);

        char revText[128]{};
        std::snprintf(revText, sizeof(revText), "РЕАНИМАЦИЯ СОЮЗНИКА [E]: %.0f%%", progress * 100.0f);
        const ImVec2 tSz = ImGui::CalcTextSize(revText);
        DrawShadowedText(
            apDrawList,
            ImVec2(barMin.x + (barW - tSz.x) * 0.5f, barMin.y + (barH - tSz.y) * 0.5f),
            IM_COL32(255, 255, 255, 250),
            revText);
    }
    else if (acSnapshot.IsLocalPlayerDowned)
    {
        const char* downedBanner = acSnapshot.IsLocalPlayerBeingRevived
            ? "[+] СОЮЗНИК ПОДНИМАЕТ ВАС НА НОГИ..."
            : "[!] ВЫ ТЯЖЕЛО РАНЕНЫ (НА КОЛЕНЕ) - ОЖИДАНИЕ ПОМОЩИ ОТРЯДА [E]";
        const ImU32 bannerCol =
            acSnapshot.IsLocalPlayerBeingRevived ? IM_COL32(61, 220, 132, 245) : IM_COL32(255, 72, 82, 245);
        const ImVec2 tSz = ImGui::CalcTextSize(downedBanner);
        const ImVec2 boxMin((aViewW - tSz.x) * 0.5f - 14.0f, aViewH * 0.62f);
        const ImVec2 boxMax((aViewW + tSz.x) * 0.5f + 14.0f, boxMin.y + tSz.y + 10.0f);
        apDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 12, 16, 225), 5.0f);
        apDrawList->AddRect(boxMin, boxMax, bannerCol, 5.0f, 0, 1.5f);
        DrawShadowedText(apDrawList, ImVec2(boxMin.x + 14.0f, boxMin.y + 5.0f), bannerCol, downedBanner);
    }
    else if (acSnapshot.IsLocalPlayerInvulnerable && acSnapshot.GraceRemainingSec > 0.05)
    {
        char graceBuf[96]{};
        std::snprintf(graceBuf, sizeof(graceBuf), "ЩИТ ПОСЛЕ ПОДЪЁМА: %.1f с", acSnapshot.GraceRemainingSec);
        const ImVec2 tSz = ImGui::CalcTextSize(graceBuf);
        const ImVec2 boxMin((aViewW - tSz.x) * 0.5f - 10.0f, aViewH * 0.62f);
        const ImVec2 boxMax((aViewW + tSz.x) * 0.5f + 10.0f, boxMin.y + tSz.y + 8.0f);
        apDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 24, 34, 210), 4.0f);
        apDrawList->AddRect(boxMin, boxMax, IM_COL32(71, 209, 255, 220), 4.0f, 0, 1.2f);
        DrawShadowedText(apDrawList, ImVec2(boxMin.x + 10.0f, boxMin.y + 4.0f), IM_COL32(165, 238, 255, 245), graceBuf);
    }

    if (acSnapshot.Mode == RfabCompat::TacticalHudMode::MarkersOnly)
        return;

    // 2. Top-Center Active Boss Encounter Health & Phase Bar
    if (acSnapshot.Boss.Active)
    {
        const auto& boss = acSnapshot.Boss;
        const float bossBarW = std::clamp(aViewW * 0.34f, 380.0f, 620.0f);
        const float bossBarH = 26.0f;
        const ImVec2 bossMin((aViewW - bossBarW) * 0.5f, 26.0f);
        const ImVec2 bossMax(bossMin.x + bossBarW, bossMin.y + bossBarH);

        char bossTitle[256]{};
        if (boss.IsEtherealPhase)
            std::snprintf(bossTitle, sizeof(bossTitle), "%s [Ур. %u] - [ЭФИРНАЯ ФАЗА / НЕУЯЗВИМ]", boss.Name.c_str(), boss.Level);
        else if (!boss.TargetPlayerName.empty())
            std::snprintf(bossTitle, sizeof(bossTitle), "%s [Ур. %u]  |  Цель: %s", boss.Name.c_str(), boss.Level, boss.TargetPlayerName.c_str());
        else
            std::snprintf(bossTitle, sizeof(bossTitle), "%s [Ур. %u]", boss.Name.c_str(), boss.Level);

        const ImVec2 titleSz = ImGui::CalcTextSize(bossTitle);
        DrawShadowedText(
            apDrawList,
            ImVec2(bossMin.x + (bossBarW - titleSz.x) * 0.5f, bossMin.y - titleSz.y - 4.0f),
            boss.IsEtherealPhase ? IM_COL32(115, 230, 255, 250) : IM_COL32(255, 224, 155, 250),
            bossTitle);

        apDrawList->AddRectFilled(bossMin, bossMax, IM_COL32(16, 14, 18, 225), 5.0f);
        const ImU32 fillCol = boss.IsEtherealPhase ? IM_COL32(65, 195, 235, 225) : IM_COL32(198, 38, 48, 235);
        apDrawList->AddRectFilled(
            ImVec2(bossMin.x + 2.0f, bossMin.y + 2.0f),
            ImVec2(bossMin.x + 2.0f + (bossBarW - 4.0f) * boss.HpRatio, bossMax.y - 2.0f),
            fillCol,
            4.0f);
        apDrawList->AddRect(
            bossMin,
            bossMax,
            boss.IsEtherealPhase ? IM_COL32(95, 225, 255, 240) : IM_COL32(215, 175, 95, 225),
            5.0f,
            0,
            1.6f);

        char hpText[96]{};
        std::snprintf(hpText, sizeof(hpText), "%.0f / %.0f (%.0f%%)", boss.CurHp, boss.MaxHp, boss.HpRatio * 100.0f);
        const ImVec2 hpSz = ImGui::CalcTextSize(hpText);
        DrawShadowedText(
            apDrawList,
            ImVec2(bossMin.x + (bossBarW - hpSz.x) * 0.5f, bossMin.y + (bossBarH - hpSz.y) * 0.5f),
            IM_COL32(255, 255, 255, 245),
            hpText);
    }

    // 3. Top-Left Co-Op Party Tactical HUD Panel
    if (acSnapshot.PartyRows.empty())
        return;

    const bool fullMode = (acSnapshot.Mode == RfabCompat::TacticalHudMode::Full);
    const float panelX = 18.0f;
    const float panelY = 18.0f;
    const float panelW = 292.0f;
    const float rowH = fullMode ? 54.0f : 38.0f;
    const size_t totalRows = acSnapshot.PartyRows.size();
    const float panelH = 34.0f + static_cast<float>(totalRows) * rowH + 6.0f;

    apDrawList->AddRectFilled(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(14, 16, 22, 200),
        6.0f);
    apDrawList->AddRect(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(180, 152, 92, 185),
        6.0f,
        0,
        1.2f);

    char headerText[128]{};
    std::snprintf(
        headerText,
        sizeof(headerText),
        "ОТРЯД RFAB (%zu)  |  %s",
        totalRows,
        acSnapshot.AllyProtectionEnabled ? "ЕДИНСТВО: ВКЛ" : "УРОН ПО СВОИМ");
    DrawShadowedText(
        apDrawList,
        ImVec2(panelX + 10.0f, panelY + 7.0f),
        acSnapshot.AllyProtectionEnabled ? IM_COL32(235, 210, 145, 245) : IM_COL32(255, 110, 110, 245),
        headerText);

    for (size_t i = 0; i < acSnapshot.PartyRows.size(); ++i)
    {
        const auto& row = acSnapshot.PartyRows[i];
        const float y0 = panelY + 32.0f + static_cast<float>(i) * rowH;
        const float x0 = panelX + 10.0f;
        const float innerW = panelW - 20.0f;

        char lineBuf[192]{};
        const char* leaderPrefix = row.IsLeader ? "[Л] " : "";
        if (row.IsDowned)
        {
            std::snprintf(
                lineBuf,
                sizeof(lineBuf),
                "%s%s [Ур.%u] - %s",
                leaderPrefix,
                row.Name.c_str(),
                row.Level,
                row.IsBeingRevived ? "РЕАНИМАЦИЯ..." : "В НОКАУТЕ!");
        }
        else if (row.IsGraceActive)
        {
            std::snprintf(lineBuf, sizeof(lineBuf), "%s%s [Ур.%u] - ЩИТ", leaderPrefix, row.Name.c_str(), row.Level);
        }
        else if (i == 0)
        {
            std::snprintf(
                lineBuf,
                sizeof(lineBuf),
                "%s%s [Ур.%u]%s",
                leaderPrefix,
                row.Name.c_str(),
                row.Level,
                row.IsInCombat ? " | В БОЮ" : "");
        }
        else if (row.IsLoaded3D)
        {
            std::snprintf(
                lineBuf,
                sizeof(lineBuf),
                "%s%s [Ур.%u] | %.0f м%s",
                leaderPrefix,
                row.Name.c_str(),
                row.Level,
                row.DistMeters,
                row.IsInCombat ? " | В БОЮ" : "");
        }
        else
        {
            std::snprintf(
                lineBuf,
                sizeof(lineBuf),
                "%s%s [Ур.%u] | %s",
                leaderPrefix,
                row.Name.c_str(),
                row.Level,
                !row.CellName.empty() ? row.CellName.c_str() : "Другая зона");
        }

        const ImU32 nameCol = row.IsDowned
            ? IM_COL32(255, 86, 96, 250)
            : (row.IsBeingRevived ? IM_COL32(75, 235, 145, 250) : IM_COL32(232, 236, 242, 245));
        DrawShadowedText(apDrawList, ImVec2(x0, y0), nameCol, lineBuf);

        const float hpY = y0 + 20.0f;
        const float hpH = 10.0f;
        const float clampedHp = std::clamp(row.HpPct, 0.0f, 100.0f) * 0.01f;
        apDrawList->AddRectFilled(ImVec2(x0, hpY), ImVec2(x0 + innerW, hpY + hpH), IM_COL32(32, 18, 22, 220), 3.0f);
        const ImU32 hpFill = row.IsDowned
            ? IM_COL32(255, 52, 62, 235)
            : (row.IsGraceActive ? IM_COL32(71, 209, 255, 230) : IM_COL32(198, 42, 52, 235));
        apDrawList->AddRectFilled(
            ImVec2(x0 + 1.0f, hpY + 1.0f),
            ImVec2(x0 + 1.0f + (innerW - 2.0f) * clampedHp, hpY + hpH - 1.0f),
            hpFill,
            2.5f);

        if (fullMode && row.IsLoaded3D)
        {
            const float subY = hpY + hpH + 3.0f;
            const float subH = 6.0f;
            const float halfW = (innerW - 6.0f) * 0.5f;
            const float clampedMp = std::clamp(row.MpPct, 0.0f, 100.0f) * 0.01f;
            const float clampedSp = std::clamp(row.SpPct, 0.0f, 100.0f) * 0.01f;

            apDrawList->AddRectFilled(ImVec2(x0, subY), ImVec2(x0 + halfW, subY + subH), IM_COL32(18, 24, 38, 215), 2.0f);
            apDrawList->AddRectFilled(
                ImVec2(x0 + 1.0f, subY + 1.0f),
                ImVec2(x0 + 1.0f + (halfW - 2.0f) * clampedMp, subY + subH - 1.0f),
                IM_COL32(52, 128, 235, 230),
                2.0f);

            const float spX = x0 + halfW + 6.0f;
            apDrawList->AddRectFilled(ImVec2(spX, subY), ImVec2(spX + halfW, subY + subH), IM_COL32(18, 34, 22, 215), 2.0f);
            apDrawList->AddRectFilled(
                ImVec2(spX + 1.0f, subY + 1.0f),
                ImVec2(spX + 1.0f + (halfW - 2.0f) * clampedSp, subY + subH - 1.0f),
                IM_COL32(48, 188, 92, 230),
                2.0f);
        }
    }
}
