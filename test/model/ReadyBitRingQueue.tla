---- MODULE ReadyBitRingQueue ----
EXTENDS Naturals, FiniteSets

\* Abstracts the ready-bit ring queue protocol used by:
\* - spsc_queue (direct publish)
\* - mpsc_queue (claim tail -> publish slot)

CONSTANTS Mode, Capacity, MaxSteps

Modes == {"spsc", "mpsc"}
SlotStates == {"empty", "claimed", "ready"}
Slots == 0..(Capacity - 1)
NoneTicket == 2 * MaxSteps + 1

Idx(x) == x % Capacity
StateCount(slots, st) == Cardinality({ i \in Slots : slots[i] = st })

VARIABLES head, tail, slots, slotTicket, issued, published, consumed, steps

vars == << head, tail, slots, slotTicket, issued, published, consumed, steps >>

ReadyCount == StateCount(slots, "ready")
ClaimedCount == StateCount(slots, "claimed")
BusySlots == { i \in Slots : slots[i] # "empty" }

Init ==
  /\ Mode \in Modes
  /\ Capacity \in Nat \ {0}
  /\ head = 0
  /\ tail = 0
  /\ slots = [i \in Slots |-> "empty"]
  /\ slotTicket = [i \in Slots |-> NoneTicket]
  /\ issued = 0
  /\ published = 0
  /\ consumed = 0
  /\ steps = 0

EnqSPSC ==
  /\ Mode = "spsc"
  /\ slots[Idx(tail)] = "empty"
  /\ slots' = [slots EXCEPT ![Idx(tail)] = "ready"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(tail)] = tail]
  /\ tail' = tail + 1
  /\ issued' = issued + 1
  /\ published' = published + 1
  /\ UNCHANGED << head, consumed >>

ClaimTailMPSC ==
  /\ Mode = "mpsc"
  /\ slots[Idx(tail)] = "empty"
  /\ slots' = [slots EXCEPT ![Idx(tail)] = "claimed"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(tail)] = tail]
  /\ tail' = tail + 1
  /\ issued' = issued + 1
  /\ UNCHANGED << head, published, consumed >>

PublishClaim ==
  /\ Mode = "mpsc"
  /\ \E i \in Slots : slots[i] = "claimed"
  /\ \E i \in Slots :
       /\ slots[i] = "claimed"
       /\ slots' = [slots EXCEPT ![i] = "ready"]
       /\ UNCHANGED slotTicket
  /\ published' = published + 1
  /\ UNCHANGED << head, tail, issued, consumed >>

Pop ==
  /\ slots[Idx(head)] = "ready"
  /\ slots' = [slots EXCEPT ![Idx(head)] = "empty"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(head)] = NoneTicket]
  /\ head' = head + 1
  /\ consumed' = consumed + 1
  /\ UNCHANGED << tail, issued, published >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(EnqSPSC)
  \/ WithStep(ClaimTailMPSC)
  \/ WithStep(PublishClaim)
  \/ WithStep(Pop)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ Mode \in Modes
  /\ head \in 0..MaxSteps
  /\ tail \in 0..(2 * MaxSteps)
  /\ slots \in [Slots -> SlotStates]
  /\ slotTicket \in [Slots -> 0..NoneTicket]
  /\ issued \in 0..(2 * MaxSteps)
  /\ published \in 0..(2 * MaxSteps)
  /\ consumed \in 0..(2 * MaxSteps)
  /\ steps \in 0..MaxSteps

QueueAccountingInv ==
  /\ issued = published + ClaimedCount
  /\ published = consumed + ReadyCount
  /\ tail - head = ReadyCount + ClaimedCount

BoundedOccupancyInv ==
  /\ ReadyCount + ClaimedCount <= Capacity
  /\ head <= tail

SPSCNoClaimInv ==
  Mode = "spsc" => ClaimedCount = 0

TicketPlacementInv ==
  /\ \A i \in Slots :
       (slots[i] = "empty") <=> (slotTicket[i] = NoneTicket)
  /\ \A i \in Slots :
       (slots[i] # "empty") => (slotTicket[i] < tail /\ Idx(slotTicket[i]) = i)
  /\ \A i, j \in BusySlots : i # j => slotTicket[i] # slotTicket[j]

=============================================================================
