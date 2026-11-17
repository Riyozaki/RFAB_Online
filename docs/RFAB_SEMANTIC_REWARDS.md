# RFAB semantic XP boundary — development, not an installed-profile compatibility claim

The policy is **one frozen base reward for eligible nearby participants, followed by each
character's own RFAB multiplier**. Personal spending and wallets remain independent. A zero
personal multiplier is legitimate. No wallet polling, balancing, dividing of already multiplied
awards, or reconnect catch-up is used.

## Implemented production path

- Protocol **5 / +coop-wire-5** requires rebuilding both client and server. Hello carries the
  installed semantic profile signature, supported kinds and at most 16 campaign receipt cursors.
  Cohort members must agree on the profile/kinds/Bizarre capability, not on personal multipliers.
- `RewardLedger` durably fixes source identity, base and at most two recipients before any offer.
  One offer per character is outstanding. Duplicate event reports return the original event,
  even after spending or a different reporter calculating a different book base.
- Kill identity is reference + authority-issued incarnation, not BaseId or a timer. The accepted
  owner's alive→dead transition freezes the nearby recipient set and publishes `RewardSource`.
  A report without this token cannot create a kill award. Downed teammates remain eligible;
  stale/loading/far-away/unrelated avatars do not. This is not a universal hostility/script or
  anti-cheat proof. Native respawn/phase transitions still need their explicit lifecycle adapter.
- Quest/custom identity is stable source + optional context + real incarnation + occurrence.
  Book identity is the book form, once per campaign. Text/amount/time is never an event key.
  Repeatable quest cycles and custom reward slots must come from the installed RFAB adapter.
- Client execution reserves the serial write-ahead native timeline and excludes an ESS save.
  It calls the registered native adapter OR dispatches a ticket to an attached Papyrus Quest.
  Applied requires verified wallet **and** engine-level XP, then a co-save receipt, then server
  commit, then native journal commit. Duplicate grants/receipts cannot invoke the adapter again.
- Missing receipts, partial mutation, timeout, changed profile, disk failure or interrupted
  native application quarantine the operation. Offered disconnect/restart is uncertain, never
  automatically replayed. A reward never offered before disconnect is not retroactively paid.
  Stale co-save revision/digest cannot overwrite the durable campaign cursor.
- The existing legacy STR skill redistribution is suppressed **only in negotiated semantic
  mode**. Local personal skill calls still execute. No unrelated single-player reward path is
  replaced just because this API exists.

## Adapter interfaces

`Coop/RewardBridge.h` defines the scalar version-1 ABI exported as
`RFAB_GetRewardBridge(1)`. Registration is bound to a world-load epoch. A registered native
callback's module is pinned until process exit; no native actor/context pointer crosses the ABI.
Callbacks/VM threads submit owned scalars to a mutex-protected queue (64 entries). Engine/ECS
work is performed on the client update thread, not by ingress callbacks.

`GameFiles/Skyrim/scripts/source/RFABCoopRewards.psc` and its reproducible, declarations-only
`.pex` expose the equivalent Papyrus functions. The receiver must be an attached Quest script:

```
Event OnRFABCoopRewardApply(string ticket, int baseXP)
```

This event is an asynchronous request, **not an applied receipt**. It must:

1. Claim the exact `epoch:token` once with `Claim(ticket)`; duplicate events do no work.
2. Calculate personal final XP using the installed RFAB multiplier/rounding policy, starting
   from the offered base. Reject invalid/overflowing results before any write.
3. Check `CanApply(ticket)` before **each** pool write. Update the personal RFAB wallet and
   engine-level XP through the actual installed RFAB paths; independently verify both changes.
   A failed/partial/unknown result must report Uncertain, not pretend that `pools=3` proves work.
4. Complete with the personal final and Wallet|Engine only after both writes are verified.
   Completion revokes permission to write the token again. Do not emit another source event
   while applying an inbound grant. Native adapters that reuse an intercepted path pass the
   `InboundToken` explicitly rather than fabricating another semantic source.
5. Never execute the original local award when Submit returns Queued or Blocked. PassThrough
   is the **only** permission to run the original code (ordinary offline RFAB behavior).

The save/native exclusion blocks normal load/save admission while an application is pending.
A forced/failed load can still invalidate the world: its epoch revokes the old ticket, and the
old receipt is quarantined. This is not a promise of crash-proof atomic mutation inside Skyrim.
The campaign inventory and reward journals are separate files, **not a coordinated checkpoint**.

## Installed integration that is NOT yet complete

The native/Papyrus API and registration/event delivery do **not** intercept RFAB by themselves.
A compatible installed-profile override must capture rewards **before personal multiplication**,
retain real quest/book/custom identities and non-XP side effects, suppress the original award,
and implement verified dual-pool application. No such override for the user's newer installed
RFAB is currently present or tested. The older public Handler/Alias/BA sources were reference
material only; they must not be shipped as a guessed replacement for newer scripts or RFAB.dll.
The newer RFAB was not searched for.

In particular, the historical no-argument Handler book callback loses the Book identity, and
historical `CustomReward(text, amount)` loses its caller/slot identity. Adding a ModXP hook,
claiming required flags or dispatching a custom event does **not** repair those missing facts.
Bizarre also has a separate source path; its flag must never be inferred from a plugin filename.
Unknown paths need an explicit audited adapter, not equalized wallets or a guessed text hash.

`Tools/rfab/compile_native_psc.py` emits only native declarations. It explicitly refuses game
script code, inheritance, properties and executable functions; it cannot compile RFAB overrides.
The checked-in native PEX is verified by `test_native_psc.py` and source/native binding counts.
This verifies bytes/signatures, **not execution in Skyrim's VM**.

## Evidence and remaining gates

- Portable core: **88 cases / 30,219 assertions PASS**; with ASan+UBSan and extended seeds,
  **88 / 277,971 PASS** (`build/core-semantic-bridge*.log`). Counts vary slightly with races.
- Real server + both production codecs: **8 scenarios + crash-record PASS**, including frozen
  equal base/different finals, legacy suppression, forged/non-owner death refusal, authoritative
  kill token, downed/loading eligibility, late duplicate/no cohort extension and matching/mismatched
  co-save cursor after restart (`build/server-semantic-rewards.log`). It does not execute Skyrim.
- The first new production XP scenario exposed a dangling range-loop key after ledger publication
  replaced its map. OfferNext now returns an owned offer captured before publication; the crash
  is fixed in the passing wire run. Older seven-scenario passes did not cover XP.
- Current production-server ASan+UBSan is still running at this writing; Windows compilation of
  these new client/Papyrus files is not yet proven. Declaration tests: **5 PASS**, storage parser:
  **18 PASS**, strict static: PASS, local PowerShell unavailable.

The overall candidate remains `ready_for_playtest=false`. There is no release/tag or intermediate
shared gameplay test; the agreed single joint test follows completion of the whole candidate.
