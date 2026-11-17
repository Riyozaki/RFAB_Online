#include <TiltedOnlinePCH.h>

#include <Misc/GarbageCollector.h>

GarbageCollector* GarbageCollector::Get() noexcept
{
    POINTER_SKYRIMSE(GarbageCollector*, s_singleton, 514180);
    return *s_singleton.Get();
}

void GarbageCollector::Add(TESBoundObject* apObject) noexcept
{
    // SE 1.5.97 has no Address Library ID for the TESBoundObject* overload (AE 36460).
    // Note: SE 35492 is GarbageCollector::Add(TESObjectREFR*, bool), NOT TESBoundObject* —
    // calling it with a TESBoundObject* corrupts memory. Skip deferred disposal on SE 1.5.97.
    (void)apObject;
}
