#include <TiltedOnlinePCH.h>

#include <Services/Rfab/RfabXpAndBookSync.h>
#include <Services/Rfab/RfabFormResolver.h>

#include <Forms/BGSListForm.h>
#include <Forms/TESGlobal.h>
#include <Games/TES.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/SyncRfabProgressionEvent.h>
#include <PlayerCharacter.h>
#include <Services/CharacterService.h>
#include <Services/TransportService.h>
#include <Utils.h>
#include <World.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

int32_t RfabXpAndBookSync::ReadRfabXp(TESGlobal* apGlobal) noexcept
{
    if (!apGlobal)
        return 0;

    return static_cast<int32_t>(std::lround(std::max(0.0f, apGlobal->f)));
}

bool RfabXpAndBookSync::ApplyDualPoolExperienceDelta(
    TESGlobal* apRfabXpGlobal,
    int32_t aDelta,
    int32_t& aOutNewTotalXp) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!apRfabXpGlobal || !pPlayer)
        return false;

    // Update BOTH pools directly in C++ memory (matching RFAB_XP_Handler.psc::ModXP + skse64 PapyrusGame.cpp):
    // 1. TESGlobal RFAB_XP_Amount (0xAD45BB in RFAB.esp) — spent inside RFAB_Menu to raise skills & perks.
    // 2. PlayerCharacter->pSkills->data->xp (offset 0x00) — exact field mutated by Game.SetPlayerExperience().
    aOutNewTotalXp = std::max(0, ReadRfabXp(apRfabXpGlobal) + aDelta);
    apRfabXpGlobal->f = static_cast<float>(aOutNewTotalXp);

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

