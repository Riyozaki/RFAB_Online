#include <TiltedOnlinePCH.h>

#include <Services/RfabAdapterService.h>

#include <World.h>
#include <Utils.h>
#include <Components.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/UpdateEvent.h>
#include <Forms/BGSListForm.h>
#include <Forms/TESGlobal.h>
#include <Games/Overrides.h>
#include <Games/TES.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/PapyrusService.h>
#include <Services/TransportService.h>

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

void RfabAdapterService::OnConnected(const ConnectedEvent&) noexcept
{
    RefreshRuntimeBindings();
    SuppressSoloRfabAutosave();

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
    }
}
