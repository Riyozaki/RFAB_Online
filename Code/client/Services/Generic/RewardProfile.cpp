#include <TiltedOnlinePCH.h>
#include <Services/RewardService.h>
#include <Services/SaveStateService.h>
#include <World.h>
#include <cryptopp/sha.h>
#include <array>
#include <filesystem>
#include <fstream>

namespace
{
bool VerifyFiles(const Coop::RewardProfile& profile)
{
    uint64_t total{};
    std::array<char, 65536> block;
    for (const auto& entry : profile.Files)
    {
        // Parse allows only flat scripts/*.pex and SKSE/Plugins/*.dll under the virtual Data
        // directory. Read through the game's actual MO2 view, not an arbitrary install folder.
        std::ifstream file(std::filesystem::path("Data") / entry.Path, std::ios::binary);
        if (!file) return false;
        CryptoPP::SHA256 hash;
        uint64_t size{};
        const uint64_t limit = entry.Path.ends_with(".pex") ? 8u * 1024 * 1024 : 128u * 1024 * 1024;
        while (file.read(block.data(), block.size()) || file.gcount())
        {
            const auto count = static_cast<uint64_t>(file.gcount());
            size += count; total += count;
            if (size > limit || total > 512u * 1024 * 1024) return false;
            hash.Update(reinterpret_cast<const CryptoPP::byte*>(block.data()), static_cast<size_t>(count));
        }
        std::array<uint8_t, CryptoPP::SHA256::DIGESTSIZE> digest{};
        hash.Final(digest.data());
        if (!size || !file.eof() || digest != entry.SHA256) return false;
    }
    return true;
}
}
void RewardService::RefreshProfile() noexcept
{
    m_profile.reset(); m_profilePresent = m_profileValid = false;
    const std::filesystem::path path("Data/SKSE/Plugins/RFAB_Rewards.tsv");
    std::error_code ec;
    m_profilePresent = std::filesystem::exists(path, ec);
    if (ec) { m_profilePresent = true; return; } // unreadable does not mean absent/legacy
    if (!m_profilePresent) return;
    try
    {
        std::ifstream file(path, std::ios::binary);
        std::string text;
        std::array<char, 4096> block;
        while (file.read(block.data(), block.size()) || file.gcount())
        {
            if (text.size() + file.gcount() > 65536) break;
            text.append(block.data(), static_cast<size_t>(file.gcount()));
        }
        if (file.eof()) m_profile = Coop::RewardProfile::Parse(text);
        m_profileValid = m_profile && VerifyFiles(*m_profile);
    }
    catch (const std::exception& error)
    {
        spdlog::error("[RFAB] installed XP profile could not be verified: {}", error.what());
    }
    if (!m_profileValid)
    {
        m_profile.reset();
        spdlog::error("[RFAB] installed XP profile/compiled scripts/native fingerprints are missing or changed; no semantic negotiation");
        Utils::ShowHudMessage("RFAB: installed XP adapter files do not match their reviewed profile. Shared XP is unavailable; rebuild the local adapter, do not reuse an older Handler.");
    }
}
bool RewardService::ProfileMatches(const Coop::Rewards::Adapter& adapter) const noexcept
{
    // Native adapters have their own versioned application implementation in a pinned module.
    // Papyrus source overrides must additionally match the exact local compiled profile.
    return adapter.ApplyReward ? !m_profilePresent : m_profileValid && m_profile && m_profile->Matches(adapter);
}
bool RewardService::ReadyToNegotiate() const
{
    if (m_blocked) return false;
    const auto adapter = RewardBridgeClient::Mailbox().Adapter(SaveStateBridge::Epoch());
    if (m_profilePresent || adapter)
        return adapter && m_world.ctx().at<SaveStateService>().Ready() && ProfileMatches(*adapter);
    return true; // no installed source override: no semantic capability is advertised
}
