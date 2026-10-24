#include <TiltedOnlinePCH.h>

#include <Services/RfabAdapterService.h>

#include <World.h>
#include <Utils.h>
#include <Components.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/PartyLeftEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/BGSListForm.h>
#include <Forms/TESGlobal.h>
#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <OverlayApp.hpp>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/OverlayService.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
std::filesystem::path DiscoverSkyrimRootDirectory() noexcept
{
#ifdef _WIN32
    wchar_t exePath[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (len > 0 && len < std::size(exePath))
    {
        std::error_code ec;
        const auto dir = std::filesystem::path(exePath).parent_path();
        if (!dir.empty() && std::filesystem::exists(dir, ec))
            return dir;
    }
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

bool EqualsIgnoreCase(std::string_view aA, std::string_view aB) noexcept
{
    if (aA.size() != aB.size())
        return false;
    for (size_t i = 0; i < aA.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(aA[i])) != std::tolower(static_cast<unsigned char>(aB[i])))
            return false;
    }
    return true;
}
} // namespace

RfabAdapterService::RfabAdapterService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
    , m_connectedConnection(aDispatcher.sink<ConnectedEvent>().connect<&RfabAdapterService::OnConnected>(this))
    , m_disconnectedConnection(aDispatcher.sink<DisconnectedEvent>().connect<&RfabAdapterService::OnDisconnected>(this))
    , m_partyJoinedConnection(aDispatcher.sink<PartyJoinedEvent>().connect<&RfabAdapterService::OnPartyJoined>(this))
    , m_partyLeftConnection(aDispatcher.sink<PartyLeftEvent>().connect<&RfabAdapterService::OnPartyLeft>(this))
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&RfabAdapterService::OnUpdate>(this))
    , m_progressionConnection(aDispatcher.sink<NotifyRfabProgressionEvent>().connect<&RfabAdapterService::OnNotifyProgressionEvent>(this))
{
    RefreshRuntimeBindings();
}

RfabAdapterService::~RfabAdapterService() noexcept
{
    RestoreSoloRfabAutosave();
}

void RfabAdapterService::RefreshRuntimeBindings() noexcept
{
    const std::filesystem::path gameRoot = DiscoverSkyrimRootDirectory();
    m_bindings = RfabCompat::LoadBindingsFromDisk(gameRoot);
    m_liveXpTable = {};

    std::string raw;
    if (RfabCompat::Detail::ReadTextFile(gameRoot / m_bindings.ExperienceIniRelPath, raw))
        RfabCompat::ParseExperienceIni(raw, m_liveXpTable);
    if (RfabCompat::Detail::ReadTextFile(gameRoot / m_bindings.QuestsIniRelPath, raw))
        RfabCompat::ParseQuestsIni(raw, m_liveXpTable);
    if (RfabCompat::Detail::ReadTextFile(gameRoot / m_bindings.XpSettingsJsonRelPath, raw))
        RfabCompat::ParseXpSettingsJson(raw, m_liveXpTable);

    m_pCachedXpGlobal = nullptr;
    m_pCachedXpMultiplierGlobal = nullptr;
    m_pNoDeathHandlingGlobal = nullptr;
    m_pXpBooksFormList = nullptr;
    m_pNoAllyDamageAllowedSpellsList.store(nullptr, std::memory_order_release);
    m_scannedGlobalsOnce = false;
    m_watchedGlobals.clear();
    m_lastGlobalValues.clear();
}

TESGlobal* RfabAdapterService::ResolveGlobalByEditorId(const std::string& acEditorId, uint32_t aFallbackBaseId) noexcept
{
    if (!acEditorId.empty())
    {
        if (TESForm* pByEdid = TESForm::GetByEditorId(BSFixedString(acEditorId.c_str())))
        {
            if (auto* pGlobal = Cast<TESGlobal>(pByEdid))
                return pGlobal;
        }

        if (auto* pModManager = ModManager::Get())
        {
            for (uint32_t i = 0; i < pModManager->globals.length; ++i)
            {
                TESGlobal* pCandidate = pModManager->globals[i];
                if (!pCandidate)
                    continue;

                const char* edid = pCandidate->GetFormEditorID();
                if (edid && EqualsIgnoreCase(edid, acEditorId))
                    return pCandidate;
            }
        }
    }

    if (aFallbackBaseId != 0)
    {
        if (auto* pModManager = ModManager::Get())
        {
            if (auto* pMod = pModManager->GetByName(m_bindings.PluginName.c_str()))
                return Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(aFallbackBaseId)));
        }
    }

    return nullptr;
}

