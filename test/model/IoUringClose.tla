---- MODULE IoUringClose ----
EXTENDS Naturals, FiniteSets

\* Borrowed-ring attachment: one serialized SQ/CQ owner, concurrent close and
\* logical cancellation. One finite request per ID; native tokens are abstract.
CONSTANTS Requests, Capacity
ASSUME /\ Requests # {} /\ 0 \notin Requests /\ Capacity > 0
Phases == {"idle", "admitted", "preparing", "staged", "submitted", "cqe", "done", "rejected"}
Live == {"admitted", "preparing", "staged", "submitted", "cqe"}
Slotted == {"preparing", "staged", "submitted", "cqe"}

VARIABLES pc, closing, tickets, owner, slots, buffers, outcome, detached
vars == <<pc, closing, tickets, owner, slots, buffers, outcome, detached>>

Init ==
  /\ pc = [r \in Requests |-> "idle"] /\ closing = FALSE /\ tickets = 0
  /\ owner = 0 /\ slots = {} /\ buffers = {}
  /\ outcome = [r \in Requests |-> "none"] /\ detached = FALSE

Admit(r) ==
  /\ owner = 0 /\ pc[r] = "idle" /\ ~closing
  /\ pc' = [pc EXCEPT ![r] = "admitted"]
  /\ tickets' = tickets + 1 /\ owner' = r /\ buffers' = buffers \cup {r}
  /\ UNCHANGED <<closing, slots, outcome, detached>>

RejectClosed(r) ==
  /\ owner = 0 /\ pc[r] = "idle" /\ closing
  /\ pc' = [pc EXCEPT ![r] = "rejected"]
  /\ outcome' = [outcome EXCEPT ![r] = "error"]
  /\ UNCHANGED <<closing, tickets, owner, slots, buffers, detached>>

Reserve(r) ==
  /\ owner = r /\ pc[r] = "admitted" /\ Cardinality(slots) < Capacity
  /\ pc' = [pc EXCEPT ![r] = "preparing"] /\ slots' = slots \cup {r}
  /\ UNCHANGED <<closing, tickets, owner, buffers, outcome, detached>>

\* Also permits a full host SQ even when the FF registry has a free slot.
RejectSubmission(r) ==
  /\ owner = r /\ pc[r] = "admitted"
  /\ pc' = [pc EXCEPT ![r] = "rejected"] /\ tickets' = tickets - 1
  /\ owner' = 0 /\ buffers' = buffers \ {r}
  /\ outcome' = [outcome EXCEPT ![r] = IF @ = "none" THEN "error" ELSE @]
  /\ UNCHANGED <<closing, slots, detached>>

Prepare(r) ==
  /\ owner = r /\ pc[r] = "preparing"
  /\ pc' = [pc EXCEPT ![r] = "staged"] /\ owner' = 0
  /\ UNCHANGED <<closing, tickets, slots, buffers, outcome, detached>>

\* One-at-a-time submits allow partial batches; failed flushes are stuttering.
Submit(r) ==
  /\ owner = 0 /\ pc[r] = "staged"
  /\ pc' = [pc EXCEPT ![r] = "submitted"]
  /\ UNCHANGED <<closing, tickets, owner, slots, buffers, outcome, detached>>

KernelCompletes(r) ==
  /\ pc[r] = "submitted" /\ pc' = [pc EXCEPT ![r] = "cqe"]
  /\ UNCHANGED <<closing, tickets, owner, slots, buffers, outcome, detached>>

\* Only dispatching the original terminal CQE retires an accepted ticket.
Complete(r) ==
  /\ owner = 0 /\ pc[r] = "cqe"
  /\ pc' = [pc EXCEPT ![r] = "done"] /\ tickets' = tickets - 1
  /\ slots' = slots \ {r} /\ buffers' = buffers \ {r}
  /\ outcome' = [outcome EXCEPT ![r] = IF @ = "none" THEN "value" ELSE @]
  /\ UNCHANGED <<closing, owner, detached>>

Cancel(r) ==
  /\ pc[r] \in Live /\ outcome[r] = "none"
  /\ outcome' = [outcome EXCEPT ![r] = "cancel"]
  /\ UNCHANGED <<pc, closing, tickets, owner, slots, buffers, detached>>

RequestClose ==
  /\ ~closing /\ closing' = TRUE
  /\ UNCHANGED <<pc, tickets, owner, slots, buffers, outcome, detached>>

Detach ==
  /\ closing /\ tickets = 0 /\ owner = 0 /\ ~detached
  /\ detached' = TRUE
  /\ UNCHANGED <<pc, closing, tickets, owner, slots, buffers, outcome>>

RequestProgress(r) == Reserve(r) \/ RejectSubmission(r) \/ Prepare(r)
                     \/ Submit(r) \/ KernelCompletes(r) \/ Complete(r)
Next == RequestClose \/ Detach
        \/ (\E r \in Requests : Admit(r) \/ RejectClosed(r) \/ Cancel(r) \/ RequestProgress(r))

\* Eventual detach assumes the host keeps flushing/draining and the kernel
\* eventually completes each accepted operation. Permanent I/O stalls excluded.
Spec == Init /\ [][Next]_vars
        /\ (\A r \in Requests : WF_vars(RequestProgress(r))) /\ WF_vars(Detach)

TypeInv ==
  /\ pc \in [Requests -> Phases] /\ closing \in BOOLEAN
  /\ tickets \in 0..Cardinality(Requests) /\ owner \in Requests \cup {0}
  /\ slots \subseteq Requests /\ buffers \subseteq Requests
  /\ outcome \in [Requests -> {"none", "value", "error", "cancel"}]
  /\ detached \in BOOLEAN
TicketAccounting == tickets = Cardinality({r \in Requests : pc[r] \in Live})
OwnershipInv ==
  /\ Cardinality(slots) <= Capacity
  /\ slots = {r \in Requests : pc[r] \in Slotted}
  /\ buffers = {r \in Requests : pc[r] \in Live}
  /\ {r \in Requests : pc[r] \in {"admitted", "preparing"}} =
       (IF owner = 0 THEN {} ELSE {owner})
NoPrematureDetach == detached =>
  /\ closing /\ tickets = 0 /\ owner = 0 /\ slots = {} /\ buffers = {}
NoPrematureSuccess == \A r \in Requests : outcome[r] = "value" => pc[r] = "done"
ShutdownCompletes == closing ~> detached
=============================================================================