void RfabXpAndBookSync::SuppressSoloRfabAutosave(const RfabCompat::RfabBindingsConfig& acBindings) noexcept
{
    if (m_autosaveSuppressed)
        return;

    const std::filesystem::path candidate =
        RfabFormResolver::DiscoverSkyrimRootDirectory() / acBindings.RfabAutosaveIniRelPath;
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

void RfabXpAndBookSync::RestoreSoloRfabAutosave() noexcept
{
    if (!m_autosaveSuppressed)
        return;

    if (!m_autosaveIniPath.empty() && !m_originalAutosaveIniContent.empty())
        RfabCompat::Detail::WriteTextFile(m_autosaveIniPath, m_originalAutosaveIniContent);

    m_autosaveSuppressed = false;
}

void RfabXpAndBookSync::ApplySessionNoDeathHandling(RfabFormResolver& aResolver) noexcept
{
    m_pNoDeathHandlingGlobal = aResolver.ResolveNoDeathHandlingGlobal();
    if (m_pNoDeathHandlingGlobal)
    {
        m_savedNoDeathHandlingValue = m_pNoDeathHandlingGlobal->f;
        m_hasSavedNoDeathHandlingValue = true;
        m_pNoDeathHandlingGlobal->f = 1.0f;
    }
}

void RfabXpAndBookSync::RestoreSessionNoDeathHandling() noexcept
{
    if (m_pNoDeathHandlingGlobal && m_hasSavedNoDeathHandlingValue)
    {
        m_pNoDeathHandlingGlobal->f = m_savedNoDeathHandlingValue;
        m_hasSavedNoDeathHandlingValue = false;
    }
    m_pNoDeathHandlingGlobal = nullptr;
}

void RfabXpAndBookSync::EnforceNoDeathHandlingTick() noexcept
{
    if (m_pNoDeathHandlingGlobal && m_pNoDeathHandlingGlobal->f < 1.0f)
        m_pNoDeathHandlingGlobal->f = 1.0f;
}

void RfabXpAndBookSync::SeedReadBooksOnConnect(RfabFormResolver& aResolver) noexcept
{
    m_knownReadBookFormIds.clear();
    if (BGSListForm* pXpBooksList = aResolver.GetXpBooksFormList())
    {
        pXpBooksList->ForEachFormId([this](uint32_t formId) {
            m_knownReadBookFormIds.insert(formId);
        });
        m_seededReadBooks = true;
    }
    else
    {
        m_seededReadBooks = false;
    }
}

void RfabXpAndBookSync::ResetRuntimeWatches() noexcept
{
    m_knownReadBookFormIds.clear();
    m_watchedGlobals.clear();
    m_lastGlobalValues.clear();
    m_scannedGlobalsOnce = false;
    m_seededReadBooks = false;
}

void RfabXpAndBookSync::ResetOnDisconnect() noexcept
{
    RestoreSoloRfabAutosave();
    RestoreSessionNoDeathHandling();
    ResetRuntimeWatches();
    m_currentWorldEpoch = 1;
}

void RfabXpAndBookSync::PollReadXpBooks(
    World& aWorld,
    TransportService& aTransport,
    RfabFormResolver& aResolver) noexcept
{
    BGSListForm* pXpBooksList = aResolver.GetXpBooksFormList();
    if (!pXpBooksList)
        return;

    if (!m_seededReadBooks)
    {
        pXpBooksList->ForEachFormId([this](uint32_t formId) {
            m_knownReadBookFormIds.insert(formId);
        });
        m_seededReadBooks = true;
        return;
    }

    auto& modSystem = aWorld.GetModSystem();
    pXpBooksList->ForEachFormId([this, &modSystem, &aTransport](uint32_t formId) {
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
        aTransport.Send(ev);
    });
}

void RfabXpAndBookSync::PollSyncedGlobals(
    TransportService& aTransport,
    RfabFormResolver& aResolver) noexcept
{
    if (!m_scannedGlobalsOnce)
    {
        auto* pModManager = ModManager::Get();
        if (!pModManager)
            return;

        const auto& bindings = aResolver.GetBindings();
        for (uint32_t i = 0; i < pModManager->globals.length; ++i)
        {
            TESGlobal* pGlob = pModManager->globals[i];
            if (!pGlob)
                continue;

            const char* edid = pGlob->GetFormEditorID();
            if (!edid || edid[0] == '\0')
                continue;

            if (RfabCompat::ShouldAutoSyncGlobalEditorId(edid, bindings))
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
        aTransport.Send(ev);
    }
}

void RfabXpAndBookSync::ApplyBookReadEvent(
    World& aWorld,
    RfabFormResolver& aResolver,
    const NotifyRfabProgressionEvent& acMessage) noexcept
{
    if (BGSListForm* pXpBooksList = aResolver.GetXpBooksFormList(); pXpBooksList && acMessage.BookId)
    {
        const uint32_t localFormId = aWorld.GetModSystem().GetGameId(acMessage.BookId);
        if (localFormId != 0)
        {
            m_knownReadBookFormIds.insert(localFormId);
            if (!pXpBooksList->ContainsFormId(localFormId))
            {
                if (TESForm* pBookForm = TESForm::GetById(localFormId))
                    pXpBooksList->AddForm(pBookForm);
            }
        }
    }

    if (acMessage.XpDelta != 0)
        aWorld.GetCharacterService().ApplyAuthoritativeRfabXpDelta(acMessage.XpDelta);
}

void RfabXpAndBookSync::ApplyGlobalValueSyncEvent(
    RfabFormResolver& aResolver,
    const NotifyRfabProgressionEvent& acMessage) noexcept
{
    const std::string editorId(acMessage.EditorId.c_str(), acMessage.EditorId.size());
    if (editorId.empty() || !RfabCompat::ShouldAutoSyncGlobalEditorId(editorId, aResolver.GetBindings()))
        return;

    TESGlobal* pTarget = nullptr;
    if (const auto it = m_watchedGlobals.find(editorId); it != m_watchedGlobals.end())
        pTarget = it->second;
    else
        pTarget = aResolver.ResolveGlobalByEditorId(editorId, 0);

    if (pTarget)
    {
        m_watchedGlobals[editorId] = pTarget;
        m_lastGlobalValues[editorId] = acMessage.GlobalValue; // Set before assignment to suppress echo!
        pTarget->f = acMessage.GlobalValue;
    }
}
