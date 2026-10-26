#include <TiltedOnlinePCH.h>

#include <Services/CoordinatedSaveService.h>

#include <World.h>
#include <Utils.h>
#include <common/CoopCheckpointStore.h>
#include <common/CoopOwnership.h>
#include <common/RfabModpackCompat.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/Overrides.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyCommitSave.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Messages/NotifyPrepareSave.h>
#include <Messages/RequestCoordinatedSaveLoad.h>
#include <PlayerCharacter.h>
#include <SaveLoad.h>
#include <Services/CharacterService.h>
#include <Services/DiscoveryService.h>
#include <Services/DownedStateService.h>
#include <Services/PartyService.h>
#include <Services/RfabAdapterService.h>
#include <Services/TransportService.h>

#include <array>
#include <cmath>

namespace
{
bool IsAutosaveName(std::string_view aName) noexcept
{
    return RfabCompat::Detail::StartsWithIgnoreCase(aName, "autosave") ||
           RfabCompat::Detail::ToLowerAscii(aName).find("autosave") != std::string::npos;
}

struct CachedSaveBlockedMenus
{
    std::array<BSFixedString, 8> HardBlocked{
        BSFixedString("Dialogue Menu"),
        BSFixedString("Lockpicking Menu"),
        BSFixedString("BarterMenu"),
        BSFixedString("ContainerMenu"),
        BSFixedString("Crafting Menu"),
        BSFixedString("RaceSex Menu"),
        BSFixedString("Sleep/Wait Menu"),
        BSFixedString("Training Menu"),
    };
    BSFixedString LoadingMenu{"Loading Menu"};
    BSFixedString MainMenu{"Main Menu"};
    BSFixedString JournalMenu{"Journal Menu"};
    BSFixedString TweenMenu{"TweenMenu"};
};

const CachedSaveBlockedMenus& GetSaveMenus() noexcept
{
    static const CachedSaveBlockedMenus* s_pMenus = new CachedSaveBlockedMenus();
    return *s_pMenus;
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
        const auto& menus = GetSaveMenus();
        return pUi->GetMenuOpen(menus.LoadingMenu) || pUi->GetMenuOpen(menus.MainMenu);
    }
    return false;
}

