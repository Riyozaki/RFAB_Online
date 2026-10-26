#include "MagicTarget.h"

#include <algorithm>
#include <chrono>

#include <Actor.h>
#include <World.h>
#include "EffectItem.h"

#include <Structs/Skyrim/AnimationGraphDescriptor_WerewolfBehavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_VampireLordBehavior.h>

#include <Events/AddTargetEvent.h>
#include <Events/HitEvent.h>

#include <Games/ActorExtension.h>
#include <Games/Overrides.h>
#include <Games/References.h>
#include <PlayerCharacter.h>
#include <CoopDamagePipeline.h>
#include <RfabModpackCompat.h>
#include <Services/RfabAdapterService.h>

#include <Effects/ActiveEffect.h>
#include <Forms/EffectSetting.h>
#include <Forms/SpellItem.h>
#include <Magic/ActorMagicCaster.h>
#include <Services/PartyService.h>

namespace
{
// Temporarily marks allied RemotePlayer clones in the party with ActorBoolBits::kPlayerTeammate (1 << 26)
// during MagicCaster::FindTargets and MagicTarget::AddTarget when the local player casts a friendly support/buff/aura spell.
// This allows Skyrim/Requiem/RFAB party auras, Leadership perks, Lionheart Cloak, Priesthood Auras, Warhorns, and
// Totem of Brotherhood (whose Creation Kit TESConditions check IsPlayerTeammate == 1) to naturally target co-op teammates.
struct ScopedCoopPartyTeammateFlags
{
    explicit ScopedCoopPartyTeammateFlags(bool aEnable) noexcept
    {
        if (!aEnable)
            return;

        auto& world = World::Get();
        TiltedPhoques::Vector<uint32_t> peerFormIds;
        world.GetRfabAdapterService().CollectPartyPeerActorFormIds(peerFormIds);
        for (const uint32_t formId : peerFormIds)
        {
            Actor* pPeerActor = Cast<Actor>(TESForm::GetById(formId));
            if (!pPeerActor || (pPeerActor->flags1 & Actor::ActorBoolBits::kPlayerTeammate) != 0)
                continue;

            pPeerActor->flags1 |= Actor::ActorBoolBits::kPlayerTeammate;
            m_modifiedActors.push_back(pPeerActor);
        }
    }

    ~ScopedCoopPartyTeammateFlags() noexcept
    {
        for (Actor* pActor : m_modifiedActors)
        {
            if (pActor)
                pActor->flags1 &= ~Actor::ActorBoolBits::kPlayerTeammate;
        }
    }

private:
    TiltedPhoques::Vector<Actor*> m_modifiedActors{};
};
} // namespace

TP_THIS_FUNCTION(TAddTarget, bool, MagicTarget, MagicTarget::AddTargetData& arData);
TP_THIS_FUNCTION(TCheckAddEffectTargetData, bool, MagicTarget::AddTargetData, void* arArgs, float afResistance);
TP_THIS_FUNCTION(TFindTargets, bool, MagicCaster, float afEffectivenessMult, int32_t* aruiTargetCount,
                 TESBoundObject* apSource, char abLoadCast, char abAdjust);
TP_THIS_FUNCTION(TAdjustForPerks, void, ActiveEffect, Actor* apCaster, MagicTarget* apTarget);
TP_THIS_FUNCTION(THasPerk, bool, Actor, TESForm* apPerk, void* apUnk1, double* afReturnValue);
TP_THIS_FUNCTION(TGetPerkRank, uint8_t, Actor, TESForm* apPerk);

static TAddTarget* RealAddTarget = nullptr;
static TCheckAddEffectTargetData* RealCheckAddEffectTargetData = nullptr;
static TFindTargets* RealFindTargets = nullptr;
static TAdjustForPerks* RealAdjustForPerks = nullptr;
static THasPerk* RealHasPerk = nullptr;
static TGetPerkRank* RealGetPerkRank = nullptr;

static thread_local bool s_autoSucceedEffectCheck = false;
static thread_local bool s_applyHealPerkBonus = false;
static thread_local bool s_applyStaminaPerkBonus = false;

void MagicTarget::DispelAllSpells(bool aNow) noexcept
{
    TP_THIS_FUNCTION(TDispelAllSpells, void, MagicTarget, bool aNow);
    POINTER_SKYRIMSE(TDispelAllSpells, dispelAllSpells, 33728);
    TiltedPhoques::ThisCall(dispelAllSpells, this, aNow);
}

