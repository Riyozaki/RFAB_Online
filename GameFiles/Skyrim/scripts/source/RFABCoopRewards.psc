Scriptname RFABCoopRewards Hidden

; Native contract v1. The installed RFAB source adapter must be generated/audited separately.
; This file is NOT an override of RFAB_XP_Handler and does not register a fake capability.
; Signature is 16 lowercase hex digits of the installed semantic profile (not a multiplier).
; Receiver must be an attached Quest script implementing OnRFABCoopRewardApply(ticket, baseXP).
bool Function RegisterAdapter(Form receiver, string profileName, string signature, int kinds, bool bizarre, string epoch) global native
string Function GetWorldEpoch() global native

; Mode: 0 offline, 1 negotiating, 2 active, 3 quarantined. Offline behavior remains RFAB's own.
int Function GetMode() global native

; Return: 0 PassThrough (ONLY case in which the original reward code may execute),
; 1 Queued (original must not execute), 2 Blocked (original must not execute; never fall back).
; Kind: 0 kill, 1 quest, 2 book, 3 custom. Form ids are runtime GetFormID values.
; Life and occurrence are 16-digit hex identities, NOT wall-clock times/amounts/wallet deltas.
; Kill: source=actor reference, actor=the same reference, life=zero lets the host resolve its
; authority-issued death token, occurrence=one. Book: source=Book, life=zero, occurrence=one.
; Quest/custom: source+context+actual incarnation/occurrence must identify the semantic event.
int Function Submit(string epoch, int kind, int source, int context, int actor, int baseXP, string life, string occurrence) global native

; Audited Papyrus float-recipe adapter: queue actual GlobalVariable property GetFormID() values.
; This claims the ticket itself. It DOES NOT mutate XP on a VM thread or accept a caller's base.
; The main thread uses the host offer, reads the personal multiplier, performs the engine wallet
; setter + SKSE level-XP semantics, and independently verifies BOTH pools under the save pin.
; Recipe: float multiply, int truncation, then floor to quantum (historical RFAB uses five).
; A changed/native RFAB formula needs a different adapter; do not mislabel it as this recipe.
; True means queued, NOT applied. No Complete call is needed for this recipe; the host owns it.
bool Function RequestPersonalApply(string ticket, int walletForm, int multiplierForm, int quantum) global native

; Other custom VM adapters must claim epoch:token once and check CanApply before EACH write.
; Duplicate/stale events cannot claim again. Normal saves/loads are excluded by the host's
; native reservation; a forced/failed load revokes the token and quarantines the old award.
bool Function Claim(string ticket) global native
bool Function CanApply(string ticket) global native

; Result: 0 NotApplied, 1 Applied, 2 Pending (not a completion), 3 Uncertain.
; Applied requires independently verified mutation of Wallet|Engine (mask3) and personal final.
; NotApplied/Uncertain require final=0, pools=0. Unknown/partial result is NEVER auto-replayed.
bool Function Complete(string ticket, int result, int personalFinalXP, int pools) global native
