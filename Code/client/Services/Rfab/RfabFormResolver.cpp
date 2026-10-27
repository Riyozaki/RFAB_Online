#include <TiltedOnlinePCH.h>

#include <Services/Rfab/RfabFormResolver.h>

#include <Forms/BGSListForm.h>
#include <Forms/TESGlobal.h>
#include <Games/TES.h>

#include <cctype>
#include <cmath>

namespace
{
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

std::filesystem::path RfabFormResolver::DiscoverSkyrimRootDirectory() noexcept
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

void RfabFormResolver::RefreshRuntimeBindings() noexcept
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
    m_pXpBooksFormList = nullptr;
}

Mod* RfabFormResolver::ResolveRfabPluginMod() const noexcept
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

TESForm* RfabFormResolver::ResolveFormByEditorIdOrBaseId(
    const std::string& acEditorId,
    uint32_t aFallbackBaseId) const noexcept
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

TESGlobal* RfabFormResolver::ResolveGlobalByEditorId(
    const std::string& acEditorId,
    uint32_t aFallbackBaseId) noexcept
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

BGSListForm* RfabFormResolver::ResolveFormListByEditorId(const std::string& acEditorId) noexcept
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

TESGlobal* RfabFormResolver::GetRfabXpGlobal() noexcept
{
    if (m_pCachedXpGlobal)
        return m_pCachedXpGlobal;

    m_pCachedXpGlobal = ResolveGlobalByEditorId(
        m_bindings.XpAmountGlobalEditorId, m_bindings.FallbackXpAmountBaseFormId);
    return m_pCachedXpGlobal;
}

float RfabFormResolver::GetLocalXpMultiplier() noexcept
{
    if (!m_pCachedXpMultiplierGlobal)
    {
        m_pCachedXpMultiplierGlobal = ResolveGlobalByEditorId("RFAB_XP_Multiplier", 0x00112815);
        if (!m_pCachedXpMultiplierGlobal && !m_bindings.XpBonusGlobalEditorId.empty())
            m_pCachedXpMultiplierGlobal = ResolveGlobalByEditorId(m_bindings.XpBonusGlobalEditorId, 0);
    }

    if (!m_pCachedXpMultiplierGlobal ||
        !std::isfinite(m_pCachedXpMultiplierGlobal->f) ||
        m_pCachedXpMultiplierGlobal->f <= 0.05f)
    {
        return 1.0f;
    }

    return m_pCachedXpMultiplierGlobal->f;
}

TESGlobal* RfabFormResolver::ResolveNoDeathHandlingGlobal() noexcept
{
    return ResolveGlobalByEditorId(m_bindings.NoDeathHandlingGlobalEditorId, 0);
}

BGSListForm* RfabFormResolver::GetXpBooksFormList() noexcept
{
    if (!m_pXpBooksFormList)
        m_pXpBooksFormList = ResolveFormListByEditorId(m_bindings.XpBooksFormListEditorId);
    return m_pXpBooksFormList;
}

BGSListForm* RfabFormResolver::ResolveNoAllyDamageAllowedSpellsList() noexcept
{
    std::lock_guard<std::mutex> lock(m_allowedSpellsMutex);
    BGSListForm* pResolved = ResolveFormListByEditorId(m_bindings.NoAllyDamageAllowedSpellsEditorId);
    if (!pResolved && m_bindings.FallbackNoAllyDamageAllowedSpellsBaseFormId != 0)
    {
        if (Mod* pMod = ResolveRfabPluginMod())
        {
            pResolved = Cast<BGSListForm>(
                TESForm::GetById(pMod->GetFormId(m_bindings.FallbackNoAllyDamageAllowedSpellsBaseFormId)));
        }
    }
    return pResolved;
}
