#include <TiltedOnlinePCH.h>

#include <Services/CoordinatedSaveService.h>

#include <World.h>
#include <Utils.h>
#include <common/RfabModpackCompat.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Games/Overrides.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyCommitSave.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Messages/NotifyPrepareSave.h>
#include <Messages/RequestCoordinatedSaveLoad.h>
#include <PlayerCharacter.h>
#include <SaveLoad.h>
#include <Services/CharacterService.h>
#include <Services/DownedStateService.h>
#include <Services/PartyService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>

namespace
{
bool IsAutosaveName(std::string_view aName) noexcept
{
    return RfabCompat::Detail::StartsWithIgnoreCase(aName, "autosave") ||
           RfabCompat::Detail::ToLowerAscii(aName).find("autosave") != std::string::npos;
}
} // namespace

CoordinatedSaveService::CoordinatedSaveService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
    , m_connectedConnection(aDispatcher.sink<ConnectedEvent>().connect<&CoordinatedSaveService::OnConnected>(this))
    , m_disconnectedConnection(aDispatcher.sink<DisconnectedEvent>().connect<&CoordinatedSaveService::OnDisconnected>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&CoordinatedSaveService::OnUpdate>(this))
    , m_prepareSaveConnection(aDispatcher.sink<NotifyPrepareSave>().connect<&CoordinatedSaveService::OnNotifyPrepareSave>(this))
    , m_commitSaveConnection(aDispatcher.sink<NotifyCommitSave>().connect<&CoordinatedSaveService::OnNotifyCommitSave>(this))
    , m_executeLoadConnection(aDispatcher.sink<NotifyExecuteLoad>().connect<&CoordinatedSaveService::OnNotifyExecuteLoad>(this))
{
}

bool CoordinatedSaveService::IsLocalPlayerAuthorizedLeader() const noexcept
{
    const auto& partyService = m_world.GetPartyService();
    if (partyService.IsInParty())
        return partyService.IsLeader();

    if (m_world.GetRfabAdapterService().IsLocalPlayerAllyProtectionHost())
        return true;

    const uint32_t localId = m_transport.GetLocalPlayerId();
    for (const auto& [peerId, _] : partyService.GetPlayers())
    {
        if (peerId != 0 && (localId == 0 || peerId < localId))
            return false;
    }

    return true;
}

bool CoordinatedSaveService::IsLoadingMenuOpen() const noexcept
{
    if (auto* pUi = UI::Get())
    {
        return pUi->GetMenuOpen(BSFixedString("Loading Menu")) ||
               pUi->GetMenuOpen(BSFixedString("Main Menu"));
    }
    return false;
}

