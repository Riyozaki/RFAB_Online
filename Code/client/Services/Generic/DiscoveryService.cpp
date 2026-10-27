#include <TiltedOnlinePCH.h>

#include <Services/DiscoveryService.h>
#include <Services/CharacterService.h>
#include <Services/CoordinatedSaveService.h>
#include <Games/TES.h>

#include <Games/References.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <Forms/TESNPC.h>

#include <Events/ActorAddedEvent.h>
#include <Events/ActorRemovedEvent.h>
#include <Events/PreUpdateEvent.h>
#include <Events/GridCellChangeEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/LocationChangeEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/ConnectionErrorEvent.h>

#include <World.h>
#include <Components.h>

DiscoveryService::DiscoveryService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_dispatcher(aDispatcher)
{
    m_preUpdateConnection = m_dispatcher.sink<PreUpdateEvent>().connect<&DiscoveryService::OnUpdate>(this);
    m_connectedConnection = m_dispatcher.sink<ConnectedEvent>().connect<&DiscoveryService::OnConnected>(this);

    EventDispatcherManager::Get()->loadGameEvent.RegisterSink(this);
}

void DiscoveryService::VisitCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    if (pPlayer->GetWorldSpace())
        VisitExteriorCell(aForceTrigger);
    else if (pPlayer->GetParentCell())
        VisitInteriorCell(aForceTrigger);

    // exactly how the game does it too
    if (m_pLocation != pPlayer->locationForm)
    {
        m_dispatcher.trigger(LocationChangeEvent());
        m_pLocation = pPlayer->locationForm;
    }
}

void DiscoveryService::VisitExteriorCell(bool aForceTrigger) noexcept
{
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    const auto pWorldSpace = pPlayer->GetWorldSpace();

    m_interiorCellId = 0;

    const TES* pTES = TES::Get();
    const uint32_t worldSpaceId = pWorldSpace->formID;
    const GridCellCoords gameCurrentGrid(pTES->currentGridX, pTES->currentGridY);
    const GridCellCoords gameCenterGrid(pTES->centerGridX, pTES->centerGridY);

    if (m_worldSpaceId != worldSpaceId || aForceTrigger)
    {
        DetectGridCellChange(pWorldSpace, true);
        // If the world space changes, then we want to send out a CellChangeEvent out too.
        aForceTrigger = true;
    }
    else if (gameCenterGrid != m_centerGrid)
    {
        DetectGridCellChange(pWorldSpace, false);
    }

    if (gameCurrentGrid != m_currentGrid || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(pWorldSpace->formID, cellChangeEvent.WorldSpaceId))
        {
            spdlog::error("Failed to find world space id for form id {:X}", pWorldSpace->formID);
            return;
        }

        TESObjectCELL* pCell = pPlayer->GetParentCellEx();
        if (!pCell)
            pCell = ModManager::Get()->GetCellFromCoordinates(gameCurrentGrid.X, gameCurrentGrid.Y, pWorldSpace, false);

        if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id for form id {:X}", pCell ? pCell->formID : 0u);
            return;
        }

        cellChangeEvent.CurrentCoords = gameCurrentGrid;

        m_dispatcher.trigger(cellChangeEvent);

        m_currentGrid = gameCurrentGrid;
    }
}

void DiscoveryService::VisitInteriorCell(bool aForceTrigger) noexcept
{
    ResetCachedCellData();

    const uint32_t cellId = PlayerCharacter::Get()->GetParentCell()->formID;
    if (m_interiorCellId != cellId || aForceTrigger)
    {
        CellChangeEvent cellChangeEvent{};

        if (!m_world.GetModSystem().GetServerModId(cellId, cellChangeEvent.CellId))
        {
            spdlog::error("Failed to find cell id {:X}", cellId);
            return;
        }

        m_dispatcher.trigger(cellChangeEvent);
        m_interiorCellId = cellId;
    }
}

