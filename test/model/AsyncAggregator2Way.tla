---- MODULE AsyncAggregator2Way ----
EXTENDS Naturals, FiniteSets

\* Abstract 2-way model for flow_async_aggregator (when_all / when_any, normal / fast).
\* Focus: completion races, result selection, and launch-success vs launch-failed split.

CONSTANTS AggKind, FastMode, MaxSteps

Kinds == {"when_all", "when_any"}
Idx == {0, 1}
ChildStates == {"none", "pending", "value", "error"}
ResultKinds == {"none", "value", "error"}

NoneIdx == 2

VARIABLES launched,
          launchSuccess,
          launchFailed,
          childState,
          winner,
          failedIdx,
          controllersCanceled,
          resumeCalls,
          resultKind,
          steps

vars ==
  << launched, launchSuccess, launchFailed, childState, winner, failedIdx,
     controllersCanceled, resumeCalls, resultKind, steps >>

PendingCount(cs) == Cardinality({ i \in Idx : cs[i] = "pending" })
AllLaunched == launched = Idx
AllDone == PendingCount(childState) = 0
AnyError(cs) == \E i \in Idx : cs[i] = "error"
AnyValue(cs) == \E i \in Idx : cs[i] = "value"

\* submit() marks launch_success when launching phase returns success:
\* - when_all: all children launched successfully
\* - when_any: at least one child launched, and if not all launched then an early winner stopped launching
LaunchSuccessReady ==
  IF AggKind = "when_all" THEN
    AllLaunched
  ELSE
    /\ launched # {}
    /\ (AllLaunched \/ winner # NoneIdx)

FinalizeValueAllowed(cs, w) ==
  /\ resumeCalls = 0
  /\ launchSuccess
  /\ ~launchFailed
  /\ AllLaunched
  /\ PendingCount(cs) = 0
  /\ IF AggKind = "when_all"
        THEN \A i \in Idx : cs[i] = "value"
        ELSE w # NoneIdx

FinalizeErrorAllowed(cs, w) ==
  /\ resumeCalls = 0
  /\ launchSuccess
  /\ ~launchFailed
  /\ AllLaunched
  /\ PendingCount(cs) = 0
  /\ IF AggKind = "when_all"
        THEN AnyError(cs)
        ELSE w = NoneIdx

Init ==
  /\ AggKind \in Kinds
  /\ FastMode \in BOOLEAN
  /\ launched = {}
  /\ launchSuccess = FALSE
  /\ launchFailed = FALSE
  /\ childState = [i \in Idx |-> "none"]
  /\ winner = NoneIdx
  /\ failedIdx = NoneIdx
  /\ controllersCanceled = {}
  /\ resumeCalls = 0
  /\ resultKind = "none"
  /\ steps = 0

LaunchChild(i) ==
  /\ i \in Idx
  /\ i \notin launched
  /\ launched' = launched \cup {i}
  /\ childState' = [childState EXCEPT ![i] = "pending"]
  /\ UNCHANGED << launchSuccess, launchFailed, winner, failedIdx, controllersCanceled, resumeCalls, resultKind >>

\* submit() marks launch-success after all child launches are started.
\* If children completed synchronously before this flag is set, finalization may happen here.
MarkLaunchSuccess ==
  /\ ~launchSuccess
  /\ ~launchFailed
  /\ LaunchSuccessReady
  /\ launchSuccess' = TRUE
  /\ IF FinalizeValueAllowed(childState, winner) THEN
        /\ resumeCalls' = resumeCalls + 1
        /\ resultKind' = "value"
     ELSE IF FinalizeErrorAllowed(childState, winner) THEN
        /\ resumeCalls' = resumeCalls + 1
        /\ resultKind' = "error"
     ELSE
        /\ UNCHANGED << resumeCalls, resultKind >>
  /\ UNCHANGED << launched, launchFailed, childState, winner, failedIdx, controllersCanceled >>

\* Abstract launch-failure marker (e.g. missing bp / launch exception path).
\* In the real code this causes submit() failure and outer awaitable submit_async() fail path.
MarkLaunchFailed ==
  /\ ~launchSuccess
  /\ ~launchFailed
  /\ resumeCalls = 0
  /\ IF AggKind = "when_any" THEN launched = {} ELSE ~AllLaunched
  /\ launchFailed' = TRUE
  /\ IF (~FastMode /\ AggKind = "when_all")
        THEN controllersCanceled' = controllersCanceled \cup launched
        ELSE UNCHANGED controllersCanceled
  /\ UNCHANGED << launched, launchSuccess, childState, winner, failedIdx, resumeCalls, resultKind >>

ChildCompleteValue(i) ==
  /\ i \in Idx
  /\ childState[i] = "pending"
  /\ LET cs1 == [childState EXCEPT ![i] = "value"] IN
     LET w1 ==
          IF AggKind = "when_any" /\ winner = NoneIdx /\ ~launchFailed
             THEN i ELSE winner
     IN
     /\ childState' = cs1
     /\ winner' = w1
     /\ IF (~FastMode /\ AggKind = "when_any" /\ winner = NoneIdx /\ ~launchFailed)
           THEN controllersCanceled' = Idx
           ELSE UNCHANGED controllersCanceled
     /\ IF AggKind = "when_any" /\ winner = NoneIdx /\ ~launchFailed /\ resumeCalls = 0 THEN
           /\ resumeCalls' = resumeCalls + 1
           /\ resultKind' = "value"
        ELSE IF FinalizeValueAllowed(cs1, w1) THEN
           /\ resumeCalls' = resumeCalls + 1
           /\ resultKind' = "value"
        ELSE
           /\ UNCHANGED << resumeCalls, resultKind >>
     /\ IF failedIdx = NoneIdx THEN UNCHANGED failedIdx ELSE failedIdx' = failedIdx
  /\ UNCHANGED << launched, launchSuccess, launchFailed >>

ChildCompleteError(i) ==
  /\ i \in Idx
  /\ childState[i] = "pending"
  /\ LET cs1 == [childState EXCEPT ![i] = "error"] IN
     /\ childState' = cs1
     /\ IF failedIdx = NoneIdx THEN failedIdx' = i ELSE failedIdx' = failedIdx
     /\ IF (~FastMode /\ AggKind = "when_all")
           THEN controllersCanceled' = Idx
           ELSE UNCHANGED controllersCanceled
     /\ IF FinalizeErrorAllowed(cs1, winner) THEN
           /\ resumeCalls' = resumeCalls + 1
           /\ resultKind' = "error"
        ELSE
           /\ UNCHANGED << resumeCalls, resultKind >>
     /\ UNCHANGED winner
  /\ UNCHANGED << launched, launchSuccess, launchFailed >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ \E i \in Idx : WithStep(LaunchChild(i))
  \/ WithStep(MarkLaunchSuccess)
  \/ WithStep(MarkLaunchFailed)
  \/ \E i \in Idx : WithStep(ChildCompleteValue(i))
  \/ \E i \in Idx : WithStep(ChildCompleteError(i))

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ AggKind \in Kinds
  /\ FastMode \in BOOLEAN
  /\ launched \subseteq Idx
  /\ launchSuccess \in BOOLEAN
  /\ launchFailed \in BOOLEAN
  /\ childState \in [Idx -> ChildStates]
  /\ winner \in {0, 1, NoneIdx}
  /\ failedIdx \in {0, 1, NoneIdx}
  /\ controllersCanceled \subseteq Idx
  /\ resumeCalls \in 0..MaxSteps
  /\ resultKind \in ResultKinds
  /\ steps \in 0..MaxSteps

LaunchFlagsInv ==
  /\ ~(launchSuccess /\ launchFailed)
  /\ launchFailed => (resumeCalls = 0 /\ resultKind = "none")

LaunchSuccessShapeInv ==
  /\ (AggKind = "when_all" /\ launchSuccess) => AllLaunched
  /\ (AggKind = "when_any" /\ launchSuccess) => (launched # {})
  /\ (AggKind = "when_any" /\ launchSuccess /\ ~AllLaunched) => (winner # NoneIdx)

LaunchStateInv ==
  /\ \A i \in Idx : (i \notin launched) => (childState[i] = "none")
  /\ \A i \in launched : childState[i] \in {"pending", "value", "error"}

AtMostOneResumeInv ==
  resumeCalls <= 1

WinnerConsistencyInv ==
  (winner = NoneIdx) \/ (childState[winner] = "value")

FailedIdxConsistencyInv ==
  (failedIdx = NoneIdx) \/ (childState[failedIdx] = "error")

ResultConsistencyInv ==
  /\ (resultKind = "none") \/ (resumeCalls = 1)
  /\ (resultKind = "value") =>
       (IF AggKind = "when_all"
           THEN \A i \in Idx : childState[i] = "value"
           ELSE winner # NoneIdx)
  /\ (resultKind = "error") =>
       (IF AggKind = "when_any"
           THEN winner = NoneIdx
           ELSE AnyError(childState))

CancelSemanticsInv ==
  /\ FastMode => controllersCanceled = {}
  /\ (~FastMode /\ AggKind = "when_any" /\ winner # NoneIdx) => controllersCanceled = Idx
  /\ (~FastMode /\ AggKind = "when_all" /\ failedIdx # NoneIdx) => controllersCanceled = Idx

=============================================================================
