#pragma once

/**
 * Pure lifecycle decisions for remote actors (Code/client/Services/Generic/CharacterService).
 *
 * When a remote actor's assignment ends (cell exit, disconnect, server prune, ownership
 * handoff) the client must decide two things, and getting either wrong is exactly the
 * "immortal invisible enemy" / "ghost NPC" cluster from the playtests:
 *
 *   1. The engine FORM: was this reference spawned by the network (a temporary we created),
 *      or is it a pre-placed / game-spawned reference that merely went remote? A network form
 *      is purged; a pre-placed form must be detached back to vanilla (SetRemote(false)) and
 *      never deleted, or it becomes an un-ownable, un-damageable ghost.
 *
 *   2. The ECS ENTITY: after the remote components are stripped, do we destroy the entity,
 *      or re-register a still-loaded pre-placed actor locally so it never becomes unsynced?
 *
 * These rules used to be inline in ~6 places in CharacterService, interleaved with entt and
 * engine calls, so they could only be checked in a live game. This is the same split PR #17
 * applied to ownership / saves and the downed rework applied to revives: the decisions live
 * here as values-in / values-out, the service performs the engine calls they ask for.
 *
 * The behaviour is intentionally identical to what the service did before the split; the
 * accompanying test (rfab_remote_lifecycle.cpp) pins it so the extraction is provably faithful.
 */

#include <cstdint>

namespace RemoteLifecycle
{

// ============================================================================
// Is this reference network-created?
// ============================================================================
// The predicate that separates "a temporary we spawned" from "a pre-placed / game-spawned
// reference that went remote". The subtle part (playtest root cause C7): a game-spawned
// leveled actor (TESLevCharacter) ALSO has a 0xFF...... form id, so the form-id range alone
// is not enough — it is only network-created if the extension says so, or if BOTH the
// reference and its base form are in the dynamic 0xFF range (a PlaceAtMe of a dynamic base).
// Deleting a native leveled 0xFF reference is a no-op on the engine side but leaves its AI
// and hitbox alive and unregistered -> immortal invisible enemy.
//
// aFormId == 0x14 (the local player) is never network-created. The caller handles a null
// actor pointer before calling this.
[[nodiscard]] inline bool IsNetworkCreatedForm(
    uint32_t aFormId,
    bool aHasExtension,
    bool aCreatedByNetwork,
    bool aIsRemotePlayer,
    bool aHasBaseForm,
    uint32_t aBaseFormId) noexcept
{
    if (aFormId == 0x14u)
        return false;

    if (aHasExtension && (aCreatedByNetwork || aIsRemotePlayer))
        return true;

    return ((aFormId & 0xFF000000u) == 0xFF000000u) &&
           aHasBaseForm &&
           ((aBaseFormId & 0xFF000000u) == 0xFF000000u);
}

// ============================================================================
// What to do with the engine form when a remote assignment ends
// ============================================================================
enum class FormDisposition : uint8_t
{
    None,             // no actor, or it is the local player -> leave it alone
    PurgeNetworkForm, // a temporary we created -> delete it cleanly
    DetachToVanilla,  // pre-placed / game-spawned -> SetRemote(false), sanitize, keep it
};

[[nodiscard]] inline FormDisposition DecideFormDisposition(
    bool aActorExists,
    bool aIsLocalPlayerForm,
    bool aIsNetworkCreated) noexcept
{
    if (!aActorExists || aIsLocalPlayerForm)
        return FormDisposition::None;
    return aIsNetworkCreated ? FormDisposition::PurgeNetworkForm : FormDisposition::DetachToVanilla;
}

// ============================================================================
// What to do with the ECS entity after its remote components are stripped
// ============================================================================
enum class EntityDisposition : uint8_t
{
    StripNetworkAndDestroyIfOrphan, // a network actor was removed -> drop form/anim components, destroy if orphan
    ReregisterPlacedLocally,        // a pre-placed actor is still loaded here -> re-register so it never goes unsynced
    DestroyIfOrphan,                // nothing live to keep -> destroy the entity if it has no components left
};

[[nodiscard]] inline EntityDisposition DecideEntityDisposition(
    bool aRemovedNetworkActor,
    bool aLivePlacedInLoadedCell,
    bool aHasFormIdComponent) noexcept
{
    if (aRemovedNetworkActor)
        return EntityDisposition::StripNetworkAndDestroyIfOrphan;
    if (aLivePlacedInLoadedCell && aHasFormIdComponent)
        return EntityDisposition::ReregisterPlacedLocally;
    return EntityDisposition::DestroyIfOrphan;
}

} // namespace RemoteLifecycle
