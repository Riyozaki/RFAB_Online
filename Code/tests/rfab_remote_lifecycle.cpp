// Behaviour of the remote-actor lifecycle rules (Code/common/RemoteLifecycle.h).
//
// These rules were extracted verbatim from CharacterService so they could be exercised in the
// fast loop instead of only in a live game. The predicate is the playtest root cause C7: a
// game-spawned leveled actor also has a 0xFF form id, so deleting it as if it were network
// spawned leaves an immortal invisible enemy. Every case pins the exact pre-split behaviour.

#include <catch2/catch.hpp>

#include <RemoteLifecycle.h>

using namespace RemoteLifecycle;

namespace
{
constexpr uint32_t kPlayerForm = 0x14;
constexpr uint32_t kDynamicForm = 0xFF001234;   // a reference in the dynamic range
constexpr uint32_t kPlacedForm = 0x00012345;    // a pre-placed reference from a plugin
constexpr uint32_t kDynamicBase = 0xFF00ABCD;   // a dynamically-created base form
constexpr uint32_t kStaticBase = 0x000ABCDE;    // a plugin-defined base form (e.g. TESLevCharacter)
} // namespace

TEST_CASE("Remote lifecycle: the local player is never a network-created form",
          "[rfab][remote][predicate]")
{
    // Even with every network flag set, 0x14 is the local player and must never be purged.
    REQUIRE_FALSE(IsNetworkCreatedForm(kPlayerForm, true, true, true, true, kDynamicBase));
    REQUIRE_FALSE(IsNetworkCreatedForm(kPlayerForm, false, false, false, false, 0));
}

TEST_CASE("Remote lifecycle: the extension flags mark a form as network-created outright",
          "[rfab][remote][predicate]")
{
    // CreatedByNetwork -> true regardless of the form-id range or base form.
    REQUIRE(IsNetworkCreatedForm(kPlacedForm, true, true, false, false, 0));
    REQUIRE(IsNetworkCreatedForm(kDynamicForm, true, true, false, true, kStaticBase));

    // A remote player clone is network-created too.
    REQUIRE(IsNetworkCreatedForm(kDynamicForm, true, false, true, false, 0));
}

TEST_CASE("Remote lifecycle: a dynamic reference of a dynamic base is network-created",
          "[rfab][remote][predicate]")
{
    // PlaceAtMe of a dynamic base: both reference and base in the 0xFF range, no ext flags.
    REQUIRE(IsNetworkCreatedForm(kDynamicForm, false, false, false, true, kDynamicBase));
    // Extension present but both flags false still falls through to the range check.
    REQUIRE(IsNetworkCreatedForm(kDynamicForm, true, false, false, true, kDynamicBase));
}

TEST_CASE("Remote lifecycle: a native leveled 0xFF actor is NOT network-created (C7 root cause)",
          "[rfab][remote][predicate]")
{
    // The playtest bug: a game-spawned TESLevCharacter has a 0xFF reference but a STATIC base.
    // Treating it as network-created and deleting it left an immortal invisible enemy.
    REQUIRE_FALSE(IsNetworkCreatedForm(kDynamicForm, false, false, false, true, kStaticBase));
    REQUIRE_FALSE(IsNetworkCreatedForm(kDynamicForm, true, false, false, true, kStaticBase));

    // A dynamic reference with no base form at all is not enough either.
    REQUIRE_FALSE(IsNetworkCreatedForm(kDynamicForm, false, false, false, false, 0));
}

TEST_CASE("Remote lifecycle: a pre-placed reference that went remote is not network-created",
          "[rfab][remote][predicate]")
{
    // A plugin-placed 0x00..0xFE reference is never network-created by the range rule,
    // even if its base happens to be dynamic.
    REQUIRE_FALSE(IsNetworkCreatedForm(kPlacedForm, false, false, false, true, kDynamicBase));
    REQUIRE_FALSE(IsNetworkCreatedForm(kPlacedForm, true, false, false, true, kStaticBase));
}

