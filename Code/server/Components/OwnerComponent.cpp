#include <Components.h>
#include <Game/Player.h>

uint32_t GetOwnerPlayerId(const Player* apPlayer) noexcept
{
    return apPlayer ? apPlayer->GetId() : Authority::kNoOwner;
}
