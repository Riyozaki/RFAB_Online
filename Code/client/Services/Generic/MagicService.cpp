#include <Services/MagicService.h>
#include <Services/RfabAdapterService.h>

#include <World.h>

#include <Events/UpdateEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/PartyJoinedEvent.h>
#include <Events/SpellCastEvent.h>
#include <Events/InterruptCastEvent.h>
#include <Events/AddTargetEvent.h>
#include <Events/RemoveSpellEvent.h>

#include <Messages/RemoveSpellRequest.h>

#include <Messages/SpellCastRequest.h>
#include <Messages/InterruptCastRequest.h>

#include <Messages/NotifySpellCast.h>
#include <Messages/NotifyInterruptCast.h>

#include <Actor.h>
#include <AI/AIProcess.h>
#include <Misc/MiddleProcess.h>
#include <Effects/ActiveEffect.h>
#include <Magic/ActorMagicCaster.h>
#include <Games/ActorExtension.h>
#include <Games/References.h>
#include <EquipManager.h>
#include <RfabModpackCompat.h>

#include <Structs/Skyrim/AnimationGraphDescriptor_VampireLordBehavior.h>
#include <Structs/Skyrim/AnimationGraphDescriptor_WerewolfBehavior.h>

#include <Games/Overrides.h>

#include <Forms/SpellItem.h>
#include <PlayerCharacter.h>

#include <Games/TES.h>

namespace
{
constexpr uint32_t kActiveEffectDispelledFlag = (1u << 18);

bool HasActiveSpellOnActor(Actor* apActor, uint32_t aSpellFormId) noexcept
{
    if (!apActor || aSpellFormId == 0 || !apActor->currentProcess || !apActor->currentProcess->middleProcess ||
        !apActor->currentProcess->middleProcess->ActiveEffects)
    {
        return false;
    }

    for (ActiveEffect* pEffect : *apActor->currentProcess->middleProcess->ActiveEffects)
    {
        if (!pEffect || !pEffect->pSpell || pEffect->pSpell->formID != aSpellFormId)
            continue;
        if ((pEffect->uiFlags & kActiveEffectDispelledFlag) != 0)
            continue;
        if (pEffect->fDuration > 0.0f && pEffect->fElapsedSeconds >= pEffect->fDuration)
            continue;
        return true;
    }

    return false;
}

// Prevents duplicate CONSTANT_EFFECT stances/auras from stacking on remote actors and refreshes timed buffs/cloaks cleanly.
bool TryRefreshOrDeduplicateActiveEffect(Actor* apActor, MagicItem* apSpell, EffectItem* apEffect, float aMagnitude) noexcept
{
    if (!apActor || !apSpell || !apEffect || !apActor->currentProcess || !apActor->currentProcess->middleProcess ||
        !apActor->currentProcess->middleProcess->ActiveEffects)
    {
        return false;
    }

    const SpellItem* pSpellItem = Cast<SpellItem>(apSpell);
    const bool isConstantEffect = pSpellItem && pSpellItem->eCastingType == MagicSystem::CastingType::CONSTANT_EFFECT;
    const bool isNonHostileBuffOrCloak =
        apSpell->iHostileCount <= 0 &&
        apEffect->pEffectSetting &&
        (apEffect->pEffectSetting->flags & (EffectSetting::kHostile | EffectSetting::kDetrimental)) == 0 &&
        (isConstantEffect ||
         apEffect->pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kCloak ||
         apEffect->data.iDuration >= 10);

    if (!isConstantEffect && !isNonHostileBuffOrCloak)
        return false;

    for (ActiveEffect* pActive : *apActor->currentProcess->middleProcess->ActiveEffects)
    {
        if (!pActive || pActive->pSpell != apSpell || pActive->pEffect != apEffect)
            continue;
        if ((pActive->uiFlags & kActiveEffectDispelledFlag) != 0)
            continue;
        if (pActive->fDuration > 0.0f && pActive->fElapsedSeconds >= pActive->fDuration)
            continue;

        pActive->fElapsedSeconds = 0.0f;
        if (std::isfinite(aMagnitude) && aMagnitude > 0.0f && isConstantEffect)
            pActive->fMagnitude = aMagnitude;
        return true;
    }

    return false;
}
} // namespace

