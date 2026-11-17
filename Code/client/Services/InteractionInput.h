#pragma once
#include <ThreadAffinity.h>
struct InputEvent;
struct InteractionInput
{
    static RFAB_ANY_THREAD void Observe(const InputEvent*) noexcept;
    static RFAB_ANY_THREAD bool Held() noexcept;
};