BGSListForm* RfabAdapterService::ResolveFormListByEditorId(const std::string& acEditorId) noexcept
{
    if (acEditorId.empty())
        return nullptr;

    if (TESForm* pByEdid = TESForm::GetByEditorId(BSFixedString(acEditorId.c_str())))
    {
        if (auto* pList = Cast<BGSListForm>(pByEdid))
            return pList;
    }

    if (auto* pModManager = ModManager::Get())
    {
        for (uint32_t i = 0; i < pModManager->formLists.length; ++i)
        {
            BGSListForm* pCandidate = pModManager->formLists[i];
            if (!pCandidate)
                continue;

            const char* edid = pCandidate->GetFormEditorID();
            if (edid && EqualsIgnoreCase(edid, acEditorId))
                return pCandidate;
        }
    }

    return nullptr;
}

TESGlobal* RfabAdapterService::GetRfabXpGlobal() noexcept
{
    if (m_pCachedXpGlobal)
        return m_pCachedXpGlobal;

    m_pCachedXpGlobal = ResolveGlobalByEditorId(m_bindings.XpAmountGlobalEditorId, m_bindings.FallbackXpAmountBaseFormId);
    return m_pCachedXpGlobal;
}

float RfabAdapterService::GetLocalXpMultiplier() noexcept
{
    if (!m_pCachedXpMultiplierGlobal)
    {
        m_pCachedXpMultiplierGlobal = ResolveGlobalByEditorId("RFAB_XP_Multiplier", 0x00112815);
        if (!m_pCachedXpMultiplierGlobal && !m_bindings.XpBonusGlobalEditorId.empty())
            m_pCachedXpMultiplierGlobal = ResolveGlobalByEditorId(m_bindings.XpBonusGlobalEditorId, 0);
    }

    if (!m_pCachedXpMultiplierGlobal || !std::isfinite(m_pCachedXpMultiplierGlobal->f) || m_pCachedXpMultiplierGlobal->f <= 0.05f)
        return 1.0f;

    return m_pCachedXpMultiplierGlobal->f;
}

int32_t RfabAdapterService::ReadRfabXp(TESGlobal* apGlobal) noexcept
{
    if (!apGlobal)
        return 0;

    return static_cast<int32_t>(std::lround(std::max(0.0f, apGlobal->f)));
}

bool RfabAdapterService::ApplyDualPoolExperienceDelta(int32_t aDelta, int32_t& aOutNewTotalXp) noexcept
{
    TESGlobal* pRfabXp = GetRfabXpGlobal();
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pRfabXp || !pPlayer)
        return false;

    // Update BOTH pools directly in C++ memory (matching RFAB_XP_Handler.psc::ModXP + skse64 PapyrusGame.cpp):
    // 1. TESGlobal RFAB_XP_Amount (0xAD45BB in RFAB.esp) — spent inside RFAB_Menu to raise skills & perks.
    // 2. PlayerCharacter->pSkills->data->xp (offset 0x00) — exact field mutated by Game.SetPlayerExperience().
    // Note: Game.GetPlayerExperience / Game.SetPlayerExperience are SKSE-registered functions in skse64_1_5_97.dll,
    // NOT vanilla SkyrimSE.exe functions, so querying PapyrusService for them previously returned nullptr!
    aOutNewTotalXp = std::max(0, ReadRfabXp(pRfabXp) + aDelta);
    pRfabXp->f = static_cast<float>(aOutNewTotalXp);

    bool levelUpReady = false;
    if (pPlayer->pSkills && pPlayer->pSkills->data)
    {
        auto* pSkillsData = pPlayer->pSkills->data;
        pSkillsData->xp = std::max(0.0f, pSkillsData->xp + static_cast<float>(aDelta));
        levelUpReady = (pSkillsData->levelThreshold > 0.0f && pSkillsData->xp >= pSkillsData->levelThreshold);
    }

    if (aDelta > 0)
    {
        char msgBuf[160]{};
        if (levelUpReady)
            std::snprintf(msgBuf, sizeof(msgBuf), "Опыт отряда: +%d XP (доступен новый уровень!)", aDelta);
        else
            std::snprintf(msgBuf, sizeof(msgBuf), "Опыт отряда: +%d XP", aDelta);
        Utils::ShowHudMessage(String(msgBuf));
    }

    return true;
}

