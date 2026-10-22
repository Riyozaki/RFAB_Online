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
#include <Services/DownedStateService.h>
#include <Services/PartyService.h>
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

    ScopedSaveLoadAllowance guard;
    const bool ok = pMgr->SaveNamed(acSlotName.c_str());
    if (ok)
        m_writtenSlotsInSession.insert(acSlotName);
    return ok;
}

bool CoordinatedSaveService::ExecuteAuthorizedLoad(const std::string& acSlotName) noexcept
{
    if (acSlotName.empty())
        return false;

    auto* pMgr = BGSSaveLoadManager::Get();
    if (!pMgr)
        return false;

    m_world.GetDownedStateService().ResetAllStates();

    ScopedSaveLoadAllowance guard;
    return pMgr->LoadNamed(acSlotName.c_str(), true);
}

bool CoordinatedSaveService::HandleEngineSaveAttempt(const char* acpFileName) noexcept
{
    if (ScopedSaveLoadAllowance::IsAllowed())
        return true;

    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
        return true;

    const std::string fileName = acpFileName ? acpFileName : "";

    // Silently suppress uncoordinated solo autosaves (vanilla door autosaves & RFAB.dll combat autosaves).
    if (IsAutosaveName(fileName))
        return false;

    if (IsLocalPlayerAuthorizedLeader())
    {
        if (m_lastInterceptNoticeCooldownSec <= 0.0)
        {
            m_lastInterceptNoticeCooldownSec = 1.5;
            RequestCoordinatedSaveLoad req{};
            req.Action = RequestCoordinatedSaveLoad::ActionType::ManualSave;
            m_transport.Send(req);
            Utils::ShowHudMessage(String("RFAB Online: Координированное сохранение группы..."));
        }
    }
    else if (m_lastInterceptNoticeCooldownSec <= 0.0)
    {
        m_lastInterceptNoticeCooldownSec = 1.5;
        Utils::ShowHudMessage(String("RFAB Online: Сохранение группы запускает лидер отряда (F5)"));
    }

    return false;
}

bool CoordinatedSaveService::HandleEngineLoadAttempt(const char* acpFileName) noexcept
{
    if (ScopedSaveLoadAllowance::IsAllowed())
        return true;

    if (!m_transport.IsConnected() || !m_world.GetServerSettings().CoordinatedSavesEnabled)
        return true;

    const std::string fileName = acpFileName ? acpFileName : "";

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
            Utils::ShowHudMessage(String("RFAB Online: Синхронная загрузка чекпоинта группы..."));
        }
    }
    else if (m_lastInterceptNoticeCooldownSec <= 0.0)
    {
        m_lastInterceptNoticeCooldownSec = 1.5;
        Utils::ShowHudMessage(String("RFAB Online: Загрузку чекпоинта запускает лидер отряда"));
    }

    return false;
}

void CoordinatedSaveService::OnConnected(const ConnectedEvent&) noexcept
{
    m_writtenSlotsInSession.clear();
    m_lastCommittedEpoch = 0;
    m_lastCommittedSlot.clear();
    m_deferredLoadSlot.clear();
    m_pendingPreJoinBackup = true;
}

void CoordinatedSaveService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    m_pendingPreJoinBackup = false;
    m_deferredLoadSlot.clear();
}

void CoordinatedSaveService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (m_lastInterceptNoticeCooldownSec > 0.0)
        m_lastInterceptNoticeCooldownSec = std::max(0.0, m_lastInterceptNoticeCooldownSec - acEvent.Delta);

    if (!m_transport.IsConnected())
        return;

    if (m_pendingPreJoinBackup && !IsLoadingMenuOpen())
    {
        std::string reason;
        if (CanSafelySaveNow(reason))
        {
            ExecuteAuthorizedSave(m_preJoinBackupSlot);
            m_pendingPreJoinBackup = false;
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
