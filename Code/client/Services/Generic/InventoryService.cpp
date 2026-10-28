#include <Services/InventoryService.h>

#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestEquipmentChanges.h>
#include <Messages/NotifyEquipmentChanges.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/NotifyDrawWeapon.h>

#include <Events/UpdateEvent.h>
#include <Events/InventoryChangeEvent.h>
#include <Events/EquipmentChangeEvent.h>

#include <World.h>
#include <Games/Skyrim/Interface/UI.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>
#include <Actor.h>
#include <Structs/ObjectData.h>
#include <Forms/TESWorldSpace.h>
#include <Games/TES.h>
#include <Games/Overrides.h>
#include <EquipManager.h>
#include <Games/ActorExtension.h>
#include <Forms/TESNPC.h>
#include <Forms/BGSOutfit.h>
#include <Forms/TESObjectARMO.h>
#include <Forms/TESObjectWEAP.h>
#include <DefaultObjectManager.h>
#include <Services/RfabAdapterService.h>

InventoryService::InventoryService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
    , m_transport(aTransport)
{
    m_updateConnection = m_dispatcher.sink<UpdateEvent>().connect<&InventoryService::OnUpdate>(this);
    m_inventoryConnection = m_dispatcher.sink<InventoryChangeEvent>().connect<&InventoryService::OnInventoryChangeEvent>(this);
    m_equipmentConnection = m_dispatcher.sink<EquipmentChangeEvent>().connect<&InventoryService::OnEquipmentChangeEvent>(this);
    m_inventoryChangeConnection = m_dispatcher.sink<NotifyInventoryChanges>().connect<&InventoryService::OnNotifyInventoryChanges>(this);
    m_equipmentChangeConnection = m_dispatcher.sink<NotifyEquipmentChanges>().connect<&InventoryService::OnNotifyEquipmentChanges>(this);
    m_drawWeaponChangeConnection = m_dispatcher.sink<NotifyDrawWeapon>().connect<&InventoryService::OnNotifyDrawWeapon>(this);
}

void InventoryService::OnUpdate(const UpdateEvent& acUpdateEvent) noexcept
{
    RunWeaponStateUpdates();
    RunNakedNPCBugChecks();
}

