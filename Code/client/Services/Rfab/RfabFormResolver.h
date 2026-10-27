#pragma once

#include <common/RfabModpackCompat.h>
#include <filesystem>
#include <mutex>
#include <string>

struct TESForm;
struct TESGlobal;
struct BGSListForm;
struct Mod;

/**
 * Resolves RFAB / Requiem forms, TESGlobals, and BGSListForms across arbitrary load orders and modpack
 * variants (standard drake1ch/RFAB, Respect Edition, custom patches) with zero hardcoded load-order prefixes:
 * 1. PO3 Tweaks runtime TESForm::GetByEditorId (SE 14464)
 * 2. Offline binary .esp/.esm/.esl EDID index (EspEdidIndex)
 * 3. Live ModManager (TESDataHandler) array scan + optional RFAB_Online_Bindings.json fallback base IDs
 */
class RfabFormResolver
{
public:
    RfabFormResolver() noexcept = default;
    ~RfabFormResolver() noexcept = default;

    TP_NOCOPYMOVE(RfabFormResolver);

    [[nodiscard]] static std::filesystem::path DiscoverSkyrimRootDirectory() noexcept;

    void RefreshRuntimeBindings() noexcept;

    [[nodiscard]] Mod* ResolveRfabPluginMod() const noexcept;
    [[nodiscard]] TESForm* ResolveFormByEditorIdOrBaseId(
        const std::string& acEditorId,
        uint32_t aFallbackBaseId = 0) const noexcept;
    [[nodiscard]] TESGlobal* ResolveGlobalByEditorId(
        const std::string& acEditorId,
        uint32_t aFallbackBaseId = 0) noexcept;
    [[nodiscard]] BGSListForm* ResolveFormListByEditorId(const std::string& acEditorId) noexcept;

    [[nodiscard]] TESGlobal* GetRfabXpGlobal() noexcept;
    [[nodiscard]] float GetLocalXpMultiplier() noexcept;
    [[nodiscard]] TESGlobal* ResolveNoDeathHandlingGlobal() noexcept;
    [[nodiscard]] BGSListForm* GetXpBooksFormList() noexcept;
    [[nodiscard]] BGSListForm* ResolveNoAllyDamageAllowedSpellsList() noexcept;

    [[nodiscard]] const RfabCompat::RfabBindingsConfig& GetBindings() const noexcept { return m_bindings; }
    [[nodiscard]] const RfabCompat::RfabLiveXpTable& GetLiveXpTable() const noexcept { return m_liveXpTable; }
    [[nodiscard]] const RfabCompat::EspEdidIndex& GetEspEdidIndex() const noexcept { return m_espEdidIndex; }

private:
    RfabCompat::RfabBindingsConfig m_bindings{};
    RfabCompat::RfabLiveXpTable m_liveXpTable{};
    RfabCompat::EspEdidIndex m_espEdidIndex{};

    TESGlobal* m_pCachedXpGlobal{nullptr};
    TESGlobal* m_pCachedXpMultiplierGlobal{nullptr};
    BGSListForm* m_pXpBooksFormList{nullptr};
    mutable std::mutex m_allowedSpellsMutex{};
};
