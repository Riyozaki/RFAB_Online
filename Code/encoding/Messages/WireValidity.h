#pragma once
// Validity is inherited by enclosing packets. A malformed bounded child must never be
// interpreted as an empty-but-valid snapshot while its unread tail becomes other fields.
template <class T> bool WirePayloadValid(const T& value) noexcept
{
    bool valid = true;
    if constexpr (requires { value.CurrentActorData.InitialActorValues.IsValid(); })
        valid &= value.CurrentActorData.InitialActorValues.IsValid() && value.CurrentActorData.InitialInventory.IsValid();
    if constexpr (requires { value.AllActorValues.IsValid(); }) valid &= value.AllActorValues.IsValid();
    if constexpr (requires { value.InitialActorValues.IsValid(); }) valid &= value.InitialActorValues.IsValid();
    if constexpr (requires { value.CurrentInventory.IsValid(); }) valid &= value.CurrentInventory.IsValid();
    if constexpr (requires { value.InventoryContent.IsValid(); }) valid &= value.InventoryContent.IsValid();
    if constexpr (requires { value.Item.WireValid; }) valid &= value.Item.WireValid;
    if constexpr (requires { value.Data.Valid; }) valid &= value.Data.Valid;
    if constexpr (requires { value.Objects.begin(); })
        for (const auto& child : value.Objects) valid &= WirePayloadValid(child);
    return valid;
}
