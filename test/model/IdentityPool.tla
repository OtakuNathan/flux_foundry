---- MODULE IdentityPool ----
EXTENDS Naturals, FiniteSets
CONSTANTS Senders, Capacity, MaxEpoch
VARIABLES phase, epoch, pc, origin, tickets, reserved, inbox, returned, adopted, badAccess
vars == <<phase, epoch, pc, origin, tickets, reserved, inbox, returned, adopted, badAccess>>

\* Each sender represents a batch of dead blocks. Atomics are abstract
\* transitions; this is a lifetime/ownership model, not a C++ memory-model proof.
Init ==
  /\ phase = "open" /\ epoch = 1
  /\ pc = [s \in Senders |-> "unallocated"]
  /\ origin = [s \in Senders |-> 0]
  /\ tickets = {} /\ reserved = {} /\ inbox = {}
  /\ returned = {} /\ adopted = {} /\ badAccess = FALSE

Allocate(s) ==
  /\ phase = "open" /\ pc[s] = "unallocated"
  /\ pc' = [pc EXCEPT ![s] = "staged"]
  /\ origin' = [origin EXCEPT ![s] = epoch]
  /\ UNCHANGED <<phase, epoch, tickets, reserved, inbox, returned, adopted, badAccess>>

Acquire(s) ==
  /\ pc[s] = "staged" /\ phase = "open" /\ origin[s] = epoch
  /\ pc' = [pc EXCEPT ![s] = "admitted"] /\ tickets' = tickets \cup {s}
  /\ UNCHANGED <<phase, epoch, origin, reserved, inbox, returned, adopted, badAccess>>

AdoptStale(s) ==
  /\ pc[s] = "staged" /\ (phase # "open" \/ origin[s] # epoch)
  /\ pc' = [pc EXCEPT ![s] = "done"] /\ adopted' = adopted \cup {s}
  /\ UNCHANGED <<phase, epoch, origin, tickets, reserved, inbox, returned, badAccess>>

Reserve(s) ==
  /\ pc[s] = "admitted" /\ Cardinality(reserved \cup inbox) < Capacity
  /\ reserved' = reserved \cup {s} /\ pc' = [pc EXCEPT ![s] = "reserved"]
  /\ UNCHANGED <<phase, epoch, origin, tickets, inbox, returned, adopted, badAccess>>

AdoptFull(s) ==
  /\ pc[s] = "admitted" /\ Cardinality(reserved \cup inbox) = Capacity
  /\ pc' = [pc EXCEPT ![s] = "done"] /\ tickets' = tickets \ {s}
  /\ adopted' = adopted \cup {s}
  /\ UNCHANGED <<phase, epoch, origin, reserved, inbox, returned, badAccess>>

Publish(s) ==
  /\ pc[s] = "reserved"
  /\ pc' = [pc EXCEPT ![s] = "published"]
  /\ inbox' = inbox \cup {s} /\ reserved' = reserved \ {s}
  /\ badAccess' = badAccess \/ phase = "gone" \/ origin[s] # epoch
  /\ UNCHANGED <<phase, epoch, origin, tickets, returned, adopted>>

Release(s) ==
  /\ pc[s] = "published"
  /\ pc' = [pc EXCEPT ![s] = "done"] /\ tickets' = tickets \ {s}
  /\ UNCHANGED <<phase, epoch, origin, reserved, inbox, returned, adopted, badAccess>>

Collect ==
  /\ phase = "open" /\ inbox # {}
  /\ returned' = returned \cup inbox /\ inbox' = {}
  /\ UNCHANGED <<phase, epoch, pc, origin, tickets, reserved, adopted, badAccess>>

Close ==
  /\ phase = "open" /\ phase' = "closing"
  /\ UNCHANGED <<epoch, pc, origin, tickets, reserved, inbox, returned, adopted, badAccess>>

Destroy ==
  /\ phase = "closing" /\ tickets = {}
  /\ returned' = returned \cup inbox /\ inbox' = {} /\ phase' = "gone"
  /\ UNCHANGED <<epoch, pc, origin, tickets, reserved, adopted, badAccess>>

Reuse ==
  /\ phase = "gone" /\ epoch < MaxEpoch
  /\ phase' = "open" /\ epoch' = epoch + 1
  /\ UNCHANGED <<pc, origin, tickets, reserved, inbox, returned, adopted, badAccess>>

Next == Close \/ Destroy \/ Reuse \/ Collect \/
  \E s \in Senders: Allocate(s) \/ Acquire(s) \/ AdoptStale(s) \/
    Reserve(s) \/ AdoptFull(s) \/ Publish(s) \/ Release(s)
Spec == Init /\ [][Next]_vars

TicketsMatch == tickets = {s \in Senders : pc[s] \in {"admitted", "reserved", "published"}}
Protected == \A s \in tickets: phase # "gone" /\ origin[s] = epoch
Bounded == Cardinality(reserved \cup inbox) <= Capacity
Ownership ==
  /\ reserved \cap inbox = {} /\ inbox \cap returned = {} /\ inbox \cap adopted = {}
  /\ reserved \cap returned = {} /\ reserved \cap adopted = {} /\ returned \cap adopted = {}
  /\ \A s \in Senders: pc[s] = "done" => s \in inbox \cup returned \cup adopted
  /\ phase = "gone" => inbox = {} /\ reserved = {} /\ tickets = {}
NoBadAccess == ~badAccess
====
