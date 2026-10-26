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
#include <Forms/ActorValueInfo.h>
#include <Forms/BGSListForm.h>
#include <Forms/TESGlobal.h>
#include <Forms/TESNPC.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Games/Overrides.h>
#include <Games/Misc/Lock.h>
#include <Games/Skyrim/AI/Movement/PlayerControls.h>
#include <Games/Skyrim/Interface/MenuControls.h>
#include <Games/Skyrim/Interface/Menus/HUDMenuUtils.h>
#include <Games/Skyrim/Interface/UI.h>
#include <Games/TES.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Camera/PlayerCamera.h>
#include <Combat/CombatController.h>
#include <Messages/NotifyPlayerCellChanged.h>
#include <Messages/NotifyPlayerHealthUpdate.h>
#include <Messages/NotifyPlayerJoined.h>
#include <Messages/NotifyPlayerLeft.h>
#include <Messages/NotifyPlayerLevel.h>
#include <Messages/NotifyPlayerList.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <OverlayApp.hpp>
#include <Actor.h>
#include <Games/ActorExtension.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/DownedStateService.h>
#include <Services/ImguiService.h>
#include <Services/OverlayService.h>
#include <Services/PapyrusService.h>
#include <Services/PartyService.h>
#include <Services/TransportService.h>
#include <imgui.h>

#include <algorithm>
#include <array>
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
    , m_playerJoinedConnection(aDispatcher.sink<NotifyPlayerJoined>().connect<&RfabAdapterService::OnPlayerJoined>(this))
    , m_playerLeftConnection(aDispatcher.sink<NotifyPlayerLeft>().connect<&RfabAdapterService::OnPlayerLeft>(this))
    , m_playerLevelConnection(aDispatcher.sink<NotifyPlayerLevel>().connect<&RfabAdapterService::OnPlayerLevel>(this))
    , m_playerCellChangedConnection(aDispatcher.sink<NotifyPlayerCellChanged>().connect<&RfabAdapterService::OnPlayerCellChanged>(this))
    , m_playerHealthUpdateConnection(aDispatcher.sink<NotifyPlayerHealthUpdate>().connect<&RfabAdapterService::OnPlayerHealthUpdate>(this))
    , m_playerListConnection(aDispatcher.sink<NotifyPlayerList>().connect<&RfabAdapterService::OnPlayerList>(this))
    , m_drawImGuiConnection(aWorld.ctx().at<ImguiService>().OnDraw.connect<&RfabAdapterService::OnDrawTacticalHud>(this))
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
    m_espEdidIndex.Clear();

    std::string raw;
    if (const auto expPath = RfabCompat::ResolveRfabConfigPath(
            gameRoot, m_bindings.ExperienceIniRelPath, {"Experience.ini", "[RFAB] Experience.ini", "RFAB_Experience.ini"});
        !expPath.empty() && RfabCompat::Detail::ReadTextFile(expPath, raw))
    {
        RfabCompat::ParseExperienceIni(raw, m_liveXpTable);
    }

    if (const auto questsPath = RfabCompat::ResolveRfabConfigPath(
            gameRoot, m_bindings.QuestsIniRelPath, {"Quests.ini", "[RFAB] Quests.ini", "RFAB_Quests.ini"});
        !questsPath.empty() && RfabCompat::Detail::ReadTextFile(questsPath, raw))
    {
        RfabCompat::ParseQuestsIni(raw, m_liveXpTable);
    }

    if (const auto xpSettingsPath = RfabCompat::ResolveRfabConfigPath(
            gameRoot, m_bindings.XpSettingsJsonRelPath, {"RFAB_XP_Settings.json", "XP_Settings.json"});
        !xpSettingsPath.empty() && RfabCompat::Detail::ReadTextFile(xpSettingsPath, raw))
    {
        RfabCompat::ParseXpSettingsJson(raw, m_liveXpTable);
    }

    if (const auto bossListPath = RfabCompat::ResolveRfabConfigPath(
            gameRoot, m_bindings.BossListJsonRelPath, {"BossList.json", "RFAB_BossList.json", "[RFAB] BossList.json"});
        !bossListPath.empty() && RfabCompat::Detail::ReadTextFile(bossListPath, raw))
    {
        RfabCompat::ParseBossListJson(raw, m_liveXpTable);
    }

    // Build zero-dependency binary EDID <-> (PluginName, LocalFormId) index from installed RFAB/Requiem .esp/.esm/.esl files
    // so forms whose runtime EDIDs Skyrim SE strips (BGSListForm, BGSPerk, TESNPC, TESObjectARMO, TESObjectACTI) resolve across ANY RFAB version!
    if (!gameRoot.empty())
    {
        const size_t indexedPlugins = m_espEdidIndex.IndexRfabPluginsInDataDir(gameRoot / "Data", m_bindings);
        if (indexedPlugins > 0)
        {
            spdlog::info(
                "[RFAB] Indexed {} EDIDs across {} .esp/.esm/.esl plugin(s) for cross-version RFAB compatibility.",
                m_espEdidIndex.Size(),
                indexedPlugins);
        }
    }

    m_pCachedXpGlobal = nullptr;
    m_pCachedXpMultiplierGlobal = nullptr;
    m_pNoDeathHandlingGlobal = nullptr;
    m_pXpBooksFormList = nullptr;
    m_pNoAllyDamageAllowedSpellsList.store(nullptr, std::memory_order_release);
    m_scannedGlobalsOnce = false;
    m_watchedGlobals.clear();
    m_lastGlobalValues.clear();
    m_lastBossGhostByServerId.clear();

    if (m_bindings.TacticalHudDefaultMode <= 3)
        m_tacticalHudMode = static_cast<RfabCompat::TacticalHudMode>(m_bindings.TacticalHudDefaultMode);
    else
        m_tacticalHudMode = RfabCompat::TacticalHudMode::Full;
}

Mod* RfabAdapterService::ResolveRfabPluginMod() const noexcept
{
    auto* pModManager = ModManager::Get();
    if (!pModManager)
        return nullptr;

    if (!m_bindings.PluginName.empty())
    {
        if (Mod* pMod = pModManager->GetByName(m_bindings.PluginName.c_str()))
            return pMod;
    }

    for (const auto& candidateName : m_bindings.SecondaryPluginNames)
    {
        if (candidateName.empty())
            continue;
        if (Mod* pMod = pModManager->GetByName(candidateName.c_str()))
            return pMod;
    }

    return nullptr;
}

