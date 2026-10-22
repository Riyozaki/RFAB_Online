#include <TiltedOnlinePCH.h>

#include <Services/RfabXp.h>

#include <Services/PapyrusService.h>
#include <World.h>

#include <Forms/TESGlobal.h>
#include <Games/References.h>
#include <Games/TES.h>
#include <PlayerCharacter.h>

#include <Gameplay/XpAward.h>

#include <cmath>

namespace
{
constexpr uint32_t cRfabXpAmountBaseId = 0xAD45BB;

TESGlobal* s_pRfabGlobal = nullptr;
bool s_rfabGlobalResolved = false;

//! Game.GetExperienceForLevel(int) -> float, resolved lazily and cached.
struct LevelUpOracle
{
    bool Resolved{false};
    GlobalPapyrusFunction<float, int32_t>* pFunction{nullptr};
};
} // namespace

namespace RfabXp
{
TESGlobal* GetGlobal() noexcept
{
    if (s_rfabGlobalResolved)
        return s_pRfabGlobal;

    auto* pModManager = ModManager::Get();
    if (pModManager == nullptr)
        return nullptr; // the game is not ready yet, try again later

    auto* pMod = pModManager->GetByName("RFAB.esp");
    if (pMod == nullptr)
    {
        // RFAB is not installed: remember that, but do not spam the log.
        s_rfabGlobalResolved = true;
        return nullptr;
    }

    s_pRfabGlobal = Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(cRfabXpAmountBaseId)));
    s_rfabGlobalResolved = true;

    if (s_pRfabGlobal == nullptr)
        spdlog::warn("RFAB XP global {:X} was not found in RFAB.esp, multiplayer XP is disabled", cRfabXpAmountBaseId);
    else
        spdlog::info("RFAB XP pipeline: global {:X} resolved", cRfabXpAmountBaseId);

    return s_pRfabGlobal;
}

int32_t Read() noexcept
{
    auto* pGlobal = GetGlobal();
    if (pGlobal == nullptr)
        return 0;

    const float value = pGlobal->f;
    if (!std::isfinite(value) || value <= 0.0f)
        return 0;

    return static_cast<int32_t>(std::lround(std::min(value, static_cast<float>(Gameplay::XpAward::kMaxTotalXp))));
}

AwardResult Award(int32_t aAmount) noexcept
{
    AwardResult result{};

    auto* pGlobal = GetGlobal();
    if (pGlobal == nullptr)
        return result;

    const auto& papyrusService = World::Get().ctx().at<PapyrusService>();
    const void* pGetExperience = papyrusService.Get("Game", "GetPlayerExperience");
    const void* pSetExperience = papyrusService.Get("Game", "SetPlayerExperience");
    if (pGetExperience == nullptr || pSetExperience == nullptr)
    {
        spdlog::warn("RFAB XP: Game.Get/SetPlayerExperience are unavailable, not awarding {} XP", aAmount);
        return result;
    }

    const int32_t current = Read();
    const auto award = Gameplay::XpAward::Apply(current, aAmount);

    if (award.Ignored)
    {
        // ModXP would still run with the value, but a negative or absurd award
        // can only come from a broken peer; refusing beats corrupting the save.
        spdlog::warn("RFAB XP: ignoring award {} (current total {})", aAmount, current);
        result.Ok = true;
        result.Total = current;
        return result;
    }

    // --- PlayerXP.Mod(aiXP) -------------------------------------------------
    pGlobal->f = static_cast<float>(award.Total);

    // --- VanilaXPGain(aiXP) -------------------------------------------------
    GlobalPapyrusFunction<float> getExperience(pGetExperience);
    GlobalPapyrusFunction<void, float> setExperience(pSetExperience);

    const float playerExperience = getExperience();
    setExperience(playerExperience + static_cast<float>(award.Applied));

    // Level-up check: `_PlayerXP >= Game.GetExperienceForLevel(Player.GetLevel())`.
    // The animation RFAB plays is UI code; a HUD line is the closest thing the
    // native side can do without a script call.
    static LevelUpOracle s_oracle;
    if (!s_oracle.Resolved)
    {
        s_oracle.Resolved = true;
        if (const void* pGetThreshold = papyrusService.Get("Game", "GetExperienceForLevel"))
            s_oracle.pFunction = new GlobalPapyrusFunction<float, int32_t>(pGetThreshold);
    }

    if (s_oracle.pFunction != nullptr)
    {
        if (auto* pPlayer = PlayerCharacter::Get())
        {
            const float threshold = (*s_oracle.pFunction)(static_cast<int32_t>(pPlayer->GetLevel()));
            if (Gameplay::XpAward::ShouldAnnounceLevelUp(award.Total, threshold))
                result.LevelUp = true;
        }
    }

    if (award.Clamped)
        spdlog::warn("RFAB XP: total clamped at {} (requested {} on top of {})", award.Total, aAmount, current);

    result.Ok = true;
    result.Total = award.Total;
    return result;
}
} // namespace RfabXp
