#include <TiltedOnlinePCH.h>
#include <Services/RewardService.h>
#include <Services/SaveStateService.h>
#include <Services/PapyrusService.h>
#include <Forms/TESGlobal.h>
#include <PlayerCharacter.h>
#include <World.h>

namespace
{
TESGlobal* ResolveGlobal(uint32_t form)
{
    auto* value = TESForm::GetById(form);
    return value && value->formType == TESGlobal::Type && !value->IsDeleted() && !value->IsStillLoading() ?
        static_cast<TESGlobal*>(value) : nullptr;
}
Skills* LevelXP()
{
    // Same level-points field as SKSE 2.0.20 PapyrusGame::{Get,Set}PlayerExperience.
    // Never AddSkillExperience: that redistributes skill XP and is NOT RFAB's engine pool.
    auto* player = PlayerCharacter::Get();
    return player && player->pSkills ? *player->pSkills : nullptr;
}
}
void RewardService::ApplyPersonal(const Coop::PersonalRewardRequest& request) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || !m_application || m_application->Epoch != request.Epoch ||
        m_application->Token != request.Token || m_application->Receipt) return;
    const auto application = *m_application;
    auto& saves = m_world.ctx().at<SaveStateService>();
    auto& bridge = RewardBridgeClient::Mailbox();
    const auto admitted = [&] {
        return application.Epoch == SaveStateBridge::Epoch() && !m_world.IsLoadingWorld() && !m_blocked &&
            m_application && m_application->Epoch == application.Epoch && m_application->Token == application.Token &&
            saves.NativeAdmitted(application.Effect) && bridge.CanApply(application.Epoch, application.Token);
    };
    const auto finish = [&](Coop::Rewards::Result state, int32_t final = 0) {
        // A forced load must never stamp/complete/release the NEW world's native reservation.
        if (application.Epoch != SaveStateBridge::Epoch() || m_world.IsLoadingWorld())
        { saves.Uncertain(application.Effect, application.Epoch); return; }
        if (!bridge.Complete(application.Epoch, {application.Epoch, application.Token, state, final,
            state == Coop::Rewards::Result::Applied ? Coop::Rewards::Wallet | Coop::Rewards::Engine : 0u}))
            Fail("Personal RFAB native executor lost its application token; no replay");
    };
    const auto adapter = bridge.Adapter(application.Epoch);
    if (!admitted() || !adapter || !adapter->Receiver || !ProfileMatches(*adapter))
    { finish(Coop::Rewards::Result::NotApplied); return; }
    auto* wallet = ResolveGlobal(request.Wallet);
    const auto* multiplier = ResolveGlobal(request.Multiplier);
    const auto* level = LevelXP();
    const auto* setter = m_world.ctx().at<PapyrusService>().Get("GlobalVariable", "SetValue");
    auto* game = GameVM::Get();
    if (!wallet || !multiplier || request.Wallet == request.Multiplier || !level || !setter || !game || !game->virtualMachine)
    { finish(Coop::Rewards::Result::NotApplied); return; }
    const auto plan = Coop::PlanPersonalReward(application.Grant.BaseXP, multiplier->f, wallet->f, level->xp, request.Quantum);
    if (!plan) { finish(Coop::Rewards::Result::NotApplied); return; }
    // The amount comes ONLY from the server offer. Papyrus supplies actual attached properties,
    // not a guessed wallet delta or a caller-selected final amount. This recipe is explicitly the
    // audited Papyrus float-multiply/int-truncate/quantum formula, not an unknown RFAB.dll formula.
    const PapyrusFunction<void, TESGlobal, float> setWallet(setter);
    if (!admitted()) { finish(Coop::Rewards::Result::NotApplied); return; }
    setWallet(wallet, plan->WalletAfter); // engine setter retains normal GlobalVariable save/change behavior
    if (!admitted()) { finish(Coop::Rewards::Result::Uncertain); return; }
    wallet = ResolveGlobal(request.Wallet);
    auto* currentLevel = LevelXP();
    if (!wallet || !currentLevel || wallet->f != plan->WalletAfter || currentLevel->xp != plan->EngineBefore)
    { finish(Coop::Rewards::Result::Uncertain); return; } // partial/concurrent personal mutation is NOT normalized away
    currentLevel->xp = plan->EngineAfter; // exact SKSE Game.SetPlayerExperience semantics, under our native/save admission
    if (!admitted()) { finish(Coop::Rewards::Result::Uncertain); return; }
    const auto* finalWallet = ResolveGlobal(request.Wallet);
    const auto* finalLevel = LevelXP();
    if (!finalWallet || !finalLevel || finalWallet->f != plan->WalletAfter || finalLevel->xp != plan->EngineAfter)
    { finish(Coop::Rewards::Result::Uncertain); return; }
    finish(Coop::Rewards::Result::Applied, plan->Final); // independently read-back wallet AND level XP, not a claimed pool mask
}