TESForm* RfabAdapterService::ResolveFormByEditorIdOrBaseId(const std::string& acEditorId, uint32_t aFallbackBaseId) const noexcept
{
    if (!acEditorId.empty())
    {
        if (TESForm* pByEdid = TESForm::GetByEditorId(BSFixedString(acEditorId.c_str())))
            return pByEdid;

        if (const auto* pEspRef = m_espEdidIndex.LookupByEditorId(acEditorId))
        {
            if (auto* pModManager = ModManager::Get())
            {
                if (Mod* pMod = pModManager->GetByName(pEspRef->PluginName.c_str()))
                {
                    if (TESForm* pResolved = TESForm::GetById(pMod->GetFormId(pEspRef->LocalFormId)))
                        return pResolved;
                }
            }
        }
    }

    if (aFallbackBaseId != 0)
    {
        if (Mod* pMod = ResolveRfabPluginMod())
            return TESForm::GetById(pMod->GetFormId(aFallbackBaseId));
    }

    return nullptr;
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

        if (const auto* pEspRef = m_espEdidIndex.LookupByEditorId(acEditorId))
        {
            if (auto* pModManager = ModManager::Get())
            {
                if (Mod* pMod = pModManager->GetByName(pEspRef->PluginName.c_str()))
                {
                    if (auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(pEspRef->LocalFormId))))
                        return pGlobal;
                }
            }
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
        if (Mod* pMod = ResolveRfabPluginMod())
            return Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(aFallbackBaseId)));
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

    if (const auto* pEspRef = m_espEdidIndex.LookupByEditorId(acEditorId))
    {
        if (auto* pModManager = ModManager::Get())
        {
            if (Mod* pMod = pModManager->GetByName(pEspRef->PluginName.c_str()))
            {
                if (auto* pList = Cast<BGSListForm>(TESForm::GetById(pMod->GetFormId(pEspRef->LocalFormId))))
                    return pList;
            }
        }
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

    BGSListForm* pResolved = ResolveFormListByEditorId(m_bindings.NoAllyDamageAllowedSpellsEditorId);
    if (!pResolved && m_bindings.FallbackNoAllyDamageAllowedSpellsBaseFormId != 0)
    {
        if (Mod* pMod = ResolveRfabPluginMod())
            pResolved = Cast<BGSListForm>(TESForm::GetById(pMod->GetFormId(m_bindings.FallbackNoAllyDamageAllowedSpellsBaseFormId)));
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

        const auto& downedSvc = m_world.GetDownedStateService();
        auto tryAppendStandingPlayer = [&](Actor* apPlayerActor, bool aIsDowned)
        {
            if (!apPlayerActor || aIsDowned || apPlayerActor->VirtIsDead(false))
                return;
            if (auto* pExt = apPlayerActor->GetExtension(); pExt && pExt->IsDownedPlayer())
                return;

            StandingPlayerSnapshotEntry entry{};
            entry.FormId = apPlayerActor->formID;
            entry.HandleBits = apPlayerActor->GetHandle().handle.iBits;
            entry.WorldSpaceId = apPlayerActor->GetWorldSpace() ? apPlayerActor->GetWorldSpace()->formID : 0u;
            entry.CellId = apPlayerActor->parentCell ? apPlayerActor->parentCell->formID : 0u;
            entry.Position = apPlayerActor->position;
            next.StandingPlayers.push_back(entry);
        };

        auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
        bool includedLocalPlayer = false;
        for (auto entity : playerView)
        {
            const uint32_t fid = playerView.get<FormIdComponent>(entity).Id;
            const uint32_t pid = playerView.get<PlayerComponent>(entity).Id;
            if (fid != 0)
            {
                next.FormIdToPlayerId[fid] = pid;
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
    }

    std::lock_guard<std::mutex> lock(m_coopSnapshotMutex);
    m_coopCombatSnapshot = std::move(next);
}

GamePtr<Actor> RfabAdapterService::FindBestStandingPlayerTargetFor(Actor* apAttacker) const noexcept
{
    if (!apAttacker)
        return nullptr;

    const uint32_t attackerWorldId = apAttacker->GetWorldSpace() ? apAttacker->GetWorldSpace()->formID : 0u;
    const uint32_t attackerCellId = apAttacker->parentCell ? apAttacker->parentCell->formID : 0u;
    const float maxUnits = RfabCompat::MetersToSkyrimUnits(120.0f);
    const float maxUnitsSq = maxUnits * maxUnits;

    uint32_t bestHandleBits = 0;
    uint32_t bestFormId = 0;
    float bestDistSq = maxUnitsSq;

    {
        std::lock_guard<std::mutex> lock(m_coopSnapshotMutex);
        if (!m_coopCombatSnapshot.Connected || m_coopCombatSnapshot.StandingPlayers.empty())
            return nullptr;

        for (const auto& candidate : m_coopCombatSnapshot.StandingPlayers)
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
    }

    GamePtr<Actor> spReplacement = bestHandleBits ? Actor::LookupByHandle(bestHandleBits) : nullptr;
    if (!spReplacement && bestFormId == 0x14)
        spReplacement = PlayerCharacter::Get();

    if (!spReplacement || spReplacement.Get() == apAttacker || spReplacement->VirtIsDead(false))
        return nullptr;
    if (auto* pExt = spReplacement->GetExtension(); pExt && pExt->IsDownedPlayer())
        return nullptr;

    return spReplacement;
}

void RfabAdapterService::CollectPartyPeerActorFormIds(TiltedPhoques::Vector<uint32_t>& aOutFormIds) const noexcept
{
    aOutFormIds.clear();
    std::lock_guard<std::mutex> lock(m_coopSnapshotMutex);
    if (!m_coopCombatSnapshot.Connected || !m_coopCombatSnapshot.InParty)
        return;

    for (const auto& [formId, playerId] : m_coopCombatSnapshot.FormIdToPlayerId)
    {
        if (formId == 0 || formId == 0x14)
            continue;
        if (m_coopCombatSnapshot.PartyMemberIds.contains(playerId))
            aOutFormIds.push_back(formId);
    }
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
    m_lastBossGhostByServerId.clear();
    m_partyHudPeers.clear();
    m_activeTacticalPings.clear();
    ClearTacticalHudSnapshot();
    m_pingCooldownRemainingSec = 0.0;
    m_pingKeyWasDown = false;
    m_hudKeyWasDown = false;
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

void RfabAdapterService::SelfHealFreeRoamMenuControls() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell || pPlayer->VirtIsDead(false))
        return;

    if (m_world.GetDownedStateService().IsLocalPlayerDowned())
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

    // Only restore kMenu / kMainFour when full free-roam movement + look + fighting are already enabled
    // (never override genuine cutscenes, bound-hands intros, or furniture animations!).
    if ((mask & kFreeRoamActiveMask) == kFreeRoamActiveMask &&
        (mask & kMenuAndMainFourMask) != kMenuAndMainFourMask)
    {
        pInputEnable->EnableOtherEvent(static_cast<int32_t>(kMenuAndMainFourMask), true, true);
        spdlog::info("[RFAB Input] Self-healed stuck ControlMap kMenu/kMainFour flags (mask was 0x{:X})", mask);
    }
}

void RfabAdapterService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    m_controlMapHealAccumulatorSec += acEvent.Delta;
    if (m_controlMapHealAccumulatorSec >= 1.0)
    {
        m_controlMapHealAccumulatorSec = 0.0;
        SelfHealFreeRoamMenuControls();
    }

    if (!m_transport.IsConnected())
    {
        ClearTacticalHudSnapshot();
        return;
    }

    UpdateTacticalPingsAndInput(acEvent.Delta);
    RefreshCoopCombatSnapshot();
    RefreshTacticalHudSnapshot();

    m_pollAccumulatorSec += acEvent.Delta;
    if (m_pollAccumulatorSec < 0.5)
        return;
    m_pollAccumulatorSec = 0.0;

    if (m_pNoDeathHandlingGlobal && m_pNoDeathHandlingGlobal->f < 1.0f)
        m_pNoDeathHandlingGlobal->f = 1.0f;

    PollReadXpBooks();
    PollSyncedGlobals();
    PollBossEncounterState();
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

bool RfabAdapterService::IsBossActor(Actor* apActor) const noexcept
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

        if (m_liveXpTable.ContainsBossForm(ownerPluginName, localFormId))
            return true;

        if (const char* baseEdid = pBase->GetFormEditorID(); baseEdid && baseEdid[0] != '\0')
        {
            if (RfabCompat::IsRfabBossEditorId(baseEdid, &m_liveXpTable))
                return true;
        }
        else if (!ownerPluginName.empty())
        {
            const std::string indexedEdid = m_espEdidIndex.LookupEditorId(ownerPluginName, localFormId);
            if (!indexedEdid.empty() && RfabCompat::IsRfabBossEditorId(indexedEdid, &m_liveXpTable))
                return true;
        }
    }

    if (const char* refEdid = apActor->GetFormEditorID(); refEdid && refEdid[0] != '\0')
    {
        if (RfabCompat::IsRfabBossEditorId(refEdid, &m_liveXpTable))
            return true;
    }

    return false;
}

