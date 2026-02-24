---- MODULE FlowControllerCancel ----
EXTENDS Naturals

\* Abstract model of flux_foundry::flow_controller lock/cancel protocol.
\* It focuses on state_ + handler-field coordination, not on the full runner.

CONSTANTS MaxEpoch, MaxSteps

Phases == {"none", "locked", "soft", "hard"}
Kinds == {"soft", "hard"}
LockOwners == {"none", "install", "reset"}

VARIABLES phase,
          lockOwner,
          epoch,
          handlerInstalled,
          cancelWinner,
          cancelCalls,
          notifyDrops,
          steps

vars == << phase, lockOwner, epoch, handlerInstalled, cancelWinner, cancelCalls, notifyDrops, steps >>

IncEpoch(e) == IF e = MaxEpoch THEN 0 ELSE e + 1

Init ==
  /\ phase = "none"
  /\ lockOwner = "none"
  /\ epoch = 0
  /\ handlerInstalled = FALSE
  /\ cancelWinner = "none"
  /\ cancelCalls = 0
  /\ notifyDrops = 0
  /\ steps = 0

\* Runner path: lock to install cancel handler.
InstallBegin ==
  /\ phase = "none"
  /\ cancelWinner = "none"
  /\ phase' = "locked"
  /\ lockOwner' = "install"
  /\ UNCHANGED << epoch, handlerInstalled, cancelWinner, cancelCalls, notifyDrops >>

\* Runner path: publish handler fields and unlock (epoch advances).
InstallCommit ==
  /\ phase = "locked"
  /\ lockOwner = "install"
  /\ phase' = "none"
  /\ lockOwner' = "none"
  /\ epoch' = IncEpoch(epoch)
  /\ handlerInstalled' = TRUE
  /\ UNCHANGED << cancelWinner, cancelCalls, notifyDrops >>

\* Runner/destructor path: lock to reset handler fields.
ResetBegin ==
  /\ phase = "none"
  /\ cancelWinner = "none"
  /\ phase' = "locked"
  /\ lockOwner' = "reset"
  /\ UNCHANGED << epoch, handlerInstalled, cancelWinner, cancelCalls, notifyDrops >>

\* Reset clears handler and drops subscription if present, then unlocks.
ResetCommit ==
  /\ phase = "locked"
  /\ lockOwner = "reset"
  /\ phase' = "none"
  /\ lockOwner' = "none"
  /\ epoch' = IncEpoch(epoch)
  /\ handlerInstalled' = FALSE
  /\ notifyDrops' = notifyDrops + IF handlerInstalled THEN 1 ELSE 0
  /\ UNCHANGED << cancelWinner, cancelCalls >>

\* External thread wins cancel race.
CancelWin(kind) ==
  /\ kind \in Kinds
  /\ phase = "none"
  /\ cancelWinner = "none"
  /\ phase' = kind
  /\ lockOwner' = "none"
  /\ handlerInstalled' = FALSE
  /\ cancelWinner' = kind
  /\ cancelCalls' = cancelCalls + IF handlerInstalled THEN 1 ELSE 0
  /\ notifyDrops' = notifyDrops + IF handlerInstalled THEN 1 ELSE 0
  /\ UNCHANGED epoch

\* Cancel after already canceled is an immediate return (modeled as stutter in [][Next]_vars).
\* Cancel while locked spins/retries until lock clears (also covered by stutter).

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(InstallBegin)
  \/ WithStep(InstallCommit)
  \/ WithStep(ResetBegin)
  \/ WithStep(ResetCommit)
  \/ \E k \in Kinds : WithStep(CancelWin(k))

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ phase \in Phases
  /\ lockOwner \in LockOwners
  /\ epoch \in 0..MaxEpoch
  /\ handlerInstalled \in BOOLEAN
  /\ cancelWinner \in {"none", "soft", "hard"}
  /\ cancelCalls \in 0..MaxSteps
  /\ notifyDrops \in 0..MaxSteps
  /\ steps \in 0..MaxSteps

LockOwnershipInv ==
  /\ (phase = "locked") <=> (lockOwner # "none")
  /\ (phase # "locked") => (lockOwner = "none")

CancelAtMostOnceInv ==
  cancelCalls <= 1

HandlerClearedWhenCanceledInv ==
  (cancelWinner # "none") => ~handlerInstalled

CancelPhaseConsistentInv ==
  (cancelWinner = "none") \/ (phase = cancelWinner)

=============================================================================