MagicService::MagicService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&MagicService::OnUpdate>(this);
    m_cellChangeConnection = m_dispatcher.sink<CellChangeEvent>().connect<&MagicService::OnCellChangeEvent>(this);
    m_partyJoinedConnection = m_dispatcher.sink<PartyJoinedEvent>().connect<&MagicService::OnPartyJoinedEvent>(this);
    m_spellCastEventConnection = m_dispatcher.sink<SpellCastEvent>().connect<&MagicService::OnSpellCastEvent>(this);
    m_notifySpellCastConnection = m_dispatcher.sink<NotifySpellCast>().connect<&MagicService::OnNotifySpellCast>(this);
    m_interruptCastEventConnection = m_dispatcher.sink<InterruptCastEvent>().connect<&MagicService::OnInterruptCastEvent>(this);
    m_notifyInterruptCastConnection = m_dispatcher.sink<NotifyInterruptCast>().connect<&MagicService::OnNotifyInterruptCast>(this);
    m_addTargetEventConnection = m_dispatcher.sink<AddTargetEvent>().connect<&MagicService::OnAddTargetEvent>(this);
    m_notifyAddTargetConnection = m_dispatcher.sink<NotifyAddTarget>().connect<&MagicService::OnNotifyAddTarget>(this);
    m_removeSpellEventConnection = m_dispatcher.sink<RemoveSpellEvent>().connect<&MagicService::OnRemoveSpellEvent>(this);
    m_notifyRemoveSpell = m_dispatcher.sink<NotifyRemoveSpell>().connect<&MagicService::OnNotifyRemoveSpell>(this);
}

void MagicService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
    {
        m_activeLocalPlayerSpells.clear();
        return;
    }

    ApplyQueuedEffects();

    UpdateRevealOtherPlayersEffect();

    PollLocalPlayerActiveBuffs(acEvent.Delta);
}

