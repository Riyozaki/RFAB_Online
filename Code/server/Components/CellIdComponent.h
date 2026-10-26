#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/GridCellCoords.h>

struct CellIdComponent
{
    CellIdComponent() {}

    CellIdComponent(GameId aCellId)
        : Cell(aCellId)
    {
    }

    CellIdComponent(GameId aCellId, GameId aWorldSpaceId, GridCellCoords aCenterCoords)
        : Cell(aCellId)
        , WorldSpaceId(aWorldSpaceId)
        , CenterCoords(aCenterCoords)
    {
    }

    bool operator==(const CellIdComponent& acRhs) const noexcept
    {
        return Cell == acRhs.Cell && WorldSpaceId == acRhs.WorldSpaceId && CenterCoords == acRhs.CenterCoords;
    }

    bool operator!=(const CellIdComponent& acRhs) const noexcept { return !operator==(acRhs); }

    [[nodiscard]] bool IsValid() const noexcept
    {
        if (WorldSpaceId)
        {
            return CenterCoords.X != std::numeric_limits<int32_t>::max() &&
                   CenterCoords.Y != std::numeric_limits<int32_t>::max();
        }

        return static_cast<bool>(Cell) && Cell.ModId != 0xFFFFFFFFu;
    }

    operator bool() const noexcept { return IsValid(); }

    bool IsInInteriorCell() const noexcept { return !WorldSpaceId; }

    bool IsInRange(const CellIdComponent& acRhs, bool aIsDragon) const noexcept
    {
        if (!IsValid() || !acRhs.IsValid())
            return false;

        if (IsInInteriorCell() != acRhs.IsInInteriorCell())
            return false;

        if (IsInInteriorCell())
            return Cell == acRhs.Cell;

        if (WorldSpaceId != acRhs.WorldSpaceId)
            return false;

        return GridCellCoords::IsCellInGridCell(acRhs.CenterCoords, CenterCoords, aIsDragon);
    }

    GameId Cell{};
    GameId WorldSpaceId{};
    GridCellCoords CenterCoords{};
};
