---- MODULE FlowRunnerAsyncNode ----
EXTENDS Naturals

\* Abstract model of flow_runner async-node dispatch path (normal + fast awaitable branches).
\* References: flow/flow_runner.h dispatch_async_node() variants.
\* "receiverCalls" models terminal receiver delivery (logical completion), not raw internal next_step invocations.

CONSTANTS FastMode, MaxSteps

CtrlStates == {"none", "locked", "soft", "hard"}
Phases ==
  {"start", "awaitable_ready", "handler_ready", "locked", "nextstep_set",
   "submitted_locked", "submitted", "terminal"}
TerminalKinds == {"none", "factory_error", "cancel_error", "submit_error", "resume"}

VARIABLES phase,
          ctrlState,
          handlerInstalled,
          lockHeld,
          notifyDroppedCalls,
          receiverCalls,
          terminalKind,
          steps

vars == << phase, ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind, steps >>

Init ==
  /\ FastMode \in BOOLEAN
  /\ phase = "start"
  /\ ctrlState = "none"
  /\ handlerInstalled = FALSE
  /\ lockHeld = FALSE
  /\ notifyDroppedCalls = 0
  /\ receiverCalls = 0
  /\ terminalKind = "none"
  /\ steps = 0

FactoryFail ==
  /\ phase = "start"
  /\ phase' = "terminal"
  /\ receiverCalls' = 1
  /\ terminalKind' = "factory_error"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls >>

FactoryOk ==
  /\ phase = "start"
  /\ phase' = "awaitable_ready"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* Fast branch skips controller cancel-handler registration/locking.
FastSetNextStep ==
  /\ FastMode
  /\ phase = "awaitable_ready"
  /\ phase' = "nextstep_set"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* Fast awaitable branch does not install a cancel handler on the controller for this node path.
\* Controller cancel may still happen concurrently and only changes controller state.
ExternalCancelFast(kind) ==
  /\ FastMode
  /\ kind \in {"soft", "hard"}
  /\ phase \in {"awaitable_ready", "nextstep_set", "submitted"}
  /\ ctrlState = "none"
  /\ ctrlState' = kind
  /\ UNCHANGED << phase, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* Normal branch provides cancel handler before trying to install it in controller.
ProvideHandlerNormal ==
  /\ ~FastMode
  /\ phase = "awaitable_ready"
  /\ phase' = "handler_ready"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* External cancellation can race before lock attempt (normal branch only).
ExternalPreCancel(kind) ==
  /\ ~FastMode
  /\ kind \in {"soft", "hard"}
  /\ phase = "handler_ready"
  /\ ctrlState = "none"
  /\ ctrlState' = kind
  /\ UNCHANGED << phase, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

LockHandlerSuccess ==
  /\ ~FastMode
  /\ phase = "handler_ready"
  /\ ctrlState = "none"
  /\ phase' = "locked"
  /\ ctrlState' = "locked"
  /\ handlerInstalled' = TRUE
  /\ lockHeld' = TRUE
  /\ UNCHANGED << notifyDroppedCalls, receiverCalls, terminalKind >>

LockHandlerFailCanceled ==
  /\ ~FastMode
  /\ phase = "handler_ready"
  /\ ctrlState \in {"soft", "hard"}
  /\ phase' = "terminal"
  /\ notifyDroppedCalls' = notifyDroppedCalls + 1
  /\ receiverCalls' = 1
  /\ terminalKind' = "cancel_error"
  /\ handlerInstalled' = FALSE
  /\ lockHeld' = FALSE
  /\ UNCHANGED ctrlState

SetNextStepNormal ==
  /\ ~FastMode
  /\ phase = "locked"
  /\ lockHeld
  /\ ctrlState = "locked"
  /\ phase' = "nextstep_set"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* submit_async() failed after lock and next-step installation.
SubmitFailNormal ==
  /\ ~FastMode
  /\ phase = "nextstep_set"
  /\ lockHeld
  /\ ctrlState = "locked"
  /\ phase' = "terminal"
  /\ ctrlState' = "none"
  /\ handlerInstalled' = FALSE
  /\ lockHeld' = FALSE
  /\ notifyDroppedCalls' = notifyDroppedCalls + 1
  /\ receiverCalls' = 1
  /\ terminalKind' = "submit_error"

SubmitOkNormal ==
  /\ ~FastMode
  /\ phase = "nextstep_set"
  /\ lockHeld
  /\ ctrlState = "locked"
  /\ phase' = "submitted_locked"
  /\ UNCHANGED << ctrlState, lockHeld >>
  /\ handlerInstalled' = TRUE
  /\ UNCHANGED << notifyDroppedCalls, receiverCalls, terminalKind >>

\* guard destructor unlocks controller after submit_async() returns.
UnlockAfterSubmitNormal ==
  /\ ~FastMode
  /\ phase = "submitted_locked"
  /\ lockHeld
  /\ ctrlState = "locked"
  /\ handlerInstalled
  /\ phase' = "submitted"
  /\ ctrlState' = "none"
  /\ lockHeld' = FALSE
  /\ UNCHANGED << handlerInstalled, notifyDroppedCalls, receiverCalls, terminalKind >>