void RfabAdapterService::SuppressSoloRfabAutosave() noexcept
{
    if (m_autosaveSuppressed)
        return;

    const std::filesystem::path candidate = DiscoverSkyrimRootDirectory() / m_bindings.RfabAutosaveIniRelPath;
    std::string raw;
    if (!RfabCompat::Detail::ReadTextFile(candidate, raw))
        return;

    m_autosaveIniPath = candidate;
    m_originalAutosaveIniContent = raw;
    const std::string suppressed = RfabCompat::SuppressRfabAutosaveIniContent(raw);
    if (suppressed != raw)
        RfabCompat::Detail::WriteTextFile(candidate, suppressed);

    m_autosaveSuppressed = true;
}

void RfabAdapterService::RestoreSoloRfabAutosave() noexcept
{
    if (!m_autosaveSuppressed)
        return;

    if (!m_autosaveIniPath.empty() && !m_originalAutosaveIniContent.empty())
        RfabCompat::Detail::WriteTextFile(m_autosaveIniPath, m_originalAutosaveIniContent);

    m_autosaveSuppressed = false;
}

BGSListForm* RfabAdapterService::GetNoAllyDamageAllowedSpellsList() noexcept
{
    if (BGSListForm* pCached = m_pNoAllyDamageAllowedSpellsList.load(std::memory_order_acquire))
        return pCached;

    std::lock_guard<std::mutex> lock(m_allowedSpellsMutex);
    if (BGSListForm* pCached = m_pNoAllyDamageAllowedSpellsList.load(std::memory_order_relaxed))
        return pCached;

    BGSListForm* pResolved = ResolveFormListByEditorId("RFAB_List_NoAllyDamage_AllowedSpells");
    if (!pResolved)
    {
        if (auto* pModManager = ModManager::Get())
        {
            if (auto* pMod = pModManager->GetByName(m_bindings.PluginName.c_str()))
                pResolved = Cast<BGSListForm>(TESForm::GetById(pMod->GetFormId(0x005CAA)));
        }
    }

    if (pResolved)
        m_pNoAllyDamageAllowedSpellsList.store(pResolved, std::memory_order_release);
    return pResolved;
}

void RfabAdapterService::ApplyAllyHitGameSetting(bool aProtectAllies) noexcept
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

bool RfabAdapterService::IsLocalPlayerAllyProtectionHost() const noexcept
{
    if (!m_transport.IsConnected())
        return false;

    const auto& partySvc = m_world.GetPartyService();
    if (partySvc.IsInParty())
        return partySvc.IsLeader();

    return m_isServerHost;
}

void RfabAdapterService::SendHostAllyProtectionRequest(bool aEnabled, bool aNotifyPeers) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::AllyProtectionSync;
    ev.EpochId = m_currentWorldEpoch;
    ev.EditorId = aNotifyPeers ? String("toggle") : String("");
    ev.GlobalValue = aEnabled ? 1.0f : 0.0f;
    m_transport.Send(ev);
}

void RfabAdapterService::PushAllyProtectionStateToOverlay() const noexcept
{
    if (auto* pOverlayApp = m_world.GetOverlayService().GetOverlayApp())
    {
        auto pArgs = CefListValue::Create();
        pArgs->SetBool(0, m_allyProtectionEnabled.load(std::memory_order_relaxed));
        pArgs->SetBool(1, IsLocalPlayerAllyProtectionHost());
        pOverlayApp->ExecuteAsync("allyProtectionState", pArgs);
    }
}