bool CoordinatedSaveService::IsBlockingInteractiveMenuOpen(std::string& aOutReason) const noexcept
{
    if (auto* pUi = UI::Get())
    {
        const auto& menus = GetSaveMenus();
        for (const auto& menuName : menus.HardBlocked)
        {
            if (pUi->GetMenuOpen(menuName))
            {
                aOutReason = "открыто интерактивное меню";
                return true;
            }
        }
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
    if (!pPlayer || pPlayer->IsDead() || pPlayer->IsStillLoading() || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
    {
        aOutReason = "персонаж недоступен";
        return false;
    }

    if (m_world.GetDownedStateService().IsLocalPlayerDowned())
    {
        aOutReason = "персонаж тяжело ранен";
        return false;
    }

    if (pPlayer->IsInCombat())
    {
        aOutReason = "персонаж в бою";
        return false;
    }

    if (IsBlockingInteractiveMenuOpen(aOutReason))
        return false;

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

    if (auto* pUi = UI::Get())
    {
        const auto& menus = GetSaveMenus();
        if (pUi->GetMenuOpen(menus.JournalMenu) || pUi->GetMenuOpen(menus.TweenMenu))
            pUi->CloseAllMenus();
    }

    m_pendingSaveSlot = cleanStem;
    m_pendingSaveEpochId = aEpochId;
    m_pendingSaveSendAck = aSendServerAck;
    // Allow 2 calm main-thread frames after marking all 0xFFxxxxxx dynamic actors & pooled TESNPCs
    // skip-save + UnsetChanged(0xFFFFFFFF) before executing Save_Impl inside BGSSaveLoadManager::ProcessEvents.
    m_pendingSavePurgeFrames = 2;

    auto& charSvc = m_world.GetCharacterService();
    charSvc.PreSavePurgeDynamicActors();
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
        const bool wasPreJoinBackup = m_isExecutingPreJoinBackup;
        m_pendingSaveSlot.clear();
        m_pendingSaveEpochId = 0;
        m_pendingSaveSendAck = false;
        m_isExecutingPreJoinBackup = false;

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

        // Restore remote actor tracking after Save_Impl has completely finished writing .ess to disk.
        charSvc.SchedulePostSaveRemoteRestore();

        if (saveOk)
        {
            const std::string normSlot = RfabCompat::Detail::NormalizeSaveSlotStem(slot);
            m_writtenSlotsInSession.insert(slot);
            m_writtenSlotsInSession.insert(normSlot);
            if (wasPreJoinBackup || normSlot == RfabCompat::Detail::NormalizeSaveSlotStem(m_preJoinBackupSlot))
            {
                m_preJoinBackupWrittenThisSession = true;
                spdlog::info("[RFAB SaveLoad] Refreshed current-session PreJoin backup '{}'", slot);
            }
            else
            {
                spdlog::info("[RFAB SaveLoad] Wrote coordinated .ess checkpoint '{}' (Epoch={})", slot, epochId);
            }
        }
        else
        {
            spdlog::warn(
                "[RFAB SaveLoad] Deferred save for slot '{}' (Epoch={}, PreJoin={}) did not complete: {}",
                slot,
                epochId,
                wasPreJoinBackup,
                failReason.empty() ? "Save_Impl rejected or .ess verification failed" : failReason);

            if (wasPreJoinBackup)
            {
                // Re-arm PreJoin backup so a temporary jump/combat/menu right after joining never leaves
                // the session without a fresh backup or forces fallback to a stale save on disk.
                m_pendingPreJoinBackup = true;
                m_preJoinBackupDelaySec = 5.0;
            }
        }

        if (sendAck && m_transport.IsConnected())
        {
            CoordinatedSaveAck ack{};
            ack.EpochId = epochId;
            ack.SlotName = String(slot.c_str());
            ack.Success = saveOk;
            if (!saveOk)
                ack.Reason = String(failReason.empty() ? "не удалось записать .ess слот (персонаж занят или в прыжке)" : failReason.c_str());
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
            m_wasAutoReconnecting = false;
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
    m_hasDeferredPrepareSave = false;
    m_isExecutingPreJoinBackup = false;

    m_world.GetDownedStateService().ResetAllStates();
    m_world.GetRfabAdapterService().NotifySaveLoadReset();
    m_world.GetCharacterService().BeginEngineWorldLoad();
    m_world.GetDiscoveryService().ResetLoadedForms();

    if (m_transport.IsConnected() || m_transport.IsOnline())
    {
        if (m_transport.HasLastServerEndpoint())
        {
            m_autoReconnectPendingAfterLoad = true;
            m_wasAutoReconnecting = true;
            m_sawLoadingScreenForReconnect = false;
            m_autoReconnectDelaySec = 1.5;
            m_autoReconnectAttemptsLeft = 6;
        }
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
    {
        m_autoReconnectPendingAfterLoad = false;
        m_wasAutoReconnecting = false;
    }
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

    // Close paused Journal Menu immediately if the player clicked ESC -> Save so the 2-frame calm
    // pre-save detachment and ProcessEvents execution are not stalled by a paused menu.
    if (auto* pUi = UI::Get())
    {
        const auto& menus = GetSaveMenus();
        if (pUi->GetMenuOpen(menus.JournalMenu))
            pUi->CloseAllMenus();
    }

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
            if (auto* pUi = UI::Get())
            {
                const auto& menus = GetSaveMenus();
                if (pUi->GetMenuOpen(menus.JournalMenu))
                    pUi->CloseAllMenus();
            }

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
    const bool reconnectedAfterAuthorizedLoad = m_wasAutoReconnecting;

    m_pendingSaveSlot.clear();
    m_pendingSaveEpochId = 0;
    m_pendingSaveSendAck = false;
    m_pendingSavePurgeFrames = 0;
    m_isExecutingPreJoinBackup = false;
    m_hasDeferredPrepareSave = false;
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
    m_autoReconnectPendingAfterLoad = false;
    m_wasAutoReconnecting = false;
    m_sawLoadingScreenForReconnect = false;
    m_autoReconnectAttemptsLeft = 0;
    m_partyCheckpointSettleSec = 0.0;
    m_initialPartyCheckpointRetries = 0;

    if (!reconnectedAfterAuthorizedLoad)
    {
        // Fresh session entry: ALWAYS overwrite RFAB_MP_PreJoin_Backup with the player's current state
        // instead of trusting any stale RFAB_MP_PreJoin_Backup.ess left over from an earlier session!
        m_writtenSlotsInSession.clear();
        m_lastCommittedEpoch = 0;
        m_lastCommittedSlot.clear();
        m_preJoinBackupWrittenThisSession = false;
        m_pendingPreJoinBackup = true;
        m_preJoinBackupDelaySec = 3.5;
    }
}

void CoordinatedSaveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingPreJoinBackup = false;
    m_isExecutingPreJoinBackup = false;
    m_hasDeferredPrepareSave = false;
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

    if (m_autoReconnectPendingAfterLoad && !m_transport.IsOnline())
    {
        if (IsLoadingMenuOpen() || m_world.GetCharacterService().IsEngineWorldLoading())
        {
            m_sawLoadingScreenForReconnect = true;
            return;
        }

        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (pPlayer && !pPlayer->IsStillLoading() && pPlayer->parentCell != nullptr)
        {
            m_autoReconnectDelaySec -= acEvent.Delta;
            if (m_autoReconnectDelaySec <= 0.0)
            {
                if (m_autoReconnectAttemptsLeft == 0)
                {
                    m_autoReconnectPendingAfterLoad = false;
                    m_wasAutoReconnecting = false;
                    m_sawLoadingScreenForReconnect = false;
                    return;
                }

                if (m_autoReconnectAttemptsLeft == 6)
                    Utils::ShowHudMessage(String("RFAB Online: Автоматическое переподключение к сессии..."));

                --m_autoReconnectAttemptsLeft;
                m_autoReconnectDelaySec = 3.5;
                m_transport.ReconnectToLastServer();
            }
        }
        return;
    }

    if (!m_transport.IsConnected())
        return;

    if (m_hasDeferredPrepareSave)
    {
        m_deferredPrepareWaitSec -= acEvent.Delta;
        std::string failReason;
        if (CanSafelySaveNow(failReason))
        {
            const std::string slot = std::move(m_deferredPrepareSlotName);
            const uint64_t epochId = m_deferredPrepareEpochId;
            m_hasDeferredPrepareSave = false;
            QueuePurgedAuthorizedSave(slot, epochId, true);
        }
        else if (m_deferredPrepareWaitSec <= 0.0)
        {
            CoordinatedSaveAck ack{};
            ack.EpochId = m_deferredPrepareEpochId;
            ack.SlotName = String(m_deferredPrepareSlotName.c_str());
            ack.Success = false;
            ack.Reason = String(failReason.c_str());
            m_hasDeferredPrepareSave = false;
            m_transport.Send(ack);
        }
        return;
    }

    if (m_pendingPreJoinBackup && m_pendingSaveSlot.empty() && !IsLoadingMenuOpen())
    {
        m_preJoinBackupDelaySec -= acEvent.Delta;
        if (m_preJoinBackupDelaySec <= 0.0)
        {
            std::string reason;
            if (CanSafelySaveNow(reason))
            {
                m_pendingPreJoinBackup = false;
                m_isExecutingPreJoinBackup = true;
                QueuePurgedAuthorizedSave(m_preJoinBackupSlot, 0, false);
            }
            else
            {
                m_preJoinBackupDelaySec = 3.0;
            }
        }
    }

    if (!m_initialPartyCheckpointSent &&
        m_lastCommittedEpoch == 0 &&
        m_pendingSaveSlot.empty() &&
        !m_pendingPreJoinBackup &&
        !IsLoadingMenuOpen() &&
        IsLocalPlayerAuthorizedLeader() &&
        m_world.GetPartyService().GetPlayers().size() >= 2)
    {
        m_partyCheckpointSettleSec += acEvent.Delta;
        if (m_partyCheckpointSettleSec >= 5.0)
        {
            std::string reason;
            if (CanSafelySaveNow(reason))
            {
                m_initialPartyCheckpointSent = true;
                RequestCoordinatedSaveLoad req{};
                req.Action = RequestCoordinatedSaveLoad::ActionType::ManualSave;
                m_transport.Send(req);
            }
            else
            {
                m_partyCheckpointSettleSec = 2.0;
            }
        }
    }
}

void CoordinatedSaveService::OnNotifyPrepareSave(const NotifyPrepareSave& acMessage) noexcept
{
    const std::string slotName = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
        std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));
    std::string failReason;

    if (auto* pUi = UI::Get())
    {
        const auto& menus = GetSaveMenus();
        if (pUi->GetMenuOpen(menus.JournalMenu) || pUi->GetMenuOpen(menus.TweenMenu))
            pUi->CloseAllMenus();
    }

    if (!CanSafelySaveNow(failReason))
    {
        // If blocked only by a closing menu (not combat or downed bleedout), wait up to 2.5s before rejecting.
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        const bool hardCombatOrDowned = !pPlayer || pPlayer->IsDead() || pPlayer->IsInCombat() ||
                                        m_world.GetDownedStateService().IsLocalPlayerDowned();
        if (!hardCombatOrDowned)
        {
            m_hasDeferredPrepareSave = true;
            m_deferredPrepareEpochId = acMessage.EpochId;
            m_deferredPrepareSlotName = slotName;
            m_deferredPrepareWaitSec = 2.5;
            return;
        }

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
    else if (m_lastCommittedEpoch == 0 && m_initialPartyCheckpointSent && m_initialPartyCheckpointRetries < 3)
    {
        ++m_initialPartyCheckpointRetries;
        m_initialPartyCheckpointSent = false;
        m_partyCheckpointSettleSec = 0.0;
    }

    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);
}

void CoordinatedSaveService::ApplyInSessionRollback(const NotifyExecuteLoad& acMessage) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // 1. Preserve 1st-person POV flag BEFORE ResetAllStates(), then clear downed/bleedout posture & grant recovery
    auto& downedSvc = m_world.GetDownedStateService();
    const bool wasFirstPersonBeforeDowned = downedSvc.ConsumeSavedFirstPersonBeforeDowned();
    const float restoreFrac = std::clamp(acMessage.RestoreHealthFraction, 0.30f, 1.0f);
    downedSvc.ResetAllStates();
    downedSvc.ApplyAuthoritativeRevive(restoreFrac, 5.0f, false);

    if (pPlayer->IsInCombat())
        pPlayer->StopCombat();
    pPlayer->DispelAllSpells();

    // 2. Restore checkpoint inventory if provided by server
    if (acMessage.HasRestoreInventory)
    {
        ScopedInventoryOverride invGuard;
        ScopedEquipOverride equipGuard;
        pPlayer->SetActorInventory(acMessage.RestoreInventory);
    }

    // 3. Teleport local player seamlessly to checkpoint WorldOrCell + Position without .ess reload or disconnect
    if (acMessage.HasRestoreTransform &&
        CoopOwnership::IsValidHandoffCoordinates(acMessage.PositionX, acMessage.PositionY, acMessage.PositionZ))
    {
        auto& modSystem = m_world.GetModSystem();
        const uint32_t targetCellFormId = modSystem.GetGameId(acMessage.TargetCellId);
        const uint32_t targetWorldFormId = modSystem.GetGameId(acMessage.TargetWorldSpaceId);

        TESObjectCELL* pTargetCell = targetCellFormId != 0 ? Cast<TESObjectCELL>(TESForm::GetById(targetCellFormId)) : nullptr;
        if (!pTargetCell && targetWorldFormId != 0)
        {
            if (auto* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(targetWorldFormId)))
            {
                const int32_t gridX = static_cast<int32_t>(std::floor(acMessage.PositionX / 4096.0f));
                const int32_t gridY = static_cast<int32_t>(std::floor(acMessage.PositionY / 4096.0f));
                if (auto* pModMgr = ModManager::Get())
                    pTargetCell = pModMgr->GetCellFromCoordinates(gridX, gridY, pWorldSpace, false);
            }
        }

        NiPoint3 targetPos{};
        targetPos.x = acMessage.PositionX;
        targetPos.y = acMessage.PositionY;
        targetPos.z = acMessage.PositionZ;

        const uint32_t curCellId = pPlayer->parentCell ? pPlayer->parentCell->formID : 0u;
        const uint32_t curWorldId = pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0u;
        const bool sameLoadedContext =
            (targetWorldFormId != 0 && curWorldId == targetWorldFormId && pTargetCell && curCellId == pTargetCell->formID) ||
            (targetWorldFormId == 0 && targetCellFormId != 0 && curCellId == targetCellFormId);

        ScopedReferencesOverride refGuard;
        pPlayer->ResetTeleportPhysics(targetPos.z);
        if (sameLoadedContext || !pTargetCell)
        {
            pPlayer->SetPosition(targetPos, true);
        }
        else
        {
            pPlayer->MoveTo(pTargetCell, targetPos);
        }
        pPlayer->SetRotation(0.0f, 0.0f, acMessage.RotationZ);
        pPlayer->ResetTeleportPhysics(targetPos.z);
    }

    // 4. Re-restore full HP AFTER SetActorInventory, re-normalize lifeState + wake nearby destination NPC AI packages,
    // schedule 2-frame unculled FaceGen QueueUpdate() + 1st-person POV restore, and flush stale interpolation views!
    {
        DownedStateService::ScopedReviveHealAllowance allowHeal(downedSvc.GetAuthorizedHealFlag());
        const float maxHp = std::max(1.0f, pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth));
        pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, maxHp * restoreFrac);
    }
    downedSvc.RestoreLivingCombatTargetability(pPlayer, true);
    downedSvc.SchedulePostReviveHeadAndCameraSettle(wasFirstPersonBeforeDowned);
    m_world.GetCharacterService().NotifyInSessionRollbackApplied();

    spdlog::info(
        "[RFAB SaveLoad] Completed seamless in-session checkpoint rollback (Epoch={}, PartyWipe={}, Transform={})",
        acMessage.EpochId,
        acMessage.IsPartyWipe,
        acMessage.HasRestoreTransform);
}