bool CoordinatedSaveService::CanSafelySaveNow(std::string& aOutReason) const noexcept
{
    if (IsLoadingMenuOpen())
    {
        aOutReason = "экран загрузки активен";
        return false;
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || pPlayer->IsDead() || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
    {
        aOutReason = "персонаж недоступен";
        return false;
    }

    if (m_world.GetDownedStateService().IsLocalPlayerDowned())
    {
        aOutReason = "персонаж тяжело ранен";
        return false;
    }

    return true;
}

void CoordinatedSaveService::QueuePurgedAuthorizedSave(const std::string& acSlotName, uint64_t aEpochId, bool aSendServerAck) noexcept
{
    const std::string cleanStem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(acSlotName);
    if (cleanStem.empty())
    {
        if (aSendServerAck && m_transport.IsConnected())
        {
            CoordinatedSaveAck ack{};
            ack.EpochId = aEpochId;
            ack.SlotName = String(acSlotName.c_str());
            ack.Success = false;
            ack.Reason = String("некорректное имя слота");
            m_transport.Send(ack);
        }
        return;
    }

    m_pendingSaveSlot = cleanStem;
    m_pendingSaveEpochId = aEpochId;
    m_pendingSaveSendAck = aSendServerAck;
    // Dynamic 0xFFxxxxxx multiplayer actors stay alive in-world and are excluded from .ess non-destructively
    // via SetSkipSaveFlag(true) (unk10 = 0xFFFF) + UnsetChanged(0xFFFFFFFF), so Save_Impl can run on the very next
    // BGSSaveLoadManager::ProcessEvents tick on the Main Thread without deleting/respawning remote avatars.
    m_pendingSavePurgeFrames = 0;

    auto& charSvc = m_world.GetCharacterService();
    charSvc.PreSavePurgeDynamicActors();
    charSvc.SchedulePostSaveRemoteRestore();
}

void CoordinatedSaveService::ProcessReadyDeferredSaveOrLoad(BGSSaveLoadManager* apMgr) noexcept
{
    if (!apMgr)
        return;

    if (!m_pendingSaveSlot.empty() && m_pendingSavePurgeFrames == 0 && !IsLoadingMenuOpen())
    {
        const std::string slot = std::move(m_pendingSaveSlot);
        const uint64_t epochId = m_pendingSaveEpochId;
        const bool sendAck = m_pendingSaveSendAck;
        m_pendingSaveSlot.clear();
        m_pendingSaveEpochId = 0;
        m_pendingSaveSendAck = false;

        std::string failReason;
        bool saveOk = false;
        auto& charSvc = m_world.GetCharacterService();

        if (CanSafelySaveNow(failReason))
        {
            charSvc.PreSavePurgeDynamicActors();
            m_authorizedPendingSaveSlot = RfabCompat::Detail::NormalizeSaveSlotStem(slot);
            ScopedSaveLoadAllowance guard;
            saveOk = apMgr->ExecuteDirectSaveFromProcessEvents(slot.c_str());
            m_authorizedPendingSaveSlot.clear();
        }

        // Only restore purged remote clones AFTER Save_Impl has completely finished writing .ess to disk!
        charSvc.SchedulePostSaveRemoteRestore();

        if (saveOk)
        {
            m_writtenSlotsInSession.insert(slot);
            m_writtenSlotsInSession.insert(RfabCompat::Detail::NormalizeSaveSlotStem(slot));
        }

        if (sendAck && m_transport.IsConnected())
        {
            CoordinatedSaveAck ack{};
            ack.EpochId = epochId;
            ack.SlotName = String(slot.c_str());
            ack.Success = saveOk;
            if (!saveOk)
                ack.Reason = String(failReason.empty() ? "не удалось записать или проверить .ess слот" : failReason.c_str());
            m_transport.Send(ack);
        }
        return;
    }

    if (!m_deferredLoadSlot.empty() && !IsLoadingMenuOpen())
    {
        const std::string slot = std::move(m_deferredLoadSlot);
        m_deferredLoadSlot.clear();

        m_authorizedPendingLoadSlot = RfabCompat::Detail::NormalizeSaveSlotStem(slot);
        ScopedSaveLoadAllowance guard;
        const bool loadOk = apMgr->ExecuteDirectLoadFromProcessEvents(slot.c_str(), false);
        m_authorizedPendingLoadSlot.clear();

        if (!loadOk)
        {
            m_autoReconnectPendingAfterLoad = false;
            spdlog::error("[RFAB SaveLoad] ExecuteDirectLoadFromProcessEvents failed for slot '{}'", slot);
            Utils::ShowHudMessage(String("RFAB Online: Не удалось загрузить слот чекпоинта"));
        }
    }
}

void CoordinatedSaveService::PrepareEngineForWorldLoad(const char* acpFileName) noexcept
{
    (void)acpFileName;
    m_pendingSaveSlot.clear();
    m_pendingSaveSendAck = false;
    m_pendingSavePurgeFrames = 0;

    m_world.GetDownedStateService().ResetAllStates();
    m_world.GetRfabAdapterService().NotifySaveLoadReset();
    m_world.GetCharacterService().BeginEngineWorldLoad();

    if (m_transport.IsConnected() && m_transport.HasLastServerEndpoint())
    {
        m_autoReconnectPendingAfterLoad = true;
        m_sawLoadingScreenForReconnect = false;
        m_autoReconnectDelaySec = 1.5;
        m_transport.Close();
    }
}

void CoordinatedSaveService::NotifyEngineSaveCompleted(const char* acpFileName) noexcept
{
    const std::string norm = RfabCompat::Detail::NormalizeSaveSlotStem(acpFileName ? acpFileName : "");
    if (!m_authorizedPendingSaveSlot.empty() && norm == m_authorizedPendingSaveSlot)
        m_authorizedPendingSaveSlot.clear();
}

void CoordinatedSaveService::NotifyEngineLoadCompleted(const char* acpFileName, bool aSucceeded) noexcept
{
    m_world.GetCharacterService().EndEngineWorldLoad();

    const std::string norm = RfabCompat::Detail::NormalizeSaveSlotStem(acpFileName ? acpFileName : "");
    if (!m_authorizedPendingLoadSlot.empty() && norm == m_authorizedPendingLoadSlot)
        m_authorizedPendingLoadSlot.clear();

    if (!aSucceeded)
        m_autoReconnectPendingAfterLoad = false;
}

bool CoordinatedSaveService::HandleEngineSaveAttempt(const char* acpFileName) noexcept
{
    const std::string rawName = acpFileName ? acpFileName : "";
    const std::string normName = RfabCompat::Detail::NormalizeSaveSlotStem(rawName);
    if (!m_authorizedPendingSaveSlot.empty() && normName == m_authorizedPendingSaveSlot)
        return true;

    if (ScopedSaveLoadAllowance::IsAllowed())
        return true;

    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
        return true;

    if (IsAutosaveName(rawName))
        return false;

    if (m_lastInterceptNoticeCooldownSec <= 0.0)
    {
        m_lastInterceptNoticeCooldownSec = 1.5;
        RequestCoordinatedSaveLoad req{};
        req.Action = RequestCoordinatedSaveLoad::ActionType::ManualSave;
        m_transport.Send(req);
        Utils::ShowHudMessage(String("RFAB Online: Синхронное сохранение всех игроков группы..."));
    }

    return false;
}

bool CoordinatedSaveService::HandleEngineLoadAttempt(const char* acpFileName) noexcept
{
    const std::string rawName = acpFileName ? acpFileName : "";
    const std::string normName = RfabCompat::Detail::NormalizeSaveSlotStem(rawName);
    if (!m_authorizedPendingLoadSlot.empty() && normName == m_authorizedPendingLoadSlot)
        return true;

    if (ScopedSaveLoadAllowance::IsAllowed())
        return true;

    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
        return true;

    if (IsLocalPlayerAuthorizedLeader())
    {
        if (m_lastInterceptNoticeCooldownSec <= 0.0)
        {
            m_lastInterceptNoticeCooldownSec = 1.5;
            RequestCoordinatedSaveLoad req{};
            const std::string cleanStem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(rawName);
            if (RfabCompat::Detail::StartsWithIgnoreCase(cleanStem, "RFAB_MP_"))
            {
                req.Action = RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot;
                req.SlotName = String(cleanStem.c_str());
            }
            else
            {
                req.Action = RequestCoordinatedSaveLoad::ActionType::LoadLatestCheckpoint;
            }
            m_transport.Send(req);
            Utils::ShowHudMessage(String("RFAB Online: Общая загрузка чекпоинта для всех игроков..."));
        }
    }
    else if (m_lastInterceptNoticeCooldownSec <= 0.0)
    {
        m_lastInterceptNoticeCooldownSec = 1.5;
        Utils::ShowHudMessage(String("RFAB Online: В кооперативе общую загрузку чекпоинта запускает лидер отряда (F9)"));
    }

    return false;
}

void CoordinatedSaveService::OnConnected(const ConnectedEvent&) noexcept
{
    m_pendingSaveSlot.clear();
    m_pendingSaveEpochId = 0;
    m_pendingSaveSendAck = false;
    m_pendingSavePurgeFrames = 0;
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
    m_autoReconnectPendingAfterLoad = false;
    m_sawLoadingScreenForReconnect = false;
    m_partyCheckpointSettleSec = 0.0;

    const std::string normBackup = RfabCompat::Detail::NormalizeSaveSlotStem(m_preJoinBackupSlot);
    if (!m_writtenSlotsInSession.contains(normBackup) && !BGSSaveLoadManager::VerifySavedSlotOnDisk(m_preJoinBackupSlot.c_str()))
    {
        m_pendingPreJoinBackup = true;
        m_preJoinBackupDelaySec = 3.5;
    }
    else
    {
        m_writtenSlotsInSession.insert(m_preJoinBackupSlot);
        m_writtenSlotsInSession.insert(normBackup);
    }
}

void CoordinatedSaveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingPreJoinBackup = false;
    m_initialPartyCheckpointSent = false;
    m_partyCheckpointSettleSec = 0.0;
    if (!m_pendingSaveSlot.empty())
    {
        m_pendingSaveSlot.clear();
        m_pendingSaveEpochId = 0;
        m_pendingSaveSendAck = false;
        m_pendingSavePurgeFrames = 0;
        m_world.GetCharacterService().SchedulePostSaveRemoteRestore();
    }
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
}