void InventoryService::OnInventoryChangeEvent(const InventoryChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    if (Actor* pTargetActor = Cast<Actor>(TESForm::GetById(acEvent.FormId)))
    {
        if (const auto* pExt = pTargetActor->GetExtension())
        {
            if (pExt->IsRemotePlayer())
                return;

            if (pExt->IsRemote() && !pTargetActor->IsDead())
            {
                auto* pUi = UI::Get();
                if (!pUi || !pUi->GetMenuOpen(BSFixedString("ContainerMenu")))
                    return;
            }
        }
    }

    const entt::entity targetEntity = m_world.GetCharacterService().FindAnyEntityByFormId(acEvent.FormId);
    if (targetEntity == entt::null)
        return;

    const bool isLeader = m_world.GetPartyService().IsLeader(); // Helps distinguish in 2-party logs
    if (auto* pWaitingAssign = m_world.try_get<WaitingForAssignmentComponent>(targetEntity))
    {
        pWaitingAssign->DirtyInventory = true;
        spdlog::debug(
            __FUNCTION__ ": WaitingForAssignment, flagged DirtyInventory actorId: {:X}, isLeader: {}, item id: {:X}",
            acEvent.FormId, isLeader, acEvent.Item.BaseId.LogFormat());
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(targetEntity);
    if (!serverIdRes.has_value())
    {
        spdlog::debug(
            __FUNCTION__ ": skipping inventory change for unassigned form id: {:X}, isLeader: {}, item id: {:X}, count: {}", acEvent.FormId,
            isLeader, acEvent.Item.BaseId.LogFormat(), acEvent.Item.Count);
        return;
    }

    RequestInventoryChanges request;
    request.ServerId = serverIdRes.value();
    request.Item = acEvent.Item;
    request.Drop = acEvent.Drop;
    request.UpdateClients = acEvent.UpdateClients;

    m_transport.Send(request);

    spdlog::info(__FUNCTION__ ": sending item request, item: {:X}, count: {}, target object: {:X}", acEvent.Item.BaseId.LogFormat(), acEvent.Item.Count, acEvent.FormId);
}

void InventoryService::OnEquipmentChangeEvent(const EquipmentChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    const entt::entity actorEntity = m_world.GetCharacterService().FindActorEntityByFormId(acEvent.ActorId);
    if (actorEntity == entt::null)
        return;

    const bool isLeader = m_world.GetPartyService().IsLeader(); // Helps distinguish in 2-party logs
    Actor* pActor = Cast<Actor>(TESForm::GetById(acEvent.ActorId));
    if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    const auto* pActorExt = pActor->GetExtension();
    if (!pActorExt || pActorExt->IsRemote())
        return;

    const char* pActorBaseName = pActor->baseForm ? pActor->baseForm->GetName() : "<null>";

    if (auto* pWaitingAssign = m_world.try_get<WaitingForAssignmentComponent>(actorEntity))
    {
        pWaitingAssign->DirtyInventory = true;
        spdlog::debug(
            __FUNCTION__ ": WaitingForAssignment, flagged DirtyInventory actorId: {:X}, isLeader: {}, "
                         "item id: {:X}, isAmmo: {}, unequip: {}, slot: {:X}, name: {}",
            acEvent.ActorId, isLeader, acEvent.ItemId, acEvent.IsAmmo, acEvent.Unequip, acEvent.EquipSlotId,
            pActorBaseName);
        return;
    }

    std::optional<uint32_t> serverIdRes = Utils::GetServerId(actorEntity);
    if (!serverIdRes.has_value())
    {
        spdlog::debug(
            __FUNCTION__ ": skipping equipment change on unassigned actorId: {:X}, isLeader: {}, item id: {:X}, isAmmo: {}, unequip: {}, slot: {:X}, name: {}",
            acEvent.ActorId, isLeader, acEvent.ItemId, acEvent.IsAmmo, acEvent.Unequip, acEvent.EquipSlotId, pActorBaseName);
        return;
    }

    auto& modSystem = World::Get().GetModSystem();

    RequestEquipmentChanges request;
    request.ServerId = serverIdRes.value();

    if (!modSystem.GetServerModId(acEvent.EquipSlotId, request.EquipSlotId))
        return;
    if (!modSystem.GetServerModId(acEvent.ItemId, request.ItemId))
        return;

    request.Count = acEvent.Count;
    request.Unequip = acEvent.Unequip;
    request.IsSpell = acEvent.IsSpell;
    request.IsShout = acEvent.IsShout;
    request.IsAmmo = acEvent.IsAmmo;
    request.CurrentInventory = pActor->GetEquipment();

    m_transport.Send(request);

    spdlog::info(
        __FUNCTION__ ": sending equipment change, actorId: {:X}, serverId {:X}, isLeader: {}, item: {:X}, count: {}, name: {}",
        acEvent.ActorId, request.ServerId, isLeader, acEvent.ItemId, acEvent.Count, pActorBaseName);
}

void InventoryService::OnNotifyInventoryChanges(const NotifyInventoryChanges& acMessage) noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedInventory(acMessage.ServerId, acMessage.Item, acMessage.Drop);

    if (acMessage.Drop)
    {
        Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
        if (!pActor || !pActor->GetNiNode() || pActor->IsStillLoading() || pActor->IsDeleted())
            return;

        ScopedInventoryOverride _;

        pActor->DropOrPickUpObject(acMessage.Item, nullptr, nullptr);
    }
    else
    {
        TESObjectREFR* pObject = Utils::GetByServerId<TESObjectREFR>(acMessage.ServerId);
        if (!pObject || pObject->IsStillLoading() || pObject->IsDeleted())
            return;

        if (Actor* pActor = Cast<Actor>(pObject); pActor && !pActor->GetNiNode())
            return;

        ScopedInventoryOverride _;

        pObject->AddOrRemoveItem(acMessage.Item);
    }
}

