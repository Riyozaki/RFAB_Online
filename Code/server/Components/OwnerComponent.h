#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct Player;
struct OwnerComponent
{
    OwnerComponent(Player* apPlayer, uint64_t aNowMs = 0)
        : pOwner(apPlayer)
        , LastChangedAtMs(aNowMs)
    {
    }

    Player* GetOwner() const { return reinterpret_cast<Player*>(pOwner); }

    uint64_t SetOwner(Player* apPlayer, uint64_t aNowMs = 0) noexcept
    {
        if (pOwner != apPlayer)
        {
            pOwner = apPlayer;
            ++Epoch;
            LastChangedAtMs = aNowMs;
        }
        return Epoch;
    }

    Player* pOwner;
    uint64_t Epoch{1};
    uint64_t LastChangedAtMs{0};
    Vector<const Player*> InvalidOwners{};
};