void RfabAdapterService::NotifyPartyLeadershipChanged() noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::RefreshCoopCombatSnapshot() noexcept
{
    // Must only be called on the World::Update() thread where entt views and PartyService are mutated.
    CoopCombatSnapshot next{};
    next.Connected = m_transport.IsConnected();
    next.AllyProtectionEnabled = m_allyProtectionEnabled.load(std::memory_order_relaxed);

    if (next.Connected)
    {
        const auto& partySvc = m_world.GetPartyService();
        next.InParty = partySvc.IsInParty();
        if (next.InParty)
        {
            const auto& members = partySvc.GetPartyMembers();
            next.PartyMemberIds.reserve(members.size());
            for (const uint32_t pid : members)
                next.PartyMemberIds.insert(pid);
        }

        auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
        for (auto entity : playerView)
        {
            const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
            if (fid != 0)
                next.FormIdToPlayerId[fid] = playerView.get<PlayerComponent>(entity).Id;
        }
    }

    std::lock_guard<std::mutex> lock(m_coopSnapshotMutex);
    m_coopCombatSnapshot = std::move(next);
}

void RfabAdapterService::SetLocalAllyProtectionEnabled(bool aEnabled, bool aShowHudNotification) noexcept
{
    if (!m_transport.IsConnected())
    {
        PushAllyProtectionStateToOverlay();
        return;
    }

    if (!IsLocalPlayerAllyProtectionHost())
    {
        if (aShowHudNotification)
            Utils::ShowHudMessage(String("RFAB Online: Только Хост может включать и выключать урон по союзникам"));
        PushAllyProtectionStateToOverlay();
        return;
    }

    m_allyProtectionEnabled.store(aEnabled, std::memory_order_relaxed);
    ApplyAllyHitGameSetting(aEnabled);
    RefreshCoopCombatSnapshot();
    SendHostAllyProtectionRequest(aEnabled, aShowHudNotification);
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::ToggleLocalAllyProtection() noexcept
{
    SetLocalAllyProtectionEnabled(!m_allyProtectionEnabled.load(std::memory_order_relaxed), true);
}

RfabCompat::CoopAllyActionPolicy RfabAdapterService::EvaluateCoopAction(
    Actor* apAttacker,
    Actor* apTarget,
    bool aIsBeneficialHealOrBuff) const noexcept
{
    if (!apAttacker || !apTarget || apAttacker == apTarget)
        return {};

    // Read a mutex-protected snapshot built on the World::Update() thread instead of touching
    // entt::registry views or TiltedPhoques::Vector<uint32_t> directly from combat/magic worker threads.
    CoopCombatSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_coopSnapshotMutex);
        if (!m_coopCombatSnapshot.Connected || !m_coopCombatSnapshot.InParty)
            return {};
        snapshot = m_coopCombatSnapshot;
    }

    struct ControllingPlayer
    {
        bool Valid{false};
        bool IsLocal{false};
        uint32_t RemotePlayerId{0};
    };

    const auto findPlayerIdByFormId = [&snapshot](uint32_t aFormId) -> std::optional<uint32_t>
    {
        if (aFormId == 0)
            return std::nullopt;
        const auto it = snapshot.FormIdToPlayerId.find(aFormId);
        if (it != snapshot.FormIdToPlayerId.end())
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

    const auto isMember = [&snapshot](uint32_t aPid) -> bool
    {
        return snapshot.PartyMemberIds.find(aPid) != snapshot.PartyMemberIds.end();
    };

    bool areSameCoopParty = false;
    if (attacker.IsLocal)
        areSameCoopParty = isMember(target.RemotePlayerId);
    else if (target.IsLocal)
        areSameCoopParty = isMember(attacker.RemotePlayerId);
    else
        areSameCoopParty = isMember(attacker.RemotePlayerId) && isMember(target.RemotePlayerId);

    return RfabCompat::EvaluateCoopAllyActionPolicy(areSameCoopParty, snapshot.AllyProtectionEnabled, aIsBeneficialHealOrBuff);
}

void RfabAdapterService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    RefreshRuntimeBindings();
    SuppressSoloRfabAutosave();
    m_localPlayerId = acEvent.PlayerId;
    m_hasLocalPlayerId = true;
    m_isServerHost = false;
    m_allyProtectionEnabled.store(true, std::memory_order_relaxed);
    ApplyAllyHitGameSetting(true);
    (void)GetNoAllyDamageAllowedSpellsList();
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();

    m_pNoDeathHandlingGlobal = ResolveGlobalByEditorId(m_bindings.NoDeathHandlingGlobalEditorId, 0);
    if (m_pNoDeathHandlingGlobal)
    {
        m_savedNoDeathHandlingValue = m_pNoDeathHandlingGlobal->f;
        m_hasSavedNoDeathHandlingValue = true;
        m_pNoDeathHandlingGlobal->f = 1.0f;
    }

    m_knownReadBookFormIds.clear();
    m_pXpBooksFormList = ResolveFormListByEditorId(m_bindings.XpBooksFormListEditorId);
    if (m_pXpBooksFormList)
    {
        m_pXpBooksFormList->ForEachFormId([this](uint32_t formId) {
            m_knownReadBookFormIds.insert(formId);
        });
        m_seededReadBooks = true;
    }
    else
    {
        m_seededReadBooks = false;
    }
}