bool MagicTarget::AddTarget(AddTargetData& arData, bool aApplyHealPerkBonus, bool aApplyStaminaPerkBonus) noexcept
{
    s_autoSucceedEffectCheck = true;
    s_applyHealPerkBonus = aApplyHealPerkBonus;
    s_applyStaminaPerkBonus = aApplyStaminaPerkBonus;

    bool result = TiltedPhoques::ThisCall(RealAddTarget, this, arData);

    s_autoSucceedEffectCheck = false;
    s_applyHealPerkBonus = false;
    s_applyStaminaPerkBonus = false;

    return result;
}

bool MagicTarget::AddTargetData::ShouldSync()
{
    if (!pEffectItem || !pEffectItem->pEffectSetting || !pSpell)
        return false;

    // Constant-effect slow time abilities (e.g. Requiem/RFAB Evasion Quick Reflexes) are added and removed locally by
    // each client's own scripts, so syncing them would leave allies permanently stuck in slow motion.
    if (pEffectItem->IsSlowEffect())
    {
        if (SpellItem* pSpellItem = Cast<SpellItem>(pSpell); pSpellItem && pSpellItem->eCastingType == MagicSystem::CastingType::CONSTANT_EFFECT)
            return false;
    }

    // Never sync player-cast `kScript` ActiveMagicEffects unless they are friendly non-SELF party support spells:
    //   1. SELF-delivered `kScript` effects (Spirit Toggle summons, Mirel_REUS tokens, Stances/Auras) corrupt puppets.
    //   2. Hostile targeted/aimed/cloak `kScript` effects (`RFAB_PapyrusFunctions.ScriptDamage`, custom RFAB procs)
    //      rely on `akCaster == Game.GetPlayer()` (`0x14`) perks, skills, and equipped items — they execute
    //      authoritatively on the casting player's own client via `ScriptDamageAuthLedger` and sync exact HP deltas!
    if (pEffectItem->pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kScript &&
        pCaster && pCaster->GetExtension() && pCaster->GetExtension()->IsPlayer())
    {
        const bool isFriendlyPartySupport =
            (pEffectItem->pEffectSetting->deliveryType != static_cast<uint32_t>(MagicSystem::Delivery::SELF)) &&
            (pSpell->IsHealingSpell() || pSpell->IsBuffSpell());
        if (!isFriendlyPartySupport)
            return false;
    }

    return !pEffectItem->IsSummonEffect() && !pSpell->IsInvisibilitySpell() && !pSpell->IsWardSpell();
}

// Some effects are player specific, and do not need to be synced.
bool MagicTarget::AddTargetData::IsForbiddenEffect(Actor* apTarget)
{
    if (apTarget != PlayerCharacter::Get() || !pEffectItem)
        return false;

    return pEffectItem->IsNightVisionEffect() || pEffectItem->IsSlowEffect();
}

Actor* MagicTarget::GetTargetAsActor()
{
    TP_THIS_FUNCTION(TGetTargetAsActor, Actor*, MagicTarget);
    POINTER_SKYRIMSE(TGetTargetAsActor, getTargetAsActor, 33745);
    return TiltedPhoques::ThisCall(getTargetAsActor, this);
}

namespace
{
CoopDamagePipeline::ConcentrationSyncLimiter<64> s_concentrationSyncLimiter{};

[[nodiscard]] uint64_t NowMagicSteadyMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

bool TP_MAKE_THISCALL(HookAddTarget, MagicTarget, MagicTarget::AddTargetData& arData)
{
    if (!World::IsCreated() || !World::IsConnectedCached())
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    Actor* pTargetActor = apThis->GetTargetAsActor();
    if (!pTargetActor || !arData.pEffectItem || !arData.pEffectItem->pEffectSetting || !arData.pSpell ||
        arData.IsForbiddenEffect(pTargetActor))
    {
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
    }

    ActorExtension* pTargetActorEx = pTargetActor->GetExtension();
    if (!pTargetActorEx)
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    const bool isFriendlySupport = arData.pSpell->IsHealingSpell() || arData.pSpell->IsBuffSpell();
    const SpellItem* pSpellItem = Cast<SpellItem>(arData.pSpell);
    const bool isConcentration = pSpellItem && pSpellItem->eCastingType == MagicSystem::CastingType::CONCENTRATION;

    const auto coopPolicy =
        arData.pCaster ? World::Get().GetRfabAdapterService().EvaluateCoopAction(arData.pCaster, pTargetActor, isFriendlySupport)
                       : RfabCompat::CoopAllyActionPolicy{};

    // RFAB «Единство в битве (Кооп)»: evaluated BEFORE ScopedSpellCastOverride so synchronous AoE / cloak / touch
    // effects inside CastSpellImmediate are also 100% blocked on co-op allies and their summons, while 100% of
    // heals & buffs (including RFAB_List_NoAllyDamage_AllowedSpells) continue working!
    if (coopPolicy.BlockHarmfulAction)
        return false;

    if (arData.pEffectItem->IsWerewolfEffect())
        pTargetActorEx->GraphDescriptorHash = AnimationGraphDescriptor_WerewolfBehavior::m_key;

    if (arData.pEffectItem->IsVampireLordEffect())
        pTargetActorEx->GraphDescriptorHash = AnimationGraphDescriptor_VampireLordBehavior::m_key;

    if (ScopedSpellCastOverride::IsOverriden() || s_autoSucceedEffectCheck)
        return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);