void CoordinatedSaveService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    BGSSaveLoadManager::InstallUniversalPostSkseHooks();

    if (m_lastInterceptNoticeCooldownSec > 0.0)
        m_lastInterceptNoticeCooldownSec = std::max(0.0, m_lastInterceptNoticeCooldownSec - acEvent.Delta);

    if (!m_pendingSaveSlot.empty() && m_pendingSavePurgeFrames > 0 && !IsLoadingMenuOpen())
        --m_pendingSavePurgeFrames;

    if (m_autoReconnectPendingAfterLoad && !m_transport.IsConnected())
    {
        if (IsLoadingMenuOpen())
        {
            m_sawLoadingScreenForReconnect = true;
            return;
        }

        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (pPlayer && (pPlayer->parentCell || pPlayer->GetWorldSpace()))
        {
            m_autoReconnectDelaySec -= acEvent.Delta;
            if (m_autoReconnectDelaySec <= 0.0)
            {
                m_autoReconnectPendingAfterLoad = false;
                m_sawLoadingScreenForReconnect = false;
                Utils::ShowHudMessage(String("RFAB Online: Автоматическое переподключение к сессии..."));
                m_transport.ReconnectToLastServer();
            }
        }
        return;
    }

    if (!m_transport.IsConnected())
        return;

    if (m_pendingPreJoinBackup && m_pendingSaveSlot.empty() && !IsLoadingMenuOpen())
    {
        m_preJoinBackupDelaySec -= acEvent.Delta;
        if (m_preJoinBackupDelaySec <= 0.0)
        {
            std::string reason;
            if (CanSafelySaveNow(reason))
            {
                m_pendingPreJoinBackup = false;
                QueuePurgedAuthorizedSave(m_preJoinBackupSlot, 0, false);
            }
        }
    }

    if (!m_initialPartyCheckpointSent &&
        m_lastCommittedEpoch == 0 &&
        m_pendingSaveSlot.empty() &&
        !IsLoadingMenuOpen() &&
        IsLocalPlayerAuthorizedLeader() &&
        m_world.GetPartyService().GetPlayers().size() >= 2)
    {
        m_partyCheckpointSettleSec += acEvent.Delta;
        if (m_partyCheckpointSettleSec >= 4.0)
        {
            std::string reason;
            if (CanSafelySaveNow(reason))
            {
                m_initialPartyCheckpointSent = true;
                RequestCoordinatedSaveLoad req{};
                req.Action = RequestCoordinatedSaveLoad::ActionType::ManualSave;
                m_transport.Send(req);
            }
        }
    }
}