void RfabAdapterService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    RestoreSoloRfabAutosave();
    ApplyAllyHitGameSetting(false);
    m_hasLocalPlayerId = false;
    m_isServerHost = false;
    m_allyProtectionEnabled.store(true, std::memory_order_relaxed);
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();

    if (m_pNoDeathHandlingGlobal && m_hasSavedNoDeathHandlingValue)
    {
        m_pNoDeathHandlingGlobal->f = m_savedNoDeathHandlingValue;
        m_hasSavedNoDeathHandlingValue = false;
    }

    m_knownReadBookFormIds.clear();
    m_watchedGlobals.clear();
    m_lastGlobalValues.clear();
    m_scannedGlobalsOnce = false;
    m_seededReadBooks = false;
    m_currentWorldEpoch = 1;
}

void RfabAdapterService::OnPartyJoined(const PartyJoinedEvent&) noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::OnPartyLeft(const PartyLeftEvent&) noexcept
{
    RefreshCoopCombatSnapshot();
    PushAllyProtectionStateToOverlay();
}

void RfabAdapterService::NotifySaveLoadReset() noexcept
{
    m_knownReadBookFormIds.clear();
    m_watchedGlobals.clear();
    m_lastGlobalValues.clear();
    m_scannedGlobalsOnce = false;
    m_seededReadBooks = false;
    // Allow 2.0s post-load settle window so BGSSaveLoadManager finishes restoring ESS state
    // and authoritative server resync packets land before we poll local FormLists/TESGlobals again.
    m_pollAccumulatorSec = -2.0;
}

void RfabAdapterService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    RefreshCoopCombatSnapshot();

    m_pollAccumulatorSec += acEvent.Delta;
    if (m_pollAccumulatorSec < 0.5)
        return;
    m_pollAccumulatorSec = 0.0;

    if (m_pNoDeathHandlingGlobal && m_pNoDeathHandlingGlobal->f < 1.0f)
        m_pNoDeathHandlingGlobal->f = 1.0f;

    PollReadXpBooks();
    PollSyncedGlobals();
}

void RfabAdapterService::PollReadXpBooks() noexcept
{
    if (!m_pXpBooksFormList)
        m_pXpBooksFormList = ResolveFormListByEditorId(m_bindings.XpBooksFormListEditorId);
    if (!m_pXpBooksFormList)
        return;

    if (!m_seededReadBooks)
    {
        m_pXpBooksFormList->ForEachFormId([this](uint32_t formId) {
            m_knownReadBookFormIds.insert(formId);
        });
        m_seededReadBooks = true;
        return;
    }

    auto& modSystem = m_world.GetModSystem();
    m_pXpBooksFormList->ForEachFormId([this, &modSystem](uint32_t formId) {
        if (!m_knownReadBookFormIds.insert(formId).second)
            return;

        GameId netBookId{};
        if (!modSystem.GetServerModId(formId, netBookId))
            return;

        char keyBuf[64]{};
        std::snprintf(keyBuf, sizeof(keyBuf), "book:%08X:%08X", netBookId.ModId, netBookId.BaseId);

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::BookRead;
        ev.EpochId = m_currentWorldEpoch;
        ev.SourceKey = String(keyBuf);
        ev.BookId = netBookId;
        ev.XpDelta = 0; // Numeric XP delta continues flowing smoothly; FormList prevents double-reading by peers!
        m_transport.Send(ev);
    });
}