    AddTargetEvent addTargetEvent{};
    addTargetEvent.TargetID = pTargetActor->formID;
    addTargetEvent.CasterID = arData.pCaster ? arData.pCaster->formID : 0;
    addTargetEvent.SpellID = arData.pSpell->formID;
    addTargetEvent.EffectID = arData.pEffectItem->pEffectSetting->formID;
    addTargetEvent.Magnitude = arData.fMagnitude;
    addTargetEvent.IsDualCasting = arData.bDualCast;

    // Preserve local player's RFAB difficulty & Cursed outgoing multiplier (Странник 2.0x / Довакин,Легенда 1.0x / Проклятый 0.5x)
    // when syncing direct player-cast hostile health spells, shouts, weapon enchantments, and poisons to remote targets.
    // On the receiving client, caster is RemotePlayer (0xFFxxxxxx != 0x14), so RFAB.dll does not re-apply ByPCVH (0 double-mult).
    if (arData.pCaster && arData.pEffectItem && arData.pEffectItem->pEffectSetting && !pTargetActorEx->IsLocalPlayer())
    {
        const auto* pMgef = arData.pEffectItem->pEffectSetting;
        const bool shouldScaleRfab = RfabCompat::ShouldScalePlayerOutgoingHealthEffect(
            Settings::IsRfabModpackActive(),
            arData.pCaster->formID,
            pMgef->flags,
            pMgef->primaryAV,
            addTargetEvent.Magnitude);
        addTargetEvent.Magnitude = RfabCompat::ScalePlayerOutgoingSpellMagnitude(
            addTargetEvent.Magnitude,
            Settings::GetRfabOutgoingDamageMult(),
            shouldScaleRfab);
    }

    if (pTargetActorEx->IsRemotePlayer())
    {
        if (!arData.pCaster)
            return false;

        const bool isPvpAllowed = World::Get().GetServerSettings().PvpEnabled || coopPolicy.AllowPartySparring;
        if (!isFriendlySupport && !isPvpAllowed)
            return false;

        ActorExtension* pCasterExtension = arData.pCaster->GetExtension();
        const Actor* pCasterCmd = arData.pCaster->GetCommandingActor();
        const bool isLocalCaster =
            (pCasterExtension && pCasterExtension->IsLocalPlayer()) ||
            (pCasterCmd && pCasterCmd->formID == 0x14) ||
            (pCasterExtension && !pCasterExtension->IsRemote() && arData.pCaster->IsPlayerSummon());
        if (!isLocalCaster)
            return false;

        if (arData.pSpell && arData.pSpell->IsHealingSpell())
        {
            addTargetEvent.ApplyHealPerkBonus = arData.pCaster->HasPerk(0x581f8);
            addTargetEvent.ApplyStaminaPerkBonus = arData.pCaster->HasPerk(0x581f9);
        }

        ScopedCoopPartyTeammateFlags teammateGuard(isFriendlySupport);
        bool result = TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
        // Concentration spells (e.g. Healing Hands / Sparks) are channeled continuously via SpellCastRequest/InterruptCastRequest;
        // Fire-and-Forget heals/buffs/spells sync deterministically via AddTargetEvent.
        if (result && !isConcentration && arData.ShouldSync())
            World::Get().GetRunner().Trigger(addTargetEvent);
        return result;
    }