void CoordinatedSaveService::OnNotifyExecuteLoad(const NotifyExecuteLoad& acMessage) noexcept
{
    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);

    // 1. MMO / SkyMP Model: Party wipes & server-directed in-session rollbacks execute immediately without
    // closing TransportService or reloading any .ess file on disk!
    if (acMessage.UseInSessionRollback)
    {
        ApplyInSessionRollback(acMessage);
        return;
    }

    // 2. Explicit manual checkpoint slot load (F9 / Load specific RFAB_MP_* slot):
    std::string targetSlot = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
        std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));

    auto isUsableSlot = [&](const std::string& aCandidate) -> bool
    {
        if (aCandidate.empty())
            return false;
        return BGSSaveLoadManager::VerifySavedSlotOnDisk(aCandidate.c_str());
    };

    if (!isUsableSlot(targetSlot))
    {
        if (!m_lastCommittedSlot.empty() && isUsableSlot(m_lastCommittedSlot))
        {
            targetSlot = m_lastCommittedSlot;
        }
        else if (CoopCheckpoint::ShouldAllowPreJoinDiskFallback(
                     acMessage.AllowPreJoinFallback,
                     m_preJoinBackupWrittenThisSession,
                     acMessage.HasRestoreTransform) &&
                 isUsableSlot(m_preJoinBackupSlot))
        {
            targetSlot = m_preJoinBackupSlot;
        }
        else
        {
            targetSlot.clear();
        }
    }

    if (targetSlot.empty())
    {
        spdlog::info(
            "[RFAB SaveLoad] Disk slot '{}' unavailable on this client — applying seamless in-session rollback (Epoch={})",
            std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()),
            acMessage.EpochId);
        ApplyInSessionRollback(acMessage);
        return;
    }

    spdlog::info("[RFAB SaveLoad] Queueing coordinated .ess load for slot '{}' (Epoch={})", targetSlot, acMessage.EpochId);
    m_deferredLoadSlot = targetSlot;
}