void CoordinatedSaveService::OnNotifyPrepareSave(const NotifyPrepareSave& acMessage) noexcept
{
    const std::string slotName = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
        std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));
    std::string failReason;

    if (!CanSafelySaveNow(failReason))
    {
        CoordinatedSaveAck ack{};
        ack.EpochId = acMessage.EpochId;
        ack.SlotName = acMessage.SlotName;
        ack.Success = false;
        ack.Reason = String(failReason.c_str());
        m_transport.Send(ack);
        return;
    }

    QueuePurgedAuthorizedSave(slotName, acMessage.EpochId, true);
}

void CoordinatedSaveService::OnNotifyCommitSave(const NotifyCommitSave& acMessage) noexcept
{
    if (acMessage.Committed)
    {
        m_lastCommittedEpoch = acMessage.EpochId;
        m_lastCommittedSlot = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
            std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));
    }

    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);
}

void CoordinatedSaveService::OnNotifyExecuteLoad(const NotifyExecuteLoad& acMessage) noexcept
{
    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);

    std::string targetSlot = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
        std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));

    auto isUsableSlot = [&](const std::string& aCandidate) -> bool
    {
        if (aCandidate.empty())
            return false;
        return BGSSaveLoadManager::VerifySavedSlotOnDisk(aCandidate.c_str());
    };

    if (!isUsableSlot(targetSlot) && acMessage.AllowPreJoinFallback)
    {
        if (isUsableSlot(m_lastCommittedSlot))
            targetSlot = m_lastCommittedSlot;
        else if (isUsableSlot(m_preJoinBackupSlot))
            targetSlot = m_preJoinBackupSlot;
        else
            targetSlot.clear();
    }
    else if (!isUsableSlot(targetSlot))
    {
        targetSlot.clear();
    }

    if (targetSlot.empty())
    {
        spdlog::warn("[RFAB SaveLoad] No valid .ess checkpoint found on disk for NotifyExecuteLoad (PartyWipe={})", acMessage.IsPartyWipe);
        if (acMessage.IsPartyWipe)
        {
            m_world.GetDownedStateService().ApplyAuthoritativeRevive(0.30f, 5.0f, false);
            Utils::ShowHudMessage(String("RFAB Online: Чекпоинт на диске отсутствует — выполнено аварийное восстановление"));
        }
        return;
    }

    // Queue for clean execution inside BGSSaveLoadManager::ProcessEvents on the next engine frame.
    m_deferredLoadSlot = targetSlot;
}