    if (pTargetActorEx->IsLocalPlayer())
    {
        if (arData.pCaster)
        {
            ActorExtension* pCasterExtension = arData.pCaster->GetExtension();
            const Actor* pCasterCmd = arData.pCaster->GetCommandingActor();
            const auto* pCasterCmdExt = pCasterCmd ? pCasterCmd->GetExtension() : nullptr;
            const bool isFromRemotePlayerOrSummon =
                (pCasterExtension && pCasterExtension->IsRemotePlayer()) ||
                (pCasterCmdExt && pCasterCmdExt->IsRemotePlayer()) ||
                (pCasterExtension && pCasterExtension->IsRemote() && arData.pCaster->IsPlayerSummon());

            if (isFromRemotePlayerOrSummon)
            {
                // Allow targeted concentration heals & buffs (e.g. Healing Hands) from allied remote players even when PvP is disabled!
                if (isFriendlySupport)
                {
                    if (!isConcentration)
                        return false; // Fire-and-forget ally heals/buffs are synced via NotifyAddTarget from caster

                    return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
                }

                if (!World::Get().GetServerSettings().PvpEnabled && !coopPolicy.AllowPartySparring)
                    return false;

                if (!isConcentration)
                    return false; // Non-concentration PvP/sparring spells & cloak ticks are synced authoritatively via NotifyAddTarget
            }
        }

        bool result = TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
        if (result && arData.ShouldSync())
            World::Get().GetRunner().Trigger(addTargetEvent);
        return result;
    }

    // Block external hostile/detrimental magic from bypassing a synchronized boss invulnerability/ethereal phase (SetGhost),
    // while allowing the boss's own self-cast phase spells/auras (arData.pCaster == pTargetActor).
    if (!isFriendlySupport && pTargetActor->IsGhost() && arData.pCaster != pTargetActor)
        return false;

    if (arData.pCaster)
    {
        ActorExtension* pCasterExtension = arData.pCaster->GetExtension();
        const Actor* pCasterCmd = arData.pCaster->GetCommandingActor();
        const auto* pCasterCmdExt = pCasterCmd ? pCasterCmd->GetExtension() : nullptr;
        const bool isLocalPlayerOrSummon =
            (pCasterExtension && pCasterExtension->IsLocalPlayer()) ||
            (pCasterCmd && pCasterCmd->formID == 0x14) ||
            (pCasterExtension && !pCasterExtension->IsRemote() && arData.pCaster->IsPlayerSummon());
        const bool isRemotePlayerOrSummon =
            (pCasterExtension && pCasterExtension->IsRemotePlayer()) ||
            (pCasterCmdExt && pCasterCmdExt->IsRemotePlayer()) ||
            (pCasterExtension && pCasterExtension->IsRemote() && arData.pCaster->IsPlayerSummon());
        if (isLocalPlayerOrSummon)
        {
            if (arData.pSpell && arData.pSpell->IsHealingSpell())
            {
                addTargetEvent.ApplyHealPerkBonus = arData.pCaster->HasPerk(0x581f8);
                addTargetEvent.ApplyStaminaPerkBonus = arData.pCaster->HasPerk(0x581f9);
            }

            if (!isFriendlySupport &&
                pTargetActorEx->IsRemote() &&
                !pTargetActorEx->IsPlayer())
            {
                const uint64_t durationMs =
                    static_cast<uint64_t>(std::max(0.0f, arData.fDuration) * 1000.0f) + 2000ULL;
                Actor::ArmLocalPlayerScriptDamageWindow(pTargetActor->formID, durationMs);
            }

            bool result = TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
            if (result && (!isFriendlySupport || !isConcentration) && arData.ShouldSync())
            {
                const bool allowPacket = !isConcentration ||
                    s_concentrationSyncLimiter.ShouldAllowSend(
                        addTargetEvent.CasterID,
                        addTargetEvent.TargetID,
                        addTargetEvent.EffectID,
                        NowMagicSteadyMs());
                if (allowPacket)
                    World::Get().GetRunner().Trigger(addTargetEvent);
            }
            return result;
        }
        else if (isRemotePlayerOrSummon)
        {
            if (isFriendlySupport)
            {
                if (isConcentration && pTargetActorEx->IsLocal())
                    return TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
                return false;
            }

            // Send out a HitEvent because TakeDamage is never triggered.
            World::Get().GetRunner().Trigger(HitEvent(arData.pCaster->formID, pTargetActor->formID));
            return false;
        }
    }

    if (pTargetActorEx->IsLocal())
    {
        bool result = TiltedPhoques::ThisCall(RealAddTarget, apThis, arData);
        if (result && arData.ShouldSync())
            World::Get().GetRunner().Trigger(addTargetEvent);
        return result;
    }
    else
    {
        return false;
    }
}