void MagicService::OnSpellCastEvent(const SpellCastEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected() || acEvent.CasterFormId == 0)
        return;

    Actor* pCasterActor = Cast<Actor>(TESForm::GetById(acEvent.CasterFormId));
    if (!pCasterActor || !pCasterActor->GetNiNode())
    {
        spdlog::debug("Spell cast event actor {:X} is no longer loaded", acEvent.CasterFormId);
        return;
    }

    TESForm* pMagicForm = TESForm::GetById(acEvent.SpellId);

    // Summon staves (EnchantmentItem : MagicItem) spawn actors that replicate through CharacterService;
    // syncing SpellCastEvent on peers spawns a second hostile duplicate summon (#894).
    if (const MagicItem* pMagicItem = Cast<MagicItem>(pMagicForm);
        pMagicItem && pMagicItem->HasSummonEffect())
    {
        spdlog::debug("Skipping SpellCastEvent sync for summon magic item {:X}", acEvent.SpellId);
        return;
    }

    // Sync concentration spells (including targeted ally concentration heals/buffs like Healing Hands,
    // while excluding self-only concentration heals so puppets don't double-heal themselves), wards, and invisibility.
    if (SpellItem* pSpell = Cast<SpellItem>(pMagicForm))
    {
        bool isSelfOnlyHeal = false;
        if (pSpell->IsHealingSpell())
        {
            isSelfOnlyHeal = true;
            for (const EffectItem* pEffect : pSpell->listOfEffects)
            {
                if (pEffect && pEffect->pEffectSetting &&
                    pEffect->pEffectSetting->deliveryType != static_cast<uint32_t>(MagicSystem::Delivery::SELF))
                {
                    isSelfOnlyHeal = false;
                    break;
                }
            }
        }

        if ((pSpell->eCastingType != MagicSystem::CastingType::CONCENTRATION || isSelfOnlyHeal) && !pSpell->IsWardSpell() && !pSpell->IsInvisibilitySpell())
        {
            spdlog::debug("Canceled magic spell");
            return;
        }
    }

    uint32_t formId = acEvent.CasterFormId;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto casterEntityIt = std::find_if(std::begin(view), std::end(view), [formId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (casterEntityIt == std::end(view))
        return;

    auto& localComponent = view.get<LocalComponent>(*casterEntityIt);

    SpellCastRequest request{};

    request.CasterId = localComponent.Id;
    request.CastingSource = acEvent.CastingSource;
    request.IsDualCasting = acEvent.IsDualCasting;
    request.EffectivenessMult =
        (formId == 0x14 && Settings::IsRfabModpackActive()) ? Settings::GetRfabOutgoingDamageMult() : 1.0f;

    if (!m_world.GetModSystem().GetServerModId(acEvent.SpellId, request.SpellFormId))
    {
        spdlog::error("Server spell id not found for spell form id {:X}", acEvent.SpellId);
        return;
    }

    if (acEvent.DesiredTargetID != 0)
    {
        auto targetView = m_world.view<FormIdComponent>();
        const auto targetEntityIt = std::find_if(std::begin(targetView), std::end(targetView), [id = acEvent.DesiredTargetID, targetView](entt::entity entity) { return targetView.get<FormIdComponent>(entity).Id == id; });

        if (targetEntityIt != std::end(targetView))
        {
            auto desiredTargetIdRes = Utils::GetServerId(*targetEntityIt);
            if (desiredTargetIdRes.has_value())
                request.DesiredTarget = desiredTargetIdRes.value();
            else
                spdlog::debug("{}: desired target {:X} still awaiting server assignment", __FUNCTION__, acEvent.DesiredTargetID);
        }
    }

    spdlog::debug("Spell cast event sent, ID: {:X}, Source: {}, IsDualCasting: {}, desired target: {:X}", request.CasterId, request.CastingSource, request.IsDualCasting, request.DesiredTarget);

    m_transport.Send(request);
}

void MagicService::OnNotifySpellCast(const NotifySpellCast& acMessage) const noexcept
{
    using CS = MagicSystem::CastingSource;

    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.CasterId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Caster with remote id {:X} not found.", acMessage.CasterId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);
    TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);
    if (!pActor)
    {
        spdlog::warn("{}: remote caster form {:X} not loaded.", __FUNCTION__, formIdComponent.Id);
        return;
    }

    pActor->GenerateMagicCasters();

    // Only left hand casters need dual casting (?)
    if (pActor->casters[CS::LEFT_HAND])
        pActor->casters[CS::LEFT_HAND]->SetDualCasting(acMessage.IsDualCasting);

    if (acMessage.CastingSource >= 4)
    {
        spdlog::warn("{}: could not find casting source {}", __FUNCTION__, acMessage.CastingSource);
        return;
    }

    MagicItem* pSpell = nullptr;

    pSpell = pActor->magicItems[acMessage.CastingSource];

    if (!pSpell)
    {
        const uint32_t cSpellFormId = World::Get().GetModSystem().GetGameId(acMessage.SpellFormId);
        if (cSpellFormId == 0)
        {
            spdlog::error("Could not find spell form id for GameId base {:X}, mod {:X}", acMessage.SpellFormId.BaseId, acMessage.SpellFormId.ModId);
            return;
        }

        TESForm* pSpellForm = TESForm::GetById(cSpellFormId);
        if (!pSpellForm)
        {
            spdlog::error("Cannot find spell form, id: {:X}.", cSpellFormId);
            return;
        }

        pSpell = Cast<MagicItem>(pSpellForm);
    }

    if (!pSpell)
    {
        spdlog::error("Could not find spell.");
        return;
    }

    TESObjectREFR* pDesiredTarget = nullptr;

    if (acMessage.DesiredTarget != 0)
    {
        auto view = m_world.view<FormIdComponent>();
        for (auto entity : view)
        {
            std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
            if (!serverIdRes.has_value())
                continue;

            uint32_t serverId = serverIdRes.value();

            if (serverId == acMessage.DesiredTarget)
            {
                const auto& formIdComponent = view.get<FormIdComponent>(entity);
                pDesiredTarget = Cast<TESObjectREFR>(TESForm::GetById(formIdComponent.Id));
                break;
            }
        }
    }

    ScopedSpellCastOverride _;

    MagicCaster* pCaster = pActor->GetMagicCaster(static_cast<CS>(acMessage.CastingSource));
    if (!pCaster)
    {
        spdlog::warn("{}: failed to find caster.", __FUNCTION__);
        return;
    }

    const float effectivenessMult = RfabCompat::SanitizeDifficultyMultiplier(acMessage.EffectivenessMult);
    const bool adjustOnlyHostile = std::abs(effectivenessMult - 1.0f) > 0.001f;
    pCaster->CastSpellImmediate(pSpell, false, pDesiredTarget, effectivenessMult, adjustOnlyHostile, 0.0f, pActor);

    spdlog::debug("Successfully casted remote spell");
}