void RfabAdapterService::BroadcastArenaBarrierUnlock(const GameId& acObjectId) const noexcept
{
    if (!m_transport.IsConnected() || !acObjectId)
        return;

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
    ev.EpochId = m_currentWorldEpoch;
    ev.SourceKey = String("arena_unlock");
    ev.BookId = acObjectId;
    ev.GlobalValue = 1.0f;
    m_transport.Send(ev);
}

void RfabAdapterService::PollBossEncounterState() noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto view = m_world.view<LocalComponent, FormIdComponent>();
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

        if (!wasTracked && !IsBossActor(pActor) && !pActor->IsGhost())
            continue;

        const bool isGhost = pActor->IsGhost();
        const bool prevGhost = wasTracked ? prevIt->second : false;
        if (isGhost == prevGhost)
            continue;

        m_lastBossGhostByServerId[serverId] = isGhost;

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::BossEncounterSync;
        ev.EpochId = m_currentWorldEpoch;
        ev.SourceKey = String("boss_ghost");
        ev.XpDelta = static_cast<int32_t>(serverId);
        ev.GlobalValue = isGhost ? 1.0f : 0.0f;
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

    case NotifyRfabProgressionEvent::EventKind::BossEncounterSync:
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
            const uint32_t cObjectId = m_world.GetModSystem().GetGameId(acMessage.BookId);
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
        break;
    }

    case NotifyRfabProgressionEvent::EventKind::TacticalPingSync:
    {
        const std::string encoded(acMessage.SourceKey.c_str(), acMessage.SourceKey.size());
        RfabCompat::TacticalPingPayload payload{};
        if (!RfabCompat::DeserializeTacticalPingPayload(encoded, payload))
            break;

        if (payload.Kind == RfabCompat::TacticalPingKind::ClearSenderPings)
        {
            m_activeTacticalPings.erase(
                std::remove_if(
                    m_activeTacticalPings.begin(),
                    m_activeTacticalPings.end(),
                    [senderId = payload.SenderPlayerId](const ActiveTacticalPing& p)
                    { return p.Payload.SenderPlayerId == senderId; }),
                m_activeTacticalPings.end());
            break;
        }

        UpsertActiveTacticalPing(payload);

        char noticeBuf[256]{};
        const char* sender = payload.SenderName.empty() ? "Союзник" : payload.SenderName.c_str();
        const char* title = RfabCompat::GetTacticalPingTitleRu(payload.Kind);
        if (!payload.TargetLabel.empty())
            std::snprintf(noticeBuf, sizeof(noticeBuf), "[Метка от %s]: %s — %s", sender, title, payload.TargetLabel.c_str());
        else
            std::snprintf(noticeBuf, sizeof(noticeBuf), "[Метка от %s]: %s", sender, title);
        Utils::ShowHudMessage(String(noticeBuf));
        break;
    }
    }
}

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

    // Must ONLY be called on the Main Game Thread (inside OnUpdate / RefreshTacticalHudSnapshot), NEVER on D3D11 Render Thread!
    // Includes CustomMenu (RFAB_Menu / RFAB_SliderMenu / RFAB_ColorPicker), TweenMenu, FavoritesMenu, GiftMenu, Training Menu, Fader/Mist.
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

void RfabAdapterService::OnPlayerJoined(const NotifyPlayerJoined& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    state.Username = std::string(acMessage.Username.c_str(), acMessage.Username.size());
    if (acMessage.Level > 0)
        state.Level = acMessage.Level;
    state.CellName = ResolveCellDisplayName(m_world, acMessage.WorldSpaceId, acMessage.CellId);
}

void RfabAdapterService::OnPlayerLeft(const NotifyPlayerLeft& acMessage) noexcept
{
    m_partyHudPeers.erase(acMessage.PlayerId);
    m_activeTacticalPings.erase(
        std::remove_if(
            m_activeTacticalPings.begin(),
            m_activeTacticalPings.end(),
            [pid = acMessage.PlayerId](const ActiveTacticalPing& p) { return p.Payload.SenderPlayerId == pid; }),
        m_activeTacticalPings.end());
}

void RfabAdapterService::OnPlayerLevel(const NotifyPlayerLevel& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    if (acMessage.NewLevel > 0)
        state.Level = acMessage.NewLevel;
}

void RfabAdapterService::OnPlayerCellChanged(const NotifyPlayerCellChanged& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    state.CellName = ResolveCellDisplayName(m_world, acMessage.WorldSpaceId, acMessage.CellId);
}

void RfabAdapterService::OnPlayerHealthUpdate(const NotifyPlayerHealthUpdate& acMessage) noexcept
{
    auto& state = m_partyHudPeers[acMessage.PlayerId];
    state.PlayerId = acMessage.PlayerId;
    if (std::isfinite(acMessage.Percentage))
        state.HealthPercent = std::clamp(acMessage.Percentage, 0.0f, 100.0f);
}

void RfabAdapterService::OnPlayerList(const NotifyPlayerList& acMessage) noexcept
{
    for (const auto& [playerId, username] : acMessage.Players)
    {
        auto& state = m_partyHudPeers[playerId];
        state.PlayerId = playerId;
        state.Username = std::string(username.c_str(), username.size());
    }
}

void RfabAdapterService::SetTacticalHudMode(RfabCompat::TacticalHudMode aMode, bool aShowNotice) noexcept
{
    m_tacticalHudMode = aMode;
    if (m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden)
        ClearTacticalHudSnapshot();
    if (aShowNotice)
    {
        char buf[128]{};
        std::snprintf(buf, sizeof(buf), "Тактический HUD: %s", RfabCompat::GetTacticalHudModeNameRu(m_tacticalHudMode));
        Utils::ShowHudMessage(String(buf));
    }
}

