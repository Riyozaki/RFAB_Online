
#include <Services/CommandService.h>
#include <Services/TransportService.h>

#include <Systems/ModSystem.h>
#include <World.h>

#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <PlayerCharacter.h>
#include <Skyrim/Interface/UI.h>
#include <Services/DownedStateService.h>
#include <common/RfabModpackCompat.h>

#include <Events/SetTimeCommandEvent.h>

#include <Messages/TeleportCommandRequest.h>
#include <Messages/TeleportCommandResponse.h>
#include "Messages/SetTimeCommandRequest.h"

#include <Structs/GridCellCoords.h>

CommandService::CommandService(World& aWorld, TransportService& aTransport, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_setTimeConnection = aDispatcher.sink<SetTimeCommandEvent>().connect<&CommandService::OnSetTimeCommand>(this);
    m_teleportConnection = aDispatcher.sink<TeleportCommandResponse>().connect<&CommandService::OnTeleportCommandResponse>(this);
}

void CommandService::OnSetTimeCommand(const SetTimeCommandEvent& acEvent) const noexcept
{
    SetTimeCommandRequest request{};
    request.Hours = acEvent.Hours;
    request.Minutes = acEvent.Minutes;
    request.PlayerId = acEvent.PlayerId;
    m_transport.Send(request);
}

void CommandService::OnTeleportCommandResponse(const TeleportCommandResponse& acMessage) noexcept
{
    spdlog::info("TeleportCommandResponse: {:X}:{:X} at position {}, {}, {}", acMessage.CellId.ModId, acMessage.CellId.BaseId, acMessage.Position.x, acMessage.Position.y, acMessage.Position.z);

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;

    const bool isDowned = m_world.GetDownedStateService().IsLocalPlayerDowned();
    if (isDowned)
    {
        m_world.GetOverlayService().SendSystemMessage("Нельзя телепортироваться в состоянии ранения на колене.");
        return;
    }

    bool isLoading = pPlayer->IsStillLoading();
    if (auto* pUi = UI::Get())
    {
        isLoading = isLoading ||
                    pUi->GetMenuOpen(BSFixedString("Loading Menu")) ||
                    pUi->GetMenuOpen(BSFixedString("Main Menu"));
    }

    const bool hasCellId = static_cast<bool>(acMessage.CellId);
    const bool isTempCellId = acMessage.CellId.ModId == 0xFFFFFFFF;
    const bool hasWorldSpaceId = static_cast<bool>(acMessage.WorldSpaceId);

    if (!RfabCompat::CanResolveTeleportDestination(
            hasCellId,
            isTempCellId,
            hasWorldSpaceId,
            acMessage.Position.x,
            acMessage.Position.y,
            acMessage.Position.z,
            isDowned,
            isLoading))
    {
        spdlog::error("Failed to fetch cell to teleport to.");
        m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
        return;
    }

    auto& modSystem = m_world.GetModSystem();
    TESObjectCELL* pCell = nullptr;

    if (hasCellId && !isTempCellId)
    {
        const uint32_t cellId = modSystem.GetGameId(acMessage.CellId);
        pCell = Cast<TESObjectCELL>(TESForm::GetById(cellId));
    }

    if (!pCell && hasWorldSpaceId)
    {
        const uint32_t worldSpaceId = modSystem.GetGameId(acMessage.WorldSpaceId);
        TESWorldSpace* pWorldSpace = Cast<TESWorldSpace>(TESForm::GetById(worldSpaceId));
        if (pWorldSpace)
        {
            GridCellCoords coordinates = GridCellCoords::CalculateGridCellCoords(acMessage.Position);
            pCell = pWorldSpace->LoadCell(coordinates.X, coordinates.Y);
        }
    }

    if (!pCell)
    {
        spdlog::error("Failed to fetch cell to teleport to.");
        m_world.GetOverlayService().SendSystemMessage("Teleporting to player failed.");
        return;
    }

    pPlayer->MoveTo(pCell, acMessage.Position);
}