void MagicService::OnInterruptCastEvent(const InterruptCastEvent& acEvent) const noexcept
{
    if (!m_transport.IsConnected())
        return;

    uint32_t formId = acEvent.CasterFormID;

    auto view = m_world.view<FormIdComponent, LocalComponent>();
    const auto casterEntityIt = std::find_if(std::begin(view), std::end(view), [formId, view](entt::entity entity) { return view.get<FormIdComponent>(entity).Id == formId; });

    if (casterEntityIt == std::end(view))
    {
        spdlog::debug("{}: skipping interrupt for non-local/unassigned caster form id {:X}", __FUNCTION__, formId);
        return;
    }

    auto& localComponent = view.get<LocalComponent>(*casterEntityIt);

    InterruptCastRequest request;
    request.CasterId = localComponent.Id;
    request.CastingSource = acEvent.CastingSource;

    spdlog::debug("Sending out interrupt cast");

    m_transport.Send(request);
}

void MagicService::OnNotifyInterruptCast(const NotifyInterruptCast& acMessage) const noexcept
{
    if (acMessage.CastingSource >= 4)
    {
        spdlog::warn("{}: could not find casting source {}", __FUNCTION__, acMessage.CastingSource);
        return;
    }

    auto remoteView = m_world.view<RemoteComponent, FormIdComponent>();
    const auto remoteIt = std::find_if(std::begin(remoteView), std::end(remoteView), [remoteView, Id = acMessage.CasterId](auto entity) { return remoteView.get<RemoteComponent>(entity).Id == Id; });

    if (remoteIt == std::end(remoteView))
    {
        spdlog::warn("Caster with remote id {:X} not found.", acMessage.CasterId);
        return;
    }

    auto formIdComponent = remoteView.get<FormIdComponent>(*remoteIt);

    const TESForm* pForm = TESForm::GetById(formIdComponent.Id);
    Actor* pActor = Cast<Actor>(pForm);
    if (!pActor)
    {
        spdlog::warn("{}: remote caster form {:X} not loaded.", __FUNCTION__, formIdComponent.Id);
        return;
    }

    pActor->GenerateMagicCasters();

    MagicCaster* pCaster = pActor->GetMagicCaster(static_cast<MagicSystem::CastingSource>(acMessage.CastingSource));
    if (!pCaster)
    {
        spdlog::warn("{}: failed to find caster.", __FUNCTION__);
        return;
    }

    pCaster->InterruptCast();

    spdlog::debug("Interrupt remote cast successful");
}

void MagicService::OnAddTargetEvent(const AddTargetEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    // These effects are applied through spell cast sync
    if (SpellItem* pSpellItem = Cast<SpellItem>(TESForm::GetById(acEvent.SpellID)))
    {
        if ((pSpellItem->eCastingType == MagicSystem::CastingType::CONCENTRATION && !pSpellItem->IsHealingSpell()) || pSpellItem->IsWardSpell() || pSpellItem->IsInvisibilitySpell() || pSpellItem->IsBoundWeaponSpell())
        {
            return;
        }
    }

    AddTargetRequest request{};

    if (!m_world.GetModSystem().GetServerModId(acEvent.SpellID, request.SpellId.ModId, request.SpellId.BaseId))
    {
        spdlog::error("{}: could not find server ID for spell with formID {:X}, discarding", __FUNCTION__, acEvent.SpellID);
        return;
    }

    if (!m_world.GetModSystem().GetServerModId(acEvent.EffectID, request.EffectId.ModId, request.EffectId.BaseId))
    {
        spdlog::error("{}: could not find server ID for effect with formID {:X}, discarding", __FUNCTION__, acEvent.EffectID);
        return;
    }

    request.Magnitude = acEvent.Magnitude;

    // Because it takes time to create Actors, the Caster or Target may not
    // exist on the server yet, or server may not have told us yet. Have to queue to compensate.
    const entt::entity targetEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.TargetID);
    if (targetEntity == entt::null)
    {
        MagicQueue::Spdlog("{}: server entity for target formID not found, formID: {:X}, queueing", __FUNCTION__, acEvent.TargetID);
        m_queuedEffects.push(MagicAddTargetEventQueue(acEvent));
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(targetEntity);
    if (!serverIdRes.has_value())
    {
        MagicQueue::Spdlog("{}: server ID for target formID not found, formID: {:X}, queueing", __FUNCTION__, acEvent.TargetID);
        m_queuedEffects.push(MagicAddTargetEventQueue(acEvent));
        return;
    }

    request.TargetId = serverIdRes.value();

    if (acEvent.CasterID)
    {
        const entt::entity casterEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.CasterID);
        if (casterEntity == entt::null)
        {
            MagicQueue::Spdlog("{}: server entity for caster formID not found, formID: {:X}, queueing", __FUNCTION__, acEvent.CasterID);
            m_queuedEffects.push(MagicAddTargetEventQueue(acEvent));
            return;
        }

        serverIdRes = Utils::GetServerId(casterEntity);
        if (!serverIdRes.has_value())
        {
            MagicQueue::Spdlog("{}: server ID for caster formID not found, formID: {:X}, queueing", __FUNCTION__, acEvent.CasterID);
            m_queuedEffects.push(MagicAddTargetEventQueue(acEvent));
            return;
        }

        request.CasterId = serverIdRes.value();
    }

    request.IsDualCasting = acEvent.IsDualCasting;
    request.ApplyHealPerkBonus = acEvent.ApplyHealPerkBonus;
    request.ApplyStaminaPerkBonus = acEvent.ApplyStaminaPerkBonus;

    if (acEvent.TargetID == 0x14 && acEvent.SpellID != 0)
        m_activeLocalPlayerSpells.insert(acEvent.SpellID);

    m_transport.Send(request);

    spdlog::debug("Sending effect sync request");
}