void RfabAdapterService::UpsertActiveTacticalPing(const RfabCompat::TacticalPingPayload& acPayload) noexcept
{
    const double baseDuration = std::clamp(static_cast<double>(m_bindings.TacticalPingDurationSec), 3.0, 60.0);
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

    // Keep at most 3 simultaneous pings per sender so the screen stays clean and readable.
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
    {
        m_activeTacticalPings.erase(m_activeTacticalPings.begin() + static_cast<std::ptrdiff_t>(oldestSenderIdx));
    }

    ActiveTacticalPing entry{};
    entry.Payload = acPayload;
    entry.TotalDurationSec = durationSec;
    entry.RemainingSec = durationSec;
    m_activeTacticalPings.push_back(std::move(entry));
}

void RfabAdapterService::ClearLocalTacticalPings() noexcept
{
    const uint32_t localPid = m_hasLocalPlayerId ? m_localPlayerId : m_transport.GetLocalPlayerId();
    m_activeTacticalPings.erase(
        std::remove_if(
            m_activeTacticalPings.begin(),
            m_activeTacticalPings.end(),
            [localPid](const ActiveTacticalPing& p) { return p.Payload.SenderPlayerId == localPid; }),
        m_activeTacticalPings.end());

    if (m_transport.IsConnected())
    {
        RfabCompat::TacticalPingPayload clearPayload{};
        clearPayload.Kind = RfabCompat::TacticalPingKind::ClearSenderPings;
        clearPayload.SenderPlayerId = localPid;

        SyncRfabProgressionEvent ev{};
        ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
        ev.EpochId = m_currentWorldEpoch;
        ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(clearPayload).c_str());
        m_transport.Send(ev);
    }

    Utils::ShowHudMessage(String("Ваши тактические метки сняты"));
}

void RfabAdapterService::TriggerContextualTacticalPing(bool aForceDanger, const std::string& acCustomLabel) noexcept
{
    if (!m_transport.IsConnected())
        return;

    if (m_pingCooldownRemainingSec > 0.0)
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    m_pingCooldownRemainingSec = std::clamp(static_cast<double>(m_bindings.TacticalPingCooldownSec), 0.15, 5.0);

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

    auto view = m_world.view<FormIdComponent>();
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

        // Score combines crosshair closeness with a slight preference for closer / boss / downed targets
        float score = screenDist + (std::sqrt(distSq) / maxScanUnits) * 0.015f;
        bool isPlayer = false;
        bool isDowned = false;
        bool isBoss = false;
        bool isHostile = false;
        bool isDead = false;
        uint32_t remotePid = 0;

        if (const auto* pPlayerComp = m_world.try_get<PlayerComponent>(entity))
        {
            isPlayer = true;
            remotePid = pPlayerComp->Id;
            isDowned = m_world.GetDownedStateService().IsRemotePlayerDowned(remotePid);
            if (isDowned)
                score -= 0.025f;
        }
        else if (pActor)
        {
            isDead = pActor->IsDead();
            if (!isDead)
            {
                isBoss = IsBossActor(pActor);
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
            if (const auto* pLocal = m_world.try_get<LocalComponent>(entity))
                bestServerId = pLocal->Id;
            else if (const auto* pRemote = m_world.try_get<RemoteComponent>(entity))
                bestServerId = pRemote->Id;
            else if (const auto* pObj = m_world.try_get<ObjectComponent>(entity))
                bestServerId = pObj->Id;
        }
    }

    RfabCompat::TacticalPingPayload payload{};
    payload.SenderPlayerId = m_hasLocalPlayerId ? m_localPlayerId : m_transport.GetLocalPlayerId();
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
            if (const auto it = m_partyHudPeers.find(bestRemotePlayerId); it != m_partyHudPeers.end() && !it->second.Username.empty())
                payload.TargetLabel = it->second.Username;
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

    UpsertActiveTacticalPing(payload);

    SyncRfabProgressionEvent ev{};
    ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
    ev.EpochId = m_currentWorldEpoch;
    ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(payload).c_str());
    m_transport.Send(ev);

    char localMsg[256]{};
    const char* title = RfabCompat::GetTacticalPingTitleRu(payload.Kind);
    if (!payload.TargetLabel.empty())
        std::snprintf(localMsg, sizeof(localMsg), "Тактическая метка: %s — %s", title, payload.TargetLabel.c_str());
    else
        std::snprintf(localMsg, sizeof(localMsg), "Тактическая метка: %s", title);
    Utils::ShowHudMessage(String(localMsg));
}

void RfabAdapterService::TriggerExplicitTacticalPing(
    RfabCompat::TacticalPingKind aKind,
    const std::string& acCustomLabel) noexcept
{
    if (aKind == RfabCompat::TacticalPingKind::ClearSenderPings)
    {
        ClearLocalTacticalPings();
        return;
    }

    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    // First run contextual resolution to capture crosshair coordinates/target, then override Kind.
    m_pingCooldownRemainingSec = 0.0;
    TriggerContextualTacticalPing(aKind == RfabCompat::TacticalPingKind::Danger, acCustomLabel);
    if (!m_activeTacticalPings.empty())
    {
        auto& newest = m_activeTacticalPings.back();
        if (newest.Payload.Kind != aKind)
        {
            newest.Payload.Kind = aKind;
            SyncRfabProgressionEvent ev{};
            ev.Kind = SyncRfabProgressionEvent::EventKind::TacticalPingSync;
            ev.EpochId = m_currentWorldEpoch;
            ev.SourceKey = String(RfabCompat::SerializeTacticalPingPayload(newest.Payload).c_str());
            m_transport.Send(ev);
        }
    }
}

void RfabAdapterService::ExecuteTacticalChatCommand(const RfabCompat::TacticalChatCommand& acCmd) noexcept
{
    switch (acCmd.Kind)
    {
    case RfabCompat::TacticalChatCommand::Action::TriggerContextualPing:
        TriggerContextualTacticalPing(false, acCmd.CustomLabel);
        break;
    case RfabCompat::TacticalChatCommand::Action::TriggerExplicitPing:
        TriggerExplicitTacticalPing(acCmd.PingKind, acCmd.CustomLabel);
        break;
    case RfabCompat::TacticalChatCommand::Action::ClearPings:
        ClearLocalTacticalPings();
        break;
    case RfabCompat::TacticalChatCommand::Action::CycleHudMode:
        SetTacticalHudMode(RfabCompat::CycleTacticalHudMode(m_tacticalHudMode), true);
        break;
    case RfabCompat::TacticalChatCommand::Action::SetHudMode:
        SetTacticalHudMode(acCmd.HudMode, true);
        break;
    case RfabCompat::TacticalChatCommand::Action::None:
    default:
        break;
    }
}

