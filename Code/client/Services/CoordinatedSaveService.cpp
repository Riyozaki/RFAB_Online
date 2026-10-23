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

    // If alone on server or before party formation, allow host request (server validates authoritatively too).
    return partyService.GetPlayers().empty();
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
    if (!pPlayer || pPlayer->IsDead())
    {
        aOutReason = "персонаж недоступен";
        return false;
    }

    return true;
}

bool CoordinatedSaveService::ExecuteAuthorizedSave(const std::string& acSlotName) noexcept
{
    if (acSlotName.empty())
        return false;

    auto* pMgr = BGSSaveLoadManager::Get();
    if (!pMgr)
        return false;

    m_authorizedPendingSaveSlot = acSlotName;
    ScopedSaveLoadAllowance guard;
    const bool ok = pMgr->SaveNamed(acSlotName.c_str());
    if (ok)
        m_writtenSlotsInSession.insert(acSlotName);
    else
        m_authorizedPendingSaveSlot.clear();
    return ok;
}

bool CoordinatedSaveService::ExecuteAuthorizedLoad(const std::string& acSlotName) noexcept
{
    if (acSlotName.empty())
        return false;

    auto* pMgr = BGSSaveLoadManager::Get();
    if (!pMgr)
        return false;

    m_authorizedPendingLoadSlot = acSlotName;
    ScopedSaveLoadAllowance guard;
    const bool ok = pMgr->LoadNamed(acSlotName.c_str(), true);
    if (!ok)
        m_authorizedPendingLoadSlot.clear();
    return ok;
}

void CoordinatedSaveService::PrepareEngineForWorldLoad(const char* acpFileName) noexcept
{
    (void)acpFileName;
    m_world.GetDownedStateService().ResetAllStates();
    m_world.GetRfabAdapterService().NotifySaveLoadReset();

    // If loading an authorized co-op checkpoint while online, close transport cleanly BEFORE BGSSaveLoadGame::Revert
    // so zero network packets arrive mid-revert, and schedule automatic reconnect as soon as Loading Menu finishes!
    if (m_transport.IsConnected() && m_transport.HasLastServerEndpoint())
    {
        m_autoReconnectPendingAfterLoad = true;
        m_sawLoadingScreenForReconnect = false;
        m_autoReconnectDelaySec = 1.5;
        m_transport.Close();
    }

    // Always purge ALL dynamic 0xFFxxxxxx actors (both remote players AND dynamic encounters like FF000862)
    // BEFORE Skyrim tears down loaded cells and heap TESNPC forms!
    m_world.GetCharacterService().PurgeAllDynamicActorsForWorldLoad();
}

void CoordinatedSaveService::NotifyEngineSaveCompleted(const char* acpFileName) noexcept
{
    const std::string fileName = acpFileName ? acpFileName : "";
    if (!m_authorizedPendingSaveSlot.empty() &&
        RfabCompat::Detail::ToLowerAscii(fileName) == RfabCompat::Detail::ToLowerAscii(m_authorizedPendingSaveSlot))
    {
        m_authorizedPendingSaveSlot.clear();
    }
}

void CoordinatedSaveService::NotifyEngineLoadCompleted(const char* acpFileName, bool aSucceeded) noexcept
{
    const std::string fileName = acpFileName ? acpFileName : "";
    if (!m_authorizedPendingLoadSlot.empty() &&
        RfabCompat::Detail::ToLowerAscii(fileName) == RfabCompat::Detail::ToLowerAscii(m_authorizedPendingLoadSlot))
    {
        m_authorizedPendingLoadSlot.clear();
    }

    if (!aSucceeded)
        m_autoReconnectPendingAfterLoad = false;
}

bool CoordinatedSaveService::HandleEngineSaveAttempt(const char* acpFileName) noexcept
{
    const std::string fileName = acpFileName ? acpFileName : "";
    if (!m_authorizedPendingSaveSlot.empty() &&
        RfabCompat::Detail::ToLowerAscii(fileName) == RfabCompat::Detail::ToLowerAscii(m_authorizedPendingSaveSlot))
    {
        return true;
    }

    if (ScopedSaveLoadAllowance::IsAllowed())
        return true;

    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
        return true;

    // Silently suppress uncoordinated solo autosaves (vanilla door autosaves & RFAB.dll combat autosaves).
    if (IsAutosaveName(fileName))
        return false;

    // ANY connected player pressing F5 or Menu -> Save triggers a synchronized two-phase checkpoint save
    // across ALL connected players simultaneously (saving both characters and exact world coordinates)!
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
    const std::string fileName = acpFileName ? acpFileName : "";
    if (!m_authorizedPendingLoadSlot.empty() &&
        RfabCompat::Detail::ToLowerAscii(fileName) == RfabCompat::Detail::ToLowerAscii(m_authorizedPendingLoadSlot))
    {
        return true;
    }

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
            if (RfabCompat::Detail::StartsWithIgnoreCase(fileName, "RFAB_MP_"))
            {
                req.Action = RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot;
                req.SlotName = String(fileName.c_str());
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
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
    m_autoReconnectPendingAfterLoad = false;
    m_sawLoadingScreenForReconnect = false;
    m_partyCheckpointSettleSec = 0.0;

    // Preserve m_writtenSlotsInSession / m_lastCommittedEpoch / m_lastCommittedSlot across auto-reconnects
    // so repeated RFAB boss attempts (Save -> Wipe #1 -> Auto-Reload -> Wipe #2 -> Auto-Reload -> Victory!)
    // ALWAYS reload the exact boss-door checkpoint cleanly every single attempt!
    if (!m_writtenSlotsInSession.contains(m_preJoinBackupSlot))
    {
        m_pendingPreJoinBackup = true;
        m_preJoinBackupDelaySec = 3.5;
    }
}

void CoordinatedSaveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingPreJoinBackup = false;
    m_initialPartyCheckpointSent = false;
    m_partyCheckpointSettleSec = 0.0;
    m_authorizedPendingSaveSlot.clear();
    m_authorizedPendingLoadSlot.clear();
    m_deferredLoadSlot.clear();
}