void MagicService::OnNotifyAddTarget(const NotifyAddTarget& acMessage) noexcept
{
    const uint32_t cSpellId = World::Get().GetModSystem().GetGameId(acMessage.SpellId);
    if (cSpellId == 0)
    {
        spdlog::error("{}: failed to retrieve formID of server spell id, GameId base: {:X}, mod: {:X}, discarding", __FUNCTION__, acMessage.SpellId.BaseId, acMessage.SpellId.ModId);
        return;
    }

    MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(cSpellId));
    if (!pSpell)
    {
        spdlog::error("{}: failed to retrieve spell by formID {:X}, discarding", __FUNCTION__, cSpellId);
        return;
    }

    const uint32_t cEffectId = World::Get().GetModSystem().GetGameId(acMessage.EffectId);
    if (cEffectId == 0)
    {
        spdlog::error("{}: failed to retrieve formID of server effect id, GameId base: {:X}, mod: {:X}, discarding", __FUNCTION__, acMessage.EffectId.BaseId, acMessage.EffectId.ModId);
        return;
    }

    EffectItem* pEffect = pSpell->GetEffect(cEffectId);
    if (!pEffect)
    {
        spdlog::error("{}: failed to retrieve effect by formID {:X}", __FUNCTION__, cEffectId);
        return;
    }

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.TargetId);
    if (!pActor)
    {
        MagicQueue::Spdlog("{}: could not find targeted Actor for serverID {:X}, queueing", __FUNCTION__, acMessage.TargetId);
        m_queuedRemoteEffects.push(acMessage);
        return;
    }

    Actor* pCaster{};
    acMessage.CasterId && (pCaster = Utils::GetByServerId<Actor>(acMessage.CasterId));
    if (acMessage.CasterId && !pCaster)
    {
        MagicQueue::Spdlog("{}: could not find caster Actor for serverID {:X}, queueing", __FUNCTION__, acMessage.CasterId);
        m_queuedRemoteEffects.push(acMessage);
        return;
    }

    if (pCaster && pActor)
    {
        const bool isFriendlySupport = pSpell->IsHealingSpell() || pSpell->IsBuffSpell();
        const auto coopPolicy = m_world.GetRfabAdapterService().EvaluateCoopAction(pCaster, pActor, isFriendlySupport);
        if (coopPolicy.BlockHarmfulAction)
            return;
    }

    // This hack is here because slow time seems to be twice as slow when cast by an npc
    if (pEffect->IsSlowEffect())
    {
        acMessage.CasterId && (pCaster = PlayerCharacter::Get());
        spdlog::debug(
            __FUNCTION__ ": hacking IsSlowEffect() targetId {:X}, casterId {:X}, magnitude {}, IsDualCasting {}", acMessage.TargetId, acMessage.CasterId, acMessage.Magnitude,
            acMessage.IsDualCasting);
    }

    MagicTarget::AddTargetData data{};
    data.pCaster = pCaster;
    data.pSpell = pSpell;
    data.pEffectItem = pEffect;
    data.fMagnitude = acMessage.Magnitude;
    data.fUnkFloat1 = 1.0f;
    data.eCastingSource = MagicSystem::CastingSource::CASTING_SOURCE_COUNT;
    data.bDualCast = acMessage.IsDualCasting;

    if (pEffect->IsWerewolfEffect())
        pActor->GetExtension()->GraphDescriptorHash = AnimationGraphDescriptor_WerewolfBehavior::m_key;

    if (pEffect->IsVampireLordEffect())
        pActor->GetExtension()->GraphDescriptorHash = AnimationGraphDescriptor_VampireLordBehavior::m_key;

    // This hack is here because slow time seems to be twice as slow when cast by an npc
    if (pEffect->IsSlowEffect())
        pActor = PlayerCharacter::Get();

    if (!pActor)
        return;

    if (TryRefreshOrDeduplicateActiveEffect(pActor, pSpell, pEffect, acMessage.Magnitude))
        return;

    pActor->magicTarget.AddTarget(data, acMessage.ApplyHealPerkBonus, acMessage.ApplyStaminaPerkBonus);
    spdlog::debug("Applied remote magic effect");
}