TEST_CASE("Remote lifecycle: form disposition — purge network, detach pre-placed, leave the player",
          "[rfab][remote][form]")
{
    // No actor -> nothing to do.
    REQUIRE(DecideFormDisposition(false, false, false) == FormDisposition::None);
    REQUIRE(DecideFormDisposition(false, false, true) == FormDisposition::None);

    // The local player is never touched.
    REQUIRE(DecideFormDisposition(true, true, false) == FormDisposition::None);
    REQUIRE(DecideFormDisposition(true, true, true) == FormDisposition::None);

    // A network-created temporary is purged.
    REQUIRE(DecideFormDisposition(true, false, true) == FormDisposition::PurgeNetworkForm);

    // A pre-placed / game-spawned reference is detached back to vanilla, never deleted.
    REQUIRE(DecideFormDisposition(true, false, false) == FormDisposition::DetachToVanilla);
}

TEST_CASE("Remote lifecycle: entity disposition — strip network, re-register live placed, else destroy",
          "[rfab][remote][entity]")
{
    // A removed network actor always wins: strip its components and destroy if orphan.
    REQUIRE(DecideEntityDisposition(true, true, true) == EntityDisposition::StripNetworkAndDestroyIfOrphan);
    REQUIRE(DecideEntityDisposition(true, false, false) == EntityDisposition::StripNetworkAndDestroyIfOrphan);

    // A pre-placed actor still loaded in the local cell with a form id is re-registered locally.
    REQUIRE(DecideEntityDisposition(false, true, true) == EntityDisposition::ReregisterPlacedLocally);

    // Live but missing its form id component -> cannot re-register, destroy if orphan.
    REQUIRE(DecideEntityDisposition(false, true, false) == EntityDisposition::DestroyIfOrphan);

    // Not live in a loaded cell -> destroy if orphan.
    REQUIRE(DecideEntityDisposition(false, false, true) == EntityDisposition::DestroyIfOrphan);
    REQUIRE(DecideEntityDisposition(false, false, false) == EntityDisposition::DestroyIfOrphan);
}

TEST_CASE("Remote lifecycle: grant handling — take a remote actor, renew an actor we already simulate",
          "[rfab][remote][grant]")
{
    // A remote actor: become its simulator.
    REQUIRE(DecideGrantHandling(true, true, false, false) == GrantAction::TakeOver);
    REQUIRE(DecideGrantHandling(true, true, false, true) == GrantAction::TakeOver);

    // A grant for an actor this peer already simulates is a renewal. It must never reach
    // `AskServer`, whose handler sends RequestOwnershipTransfer and hands the actor back:
    // that is the peer evicting itself from an actor it is still animating locally.
    REQUIRE(DecideGrantHandling(true, false, true, false) == GrantAction::Renew);

    // The queued-model case: the form is not materialized yet, so there is nothing to
    // take over, but the epoch must be remembered rather than declined.
    REQUIRE(DecideGrantHandling(true, false, false, true) == GrantAction::Defer);
    REQUIRE(DecideGrantHandling(false, false, false, true) == GrantAction::Defer);

    // Nothing known about the actor and nothing queued: only the server can decide.
    REQUIRE(DecideGrantHandling(true, false, false, false) == GrantAction::AskServer);
    REQUIRE(DecideGrantHandling(false, false, false, false) == GrantAction::AskServer);

    // Stale components can co-exist (a take-over that has not finished stripping
    // RemoteComponent yet). Simulating it locally wins: taking it over a second time
    // would send a redundant claim, renewing it is harmless.
    REQUIRE(DecideGrantHandling(true, true, true, false) == GrantAction::Renew);

    // Renewal must be reachable for every epoch, including a first grant (epoch 0 with no
    // lease recorded): the decision never looks at the epoch, so it cannot disagree with
    // the table about ordering.
    REQUIRE(DecideGrantHandling(true, false, true, true) == GrantAction::Renew);
}