void InventoryService::OnNotifyEquipmentChanges(const NotifyEquipmentChanges& acMessage) noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedEquipment(acMessage);

    Actor* pActor = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pActor || !pActor->GetNiNode() || pActor->IsStillLoading() || pActor->IsDeleted())
        return;

    auto& modSystem = World::Get().GetModSystem();

    uint32_t itemId = modSystem.GetGameId(acMessage.ItemId);
    TESForm* pItem = TESForm::GetById(itemId);

    if (!pItem)
    {
        spdlog::error("Could not find inventory item {:X}:{:X}", acMessage.ItemId.ModId, acMessage.ItemId.BaseId);
        return;
    }

    uint32_t equipSlotId = modSystem.GetGameId(acMessage.EquipSlotId);
    TESForm* pEquipSlot = TESForm::GetById(equipSlotId);

    uint32_t slotId = 0;
    if (pEquipSlot == DefaultObjectManager::Get().rightEquipSlot)
        slotId = 1;

    auto* pEquipManager = EquipManager::Get();
    ScopedEquipOverride equipGuard;
    ScopedInventoryOverride invGuard;

    if (acMessage.IsSpell)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipSpell(pActor, pItem, slotId);
        else
            pEquipManager->EquipSpell(pActor, pItem, slotId);

        return;
    }
    else if (acMessage.IsShout)
    {
        if (acMessage.Unequip)
            pEquipManager->UnEquipShout(pActor, pItem);
        else
            pEquipManager->EquipShout(pActor, pItem);

        return;
    }

    // Never equip/unequip RFAB's self-destructing Adventurer's Ring (RFAB_ExperienceRing = local 0x112812 in RFAB.esp,
    // e.g. 0x07112812 in RFAB Respect Edition or 0x06112812 in classic RFAB) or dual-ring items on remote player puppets
    // (RFAB.dll's 2-ring patch applies strictly to 0x14!).
    const auto* pActorExt = pActor->GetExtension();
    if (pActorExt && pActorExt->IsRemotePlayer())
    {
        const auto& rfabAdapter = m_world.GetRfabAdapterService();
        const auto& bindings = rfabAdapter.GetBindings();
        if (TESForm* pAdvRing = rfabAdapter.ResolveFormByEditorIdOrBaseId(
                bindings.AdventurerRingEditorId, bindings.FallbackAdventurerRingBaseFormId))
        {
            if (itemId == pAdvRing->formID)
                return;
        }
        else if ((itemId & 0xFF000000u) != 0xFE000000u &&
                 (itemId & 0x00FFFFFFu) == bindings.FallbackAdventurerRingBaseFormId)
        {
            return;
        }
    }

    if (acMessage.Unequip)
    {
        pEquipManager->UnEquip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false, nullptr);
    }
    else
    {
        if (pItem->formType == FormType::Armor)
        {
            const auto* pIncomingArmor = Cast<TESObjectARMO>(pItem);
            const uint32_t incomingSlotMask = pIncomingArmor ? pIncomingArmor->slotType : 0u;
            constexpr uint32_t kBipedRingSlotMask = (1u << 6); // Biped slot 36 (Ring)

            // On remote players, never displace worn rings (since RFAB 0x14 wears 2 rings while 0xFF puppets have 1 slot).
            if (pActorExt && pActorExt->IsRemotePlayer() && (incomingSlotMask & kBipedRingSlotMask) != 0)
                return;

            // Only unequip existing worn armor pieces whose Biped slot mask overlaps incomingSlotMask,
            // instead of unequipping every worn item (which triggered OnUnequipped break scripts!).
            if (incomingSlotMask != 0)
            {
                const Inventory wornArmor = pActor->GetWornArmor();
                for (const auto& armor : wornArmor.Entries)
                {
                    const uint32_t armorId = modSystem.GetGameId(armor.BaseId);
                    if (armorId == itemId)
                        continue;

                    TESForm* pArmorForm = TESForm::GetById(armorId);
                    const auto* pWornArmor = Cast<TESObjectARMO>(pArmorForm);
                    if (pArmorForm && pWornArmor && (pWornArmor->slotType & incomingSlotMask) != 0)
                        pEquipManager->UnEquip(pActor, pArmorForm, nullptr, 1, pEquipSlot, false, true, false, false, nullptr);
                }
            }
        }

        pEquipManager->Equip(pActor, pItem, nullptr, acMessage.Count, pEquipSlot, false, true, false, false);
    }
}

void InventoryService::OnNotifyDrawWeapon(const NotifyDrawWeapon& acMessage) noexcept
{
    m_world.GetCharacterService().SyncRemoteCachedWeaponDraw(acMessage.Id, acMessage.IsWeaponDrawn);

    if (Actor* pActor = Utils::GetByServerId<Actor>(acMessage.Id))
    {
        if (!pActor->GetNiNode() || pActor->IsStillLoading() || pActor->IsDeleted())
            return;

        if (const auto* pExt = pActor->GetExtension(); pExt && pExt->IsRemote() && !pActor->IsDead())
            pActor->SetWeaponDrawnEx(acMessage.IsWeaponDrawn);
    }
}

void InventoryService::RunWeaponStateUpdates() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 500ms;

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent, LocalComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* const pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted() || !pActor->GetExtension())
            continue;

        auto& localComponent = view.get<LocalComponent>(entity);

        bool isWeaponDrawn = pActor->actorState.IsWeaponDrawn();
        if (isWeaponDrawn != localComponent.IsWeaponDrawn)
        {
            localComponent.IsWeaponDrawn = isWeaponDrawn;

            DrawWeaponRequest request;
            request.Id = localComponent.Id;
            request.IsWeaponDrawn = isWeaponDrawn;

            m_transport.Send(request);
        }
    }
}