void MagicService::OnRemoveSpellEvent(const RemoveSpellEvent& acEvent) noexcept
{
    if (acEvent.TargetId == 0x14)
        m_activeLocalPlayerSpells.erase(acEvent.SpellId);

    if (!m_transport.IsConnected() || ScopedSpellCastOverride::IsOverriden())
        return;

    // Never echo RemoveSpell triggered on remote puppets back across the network!
    Actor* pTargetActor = Cast<Actor>(TESForm::GetById(acEvent.TargetId));
    if (!pTargetActor || pTargetActor->GetExtension()->IsRemote())
        return;

    RemoveSpellRequest request{};

    if (!m_world.GetModSystem().GetServerModId(acEvent.SpellId, request.SpellId.ModId, request.SpellId.BaseId))
    {
        spdlog::error("{}: Could not find spell with form {:X}", __FUNCTION__, acEvent.SpellId);
        return;
    }

    const entt::entity targetEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.TargetId);
    if (targetEntity == entt::null)
    {
        spdlog::warn("Form id not found for magic remove target, form id: {:X}", acEvent.TargetId);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(targetEntity);
    if (!serverIdRes.has_value())
    {
        spdlog::warn("Server id not found for magic remove target, form id: {:X}", acEvent.TargetId);
        return;
    }

    request.TargetId = serverIdRes.value();

    //spdlog::info(__FUNCTION__ ": requesting remove spell with base id {:X} from actor with server id {:X}", request.SpellId.BaseId, request.TargetId);

    m_transport.Send(request);
}

void MagicService::OnNotifyRemoveSpell(const NotifyRemoveSpell& acMessage) noexcept
{
    uint32_t targetFormId = acMessage.TargetId;

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.TargetId);
    if (!pActor)
    {
        spdlog::warn(__FUNCTION__ ": could not find actor server id {:X}", acMessage.TargetId);
        return;
    }

    const uint32_t cSpellId = World::Get().GetModSystem().GetGameId(acMessage.SpellId);
    if (cSpellId == 0)
    {
        spdlog::error("{}: failed to retrieve spell id, GameId base: {:X}, mod: {:X}", __FUNCTION__,
                      acMessage.SpellId.BaseId, acMessage.SpellId.ModId);
        return;
    }

    MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(cSpellId));
    if (!pSpell)
    {
        spdlog::error("{}: Failed to retrieve spell by id {:X}", __FUNCTION__, cSpellId);
        return;
    }

    // Remove the spell from the actor without echoing RemoveSpellEvent back over the network
    ScopedSpellCastOverride _;
    pActor->RemoveSpell(pSpell);
}

