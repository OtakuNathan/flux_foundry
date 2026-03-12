---- MODULE SimpleExecutorTickets ----
EXTENDS Naturals

\* Abstract model of flux_foundry::simple_executor ticket accounting.
\* Queue internals are abstracted into qLen with MPSC contract assumed.

CONSTANTS Capacity, MaxSteps

States == {"idle", "running", "shutdown"}

VARIABLES state,
          consumerActive,
          pending,
          qLen,
          issued,
          retired,
          inlined,
          popped,
          aborts,
          steps

vars == << state, consumerActive, pending, qLen, issued, retired, inlined, popped, aborts, steps >>

Init ==
  /\ state = "idle"
  /\ consumerActive = FALSE
  /\ pending = 0
  /\ qLen = 0
  /\ issued = 0
  /\ retired = 0
  /\ inlined = 0
  /\ popped = 0
  /\ aborts = 0
  /\ steps = 0

\* run(): idle -> running, single consumer enters loop.
RunStartFromIdle ==
  /\ state = "idle"
  /\ ~consumerActive
  /\ state' = "running"
  /\ consumerActive' = TRUE
  /\ UNCHANGED << pending, qLen, issued, retired, inlined, popped, aborts >>

\* run(): if shutdown was requested before run() starts, the consumer may still enter
\* once to drain pre-admitted work and then exit.
RunStartFromShutdown ==
  /\ state = "shutdown"
  /\ ~consumerActive
  /\ state' = state
  /\ consumerActive' = TRUE
  /\ UNCHANGED << pending, qLen, issued, retired, inlined, popped, aborts >>

\* dispatch() before shutdown: ticket admitted and queued.
DispatchEnqueue ==
  /\ state # "shutdown"
  /\ qLen < Capacity
  /\ pending' = pending + 1
  /\ qLen' = qLen + 1
  /\ issued' = issued + 1
  /\ UNCHANGED << state, consumerActive, retired, inlined, popped, aborts >>

\* dispatch() from the consumer thread may fall back to inline execution if q.try_emplace()
\* misses (queue full or transient enqueue miss under contention).
\* ticket is admitted then executed inline and retired in the same abstract step.
DispatchInlineOnEnqueueMiss ==
  /\ state # "shutdown"
  /\ consumerActive
  /\ state' = state
  /\ consumerActive' = consumerActive
  /\ pending' = pending
  /\ qLen' = qLen
  /\ issued' = issued + 1
  /\ retired' = retired + 1
  /\ inlined' = inlined + 1
  /\ UNCHANGED << popped, aborts >>

\* dispatch() after shutdown aborts and rolls back the admitted ticket (net zero accounting effect).
DispatchAfterShutdownAbort ==
  /\ state = "shutdown"
  /\ aborts' = aborts + 1
  /\ UNCHANGED << state, consumerActive, pending, qLen, issued, retired, inlined, popped >>

\* run(): consume one queued task and retire one ticket.
RunPop ==
  /\ consumerActive
  /\ qLen > 0
  /\ state \in {"running", "shutdown"}
  /\ qLen' = qLen - 1
  /\ pending' = pending - 1
  /\ retired' = retired + 1
  /\ popped' = popped + 1
  /\ UNCHANGED << state, consumerActive, issued, inlined, aborts >>

\* try_shutdown(): idle/running -> shutdown.
TryShutdown ==
  /\ state \in {"idle", "running"}
  /\ state' = "shutdown"
  /\ UNCHANGED << consumerActive, pending, qLen, issued, retired, inlined, popped, aborts >>

\* run() returns only after shutdown observed and pending drained.
RunExitAfterDrain ==
  /\ consumerActive
  /\ state = "shutdown"
  /\ pending = 0
  /\ consumerActive' = FALSE
  /\ UNCHANGED << state, pending, qLen, issued, retired, inlined, popped, aborts >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(RunStartFromIdle)
  \/ WithStep(RunStartFromShutdown)
  \/ WithStep(DispatchEnqueue)
  \/ WithStep(DispatchInlineOnEnqueueMiss)
  \/ WithStep(DispatchAfterShutdownAbort)
  \/ WithStep(RunPop)
  \/ WithStep(TryShutdown)
  \/ WithStep(RunExitAfterDrain)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ state \in States
  /\ consumerActive \in BOOLEAN
  /\ pending \in 0..MaxSteps
  /\ qLen \in 0..Capacity
  /\ issued \in 0..MaxSteps
  /\ retired \in 0..MaxSteps
  /\ inlined \in 0..MaxSteps
  /\ popped \in 0..MaxSteps
  /\ aborts \in 0..MaxSteps
  /\ steps \in 0..MaxSteps

ConsumerStateInv ==
  /\ (state = "idle") => ~consumerActive
  /\ consumerActive => state \in {"running", "shutdown"}

AccountingInv ==
  /\ issued = pending + retired
  /\ retired = popped + inlined

QueueBoundInv ==
  /\ qLen <= Capacity
  /\ qLen <= pending

AbstractQueueAccountingInv ==
  pending = qLen

=============================================================================