void CoordinatedSaveService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    BGSSaveLoadManager::InstallUniversalPostSkseHooks();

    if (m_lastInterceptNoticeCooldownSec > 0.0)
        m_lastInterceptNoticeCooldownSec = std::max(0.0, m_lastInterceptNoticeCooldownSec - acEvent.Delta);

    // Seamless automatic reconnect after synchronized F9 / Menu Load / Party Wipe finishes loading world!
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

    if (m_pendingPreJoinBackup && !IsLoadingMenuOpen())
    {
        m_preJoinBackupDelaySec -= acEvent.Delta;
        if (m_preJoinBackupDelaySec <= 0.0)
        {
            std::string reason;
            if (CanSafelySaveNow(reason))
            {
                ExecuteAuthorizedSave(m_preJoinBackupSlot);
                m_pendingPreJoinBackup = false;
            }
        }
    }

    // Once 2+ players have joined the party, automatically take Checkpoint #1 after a short 4.0s settle
    // so both players immediately share a synchronized world + character save even before pressing F5!
    if (!m_initialPartyCheckpointSent &&
        m_lastCommittedEpoch == 0 &&
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

    if (!m_deferredLoadSlot.empty() && !IsLoadingMenuOpen())
    {
        const std::string slot = std::move(m_deferredLoadSlot);
        m_deferredLoadSlot.clear();
        ExecuteAuthorizedLoad(slot);
    }
}

void CoordinatedSaveService::OnNotifyPrepareSave(const NotifyPrepareSave& acMessage) noexcept
{
    const std::string slotName(acMessage.SlotName.c_str(), acMessage.SlotName.size());
    std::string failReason;

    CoordinatedSaveAck ack{};
    ack.EpochId = acMessage.EpochId;
    ack.SlotName = acMessage.SlotName;

    if (!CanSafelySaveNow(failReason))
    {
        ack.Success = false;
        ack.Reason = String(failReason.c_str());
        m_transport.Send(ack);
        return;
    }

    ack.Success = ExecuteAuthorizedSave(slotName);
    if (!ack.Success)
        ack.Reason = String("не удалось записать слот сохранения");

    m_transport.Send(ack);
}

void CoordinatedSaveService::OnNotifyCommitSave(const NotifyCommitSave& acMessage) noexcept
{
    if (acMessage.Committed)
    {
        m_lastCommittedEpoch = acMessage.EpochId;
        m_lastCommittedSlot = std::string(acMessage.SlotName.c_str(), acMessage.SlotName.size());
    }

    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);
}

void CoordinatedSaveService::OnNotifyExecuteLoad(const NotifyExecuteLoad& acMessage) noexcept
{
    if (!acMessage.Message.empty())
        Utils::ShowHudMessage(acMessage.Message);

    std::string targetSlot(acMessage.SlotName.c_str(), acMessage.SlotName.size());

    // If this client joined mid-session AFTER EpochId was written and doesn't have targetSlot locally,
    // fall back cleanly to its own latest local checkpoint or PreJoin backup slot!
    if ((targetSlot.empty() || !m_writtenSlotsInSession.contains(targetSlot)) && acMessage.AllowPreJoinFallback)
    {
        if (!m_lastCommittedSlot.empty() && m_writtenSlotsInSession.contains(m_lastCommittedSlot))
            targetSlot = m_lastCommittedSlot;
        else if (m_writtenSlotsInSession.contains(m_preJoinBackupSlot))
            targetSlot = m_preJoinBackupSlot;
    }

    if (targetSlot.empty())
        return;

    if (IsLoadingMenuOpen())
    {
        m_deferredLoadSlot = targetSlot;
        return;
    }

    ExecuteAuthorizedLoad(targetSlot);
}
