---- MODULE AwaitableLifecycle ----
EXTENDS Naturals

\* Abstract model for flow_awaitable.h:
\* - awaitable_base (normal)
\* - fast_awaitable_base (fast)
\* Mode is selected by cfg: "normal" or "fast".

CONSTANTS Mode, MaxSteps

Modes == {"normal", "fast"}
Phases == {"idle", "waiting", "done"}

VARIABLES phase,
          nextStepInstalled,
          cancelRegProvided,
          callbackCalls,
          resumeCalls,
          cancelCallbackCalls,
          dropCalls,
          submitOkCalls,
          submitFailCalls,
          cancelRequests,
          steps

vars ==
  << phase, nextStepInstalled, cancelRegProvided, callbackCalls, resumeCalls,
     cancelCallbackCalls, dropCalls, submitOkCalls, submitFailCalls, cancelRequests, steps >>

IsNormal == Mode = "normal"
IsFast == Mode = "fast"

Init ==
  /\ Mode \in Modes
  /\ phase = "idle"
  /\ nextStepInstalled = FALSE
  /\ cancelRegProvided = FALSE
  /\ callbackCalls = 0
  /\ resumeCalls = 0
  /\ cancelCallbackCalls = 0
  /\ dropCalls = 0
  /\ submitOkCalls = 0
  /\ submitFailCalls = 0
  /\ cancelRequests = 0
  /\ steps = 0

InstallNextStep ==
  /\ ~nextStepInstalled
  /\ nextStepInstalled' = TRUE
  /\ UNCHANGED << phase, cancelRegProvided, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitOkCalls, submitFailCalls, cancelRequests >>

ProvideCancelReg ==
  /\ IsNormal
  /\ ~cancelRegProvided
  /\ cancelRegProvided' = TRUE
  /\ UNCHANGED << phase, nextStepInstalled, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitOkCalls, submitFailCalls, cancelRequests >>

DropCancelReg ==
  /\ IsNormal
  /\ cancelRegProvided
  /\ cancelRegProvided' = FALSE
  /\ dropCalls' = dropCalls + 1
  /\ UNCHANGED << phase, nextStepInstalled, callbackCalls, resumeCalls,
                 cancelCallbackCalls, submitOkCalls, submitFailCalls, cancelRequests >>

\* submit_async() success path
SubmitOk ==
  /\ phase = "idle"
  /\ phase' = "waiting"
  /\ submitOkCalls' = submitOkCalls + 1
  /\ UNCHANGED << nextStepInstalled, cancelRegProvided, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitFailCalls, cancelRequests >>

\* submit_async() fail path:
\* normal mode transitions idle->waiting->idle internally (abstracted as staying idle);
\* fast mode just returns error code.
SubmitFail ==
  /\ phase = "idle"
  /\ phase' = "idle"
  /\ submitFailCalls' = submitFailCalls + 1
  /\ UNCHANGED << nextStepInstalled, cancelRegProvided, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitOkCalls, cancelRequests >>

\* Backend completion wins.
ResumeWin ==
  /\ phase = "waiting"
  /\ callbackCalls = 0
  /\ nextStepInstalled
  /\ phase' = "done"
  /\ callbackCalls' = callbackCalls + 1
  /\ resumeCalls' = resumeCalls + 1
  /\ UNCHANGED << nextStepInstalled, cancelRegProvided, cancelCallbackCalls, dropCalls,
                 submitOkCalls, submitFailCalls, cancelRequests >>

\* cancel() request entrypoint (external request observed).
CancelRequest ==
  /\ phase \in {"idle", "waiting", "done"}
  /\ cancelRequests' = cancelRequests + 1
  /\ UNCHANGED << phase, nextStepInstalled, cancelRegProvided, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitOkCalls, submitFailCalls >>

\* Normal awaitable: cancel CAS wins waiting->done and delivers cancel callback exactly once.
CancelWinNormal ==
  /\ IsNormal
  /\ phase = "waiting"
  /\ cancelRegProvided
  /\ callbackCalls = 0
  /\ nextStepInstalled
  /\ phase' = "done"
  /\ callbackCalls' = callbackCalls + 1
  /\ cancelCallbackCalls' = cancelCallbackCalls + 1
  /\ UNCHANGED << nextStepInstalled, cancelRegProvided, resumeCalls, dropCalls,
                 submitOkCalls, submitFailCalls, cancelRequests >>

\* Fast awaitable cancel handler is a no-op by design.
CancelNoOpFast ==
  /\ IsFast
  /\ phase \in {"idle", "waiting", "done"}
  /\ UNCHANGED << phase, nextStepInstalled, cancelRegProvided, callbackCalls, resumeCalls,
                 cancelCallbackCalls, dropCalls, submitOkCalls, submitFailCalls, cancelRequests >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(InstallNextStep)
  \/ WithStep(ProvideCancelReg)
  \/ WithStep(DropCancelReg)
  \/ WithStep(SubmitOk)
  \/ WithStep(SubmitFail)
  \/ WithStep(ResumeWin)
  \/ WithStep(CancelRequest)
  \/ WithStep(CancelWinNormal)
  \/ WithStep(CancelNoOpFast)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ Mode \in Modes
  /\ phase \in Phases
  /\ nextStepInstalled \in BOOLEAN
  /\ cancelRegProvided \in BOOLEAN
  /\ callbackCalls \in 0..MaxSteps
  /\ resumeCalls \in 0..MaxSteps
  /\ cancelCallbackCalls \in 0..MaxSteps
  /\ dropCalls \in 0..MaxSteps
  /\ submitOkCalls \in 0..MaxSteps
  /\ submitFailCalls \in 0..MaxSteps
  /\ cancelRequests \in 0..MaxSteps
  /\ steps \in 0..MaxSteps

AtMostOneCallbackInv ==
  callbackCalls <= 1

CallbackAccountingInv ==
  callbackCalls = resumeCalls + cancelCallbackCalls

DoneImpliesCallbackInv ==
  (phase = "done") => (callbackCalls = 1)

CallbackNeedsNextStepInv ==
  (callbackCalls > 0) => nextStepInstalled

FastCancelNoOpInv ==
  IsFast => (cancelCallbackCalls = 0 /\ dropCalls = 0 /\ ~cancelRegProvided)

NormalCancelSingleInv ==
  IsNormal => cancelCallbackCalls <= 1

=============================================================================