\* Fast branch submit failure/success.
SubmitFailFast ==
  /\ FastMode
  /\ phase = "nextstep_set"
  /\ phase' = "terminal"
  /\ receiverCalls' = 1
  /\ terminalKind' = "submit_error"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls >>

SubmitOkFast ==
  /\ FastMode
  /\ phase = "nextstep_set"
  /\ phase' = "submitted"
  /\ UNCHANGED << ctrlState, handlerInstalled, lockHeld, notifyDroppedCalls, receiverCalls, terminalKind >>

\* Normal branch: external cancel after successful submit races with backend resume.
ExternalCancelAfterSubmit(kind) ==
  /\ ~FastMode
  /\ kind \in {"soft", "hard"}
  /\ phase = "submitted"
  /\ ctrlState = "none"
  /\ handlerInstalled
  /\ receiverCalls = 0
  /\ phase' = "terminal"
  /\ ctrlState' = kind
  /\ handlerInstalled' = FALSE
  /\ notifyDroppedCalls' = notifyDroppedCalls + 1
  /\ receiverCalls' = 1
  /\ terminalKind' = "cancel_error"
  /\ lockHeld' = FALSE

\* Backend completion callback reaches next-step after guard unlock (common path).
AsyncResume ==
  /\ phase = "submitted"
  /\ receiverCalls = 0
  /\ phase' = "terminal"
  /\ receiverCalls' = 1
  /\ terminalKind' = "resume"
  /\ IF FastMode THEN
        /\ UNCHANGED << ctrlState, handlerInstalled, notifyDroppedCalls, lockHeld >>
     ELSE
        /\ ctrlState = "none"
        /\ handlerInstalled
        /\ handlerInstalled' = FALSE
        /\ notifyDroppedCalls' = notifyDroppedCalls + 1
        /\ lockHeld' = FALSE
        /\ UNCHANGED ctrlState

\* Backend completion may happen synchronously inside submit_async(), before guard unlocks.
AsyncResumeBeforeUnlock ==
  /\ ~FastMode
  /\ phase = "submitted_locked"
  /\ receiverCalls = 0
  /\ lockHeld
  /\ ctrlState = "locked"
  /\ handlerInstalled
  /\ phase' = "terminal"
  /\ receiverCalls' = receiverCalls + 1
  /\ terminalKind' = "resume"
  /\ ctrlState' = "none"
  /\ handlerInstalled' = FALSE
  /\ lockHeld' = FALSE
  /\ notifyDroppedCalls' = notifyDroppedCalls + 1

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(FactoryFail)
  \/ WithStep(FactoryOk)
  \/ WithStep(FastSetNextStep)
  \/ \E k \in {"soft", "hard"} : WithStep(ExternalCancelFast(k))
  \/ WithStep(ProvideHandlerNormal)
  \/ \E k \in {"soft", "hard"} : WithStep(ExternalPreCancel(k))
  \/ WithStep(LockHandlerSuccess)
  \/ WithStep(LockHandlerFailCanceled)
  \/ WithStep(SetNextStepNormal)
  \/ WithStep(SubmitFailNormal)
  \/ WithStep(SubmitOkNormal)
  \/ WithStep(UnlockAfterSubmitNormal)
  \/ WithStep(SubmitFailFast)
  \/ WithStep(SubmitOkFast)
  \/ \E k \in {"soft", "hard"} : WithStep(ExternalCancelAfterSubmit(k))
  \/ WithStep(AsyncResume)
  \/ WithStep(AsyncResumeBeforeUnlock)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ phase \in Phases
  /\ ctrlState \in CtrlStates
  /\ handlerInstalled \in BOOLEAN
  /\ lockHeld \in BOOLEAN
  /\ notifyDroppedCalls \in 0..MaxSteps
  /\ receiverCalls \in 0..MaxSteps
  /\ terminalKind \in TerminalKinds
  /\ steps \in 0..MaxSteps

AtMostOneReceiverInv ==
  receiverCalls <= 1

AtMostOneDropInv ==
  notifyDroppedCalls <= 1

LockConsistencyInv ==
  lockHeld <=> (ctrlState = "locked")

FastNoHandlerPathInv ==
  FastMode => (~handlerInstalled /\ ~lockHeld /\ notifyDroppedCalls = 0 /\ ctrlState \in {"none", "soft", "hard"})

HandlerOnlyNormalInv ==
  handlerInstalled => (~FastMode /\ phase \in {"locked", "nextstep_set", "submitted_locked", "submitted"} /\ ctrlState \in {"locked", "none"})

TerminalCleanupInv ==
  (phase = "terminal") => ~lockHeld

TerminalKindConsistencyInv ==
  (terminalKind = "none") \/ (phase = "terminal")

=============================================================================