void InventoryService::RunNakedNPCBugChecks() noexcept
{
    if (!m_transport.IsConnected())
        return;

    static std::chrono::steady_clock::time_point lastSendTimePoint;
    constexpr auto cDelayBetweenUpdates = 1000ms;
    constexpr auto cDelayAfterAssignment = 3s;
    constexpr auto cNoDeadline = std::chrono::steady_clock::time_point{};

    const auto now = std::chrono::steady_clock::now();
    if (now - lastSendTimePoint < cDelayBetweenUpdates)
        return;

    lastSendTimePoint = now;

    auto view = m_world.view<FormIdComponent>();

    for (auto entity : view)
    {
        const auto& formIdComponent = view.get<FormIdComponent>(entity);
        Actor* pActor = Cast<Actor>(TESForm::GetById(formIdComponent.Id));
        if (!pActor || pActor->IsStillLoading() || pActor->IsDeleted())
            continue;

        auto* pActorExt = pActor->GetExtension();
        if (!pActorExt || pActorExt->IsPlayer())
            continue;

        if (pActorExt->nakedDeadline == cNoDeadline)
            continue;

        const char* pBaseName = pActor->baseForm ? pActor->baseForm->GetName() : "<null>";

        if (pActor->IsDead() || !pActor->ShouldWearBodyPiece())
        {
            pActorExt->nakedDeadline = cNoDeadline;
            spdlog::debug(__FUNCTION__ ": actorId {:X} naked check is irrelevant {}", pActor->formID, pBaseName);
            continue; 
        }

        // NEVER re-dress an actor that is fighting or already holding a weapon.
        // `EquipOutfit()` equips the *whole* default outfit, and Skyrim outfits may contain weapons: on RFAB
        // bosses whose weapon is granted by script (not by the base outfit) this replaced the real main-hand
        // weapon with the template one, so the NPC swung a "non-primary" weapon right after a takeover/reload.
        // The naked check exists purely for appearance after a respawn/re-materialisation, so a fighting or
        // armed actor must be left alone.
        const bool holdsRealWeapon =
            Cast<TESObjectWEAP>(pActor->GetEquippedWeapon(0)) != nullptr ||
            Cast<TESObjectWEAP>(pActor->GetEquippedWeapon(1)) != nullptr;
        if ((pActor->IsInCombat() || holdsRealWeapon) && pActor->IsWearingBodyPiece())
        {
            pActorExt->nakedDeadline = cNoDeadline;
            spdlog::debug(
                __FUNCTION__ ": actorId {:X} naked check skipped (dressed and in combat or armed) {}",
                pActor->formID,
                pBaseName);
            continue;
        }

        if (now < pActorExt->nakedDeadline + cDelayAfterAssignment)
        {
            spdlog::debug(__FUNCTION__ ": actorId {:X} with naked check deadline {}", pActor->formID, pBaseName);
            continue; 
        }

        else
        {
            spdlog::debug(__FUNCTION__ ": actorId {:X} naked check deadline expires {}", pActor->formID, pBaseName);
            if (m_world.try_get<WaitingForAssignmentComponent>(entity))
            {
                spdlog::debug(__FUNCTION__ ": but still WaitingForAssignment", pActor->formID, pBaseName);
                pActor->GetExtension()->SetNakedDeadline();
                continue;
            }

            pActor->GetExtension()->nakedDeadline = cNoDeadline;   
        }

        if (pActor->GetExtension()->IsPlayer() || pActor->IsDead())
        {
            spdlog::debug(__FUNCTION__ ": actorId {:X} naked check skipped (player or dead) {}", pActor->formID, pBaseName);
            continue;
        }

        if (!pActor->GetNiNode())
        {
            // Keep deadline armed until 3D actually finishes streaming in on slow cells.
            pActor->GetExtension()->SetNakedDeadline();
            continue;
        }

        // Dress both local and remote humanoid NPCs whose body outfit was stripped during initial sync,
        // while preserving any scripted main-hand / off-hand weapons already held by RFAB bosses/bandits.
        TESNPC* pBase = Cast<TESNPC>(pActor->baseForm);
        if (!pActor->IsWearingBodyPiece() && pBase && pBase->outfits[0] != nullptr)
        {
            spdlog::debug(__FUNCTION__ ": actorId {:X} naked check fires {}", pActor->formID, pBaseName);
            TESForm* const pSavedRightWeap = Cast<TESObjectWEAP>(pActor->GetEquippedWeapon(0));
            TESForm* const pSavedLeftWeap = Cast<TESObjectWEAP>(pActor->GetEquippedWeapon(1));
            ScopedEquipOverride equipGuard;
            ScopedInventoryOverride invGuard;
            pActor->EquipOutfit();
            if (EquipManager* pEquipMgr = EquipManager::Get())
            {
                if (pSavedRightWeap && pActor->GetEquippedWeapon(0) != pSavedRightWeap)
                    pEquipMgr->Equip(pActor, pSavedRightWeap, nullptr, 1, DefaultEquipSlot::GetRightHandSlot(), false, true, false, false);
                if (pSavedLeftWeap && pActor->GetEquippedWeapon(1) != pSavedLeftWeap)
                    pEquipMgr->Equip(pActor, pSavedLeftWeap, nullptr, 1, DefaultEquipSlot::GetLeftHandSlot(), false, true, false, false);
            }
        }
    }
}
