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

This event is an asynchronous request, **not an applied receipt**. The reviewed built-in Papyrus
recipe calls `RequestPersonalApply(ticket, walletForm, multiplierForm, quantum)` and lets the host
claim, compute and independently verify both pools. It must **not** pre-Claim or Complete that
request. The generated receiver passes the actual attached properties' GetFormID values; VM ingress
never resolves their native pointers. Other custom adapters must:

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

## Exact-preimage installed source tool (not an installed compatibility profile)

`generate_reward_adapter.py` now prepares reviewed source-level interception/attached receivers,
checks the original calculator **and both pool writer bodies**, retains explicit identities and
unrelated side effects, and can invoke a real local Papyrus compiler. Source-only output has no
PEX/capability table. Compiled output fingerprints its PEX and declared native DLLs; the client
checks that table at world load and waits for the actual receiver before semantic Hello.
See [installed profile contract](RFAB_INSTALLED_REWARD_PROFILE.md). The 18 synthetic tool tests
exercise refusal, preimages, forwarding, encoding and staging; compiler subprocesses in the two
staging tests are explicitly **mocked**, not executable-Papyrus compilation or installed evidence.

## Installed integration that is NOT yet complete

The native/Papyrus API and registration/event delivery do **not** intercept RFAB by themselves.
A compatible installed-profile override must capture rewards **before personal multiplication**,
retain real quest/book/custom identities and non-XP side effects, suppress the original award,
and implement verified dual-pool application. No reviewed/compiled override for the user's newer installed
RFAB is currently present or tested. The exact-preimage tool is not that missing profile. The older public Handler/Alias/BA sources were reference
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

- Current portable core: **93 cases / 30,520 assertions PASS** (`build/core-installed-xp.log`),
  including independent personal float pools/property mailbox and bounded deployed-profile parser.
- Earlier core ASan+UBSan: **88 / 277,971 PASS** (extended seeds); the new 93-case sanitized run
  is pending at this writing and must not be confused with that older result.
- Production server: **8 scenarios + crash-record PASS**, plain **and ASan+UBSan**, including frozen
  equal base/different finals, authority/dedup/downed/loading/restart cursors. The server sanitizer
  process completed successfully after the preceding documentation was written. These runs do
  not execute Skyrim or the new client executor/profile checks.
- The new production XP scenario found an OfferNext reference invalidated by ledger publication;
  returning the owned offer captured before publication fixed the crash in both passing runs.
- Native declarations: **8 functions / 665-byte deterministic PEX**, **5 tests PASS**. Installed
  reward generator: **18 synthetic tests PASS**; storage generator: **18 PASS**. Full strict static
  PASS; local PowerShell unavailable. None of these is Skyrim VM execution.
- Windows startup-fix commit `3fe07e6`, run44169445636: **SUCCESS**, including build/test/package
  and the unchanged live server smoke gate. That does not prove the earlier startup AV's cause.
- Semantic commit `20775b9`, run44172443126: **FAIL**, native compile error from treating the
  nested `TESQuest::Type` enum as a Form-type value. Current source uses `FormType::QuestItem`;
  this fix and new client execution await the next Windows build. Linux20775/run44172443038 PASS.

The overall candidate remains `ready_for_playtest=false`. There is no release/tag or intermediate
shared gameplay test; the agreed single joint test follows completion of the whole candidate.