void DiscoveryService::DetectGridCellChange(TESWorldSpace* aWorldSpace, bool aNewCellGrid) noexcept
{
    GridCellChangeEvent changeEvent{};

    const uint32_t worldSpaceId = aWorldSpace->formID;
    changeEvent.WorldSpaceId = worldSpaceId;

    const TES* pTES = TES::Get();
    const int32_t startGridX = pTES->centerGridX - GridCellCoords::m_gridsToLoad / 2;
    const int32_t startGridY = pTES->centerGridY - GridCellCoords::m_gridsToLoad / 2;

    for (int32_t i = 0; i < GridCellCoords::m_gridsToLoad; ++i)
    {
        for (int32_t j = 0; j < GridCellCoords::m_gridsToLoad; ++j)
        {
            // If it is a new cell grid, don't check for previously loaded cells.
            if (!aNewCellGrid)
            {
                if (GridCellCoords::IsCellInGridCell(m_centerGrid, {startGridX + i, startGridY + j}, false))
                    continue;
            }

            const TESObjectCELL* pCell = ModManager::Get()->GetCellFromCoordinates(startGridX + i, startGridY + j, aWorldSpace, 0);

            if (!pCell)
            {
                spdlog::warn("Cell not found at coordinates ({}, {}) in worldspace {:X}", startGridX + i, startGridY + j, aWorldSpace->formID);
                continue;
            }

            GameId cellId{};
            if (!m_world.GetModSystem().GetServerModId(pCell->formID, cellId))
            {
                spdlog::error("Failed to find cell id for form id {:X}", pCell->formID);
                continue;
            }

            changeEvent.Cells.push_back(cellId);
        }
    }

    TESObjectCELL* pCell = PlayerCharacter::Get()->GetParentCellEx();
    if (!pCell)
        pCell = ModManager::Get()->GetCellFromCoordinates(pTES->currentGridX, pTES->currentGridY, aWorldSpace, false);

    if (!pCell || !m_world.GetModSystem().GetServerModId(pCell->formID, changeEvent.PlayerCell))
    {
        spdlog::error("Failed to find cell id for form id {:X}", pCell ? pCell->formID : 0u);
        return;
    }

    changeEvent.CenterCoords = m_centerGrid = {pTES->centerGridX, pTES->centerGridY};

    m_dispatcher.trigger(changeEvent);

    m_worldSpaceId = worldSpaceId;
}

void DiscoveryService::VisitForms() noexcept
{
    static Set<uint32_t> s_previousForms;
    s_previousForms = m_forms;

    const auto visitor = [this](TESObjectREFR* apReference)
    {
        const auto formId = apReference->formID;

        if (!m_forms.count(formId))
        {
            m_forms.insert(formId);

            m_dispatcher.enqueue(ActorAddedEvent(formId));
        }
        else
            s_previousForms.erase(formId);
    };

    ProcessLists* const pProcessLists = ProcessLists::Get();
    if (!pProcessLists)
        return;

    for (uint32_t i = 0; i < pProcessLists->highActorHandleArray.length; ++i)
    {
        GamePtr<TESObjectREFR> spRefr = TESObjectREFR::LookupByHandle(pProcessLists->highActorHandleArray[i]);
        TESObjectREFR* const pRefr = spRefr.Get();
        if (!pRefr)
            continue;

        if (pRefr->GetNiNode())
        {
            m_niNodeFlickerGraceFrames.erase(pRefr->formID);
            visitor(pRefr);
        }
        else if (m_forms.count(pRefr->formID) != 0 &&
                 !pRefr->IsDisabled() &&
                 !pRefr->IsDeleted() &&
                 pRefr->parentCell &&
                 pRefr->parentCell->loadedCellData != nullptr)
        {
            // Keep already-registered high-process actors alive across short (<15-frame) 3D rebuilds
            // (QueueNiNodeUpdate, helmet/armor equip, vampire/werewolf transform, exterior grid seam).
            constexpr uint8_t kMaxNiNodeFlickerFrames = 15;
            uint8_t& missingFrames = m_niNodeFlickerGraceFrames[pRefr->formID];
            if (missingFrames < kMaxNiNodeFlickerFrames)
            {
                ++missingFrames;
                s_previousForms.erase(pRefr->formID);
            }
        }
    }

    // Not in actor holder
    if (PlayerCharacter* pPlayer = PlayerCharacter::Get())
        visitor(pPlayer);

    // We dispatch removal events first to prevent needless reallocations
    for (uint32_t formId : s_previousForms)
    {
        m_niNodeFlickerGraceFrames.erase(formId);
        m_dispatcher.trigger(ActorRemovedEvent(formId));
        m_forms.erase(formId);
    }

    // Dispatch all adds
    m_dispatcher.update<ActorAddedEvent>();
}