void MagicService::ApplyQueuedEffects() noexcept
{
    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 100ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    // Search queued events
    while (!m_queuedEffects.empty())
    {
        AddTargetEvent target = m_queuedEffects.front().Target();
        Actor* pCaster = target.CasterID ? Cast<Actor>(TESForm::GetById(target.CasterID)) : nullptr;
        Actor* pTarget = target.TargetID ? Cast<Actor>(TESForm::GetById(target.TargetID)) : nullptr;
        auto pTargetName = (!pTarget || !pTarget->baseForm) ? "" : pTarget->baseForm->GetName();
        auto pCasterName = (!pCaster || !pCaster->baseForm) ? "" : pCaster->baseForm->GetName();

        // Check for and skip expired (timed out) events, that Actor isn't likely to exist anymore.
        if (m_queuedEffects.front().Expired())
            MagicQueue::Spdlog("{}: removing expired AddTargetEvent from queue: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                               __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
        else
        {
            const entt::entity entity = m_world.GetCharacterService().FindActorEntityByFormId(target.TargetID);
            if (entity == entt::null)
            {
                spdlog::debug("{}: server entity for AddTargetEvent target formID still not found: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                              __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
                break;
            }

            std::optional<uint32_t> serverIdRes = Utils::GetServerId(entity);
            if (!serverIdRes.has_value())
            {
                spdlog::debug("{}: serverID for AddTargetEvent target formID still not found: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                              __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
                break;
            }

            if (target.CasterID)
            {
                const entt::entity casterEntity = m_world.GetCharacterService().FindActorEntityByFormId(target.CasterID);
                if (casterEntity == entt::null)
                {
                    spdlog::debug("{}: serverID for AddTargetEvent caster formID still not found: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                                  __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
                    break;
                }

                serverIdRes = Utils::GetServerId(casterEntity);
                if (!serverIdRes.has_value())
                {
                    spdlog::debug("{}: serverID for AddTargetEvent caster formID still not found: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                                  __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
                    break;
                }
            }

            // At this point, it will succeed or fail, but not queue another one ad infinitum
            MagicQueue::Spdlog("{}: retrying AddTargetEvent for caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})", 
                               __FUNCTION__, pCasterName, target.CasterID, target.SpellID, target.EffectID, pTargetName, target.TargetID);
            OnAddTargetEvent(target);        
        }
        m_queuedEffects.pop();
    }

    // Same again for remote events
    while (!m_queuedRemoteEffects.empty())
    {
        NotifyAddTarget target = m_queuedRemoteEffects.front().Target();
        Actor* pTarget = Utils::GetByServerId<Actor>(target.TargetId); 
        Actor* pCaster = Utils::GetByServerId<Actor>(target.CasterId); 
        auto pTargetName = (!pTarget || !pTarget->baseForm) ? "" : pTarget->baseForm->GetName();
        auto pCasterName = (!pCaster || !pCaster->baseForm) ? "" : pCaster->baseForm->GetName(); 

        if (m_queuedRemoteEffects.front().Expired())
            MagicQueue::Spdlog("{}: removing expired NotifyAddTarget event from queue: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                               __FUNCTION__, pCasterName, target.CasterId, target.SpellId, target.EffectId, pTargetName, target.TargetId);
        else
        {
            if (!pTarget)
            {
                spdlog::debug("{}: Actor for target serverID still not found for NotifyAddTarget: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                              __FUNCTION__, pCasterName, target.CasterId, target.SpellId, target.EffectId, pTargetName, target.TargetId);
                break;
            }

            if (target.CasterId && !pCaster)
            {
                spdlog::debug("{}: Actor for caster serverID still not found for NotifyAddTarget: caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                              __FUNCTION__, pCasterName, target.CasterId, target.SpellId, target.EffectId, pTargetName, target.TargetId);
                break; 
            }

            MagicQueue::Spdlog("{}: retrying NotifyAddTarget for caster {}({:X}), spell {:X}, effect {:X}, target {}({:X})",
                               __FUNCTION__, pCasterName, target.CasterId, target.SpellId, target.EffectId, pTargetName,target.TargetId);
            OnNotifyAddTarget(target);
        }
        m_queuedRemoteEffects.pop();
    }
}

void MagicService::StartRevealingOtherPlayers() noexcept
{
    UpdateRevealOtherPlayersEffect(/*forceTrigger=*/true);
}

void MagicService::UpdateRevealOtherPlayersEffect(bool aForceTrigger) noexcept
{
    constexpr auto cRevealDuration = 10s;
    constexpr auto cDelayBetweenUpdates = 2s;

    // Effect's activation and lifecycle

    static std::chrono::steady_clock::time_point revealStartTimePoint;
    static std::chrono::steady_clock::time_point lastSendTimePoint;

    const bool shouldActivate = aForceTrigger || GetAsyncKeyState(VK_F4) & 0x01;

    if (shouldActivate && !m_revealingOtherPlayers)
    {
        m_revealingOtherPlayers = true;
        revealStartTimePoint = std::chrono::steady_clock::now();
    }

    if (!m_revealingOtherPlayers)
        return;

    const auto now = std::chrono::steady_clock::now();

    if (now - revealStartTimePoint > cRevealDuration)
    {
        m_revealingOtherPlayers = false;
        return;
    }

    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    // When active

    Mod* pSkyrimTogether = ModManager::Get()->GetByName("SkyrimTogether.esp");
    if (!pSkyrimTogether)
        return;

    MagicItem* pSpell = Cast<MagicItem>(TESForm::GetById(pSkyrimTogether->GetFormId(0x1825)));

    if (!pSpell)
        return;

    MagicTarget::AddTargetData data{};
    data.pSpell = pSpell;
    data.pEffectItem = pSpell->GetEffect(pSkyrimTogether->GetFormId(0x1824));
    if (!data.pEffectItem)
        return;
    data.fMagnitude = 1.f;
    data.fUnkFloat1 = 1.f;
    data.eCastingSource = MagicSystem::CastingSource::CASTING_SOURCE_COUNT;

    auto view = World::Get().view<FormIdComponent, PlayerComponent>();
    for (const auto entity : view)
    {
        auto& formIdComponent = view.get<FormIdComponent>(entity);
        if (formIdComponent.Id == 0x14)
            continue;

        auto* pRemotePlayer = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pRemotePlayer)
            continue;

        pRemotePlayer->magicTarget.AddTarget(data, false, false);
    }
}

void MagicService::PollLocalPlayerActiveBuffs(double aDeltaSec) noexcept
{
    m_buffPollAccumulatorSec += aDeltaSec;
    if (m_buffPollAccumulatorSec < 0.5)
        return;
    m_buffPollAccumulatorSec = 0.0;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->currentProcess || !pPlayer->currentProcess->middleProcess ||
        !pPlayer->currentProcess->middleProcess->ActiveEffects)
    {
        return;
    }

    TiltedPhoques::Vector<uint32_t> endedSpells{};
    for (uint32_t spellFormId : m_activeLocalPlayerSpells)
    {
        if (!HasActiveSpellOnActor(pPlayer, spellFormId))
            endedSpells.push_back(spellFormId);
    }

    for (uint32_t spellFormId : endedSpells)
    {
        m_activeLocalPlayerSpells.erase(spellFormId);
        RemoveSpellEvent removalEvent{};
        removalEvent.TargetId = 0x14;
        removalEvent.SpellId = spellFormId;
        OnRemoveSpellEvent(removalEvent);
    }
}

void MagicService::ResyncLocalPlayerActiveBuffs() noexcept
{
    if (!m_transport.IsConnected())
        return;

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->currentProcess || !pPlayer->currentProcess->middleProcess ||
        !pPlayer->currentProcess->middleProcess->ActiveEffects)
    {
        return;
    }

    for (ActiveEffect* pActive : *pPlayer->currentProcess->middleProcess->ActiveEffects)
    {
        if (!pActive || !pActive->pSpell || !pActive->pEffect)
            continue;
        if ((pActive->uiFlags & kActiveEffectDispelledFlag) != 0)
            continue;
        if (pActive->fDuration > 0.0f && (pActive->fDuration - pActive->fElapsedSeconds) < 3.0f)
            continue;

        auto* pEffectItem = static_cast<EffectItem*>(pActive->pEffect);
        if (!pEffectItem->pEffectSetting)
            continue;

        if (!m_activeLocalPlayerSpells.contains(pActive->pSpell->formID))
            continue;

        MagicTarget::AddTargetData checkData{};
        checkData.pCaster = pPlayer;
        checkData.pSpell = pActive->pSpell;
        checkData.pEffectItem = pEffectItem;
        if (!checkData.ShouldSync())
            continue;

        AddTargetEvent resyncEvt{};
        resyncEvt.TargetID = 0x14;
        resyncEvt.CasterID = 0x14;
        resyncEvt.SpellID = pActive->pSpell->formID;
        resyncEvt.EffectID = pEffectItem->pEffectSetting->formID;
        resyncEvt.Magnitude = pActive->fMagnitude;
        resyncEvt.IsDualCasting = false;
        OnAddTargetEvent(resyncEvt);
    }
}

void MagicService::OnCellChangeEvent(const CellChangeEvent&) noexcept
{
    ResyncLocalPlayerActiveBuffs();
}

void MagicService::OnPartyJoinedEvent(const PartyJoinedEvent&) noexcept
{
    ResyncLocalPlayerActiveBuffs();
}