void RfabAdapterService::PollSyncedGlobals() noexcept
{
    if (!m_scannedGlobalsOnce)
    {
        auto* pModManager = ModManager::Get();
        if (!pModManager)
            return;

        for (uint32_t i = 0; i < pModManager->globals.length; ++i)
        {
            TESGlobal* pGlob = pModManager->globals[i];
            if (!pGlob)
                continue;

            const char* edid = pGlob->GetFormEditorID();
            if (!edid || edid[0] == '\0')
                continue;

            if (RfabCompat::ShouldAutoSyncGlobalEditorId(edid, m_bindings))
            {
                std::string name(edid);
                m_watchedGlobals[name] = pGlob;
                m_lastGlobalValues[name] = pGlob->f;
            }
        }
        m_scannedGlobalsOnce = true;
    }

    for (auto& [editorId, pGlob] : m_watchedGlobals)
    {
        if (!pGlob || !std::isfinite(pGlob->f))
            continue;

        const float currentVal = pGlob->f;
        const float prevVal = m_lastGlobalValues[editorId];
        if (std::fabs(currentVal - prevVal) < 1e-4f)
            continue;

        m_lastGlobalValues[editorId] = currentVal;

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::GlobalValueSync;
        ev.EpochId = m_currentWorldEpoch;
        ev.EditorId = String(editorId.c_str());
        ev.GlobalValue = currentVal;
        m_transport.Send(ev);
    }
}

void RfabAdapterService::OnNotifyProgressionEvent(const NotifyRfabProgressionEvent& acMessage) noexcept
{
    if (acMessage.EpochId > m_currentWorldEpoch)
        m_currentWorldEpoch = acMessage.EpochId;

    switch (acMessage.Kind)
    {
    case NotifyRfabProgressionEvent::EventKind::BookRead:
    {
        if (!m_pXpBooksFormList)
            m_pXpBooksFormList = ResolveFormListByEditorId(m_bindings.XpBooksFormListEditorId);

        if (m_pXpBooksFormList && acMessage.BookId)
        {
            const uint32_t localFormId = m_world.GetModSystem().GetGameId(acMessage.BookId);
            if (localFormId != 0)
            {
                m_knownReadBookFormIds.insert(localFormId);
                if (!m_pXpBooksFormList->ContainsFormId(localFormId))
                {
                    if (TESForm* pBookForm = TESForm::GetById(localFormId))
                        m_pXpBooksFormList->AddForm(pBookForm);
                }
            }
        }

        if (acMessage.XpDelta != 0)
            m_world.GetCharacterService().ApplyAuthoritativeRfabXpDelta(acMessage.XpDelta);
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::LedgerXpGrant:
    case NotifyRfabProgressionEvent::EventKind::LedgerXpRebate:
    {
        if (acMessage.XpDelta != 0)
            m_world.GetCharacterService().ApplyAuthoritativeRfabXpDelta(acMessage.XpDelta);
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::GlobalValueSync:
    {
        const std::string editorId(acMessage.EditorId.c_str(), acMessage.EditorId.size());
        if (editorId.empty() || !RfabCompat::ShouldAutoSyncGlobalEditorId(editorId, m_bindings))
            break;

        TESGlobal* pTarget = nullptr;
        if (const auto it = m_watchedGlobals.find(editorId); it != m_watchedGlobals.end())
            pTarget = it->second;
        else
            pTarget = ResolveGlobalByEditorId(editorId, 0);

        if (pTarget)
        {
            m_watchedGlobals[editorId] = pTarget;
            m_lastGlobalValues[editorId] = acMessage.GlobalValue; // Set before assignment to suppress echo!
            pTarget->f = acMessage.GlobalValue;
        }
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::FullLedgerResync:
        break;

    case NotifyRfabProgressionEvent::EventKind::AllyProtectionSync:
    {
        const bool newState = (acMessage.GlobalValue >= 0.5f);
        m_allyProtectionEnabled.store(newState, std::memory_order_relaxed);

        if (m_hasLocalPlayerId && acMessage.XpDelta >= 0)
            m_isServerHost = (static_cast<uint32_t>(acMessage.XpDelta) == m_localPlayerId);
        else
            m_isServerHost = false;

        ApplyAllyHitGameSetting(newState);
        RefreshCoopCombatSnapshot();
        PushAllyProtectionStateToOverlay();

        if (acMessage.EditorId == "toggle")
        {
            if (newState)
                Utils::ShowHudMessage(String("Единство в битве (Кооп): урон по союзникам отключен"));
            else
                Utils::ShowHudMessage(String("Единство в битве (Кооп): урон по союзникам включен"));
        }
        break;
    }
    }
}
