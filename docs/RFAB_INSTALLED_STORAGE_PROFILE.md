# Installed-profile secure storage catalog (V2)

This is an **offline, read-only profile adapter**, not a catalog downloaded from a presumed
current RFAB release. It does not change a plugin, import a player's items, or prove that a
boss/reward/native ABI is supported. Do not distribute your campaign journals or character receipts.

## Generate from the selected installed profile

Use Python 3.10+ and the selected MO2 profile's **merged virtual Data** directory, not a random
physical mod directory that omits winning overrides. Close the game, server and profile managers
while taking the input snapshot. An explicit ordered `--plugin` list must include all active
masters and overrides, once each, with masters before dependents. For example (paths illustrative):

```powershell
python generate_storage_profile.py `
  --plugin 'D:\ProfileData\Skyrim.esm' `
  --plugin 'D:\ProfileData\Update.esm' `
  --plugin 'D:\ProfileData\RFAB.esp' `
  --output 'D:\RFAB-profile-output-new'
```

That is not a complete list for an actual RFAB installation: pass **your full active order**.
Alternatively, `--data <merged Data> --plugins <ordered active plugins.txt>` is supported; this
list must also include the active official masters. Some mod managers omit those masters from
`plugins.txt`; an incomplete list is refused, not automatically repaired by guessed IDs/order.

The output directory must be new. The tool writes:

- `RFAB_Storage.tsv` — the same V2 file goes to client `Data/SKSE/Plugins/` and server `config/`.
- `RFAB_Profile.json` — schema 2 audit: plugin names/kinds/SHA256, winning access/backing forms,
  enclosing CELL/WRLD and exact access coordinates. Keep the audit with the generated file.

**Old six-column V1 files are refused. Regenerate them from the installed profile; do not add
made-up coordinates to an old file.** Installers preserve an existing profile as live data rather
than silently replacing it with another user's catalog. An explicit adapter update is required.

## What establishes access

Each rule is fifteen TAB-separated fields, with no remote path or load-order index:

| Columns | Meaning |
|---|---|
| 1–3 | Access plugin basename, local hex reference ID, light flag 0/1 |
| 4–6 | Hidden backing plugin basename, local hex reference ID, light flag |
| 7–9 | Access parent CELL plugin, local ID, light flag |
| 10–12 | Parent WRLD plugin, local ID, light flag; `-`, `0`, `0` only for an interior |
| 13–15 | Winning REFR DATA X, Y, Z as finite engine-unit coordinates |

The scanner resolves the live `SecureChest` VMAD object property (including inherited properties),
validates the hidden reference's container base, and retains GRUP CELL/WRLD context. Winning
REFR placement and CELL/WRLD overrides matter. Missing placement, unresolved parents, aliases,
malformed/compressed records, non-finite coordinates and contradictory interior/world context
are refused. Both documented one- and two-byte CELL DATA flag layouts are accepted.

The client requires an attached/loaded nearby visible access, native distance ≤256 units, and a
position within 32 units of its installed-profile placement. A script-moved access needs a specific
adapter; it is not silently trusted just because it was registered.

The server checks the registered point's lock/location and the current **own-avatar** accepted
movement against the server's catalog, at open, read and **each** transfer. Distance is ≤320 units
(the client radius plus transport tolerance); a fresh two-second menu lease does not bypass it.
Interior CELL identity must match. For exterior persistent references, world identity and spatial
distance are used: the reference's persistent parent CELL need not equal the player's grid CELL.
Only canonical backing identity is shared across cities; no hidden-cell proximity is required.

Client/server signatures include canonical forms, kinds, CELL/WRLD and exact float coordinate
bits, independent of row order and equivalent float spelling. FNV is a **mismatch detector**, not
a cryptographic authenticity guarantee. Own-avatar movement is still client-authoritative Skyrim
movement, not server physics or anti-teleport validation. A catalog does not certify line of sight,
navmesh reachability, arbitrary script placement changes or all future RFAB/Bizarre updates.

## Contents and persistence remain explicit

Opening a verified access never silently imports an old personal stash. `rfab_storage_import`
initializes only an uninitialized campaign bank after local confirmation; `rfab_storage_attach`
adopts only an empty or already matching local cache. Different old private contents are not erased
or merged by inference. Campaign receipts, saved cache revisions, exact item extras and paired
source→destination arbitration remain separate requirements; this catalog is only access authority.

Automated evidence is synthetic: parser tests, portable position/signature rules and real-server
wire scenarios. It is not a claim that a current installed profile or Skyrim has been tested.