bool TP_MAKE_THISCALL(HookCheckAddEffectTargetData, MagicTarget::AddTargetData, void* arArgs, float afResistance)
{
    if (s_autoSucceedEffectCheck)
        return true;

    return TiltedPhoques::ThisCall(RealCheckAddEffectTargetData, apThis, arArgs, afResistance);
}

bool TP_MAKE_THISCALL(HookFindTargets, MagicCaster, float afEffectivenessMult, int32_t* aruiTargetCount, TESBoundObject* apSource, char abLoadCast, char abAdjust)
{
    bool enablePartyTeammateAuras = false;
    if (apThis && apThis->pCurrentSpell && (apThis->pCurrentSpell->IsHealingSpell() || apThis->pCurrentSpell->IsBuffSpell()))
    {
        const auto* pActorCaster = static_cast<const ActorMagicCaster*>(apThis);
        if (pActorCaster->pCasterActor && pActorCaster->pCasterActor->formID == 0x14)
            enablePartyTeammateAuras = true;
    }

    ScopedCoopPartyTeammateFlags teammateGuard(enablePartyTeammateAuras);
    return TiltedPhoques::ThisCall(RealFindTargets, apThis, afEffectivenessMult, aruiTargetCount, apSource, abLoadCast, abAdjust);
}

void TP_MAKE_THISCALL(HookAdjustForPerks, ActiveEffect, Actor* apCaster, MagicTarget* apTarget)
{
    TiltedPhoques::ThisCall(RealAdjustForPerks, apThis, apCaster, apTarget);

    if (s_applyHealPerkBonus && apThis)
    {
        const auto* pEffItem = static_cast<const EffectItem*>(apThis->pEffect);
        const auto* pMgef = pEffItem ? pEffItem->pEffectSetting : nullptr;
        if (!pMgef || RfabCompat::ShouldApplyImprovedHealingPerk(
                pMgef->flags,
                pMgef->primaryAV,
                static_cast<int32_t>(pMgef->eArchetype),
                apThis->fMagnitude))
        {
            apThis->fMagnitude *= RfabCompat::GetRestorationImprovedHealingMultiplier(Settings::IsRfabModpackActive());
        }
    }
}

bool TP_MAKE_THISCALL(HookHasPerk, Actor, TESForm* apPerk, void* apUnk1, double* afReturnValue)
{
    ActorExtension* pExtension = apThis ? apThis->GetExtension() : nullptr;

    if (pExtension && pExtension->IsRemotePlayer())
    {
        if (apPerk && apPerk->formID == 0x581f9)
        {
            if (s_applyStaminaPerkBonus)
            {
                if (afReturnValue)
                    *afReturnValue = 1.f;
                return true;
            }
        }
    }

    return TiltedPhoques::ThisCall(RealHasPerk, apThis, apPerk, apUnk1, afReturnValue);
}

uint8_t TP_MAKE_THISCALL(HookGetPerkRank, Actor, TESForm* apPerk)
{
    return TiltedPhoques::ThisCall(RealGetPerkRank, apThis, apPerk);
}

static TiltedPhoques::Initializer s_magicTargetHooks([]() {
    POINTER_SKYRIMSE(TAddTarget, addTarget, 33742);
    POINTER_SKYRIMSE(TCheckAddEffectTargetData, checkAddEffectTargetData, 33741);
    POINTER_SKYRIMSE(TFindTargets, findTargets, 33632);
    POINTER_SKYRIMSE(TAdjustForPerks, adjustForPerks, 33278);
    POINTER_SKYRIMSE(THasPerk, hasPerk, 21167);
    POINTER_SKYRIMSE(TGetPerkRank, getPerkRank, 36690);

    RealAddTarget = addTarget.Get();
    RealCheckAddEffectTargetData = checkAddEffectTargetData.Get();
    RealFindTargets = findTargets.Get();
    RealAdjustForPerks = adjustForPerks.Get();
    RealHasPerk = hasPerk.Get();
    RealGetPerkRank = getPerkRank.Get();

    TP_HOOK(&RealAddTarget, HookAddTarget);
    TP_HOOK(&RealCheckAddEffectTargetData, HookCheckAddEffectTargetData);
    TP_HOOK(&RealFindTargets, HookFindTargets);
    TP_HOOK(&RealAdjustForPerks, HookAdjustForPerks);
    TP_HOOK(&RealHasPerk, HookHasPerk);
    //TP_HOOK(&RealGetPerkRank, HookGetPerkRank);
});
