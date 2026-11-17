#include <TiltedOnlinePCH.h>
#include <Services/InteractionInput.h>
#include <BSInput/InputEvent.h>
#include <cstring>
#include <cmath>
namespace
{
std::atomic_bool s_held{};
std::atomic<uint64_t> s_seen{};
}
void InteractionInput::Observe(const InputEvent* event) noexcept
{
    // Gameplay has already mapped keyboard/mouse/gamepad to the named Activate action.
    // Never assume E/H or reinterpret a virtual-key code as a DirectInput/gamepad code.
    for (unsigned count = 0; event && count < 512; event = event->Next, ++count)
        if (event->EventType == 0)
        {
            const auto* button = static_cast<const ButtonEvent*>(event);
            if (button->UserEvent.AsAscii() && std::strcmp(button->UserEvent.AsAscii(), "Activate") == 0)
            {
                s_held.store(std::isfinite(button->Value) && std::isfinite(button->HeldSeconds) && button->Value > 0.f, std::memory_order_release);
                s_seen.store(GetTickCount64(), std::memory_order_release);
            }
        }
}
bool InteractionInput::Held() noexcept
{
    const auto seen = s_seen.load(std::memory_order_acquire);
    return s_held.load(std::memory_order_acquire) && seen && GetTickCount64() - seen <= 300;
}
