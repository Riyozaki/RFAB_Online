# Installed RFAB reward source adapter (development)

This tool builds **reviewed local source overrides**, not a replacement downloaded RFAB Handler.
It is not runtime decompilation or automatic support for a newer RFAB/RFAB.dll/Bizarre build.
No installed compatibility profile is shipped: the user's newer baseline is unavailable here
and has not been searched for. The synthetic scripts in the tests are not RFAB overrides.

## Boundary

`Tools/rfab/generate_reward_adapter.py` requires a locally reviewed JSON profile and exact
SHA256 preimages for every changed `.psc` and declared native DLL input. It writes a **new**
directory, never the game/MO2/source directory in place. A source update, missing identity,
ambiguous region, extra unhandled `ModXP` call, different formula or unknown XP side effect
refuses output. Merely editing a SHA to accept an update is not a compatibility review.

A supported source recipe:

* Captures the original base expression **once**, before personal `CalculateXP`.
* Reports actual Form/cycle/reward-slot identity to the native Submit API. Only PassThrough=0
  may execute the original XP region. Queued/Blocked do not fall back to a local award.
* Leaves unrelated side effects and explicitly reviewed personal spending in the original path.
* Can forward an identity-losing old entry to a new explicit-identity function. Only enumerated,
  exact caller lines gain real Form/slot arguments; an unknown old caller remains invalid online
  instead of generating a guessed identity from text, amounts or time. Offline behavior remains
  the original source behavior.
* Registers the existing **attached Quest** through reviewed real init/player-load callbacks.
  Does not manufacture a new unattached Quest or silently alter an ESP.
* Dispatches application to `RequestPersonalApply(ticket, wallet.GetFormID(), multiplier.GetFormID(), quantum)`.
  The VM queues scalar IDs, not native pointers or caller-selected base/final amounts. This
  function claims the application itself. Do **not** Claim it first or call Complete afterwards.

The built-in personal recipe is deliberately narrow: Papyrus **float multiplication → int
truncation → floor to the declared quantum**; original wallet `GlobalVariable.Mod`, original
engine `Game.SetPlayerExperience(Game.GetPlayerExperience() + xp)`, optionally through one
reviewed helper, and a recognized level-notification block. ModXP, helper and calculator bodies
are checked, not just names or flags in JSON. Unknown native formulas, bonus/statistic writes or
other pools require a different audited adapter. These are not normalized away.

Native application uses the frozen host offer and actual attached multiplier, the registered
engine wallet setter, and the SKSE level-points field (not AddSkillExperience). Both independent
increments must be exactly representable in their float stores and independently read back
under the save/native reservation. Partial/unknown mutation is quarantined, never replayed.
A large float wallet can therefore refuse an award instead of silently losing precision.
An optional original level notification runs after commit; it must not issue another XP award.

## Profile data

Schema 1 has the following reviewed inputs (a complete **synthetic** example is built in
`Tools/rfab/test_reward_adapter.py`; it must not be activated as a mod):

| Key | Meaning |
| --- | --- |
| `schema`, `name`, `bizarre` | Version 1, bounded profile name, explicit reviewed Bizarre policy |
| `files` | Flat `.psc` name → exact `sha256` and explicit `utf-8` / `utf-8-sig` / `cp1251` encoding |
| `native_files` | Exact `SKSE/Plugins/*.dll` preimages, or explicitly empty for a reviewed script-only path |
| `receiver` | Attached Quest script; actual wallet/multiplier properties; calculator/quantum; pool recipe; optional engine helper and original level notification |
| `hooks` | Script/method; unique first/last original award lines; kind; actual source/context/actor variables; incarnation/occurrence policy; optional explicit forwarding/callsite changes |
| `personal_spending_methods` | Reviewed exceptions for original negative/personal spending, not a positive-award bypass |
| `registrations` | Real init/player-load methods and the actual receiver property/self |

Kill uses victim **reference**, life0/occurrence1 (the host resolves the authority-issued life).
Book uses the actual Book, life0/occurrence1. Repeatable quest/custom events need explicit caller
string identities encoded as nonzero 16-digit hex; once-per-campaign is a separate explicit
review choice. A string parameter's name alone does not prove its semantic correctness: the
review must establish its real producer. Sources outside the provided/fingerprinted set are
not magically discovered. States, complex methods/multiple award spans, hidden native paths
and missing source need another recipe, not permissive regex substitution.

## Prepare / compile

Source review only (creates `UNCOMPILED-NOT-A-MOD.txt`, **no PEX and no runtime capability table**):

```text
python Tools/rfab/generate_reward_adapter.py --profile reviewed-installed-rewards.json --source-root <actual-source-folder> --data-root <actual-virtual-Data-folder> --output <new-review-directory>
```

Add `--compiler <PapyrusCompiler.exe> --flags <TESV_Papyrus_Flags.flg> --imports <installed-imports>`
to compile against the installed game/RFAB imports. Repeat `--imports` as needed. This invokes
a real executable Papyrus compiler, checks each Skyrim3.2/game1 PEX header and publishes only
after all compiles succeed. The declarations-only emitter **cannot compile these overrides**.
The real compiler/SDK/imports are not redistributed. An absent compiler is not reported as a
successful executable build. The compiler subprocess is trusted local tooling, not downloaded
or executed from a profile/network URL.

Compiled output contains source+PEX, the native API PEX, an advisory manifest, and
`SKSE/Plugins/RFAB_Rewards.tsv`. Activate only the separately reviewed compiled output as a
higher-priority local MO2 mod; do not overwrite RFAB's originals or copy old historical scripts.
Do not edit/install/update profiles with the game running. Matching updated client/server and
save companion are still required.

## Runtime refusal / scope of evidence

At world load the client hashes the profile's compiled PEX and declared DLLs through its virtual
`Data` view. Only a matching attached receiver can advertise that Papyrus profile. Hello waits
for its load/init registration rather than racing into legacy XP; missing/mismatched registration
closes the attempted connection after 15s. Changed bytes refuse semantic negotiation. Native
callback adapters use their separate versioned implementation in a pinned module, not this
Papyrus formula by declaration alone. No profile is inferred from an RFAB/Bizarre filename.

SHA256 detects deployed-file changes. It is **not** authentication, verification of all plugins,
a proof of already cached VM bytecode, a DLL ABI audit, or engine-threading/gameplay evidence.
The source overrides, actual VM dispatch, level notifications, forced-load behavior and the
installed baseline must still pass the final candidate matrix. Both clients must agree on the
semantic recipe/profile signature and kinds, not personal multipliers, wallets or spent XP.
The output belongs to that installed baseline; updates need a new review.