void RfabAdapterService::UpdateTacticalPingsAndInput(double aDeltaSec) noexcept
{
    if (m_pingCooldownRemainingSec > 0.0)
        m_pingCooldownRemainingSec = std::max(0.0, m_pingCooldownRemainingSec - aDeltaSec);

    // Update live 3D coordinates of entity-attached pings and expire finished pings.
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
    if (!pWindow || !pWindow->IsForeground() || IsHotkeyInputBlockedByTextMenu(m_world))
    {
        m_pingKeyWasDown = false;
        m_hudKeyWasDown = false;
        return;
    }

    const int hudVk = static_cast<int>(m_bindings.TacticalHudToggleVirtualKey & 0xFFu);
    const bool hudKeyDown = (hudVk > 0) && ((GetAsyncKeyState(hudVk) & 0x8000) != 0);
    if (hudKeyDown && !m_hudKeyWasDown)
        SetTacticalHudMode(RfabCompat::CycleTacticalHudMode(m_tacticalHudMode), true);
    m_hudKeyWasDown = hudKeyDown;

    const int pingVk = static_cast<int>(m_bindings.TacticalPingVirtualKey & 0xFFu);
    const bool primaryPingDown = (pingVk > 0) && ((GetAsyncKeyState(pingVk) & 0x8000) != 0);
    const bool altMmbPingDown =
        ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) && ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    const bool pingKeyDown = primaryPingDown || altMmbPingDown;

    if (pingKeyDown && !m_pingKeyWasDown)
    {
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrlHeld)
            ClearLocalTacticalPings();
        else
            TriggerContextualTacticalPing(shiftHeld, "");
    }
    m_pingKeyWasDown = pingKeyDown;
#endif
}

void RfabAdapterService::ClearTacticalHudSnapshot() noexcept
{
    std::lock_guard<std::mutex> lock(m_tacticalHudSnapshotMutex);
    m_tacticalHudSnapshot = TacticalHudRenderSnapshot{};
}

void RfabAdapterService::RefreshTacticalHudSnapshot() noexcept
{
    // Runs EXCLUSIVELY on the Main Game Thread during World::Update() so UI::GetMenuOpen, BSFixedString,
    // entt::registry, TESForm::GetById, Actor::GetActorValue, HUDMenuUtils::WorldPtToScreenPt3, and
    // service state are never touched from the D3D11 Render Thread (Hook_Renderer_End).
    if (!m_transport.IsConnected() || m_tacticalHudMode == RfabCompat::TacticalHudMode::Hidden)
    {
        ClearTacticalHudSnapshot();
        return;
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || (!pPlayer->parentCell && !pPlayer->GetWorldSpace()))
    {
        ClearTacticalHudSnapshot();
        return;
    }

    if (IsTacticalHudBlockedByModalMenu())
    {
        ClearTacticalHudSnapshot();
        return;
    }

    TacticalHudRenderSnapshot next{};
    next.Visible = true;
    next.Mode = m_tacticalHudMode;
    next.AllyProtectionEnabled = IsAllyProtectionEnabled();

    const uint32_t localWorldId = pPlayer->GetWorldSpace() ? pPlayer->GetWorldSpace()->formID : 0u;
    const uint32_t localCellId = pPlayer->parentCell ? pPlayer->parentCell->formID : 0u;
    const auto& downedService = m_world.GetDownedStateService();
    const auto& partyService = m_world.GetPartyService();
    const uint32_t localPid = m_hasLocalPlayerId ? m_localPlayerId : m_transport.GetLocalPlayerId();

    next.IsChannelingRevive = downedService.IsChannelingRevive();
    next.ReviveChannelProgress = downedService.GetReviveChannelProgress();
    next.IsLocalPlayerDowned = downedService.IsLocalPlayerDowned();
    next.IsLocalPlayerBeingRevived = downedService.IsPlayerBeingRevived(localPid);
    next.IsLocalPlayerInvulnerable = downedService.IsLocalPlayerInvulnerable();
    next.GraceRemainingSec = downedService.GetGraceRemainingSec();

    std::unordered_map<uint32_t, Actor*> loadedActorByPlayerId;
    std::unordered_map<uint32_t, std::string> playerNameByFormId;
    if (pPlayer->baseForm && pPlayer->baseForm->GetName())
        playerNameByFormId[pPlayer->formID] = pPlayer->baseForm->GetName();

    auto playerView = m_world.view<FormIdComponent, PlayerComponent>();
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

    // 1. 3D World-Space Downed Teammate Beacons (projected safely on Main Game Thread)
    if (m_bindings.ShowDowned3dBeacons)
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

            DownedBeaconSnapshotEntry entry{};
            entry.AllyName = "Союзник";
            if (auto it = m_partyHudPeers.find(pid); it != m_partyHudPeers.end() && !it->second.Username.empty())
                entry.AllyName = it->second.Username;
            entry.DistMeters = distMeters;
            entry.NormScreenX = normScreen.x;
            entry.NormScreenY = normScreen.y;
            entry.NormScreenZ = normScreen.z;
            entry.BeingRevived = downedService.IsPlayerBeingRevived(pid);
            next.DownedBeacons.push_back(std::move(entry));
        }
    }

    // 2. Active Tactical 3D Pings (projected safely on Main Game Thread)
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

        TacticalPingSnapshotEntry snapPing{};
        snapPing.Kind = ping.Kind;
        snapPing.TargetLabel = ping.TargetLabel;
        snapPing.DistMeters = distMeters;
        snapPing.NormScreenX = normScreen.x;
        snapPing.NormScreenY = normScreen.y;
        snapPing.NormScreenZ = normScreen.z;
        snapPing.RemainingSec = pingEntry.RemainingSec;
        snapPing.TotalDurationSec = pingEntry.TotalDurationSec;
        next.ActivePings.push_back(std::move(snapPing));
    }

    if (m_tacticalHudMode != RfabCompat::TacticalHudMode::MarkersOnly)
    {
        // 3. Active Boss Encounter Health & Phase Snapshot
        if (m_bindings.ShowBossEncounterBar)
        {
            Actor* pBestBoss = nullptr;
            uint32_t bestBossServerId = 0;
            float bestBossDistSq = RfabCompat::MetersToSkyrimUnits(65.0f) * RfabCompat::MetersToSkyrimUnits(65.0f);

            auto actorView = m_world.view<FormIdComponent>();
            for (auto entity : actorView)
            {
                const uint32_t fid = actorView.get<FormIdComponent>(entity).Id;
                if (fid == 0 || fid == 0x14)
                    continue;
                Actor* pNpc = Cast<Actor>(TESForm::GetById(fid));
                if (!pNpc || pNpc->IsDead() || !pNpc->IsInCombat() || !IsBossActor(pNpc))
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
                    if (const auto* pLocal = m_world.try_get<LocalComponent>(entity))
                        bestBossServerId = pLocal->Id;
                    else if (const auto* pRemote = m_world.try_get<RemoteComponent>(entity))
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
                bool isEtherealPhase = pBestBoss->IsGhost();
                if (!isEtherealPhase && bestBossServerId != 0)
                {
                    if (const auto itGhost = m_lastBossGhostByServerId.find(bestBossServerId);
                        itGhost != m_lastBossGhostByServerId.end() && itGhost->second)
                    {
                        isEtherealPhase = true;
                    }
                }

                const char* bossName = (pBestBoss->baseForm && pBestBoss->baseForm->GetName() && pBestBoss->baseForm->GetName()[0] != '\0')
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

                next.Boss.Active = true;
                next.Boss.Name = bossName;
                next.Boss.TargetPlayerName = std::move(targetPlayerName);
                next.Boss.Level = pBestBoss->GetLevel();
                next.Boss.CurHp = curHp;
                next.Boss.MaxHp = maxHp;
                next.Boss.HpRatio = hpRatio;
                next.Boss.IsEtherealPhase = isEtherealPhase;
            }
        }

        // 4. Party / Connected Peers Snapshot
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
                if (pid != localPid)
                    displayPeerIds.push_back(pid);
            }
        }
        else
        {
            for (const auto& [pid, peerState] : m_partyHudPeers)
            {
                (void)peerState;
                if (pid != localPid)
                    displayPeerIds.push_back(pid);
            }
            std::sort(displayPeerIds.begin(), displayPeerIds.end());
        }

        next.PartyRows.reserve(1 + displayPeerIds.size());

        // Row 0: Local Player
        PartyMemberHudSnapshotEntry selfRow{};
        selfRow.Name = (pPlayer->baseForm && pPlayer->baseForm->GetName() && pPlayer->baseForm->GetName()[0] != '\0')
            ? pPlayer->baseForm->GetName()
            : "Вы";
        selfRow.Level = pPlayer->GetLevel();
        selfRow.IsLeader = IsLocalPlayerAllyProtectionHost();
        selfRow.IsDowned = next.IsLocalPlayerDowned;
        selfRow.IsBeingRevived = next.IsLocalPlayerBeingRevived;
        selfRow.IsGraceActive = next.IsLocalPlayerInvulnerable;
        selfRow.IsInCombat = pPlayer->IsInCombat();
        selfRow.IsLoaded3D = true;
        selfRow.DistMeters = 0.0f;
        selfRow.HpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kHealth, pPlayer->healthModifiers.temporaryModifier);
        selfRow.MpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kMagicka, pPlayer->magickaModifiers.temporaryModifier);
        selfRow.SpPct = computeActorVitalPct(pPlayer, ActorValueInfo::kStamina, pPlayer->staminaModifiers.temporaryModifier);
        next.PartyRows.push_back(std::move(selfRow));

        for (uint32_t pid : displayPeerIds)
        {
            PartyMemberHudSnapshotEntry row{};
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

            next.PartyRows.push_back(std::move(row));
        }
    }

    std::lock_guard<std::mutex> lock(m_tacticalHudSnapshotMutex);
    m_tacticalHudSnapshot = std::move(next);
}

