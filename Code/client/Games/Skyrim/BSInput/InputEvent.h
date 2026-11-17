#pragma once
#include <Misc/BSFixedString.h>
#include <cstdint>

// Read-only engine event layout, CommonLibSSE-NG b93280e... InputEvent/IDEvent/ButtonEvent.
struct InputEvent
{
    virtual ~InputEvent();
    virtual bool HasIDCode() const;
    virtual const BSFixedString& QUserEvent() const;
    uint32_t Device, EventType;
    InputEvent* Next;
};
struct ButtonEvent : InputEvent
{
    BSFixedString UserEvent;
    uint32_t Code, Padding;
    float Value, HeldSeconds;
};
static_assert(sizeof(InputEvent) == 0x18);
static_assert(offsetof(ButtonEvent, UserEvent) == 0x18);
static_assert(offsetof(ButtonEvent, Value) == 0x28);
static_assert(sizeof(ButtonEvent) == 0x30);
