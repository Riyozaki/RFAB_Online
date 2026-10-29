#include <TiltedOnlinePCH.h>

#include <Services/CoordinatedSaveService.h>
#include <Services/ObjectService.h>

#include <World.h>
#include <Utils.h>
#include <common/CoopCheckpointStore.h>
#include <common/CoopOwnership.h>
#include <common/CoopWorldStabilizer.h>
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
#include <Services/ActorValueService.h>
#include <Services/CharacterService.h>
#include <Services/DiscoveryService.h>
#include <Services/DownedStateService.h>
#include <Services/MagicService.h>
#include <Services/PartyService.h>
#include <Services/PlayerService.h>
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
    // Budget 24 retries (6.0s at 0.25s cadence) so mid-air jumps, slope falls, and finishing MCO attacks
    // complete cleanly instead of rejecting manual/auto checkpoints after 2.0s.
    m_pendingSavePurgeFrames = 2;
    m_pendingSaveRetriesLeft = 24;
    m_pendingSaveRetryDelaySec = 0.0;

    auto& charSvc = m_world.GetCharacterService();
    charSvc.PreSavePurgeDynamicActors();
}

void CoordinatedSaveService::ProcessReadyDeferredSaveOrLoad(BGSSaveLoadManager* apMgr) noexcept
{
    if (!apMgr)
        return;

    if (!m_pendingSaveSlot.empty() &&
        m_pendingSavePurgeFrames == 0 &&
        m_pendingSaveRetryDelaySec <= 0.0 &&
        !IsLoadingMenuOpen())
    {
        const std::string slot = m_pendingSaveSlot;
        const uint64_t epochId = m_pendingSaveEpochId;
        const bool sendAck = m_pendingSaveSendAck;
        const bool wasPreJoinBackup = m_isExecutingPreJoinBackup;

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

        if (!saveOk && m_pendingSaveRetriesLeft > 0)
        {
            --m_pendingSaveRetriesLeft;
            m_pendingSaveRetryDelaySec = 0.25;
            m_pendingSavePurgeFrames = 1;
            return;
        }

        m_pendingSaveSlot.clear();
        m_pendingSaveEpochId = 0;
        m_pendingSaveSendAck = false;
        m_pendingSaveRetriesLeft = 0;
        m_pendingSaveRetryDelaySec = 0.0;
        m_isExecutingPreJoinBackup = false;

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
                m_lastWrittenCoopSlot = slot;
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
            const bool wasLiveBarrierLoad = m_liveSessionWorldReloadPending || m_liveSessionWorldReloadInFlight;
            m_liveSessionWorldReloadPending = false;
            m_liveSessionWorldReloadInFlight = false;
            m_postReloadSyncPending = false;
            m_waitingForHostBarrierRelease = false;
            m_autoReconnectPendingAfterLoad = false;
            m_wasAutoReconnecting = false;
            m_world.GetCharacterService().EndEngineWorldLoad();
            m_world.GetDiscoveryService().ReconcileMissingWorldEntities();
            if (wasLiveBarrierLoad)
                SendReloadBarrierReadyAck(m_activeReloadBarrierEpoch, slot, false);
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
    m_world.GetObjectService().ResetForWorldReload();
    m_world.GetMagicService().ResetForWorldReload();
    m_world.GetActorValueService().ResetForWorldReload();

    // Phase 3 (`WorldReloadBarrier`): when executing an authorized coordinated `.ess` reload (Party Wipe
    // or manual `F9`), KEEP `m_transport`, `PlayerId`, and `PartyService` open instead of calling
    // `m_transport.Close()`! Only fall back to disconnect + auto-reconnect on uncoordinated loads.
    if (m_liveSessionWorldReloadPending && (m_transport.IsConnected() || m_transport.IsOnline()))
    {
        m_liveSessionWorldReloadPending = false;
        m_liveSessionWorldReloadInFlight = true;
        m_postReloadSyncPending = true;
        m_waitingForHostBarrierRelease = false;
        spdlog::info(
            "[RFAB WorldReloadBarrier] Entering live-socket .ess load '{}' (Epoch={}, transport stays open)",
            acpFileName ? acpFileName : "",
            m_activeReloadBarrierEpoch);
        return;
    }

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

    if (m_liveSessionWorldReloadInFlight)
    {
        m_liveSessionWorldReloadInFlight = false;
        if (!aSucceeded)
        {
            m_postReloadSyncPending = false;
            m_waitingForHostBarrierRelease = false;
            SendReloadBarrierReadyAck(m_activeReloadBarrierEpoch, m_activeReloadBarrierSlot, false);
            m_world.GetDiscoveryService().ReconcileMissingWorldEntities();
        }
        return;
    }

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

    const auto& menus = GetSaveMenus();
    auto* pUi = UI::Get();
    const bool isJournalMenuOpen = pUi && pUi->GetMenuOpen(menus.JournalMenu);
#ifdef _WIN32
    const bool isF9QuickloadKey = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
#else
    const bool isF9QuickloadKey = false;
#endif
    const bool isQuickloadAttempt = rawName.empty() || isF9QuickloadKey || !isJournalMenuOpen;

    // Post-disconnect / offline quickload guard:
    // If the player presses F9 right after disconnecting (or before reconnecting), vanilla Skyrim's
    // Quickload would otherwise load a hours-old solo `Quicksave0_...` or `RFAB_MP_PreJoin_Backup.ess`
    // instead of the freshest co-op checkpoint written during this session!
    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
    {
        if (isQuickloadAttempt)
        {
            const std::string& preferredCoopSlot =
                !m_lastCommittedSlot.empty() ? m_lastCommittedSlot : m_lastWrittenCoopSlot;
            if (!preferredCoopSlot.empty() &&
                preferredCoopSlot != m_preJoinBackupSlot &&
                BGSSaveLoadManager::VerifySavedSlotOnDisk(preferredCoopSlot.c_str()))
            {
                m_deferredLoadSlot = preferredCoopSlot;
                return false;
            }
        }
        return true;
    }

    if (IsLocalPlayerAuthorizedLeader())
    {
        if (m_lastInterceptNoticeCooldownSec <= 0.0)
        {
            m_lastInterceptNoticeCooldownSec = 1.5;
            if (isJournalMenuOpen)
                pUi->CloseAllMenus();

            RequestCoordinatedSaveLoad req{};
            const std::string cleanStem = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(rawName);
            const std::string normPreJoin = RfabCompat::Detail::NormalizeSaveSlotStem(m_preJoinBackupSlot);
            // CRITICAL: Only load a specific `RFAB_MP_Save_XXXX` slot when the leader explicitly clicked
            // it inside `ESC -> Load` (`isJournalMenuOpen && !isF9QuickloadKey`), and NEVER treat
            // `RFAB_MP_PreJoin_Backup` as `LoadSpecificSlot`! Pressing `F9` ALWAYS requests the server's
            // newest committed party checkpoint (`LoadLatestCheckpoint`).
            if (!isQuickloadAttempt &&
                normName != normPreJoin &&
                RfabCompat::Detail::StartsWithIgnoreCase(cleanStem, "RFAB_MP_"))
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
    m_liveSessionWorldReloadPending = false;
    m_liveSessionWorldReloadInFlight = false;
    m_postReloadSyncPending = false;
    m_waitingForHostBarrierRelease = false;
    m_hostBarrierHoldRemainingSec = 0.0;
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
    m_autoReconnectPendingAfterLoad = false;
    m_wasAutoReconnecting = false;
    m_sawLoadingScreenForReconnect = false;
    m_autoReconnectAttemptsLeft = 0;
    m_partyCheckpointSettleSec = 0.0;
    m_initialPartyCheckpointRetries = 0;

    if (!reconnectedAfterAuthorizedLoad && !m_preJoinBackupWrittenThisSession)
    {
        // First session entry in this game run: write a fresh `RFAB_MP_PreJoin_Backup` instead of
        // trusting any stale `RFAB_MP_PreJoin_Backup.ess` left over from a previous run, while
        // preserving `m_lastCommittedSlot` / `m_preJoinBackupWrittenThisSession` across temporary
        // mid-session Wi-Fi/UDP reconnects so `RFAB_MP_PreJoin_Backup` is never clobbered mid-run!
        m_writtenSlotsInSession.clear();
        m_lastCommittedEpoch = 0;
        m_lastCommittedSlot.clear();
        m_lastWrittenCoopSlot.clear();
        m_pendingPreJoinBackup = true;
        m_preJoinBackupDelaySec = 3.5;
    }
}

void CoordinatedSaveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // Safety net: if a physical Wi-Fi/UDP blip occurred mid-reload, allow automatic reconnect once
    // the engine finishes loading.
    if ((m_liveSessionWorldReloadInFlight || m_postReloadSyncPending) && m_transport.HasLastServerEndpoint())
    {
        m_autoReconnectPendingAfterLoad = true;
        m_wasAutoReconnecting = true;
        m_sawLoadingScreenForReconnect = false;
        m_autoReconnectDelaySec = 1.5;
        m_autoReconnectAttemptsLeft = 6;
    }

    m_liveSessionWorldReloadPending = false;
    m_liveSessionWorldReloadInFlight = false;
    m_postReloadSyncPending = false;
    m_waitingForHostBarrierRelease = false;
    m_hostBarrierHoldRemainingSec = 0.0;

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

void CoordinatedSaveService::SendReloadBarrierReadyAck(
    uint64_t aEpochId,
    const std::string& acSlotName,
    bool aSuccess) noexcept
{
    if (!m_transport.IsConnected())
        return;

    CoordinatedSaveAck ack{};
    ack.EpochId = CoopCheckpoint::EncodeReloadBarrierEpoch(aEpochId);
    ack.SlotName = String(acSlotName.c_str());
    ack.Success = aSuccess;
    if (!aSuccess)
        ack.Reason = String("не удалось загрузить .ess слот чекпоинта");
    m_transport.Send(ack);
}

void CoordinatedSaveService::ReleaseGuestWorldDiscoveryHold() noexcept
{
    if (!m_waitingForHostBarrierRelease)
        return;

    m_waitingForHostBarrierRelease = false;
    m_hostBarrierHoldRemainingSec = 0.0;

    m_world.GetDiscoveryService().ReconcileMissingWorldEntities();
    if (auto* pObjSvc = m_world.ctx().find<ObjectService>())
        pObjSvc->RefreshCurrentCellObjectsAfterRollback();

    spdlog::info(
        "[RFAB WorldReloadBarrier] Host gate released — reconciled guest world entities (Epoch={})",
        m_activeReloadBarrierEpoch);
}

void CoordinatedSaveService::CompleteLiveWorldReloadAfterEngineReady() noexcept
{
    m_postReloadSyncPending = false;

    m_world.GetPlayerService().RestoreMultiplayerGlobalsAfterWorldReload();
    m_world.GetDownedStateService().ResetAllStates();
    m_world.GetCharacterService().EnsureLocalPlayerRegistered();
    m_world.GetDiscoveryService().VisitCell(true);

    SendReloadBarrierReadyAck(m_activeReloadBarrierEpoch, m_activeReloadBarrierSlot, true);

    if (IsLocalPlayerAuthorizedLeader())
    {
        m_waitingForHostBarrierRelease = false;
        m_hostBarrierHoldRemainingSec = 0.0;
        m_world.GetDiscoveryService().ReconcileMissingWorldEntities();
        if (auto* pObjSvc = m_world.ctx().find<ObjectService>())
            pObjSvc->RefreshCurrentCellObjectsAfterRollback();

        spdlog::info(
            "[RFAB WorldReloadBarrier] Host completed live-socket .ess reload '{}' (Epoch={}) and claimed canonical cell authority",
            m_activeReloadBarrierSlot,
            m_activeReloadBarrierEpoch);
    }
    else
    {
        // Hold Guest NPC VisitForms() briefly until Server confirms Host reload readiness (or 6.0s cap)
        // so Host claims canonical Sticky Host Cell Authority without double-transfer churn.
        m_waitingForHostBarrierRelease = true;
        m_hostBarrierHoldRemainingSec = 6.0;
        spdlog::info(
            "[RFAB WorldReloadBarrier] Guest completed live-socket .ess reload '{}' (Epoch={}), awaiting Host barrier gate",
            m_activeReloadBarrierSlot,
            m_activeReloadBarrierEpoch);
    }
}

void CoordinatedSaveService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    BGSSaveLoadManager::InstallUniversalPostSkseHooks();

    if (m_lastInterceptNoticeCooldownSec > 0.0)
        m_lastInterceptNoticeCooldownSec = std::max(0.0, m_lastInterceptNoticeCooldownSec - acEvent.Delta);

    if (m_pendingSaveRetryDelaySec > 0.0)
        m_pendingSaveRetryDelaySec = std::max(0.0, m_pendingSaveRetryDelaySec - acEvent.Delta);

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

    // Phase 3 (`WorldReloadBarrier`): finalize live-socket `.ess` reload as soon as the loading screen
    // closes and `PlayerCharacter` has a valid `parentCell` and finished 3D/cell streaming.
    if (m_postReloadSyncPending)
    {
        if (IsLoadingMenuOpen() || m_world.GetCharacterService().IsEngineWorldLoading())
            return;

        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (pPlayer && !pPlayer->IsStillLoading() && pPlayer->parentCell != nullptr)
            CompleteLiveWorldReloadAfterEngineReady();
        return;
    }

    if (m_waitingForHostBarrierRelease)
    {
        m_hostBarrierHoldRemainingSec -= acEvent.Delta;
        if (m_hostBarrierHoldRemainingSec <= 0.0 || IsLocalPlayerAuthorizedLeader())
            ReleaseGuestWorldDiscoveryHold();
    }

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
            m_deferredPrepareWaitSec = 5.0;
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
    if (CoopCheckpoint::IsReloadBarrierEpoch(acMessage.EpochId))
    {
        ReleaseGuestWorldDiscoveryHold();
        if (!acMessage.Message.empty())
            Utils::ShowHudMessage(acMessage.Message);
        return;
    }

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

    // Emergency missing-slot fallback ONLY (e.g. a peer joined mid-session after a checkpoint was
    // written and before their current-session `RFAB_MP_PreJoin_Backup.ess` finished writing):
    // 1. Preserve 1st-person POV flag BEFORE ResetAllStates(), then clear downed/bleedout posture & grant recovery.
    auto& downedSvc = m_world.GetDownedStateService();
    const bool wasFirstPersonBeforeDowned = downedSvc.ConsumeSavedFirstPersonBeforeDowned();
    const float restoreFrac = std::clamp(acMessage.RestoreHealthFraction, 0.30f, 1.0f);
    downedSvc.ResetAllStates();
    downedSvc.ApplyAuthoritativeRevive(restoreFrac, 5.0f, false);

    if (pPlayer->IsInCombat())
        pPlayer->StopCombat();
    pPlayer->DispelHostileEffects();

    // NOTE (v3.0 Phase 3): NEVER call `SetActorInventory` on `0x14`! Rebuilding the local player's
    // inventory via RemoveAllItems + AddItem destroys SkyUI hotkeys, ExtraUniqueID, and custom
    // `RFAB.dll` item metadata. Full inventory/Papyrus/CustomSkills rollback is performed by
    // synchronized `.ess` world reload via `WorldReloadBarrier`.

    // 2. Teleport local player cleanly to checkpoint WorldOrCell + Position.
    if (acMessage.HasRestoreTransform &&
        CoopOwnership::IsValidHandoffCoordinates(acMessage.PositionX, acMessage.PositionY, acMessage.PositionZ))
    {
        auto& modSystem = m_world.GetModSystem();
        const uint32_t targetCellFormId = modSystem.GetGameId(acMessage.TargetCellId);
        const uint32_t targetWorldFormId = modSystem.GetGameId(acMessage.TargetWorldSpaceId);

        TESObjectCELL* pTargetCell = targetCellFormId != 0 ? Cast<TESObjectCELL>(TESForm::GetById(targetCellFormId)) : nullptr;
        TESWorldSpace* pTargetWorldSpace = targetWorldFormId != 0 ? Cast<TESWorldSpace>(TESForm::GetById(targetWorldFormId)) : nullptr;
        if (!pTargetCell && pTargetWorldSpace)
        {
            const int32_t gridX = static_cast<int32_t>(std::floor(acMessage.PositionX / 4096.0f));
            const int32_t gridY = static_cast<int32_t>(std::floor(acMessage.PositionY / 4096.0f));
            if (auto* pModMgr = ModManager::Get())
                pTargetCell = pModMgr->GetCellFromCoordinates(gridX, gridY, pTargetWorldSpace, false);
        }

        NiPoint3 targetPos{};
        targetPos.x = acMessage.PositionX;
        targetPos.y = acMessage.PositionY;
        targetPos.z = acMessage.PositionZ;

        NiPoint3 targetRot = pPlayer->rotation;
        targetRot.x = 0.0f;
        targetRot.z = acMessage.RotationZ;

        const uint32_t curCellId = pPlayer->parentCell ? pPlayer->parentCell->formID : 0u;
        const uint32_t curWorldId = pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0u;
        const bool sameLoadedContext =
            (targetWorldFormId != 0 && curWorldId == targetWorldFormId && pTargetCell && curCellId == pTargetCell->formID) ||
            (targetWorldFormId == 0 && targetCellFormId != 0 && curCellId == targetCellFormId);

        ScopedReferencesOverride refGuard;
        pPlayer->ResetTeleportPhysics(targetPos.z);
        if (sameLoadedContext)
        {
            pPlayer->SetPosition(targetPos, true);
            pPlayer->SetRotation(0.0f, 0.0f, acMessage.RotationZ);
        }
        else if (pTargetCell || pTargetWorldSpace)
        {
            // Native InternalMoveTo (SE 56227) accepts (nullptr, pTargetWorldSpace, targetPos, targetRot)
            // when rolling back from an interior dungeon to an unloaded exterior checkpoint cell!
            pPlayer->MoveToWorldOrCell(pTargetCell, pTargetWorldSpace, targetPos, targetRot);
        }
        else
        {
            pPlayer->SetPosition(targetPos, true);
            pPlayer->SetRotation(0.0f, 0.0f, acMessage.RotationZ);
        }
        pPlayer->ResetTeleportPhysics(targetPos.z);
    }

    // 3. Restore HP, re-normalize lifeState + wake nearby destination NPC AI packages,
    // schedule 2-frame unculled FaceGen QueueUpdate() + 1st-person POV restore, and flush stale interpolation views!
    {
        DownedStateService::ScopedReviveHealAllowance allowHeal(downedSvc.GetAuthorizedHealFlag());
        const float maxHp = std::max(1.0f, pPlayer->GetActorPermanentValue(ActorValueInfo::kHealth));
        pPlayer->ForceActorValue(ActorValueOwner::ForceMode::DAMAGE, ActorValueInfo::kHealth, maxHp * restoreFrac);
    }
    downedSvc.RestoreLivingCombatTargetability(pPlayer, true);
    downedSvc.SchedulePostReviveHeadAndCameraSettle(wasFirstPersonBeforeDowned);
    m_world.GetCharacterService().NotifyInSessionRollbackApplied();
    if (auto* pObjSvc = m_world.ctx().find<ObjectService>())
        pObjSvc->RefreshCurrentCellObjectsAfterRollback();

    SendReloadBarrierReadyAck(acMessage.EpochId, "", true);

    spdlog::info(
        "[RFAB WorldReloadBarrier] Applied emergency missing-slot transform fallback without SetActorInventory (Epoch={}, PartyWipe={}, Transform={})",
        acMessage.EpochId,
        acMessage.IsPartyWipe,
        acMessage.HasRestoreTransform);
}

void CoordinatedSaveService::OnNotifyExecuteLoad(const NotifyExecuteLoad& acMessage) noexcept
{
    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);

    if (acMessage.UseInSessionRollback)
    {
        ApplyInSessionRollback(acMessage);
        return;
    }

    // Phase 3 (`WorldReloadBarrier`): unified synchronized `.ess` checkpoint reload for BOTH Party Wipe
    // and Manual Load (`F9`), keeping `TransportService` socket & `PartyService` open!
    std::string targetSlot = RfabCompat::Detail::StripSaveSlotExtensionPreserveCase(
        std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()));
    const std::string normPreJoin = RfabCompat::Detail::NormalizeSaveSlotStem(m_preJoinBackupSlot);

    auto isUsableCurrentSessionSlot = [&](const std::string& aCandidate) -> bool
    {
        if (aCandidate.empty())
            return false;
        const std::string normCandidate = RfabCompat::Detail::NormalizeSaveSlotStem(aCandidate);
        if (normCandidate == normPreJoin &&
            !CoopCheckpoint::CanLoadPreJoinSlotInCurrentSession(
                acMessage.AllowPreJoinFallback,
                m_preJoinBackupWrittenThisSession))
        {
            return false;
        }
        return BGSSaveLoadManager::VerifySavedSlotOnDisk(aCandidate.c_str());
    };

    if (!isUsableCurrentSessionSlot(targetSlot))
    {
        if (!m_lastCommittedSlot.empty() && isUsableCurrentSessionSlot(m_lastCommittedSlot))
        {
            targetSlot = m_lastCommittedSlot;
        }
        else if (CoopCheckpoint::CanLoadPreJoinSlotInCurrentSession(
                     acMessage.AllowPreJoinFallback,
                     m_preJoinBackupWrittenThisSession) &&
                 isUsableCurrentSessionSlot(m_preJoinBackupSlot))
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
        spdlog::warn(
            "[RFAB WorldReloadBarrier] Disk slot '{}' unavailable in current session — applying non-destructive emergency fallback (Epoch={})",
            std::string_view(acMessage.SlotName.c_str(), acMessage.SlotName.size()),
            acMessage.EpochId);
        ApplyInSessionRollback(acMessage);
        return;
    }

    if (auto* pUi = UI::Get())
        pUi->CloseAllMenus();

    m_liveSessionWorldReloadPending = true;
    m_liveSessionWorldReloadInFlight = false;
    m_postReloadSyncPending = false;
    m_waitingForHostBarrierRelease = false;
    m_activeReloadBarrierEpoch = acMessage.EpochId;
    m_activeReloadBarrierSlot = targetSlot;

    // Immediately gate incoming remote actor materialization before `ExecuteDirectLoadFromProcessEvents` runs.
    m_world.GetCharacterService().BeginEngineWorldLoad();

    spdlog::info(
        "[RFAB WorldReloadBarrier] Queueing live-socket .ess reload for slot '{}' (Epoch={}, PartyWipe={})",
        targetSlot,
        acMessage.EpochId,
        acMessage.IsPartyWipe);
    m_deferredLoadSlot = targetSlot;
}