void RfabAdapterService::OnDrawTacticalHud() noexcept
{
    // Runs on D3D11 Render Thread (BSGraphics::Hook_Renderer_End): ONLY reads a lock-copied POD/string snapshot!
    TacticalHudRenderSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(m_tacticalHudSnapshotMutex);
        if (!m_tacticalHudSnapshot.Visible)
            return;
        snapshot = m_tacticalHudSnapshot;
    }

    auto* pViewport = BSGraphics::GetMainWindow();
    if (!pViewport || pViewport->uiWindowWidth == 0 || pViewport->uiWindowHeight == 0)
        return;

    ImDrawList* pDrawList = ImGui::GetBackgroundDrawList();
    if (!pDrawList)
        return;

    const float viewW = static_cast<float>(pViewport->uiWindowWidth);
    const float viewH = static_cast<float>(pViewport->uiWindowHeight);

    // ------------------------------------------------------------------------
    // 1. 3D World-Space Downed Teammate Beacons & Off-Screen Edge Arrows
    // ------------------------------------------------------------------------
    for (const auto& beacon : snapshot.DownedBeacons)
    {
        const auto proj = RfabCompat::ComputeTacticalMarkerScreenPlacement(
            beacon.NormScreenX, beacon.NormScreenY, beacon.NormScreenZ, viewW, viewH, 56.0f);

        const ImU32 beaconCol = beacon.BeingRevived ? IM_COL32(61, 220, 132, 245) : IM_COL32(255, 58, 76, 245);
        const ImVec2 center(proj.ScreenX, proj.ScreenY);

        if (proj.IsOnScreen)
        {
            const float r = 11.0f;
            const ImVec2 pts[4] = {
                ImVec2(center.x, center.y - r),
                ImVec2(center.x + r, center.y),
                ImVec2(center.x, center.y + r),
                ImVec2(center.x - r, center.y),
            };
            pDrawList->AddConvexPolyFilled(pts, 4, beaconCol);
            pDrawList->AddPolyline(pts, 4, IM_COL32(255, 235, 190, 240), ImDrawFlags_Closed, 2.0f);

            char line1[128]{};
            if (beacon.BeingRevived)
                std::snprintf(line1, sizeof(line1), "[+] РЕАНИМАЦИЯ: %s (%.0f м)", beacon.AllyName.c_str(), beacon.DistMeters);
            else if (beacon.DistMeters <= 3.2f)
                std::snprintf(line1, sizeof(line1), "[E] ПОДНЯТЬ: %s (%.1f м)", beacon.AllyName.c_str(), beacon.DistMeters);
            else
                std::snprintf(line1, sizeof(line1), "[!] В НОКАУТЕ: %s (%.0f м)", beacon.AllyName.c_str(), beacon.DistMeters);

            const ImVec2 textSz = ImGui::CalcTextSize(line1);
            const ImVec2 boxMin(center.x - textSz.x * 0.5f - 7.0f, center.y - r - textSz.y - 10.0f);
            const ImVec2 boxMax(center.x + textSz.x * 0.5f + 7.0f, center.y - r - 2.0f);
            pDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 12, 16, 215), 4.0f);
            pDrawList->AddRect(boxMin, boxMax, beaconCol, 4.0f, 0, 1.5f);
            DrawShadowedText(pDrawList, ImVec2(boxMin.x + 7.0f, boxMin.y + 3.0f), beaconCol, line1);
        }
        else
        {
            const ImVec2 tip(center.x + proj.DirX * 14.0f, center.y + proj.DirY * 14.0f);
            const ImVec2 perp(-proj.DirY * 9.0f, proj.DirX * 9.0f);
            const ImVec2 baseCenter(center.x - proj.DirX * 6.0f, center.y - proj.DirY * 6.0f);
            pDrawList->AddTriangleFilled(
                tip,
                ImVec2(baseCenter.x + perp.x, baseCenter.y + perp.y),
                ImVec2(baseCenter.x - perp.x, baseCenter.y - perp.y),
                beaconCol);

            char edgeLabel[96]{};
            std::snprintf(edgeLabel, sizeof(edgeLabel), "[!] %s (%.0fм)", beacon.AllyName.c_str(), beacon.DistMeters);
            const ImVec2 textSz = ImGui::CalcTextSize(edgeLabel);
            DrawShadowedText(
                pDrawList,
                ImVec2(center.x - textSz.x * 0.5f, center.y + 16.0f),
                beaconCol,
                edgeLabel);
        }
    }

    // ------------------------------------------------------------------------
    // 2. Active Tactical 3D Pings & Off-Screen Edge Arrows
    // ------------------------------------------------------------------------
    for (const auto& ping : snapshot.ActivePings)
    {
        const auto proj = RfabCompat::ComputeTacticalMarkerScreenPlacement(
            ping.NormScreenX, ping.NormScreenY, ping.NormScreenZ, viewW, viewH, 52.0f);

        const float alphaFade = static_cast<float>(std::clamp(ping.RemainingSec / 1.5, 0.25, 1.0));
        const uint8_t alphaByte = static_cast<uint8_t>(240.0f * alphaFade);
        const ImU32 pingCol = GetPingPrimaryColor(ping.Kind, alphaByte);
        const ImVec2 center(proj.ScreenX, proj.ScreenY);

        if (proj.IsOnScreen)
        {
            const float r = 9.5f;
            const ImVec2 pts[4] = {
                ImVec2(center.x, center.y - r),
                ImVec2(center.x + r, center.y),
                ImVec2(center.x, center.y + r),
                ImVec2(center.x - r, center.y),
            };
            pDrawList->AddConvexPolyFilled(pts, 4, pingCol);
            pDrawList->AddPolyline(pts, 4, IM_COL32(255, 255, 255, alphaByte), ImDrawFlags_Closed, 1.5f);

            char headerBuf[160]{};
            const char* badge = RfabCompat::GetTacticalPingBadgeAscii(ping.Kind);
            const char* title = RfabCompat::GetTacticalPingTitleRu(ping.Kind);
            if (!ping.TargetLabel.empty())
                std::snprintf(headerBuf, sizeof(headerBuf), "%s %s: %s (%.0f м)", badge, title, ping.TargetLabel.c_str(), ping.DistMeters);
            else
                std::snprintf(headerBuf, sizeof(headerBuf), "%s %s (%.0f м)", badge, title, ping.DistMeters);

            const ImVec2 textSz = ImGui::CalcTextSize(headerBuf);
            const ImVec2 boxMin(center.x - textSz.x * 0.5f - 7.0f, center.y - r - textSz.y - 12.0f);
            const ImVec2 boxMax(center.x + textSz.x * 0.5f + 7.0f, center.y - r - 2.0f);
            pDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 17, 24, static_cast<uint8_t>(210 * alphaFade)), 4.0f);
            pDrawList->AddRect(boxMin, boxMax, pingCol, 4.0f, 0, 1.2f);
            DrawShadowedText(pDrawList, ImVec2(boxMin.x + 7.0f, boxMin.y + 2.0f), pingCol, headerBuf);

            const float lifeFrac = (ping.TotalDurationSec > 0.1)
                ? static_cast<float>(std::clamp(ping.RemainingSec / ping.TotalDurationSec, 0.0, 1.0))
                : 0.0f;
            const float barW = boxMax.x - boxMin.x - 4.0f;
            pDrawList->AddRectFilled(
                ImVec2(boxMin.x + 2.0f, boxMax.y - 3.0f),
                ImVec2(boxMin.x + 2.0f + barW * lifeFrac, boxMax.y - 1.0f),
                pingCol);
        }
        else
        {
            const ImVec2 tip(center.x + proj.DirX * 12.0f, center.y + proj.DirY * 12.0f);
            const ImVec2 perp(-proj.DirY * 7.5f, proj.DirX * 7.5f);
            const ImVec2 baseCenter(center.x - proj.DirX * 5.0f, center.y - proj.DirY * 5.0f);
            pDrawList->AddTriangleFilled(
                tip,
                ImVec2(baseCenter.x + perp.x, baseCenter.y + perp.y),
                ImVec2(baseCenter.x - perp.x, baseCenter.y - perp.y),
                pingCol);

            char edgeBuf[64]{};
            std::snprintf(edgeBuf, sizeof(edgeBuf), "%s %.0fм", RfabCompat::GetTacticalPingBadgeAscii(ping.Kind), ping.DistMeters);
            const ImVec2 textSz = ImGui::CalcTextSize(edgeBuf);
            DrawShadowedText(pDrawList, ImVec2(center.x - textSz.x * 0.5f, center.y + 13.0f), pingCol, edgeBuf);
        }
    }

    // ------------------------------------------------------------------------
    // 3. Center-Screen Revive Channeling / Downed / Grace Progress Bar
    // ------------------------------------------------------------------------
    if (snapshot.IsChannelingRevive)
    {
        const float progress = snapshot.ReviveChannelProgress;
        const float barW = 300.0f;
        const float barH = 22.0f;
        const ImVec2 barMin((viewW - barW) * 0.5f, viewH * 0.62f);
        const ImVec2 barMax(barMin.x + barW, barMin.y + barH);

        pDrawList->AddRectFilled(barMin, barMax, IM_COL32(14, 18, 22, 225), 5.0f);
        pDrawList->AddRectFilled(
            ImVec2(barMin.x + 2.0f, barMin.y + 2.0f),
            ImVec2(barMin.x + 2.0f + (barW - 4.0f) * progress, barMax.y - 2.0f),
            IM_COL32(61, 220, 132, 230),
            4.0f);
        pDrawList->AddRect(barMin, barMax, IM_COL32(196, 235, 200, 220), 5.0f, 0, 1.5f);

        char revText[128]{};
        std::snprintf(revText, sizeof(revText), "РЕАНИМАЦИЯ СОЮЗНИКА [E]: %.0f%%", progress * 100.0f);
        const ImVec2 tSz = ImGui::CalcTextSize(revText);
        DrawShadowedText(
            pDrawList,
            ImVec2(barMin.x + (barW - tSz.x) * 0.5f, barMin.y + (barH - tSz.y) * 0.5f),
            IM_COL32(255, 255, 255, 250),
            revText);
    }
    else if (snapshot.IsLocalPlayerDowned)
    {
        const char* downedBanner = snapshot.IsLocalPlayerBeingRevived
            ? "[+] СОЮЗНИК ПОДНИМАЕТ ВАС НА НОГИ..."
            : "[!] ВЫ ТЯЖЕЛО РАНЕНЫ (НА КОЛЕНЕ) - ОЖИДАНИЕ ПОМОЩИ ОТРЯДА [E]";
        const ImU32 bannerCol = snapshot.IsLocalPlayerBeingRevived ? IM_COL32(61, 220, 132, 245) : IM_COL32(255, 72, 82, 245);
        const ImVec2 tSz = ImGui::CalcTextSize(downedBanner);
        const ImVec2 boxMin((viewW - tSz.x) * 0.5f - 14.0f, viewH * 0.62f);
        const ImVec2 boxMax((viewW + tSz.x) * 0.5f + 14.0f, boxMin.y + tSz.y + 10.0f);
        pDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 12, 16, 225), 5.0f);
        pDrawList->AddRect(boxMin, boxMax, bannerCol, 5.0f, 0, 1.5f);
        DrawShadowedText(pDrawList, ImVec2(boxMin.x + 14.0f, boxMin.y + 5.0f), bannerCol, downedBanner);
    }
    else if (snapshot.IsLocalPlayerInvulnerable && snapshot.GraceRemainingSec > 0.05)
    {
        char graceBuf[96]{};
        std::snprintf(graceBuf, sizeof(graceBuf), "ЩИТ ПОСЛЕ ПОДЪЁМА: %.1f с", snapshot.GraceRemainingSec);
        const ImVec2 tSz = ImGui::CalcTextSize(graceBuf);
        const ImVec2 boxMin((viewW - tSz.x) * 0.5f - 10.0f, viewH * 0.62f);
        const ImVec2 boxMax((viewW + tSz.x) * 0.5f + 10.0f, boxMin.y + tSz.y + 8.0f);
        pDrawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 24, 34, 210), 4.0f);
        pDrawList->AddRect(boxMin, boxMax, IM_COL32(71, 209, 255, 220), 4.0f, 0, 1.2f);
        DrawShadowedText(pDrawList, ImVec2(boxMin.x + 10.0f, boxMin.y + 4.0f), IM_COL32(165, 238, 255, 245), graceBuf);
    }

    if (snapshot.Mode == RfabCompat::TacticalHudMode::MarkersOnly)
        return;

    // ------------------------------------------------------------------------
    // 4. Top-Center Active Boss Encounter Health & Phase Bar
    // ------------------------------------------------------------------------
    if (snapshot.Boss.Active)
    {
        const auto& boss = snapshot.Boss;
        const float bossBarW = std::clamp(viewW * 0.34f, 380.0f, 620.0f);
        const float bossBarH = 26.0f;
        const ImVec2 bossMin((viewW - bossBarW) * 0.5f, 26.0f);
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
            pDrawList,
            ImVec2(bossMin.x + (bossBarW - titleSz.x) * 0.5f, bossMin.y - titleSz.y - 4.0f),
            boss.IsEtherealPhase ? IM_COL32(115, 230, 255, 250) : IM_COL32(255, 224, 155, 250),
            bossTitle);

        pDrawList->AddRectFilled(bossMin, bossMax, IM_COL32(16, 14, 18, 225), 5.0f);
        const ImU32 fillCol = boss.IsEtherealPhase ? IM_COL32(65, 195, 235, 225) : IM_COL32(198, 38, 48, 235);
        pDrawList->AddRectFilled(
            ImVec2(bossMin.x + 2.0f, bossMin.y + 2.0f),
            ImVec2(bossMin.x + 2.0f + (bossBarW - 4.0f) * boss.HpRatio, bossMax.y - 2.0f),
            fillCol,
            4.0f);
        pDrawList->AddRect(
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
            pDrawList,
            ImVec2(bossMin.x + (bossBarW - hpSz.x) * 0.5f, bossMin.y + (bossBarH - hpSz.y) * 0.5f),
            IM_COL32(255, 255, 255, 245),
            hpText);
    }

    // ------------------------------------------------------------------------
    // 5. Top-Left Co-Op Party Tactical HUD Panel
    // ------------------------------------------------------------------------
    if (snapshot.PartyRows.empty())
        return;

    const bool fullMode = (snapshot.Mode == RfabCompat::TacticalHudMode::Full);
    const float panelX = 18.0f;
    const float panelY = 18.0f;
    const float panelW = 292.0f;
    const float rowH = fullMode ? 54.0f : 38.0f;
    const size_t totalRows = snapshot.PartyRows.size();
    const float panelH = 34.0f + static_cast<float>(totalRows) * rowH + 6.0f;

    pDrawList->AddRectFilled(
        ImVec2(panelX, panelY),
        ImVec2(panelX + panelW, panelY + panelH),
        IM_COL32(14, 16, 22, 200),
        6.0f);
    pDrawList->AddRect(
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
        snapshot.AllyProtectionEnabled ? "ЕДИНСТВО: ВКЛ" : "УРОН ПО СВОИМ");
    DrawShadowedText(
        pDrawList,
        ImVec2(panelX + 10.0f, panelY + 7.0f),
        snapshot.AllyProtectionEnabled ? IM_COL32(235, 210, 145, 245) : IM_COL32(255, 110, 110, 245),
        headerText);

    for (size_t i = 0; i < snapshot.PartyRows.size(); ++i)
    {
        const auto& row = snapshot.PartyRows[i];
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
        DrawShadowedText(pDrawList, ImVec2(x0, y0), nameCol, lineBuf);

        // HP Bar
        const float hpY = y0 + 20.0f;
        const float hpH = 10.0f;
        const float clampedHp = std::clamp(row.HpPct, 0.0f, 100.0f) * 0.01f;
        pDrawList->AddRectFilled(ImVec2(x0, hpY), ImVec2(x0 + innerW, hpY + hpH), IM_COL32(32, 18, 22, 220), 3.0f);
        const ImU32 hpFill = row.IsDowned
            ? IM_COL32(255, 52, 62, 235)
            : (row.IsGraceActive ? IM_COL32(71, 209, 255, 230) : IM_COL32(198, 42, 52, 235));
        pDrawList->AddRectFilled(
            ImVec2(x0 + 1.0f, hpY + 1.0f),
            ImVec2(x0 + 1.0f + (innerW - 2.0f) * clampedHp, hpY + hpH - 1.0f),
            hpFill,
            2.5f);

        // Dual MP + SP Bars in Full mode when 3D actor is loaded
        if (fullMode && row.IsLoaded3D)
        {
            const float subY = hpY + hpH + 3.0f;
            const float subH = 6.0f;
            const float halfW = (innerW - 6.0f) * 0.5f;
            const float clampedMp = std::clamp(row.MpPct, 0.0f, 100.0f) * 0.01f;
            const float clampedSp = std::clamp(row.SpPct, 0.0f, 100.0f) * 0.01f;

            // Magicka (Left)
            pDrawList->AddRectFilled(ImVec2(x0, subY), ImVec2(x0 + halfW, subY + subH), IM_COL32(18, 24, 38, 215), 2.0f);
            pDrawList->AddRectFilled(
                ImVec2(x0 + 1.0f, subY + 1.0f),
                ImVec2(x0 + 1.0f + (halfW - 2.0f) * clampedMp, subY + subH - 1.0f),
                IM_COL32(52, 128, 235, 230),
                2.0f);

            // Stamina (Right)
            const float spX = x0 + halfW + 6.0f;
            pDrawList->AddRectFilled(ImVec2(spX, subY), ImVec2(spX + halfW, subY + subH), IM_COL32(18, 34, 22, 215), 2.0f);
            pDrawList->AddRectFilled(
                ImVec2(spX + 1.0f, subY + 1.0f),
                ImVec2(spX + 1.0f + (halfW - 2.0f) * clampedSp, subY + subH - 1.0f),
                IM_COL32(48, 188, 92, 230),
                2.0f);
        }
    }
}