void DiscoveryService::ResetLoadedForms() noexcept
{
    m_forms.clear();
    m_niNodeFlickerGraceFrames.clear();
    m_interiorCellId = 0;
    m_pLocation = nullptr;
    ResetCachedCellData();
}

void DiscoveryService::ReconcileMissingWorldEntities() noexcept
{
    if (!m_forms.empty())
    {
        Set<uint32_t> liveEntityFormIds;
        auto actorView = m_world.view<FormIdComponent>(entt::exclude<ObjectComponent>);
        for (entt::entity entity : actorView)
            liveEntityFormIds.insert(actorView.get<FormIdComponent>(entity).Id);

        for (auto it = m_forms.begin(); it != m_forms.end();)
        {
            if (!liveEntityFormIds.count(*it))
                it = m_forms.erase(it);
            else
                ++it;
        }
    }

    VisitForms();
}

void DiscoveryService::OnUpdate(const PreUpdateEvent& acUpdateEvent) noexcept
{
    TP_UNUSED(acUpdateEvent);

    if (m_world.GetCharacterService().IsEngineWorldLoading())
        return;

    VisitCell();

    // Phase 3 (`WorldReloadBarrier`): when a non-leader Guest finishes `.ess` loading before Host,
    // keep updating `VisitCell()` (so cell topology stays accurate) while holding NPC `VisitForms()`
    // until Host releases the reload barrier (or the safety timeout expires), guaranteeing Host claims
    // canonical `Sticky Host Cell Authority` without double-transfer churn.
    if (m_world.GetCoordinatedSaveService().IsHoldingWorldDiscoveryForHostBarrier())
        return;

    VisitForms();
}

void DiscoveryService::OnConnected(const ConnectedEvent& acEvent) noexcept
{
    // uGridsToLoad should always be 5, as this is what the server enforces
    auto* pSetting = INISettingCollection::Get()->GetSetting("uGridsToLoad:General");
    if (pSetting && pSetting->data != 5)
    {
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"bad_uGridsToLoad\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }

    // Ensure 0x14 (local PlayerCharacter) and all currently loaded actors have live EnTT entities
    // even if a save load previously detached all actor entities via DetachAllActorsForEngineLoad().
    ReconcileMissingWorldEntities();
    VisitCell(true);
}

BSTEventResult DiscoveryService::OnEvent(const TESLoadGameEvent*, const EventDispatcher<TESLoadGameEvent>*)
{
    spdlog::info("Finished loading, triggering visit cell and fresh actor discovery");

    // RFAB Online: RFAB runs hundreds of Requiem/SKSE plugins by design.
    // Only warn if SkyrimTogether.esp itself was not enabled in MO2's load order;
    // never spam the upstream vanilla-only "non_default_install" popup on save load.
    Mod* pSkyrimTogether = ModManager::Get()->GetByName("SkyrimTogether.esp");
    if (!pSkyrimTogether)
    {
        spdlog::warn("[RFAB] SkyrimTogether.esp is missing from active load order ({} mods loaded)!",
                     ModManager::Get()->mods.Size());
        ConnectionErrorEvent errorEvent{};
        errorEvent.ErrorDetail = "{\"error\": \"non_default_install\"}";

        m_world.GetRunner().Trigger(errorEvent);
    }
    else
    {
        spdlog::info("[RFAB] SkyrimTogether.esp verified active (slot {:02X}) across {} loaded mods.",
                     pSkyrimTogether->standardId, ModManager::Get()->mods.Size());
    }

    // Clear stale pre-load form IDs (especially 0x14 PlayerCharacter and nearby NPCs destroyed during
    // DetachAllActorsForEngineLoad) so VisitForms() immediately re-creates their EnTT entities once
    // `EndEngineWorldLoad()` and the `WorldReloadBarrier` Host gate settle.
    ResetLoadedForms();
    if (!m_world.GetCharacterService().IsEngineWorldLoading())
    {
        VisitCell(true);
        if (!m_world.GetCoordinatedSaveService().IsHoldingWorldDiscoveryForHostBarrier())
            VisitForms();
    }

    return BSTEventResult::kOk;
}

void DiscoveryService::ResetCachedCellData() noexcept
{
    m_worldSpaceId = 0;
    m_centerGrid.Reset();
    m_currentGrid.Reset();
}
