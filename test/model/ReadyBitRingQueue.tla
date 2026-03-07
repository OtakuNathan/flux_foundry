---- MODULE ReadyBitRingQueue ----
EXTENDS Naturals, FiniteSets

\* Abstracts:
\* - spsc_queue: ready-bit ring
\* - mpsc_queue: generation-tagged slot protocol (claim tail, publish odd seq, consume to next even seq)

CONSTANTS Mode, Capacity, MaxSteps

Modes == {"spsc", "mpsc"}
SlotStates == {"empty", "ready"}
Slots == 0..(Capacity - 1)
NoneTicket == 2 * MaxSteps + 1

Idx(x) == x % Capacity
RoundNo(x) == x \div Capacity
StateCount(slots, st) == Cardinality({ i \in Slots : slots[i] = st })

MPSCReady(slotSeq, slotTicket, reserved, i) ==
  /\ i \notin reserved
  /\ slotTicket[i] # NoneTicket
  /\ slotSeq[i] = 2 * RoundNo(slotTicket[i]) + 1

VARIABLES head, tail, slotState, slotSeq, slotTicket, reserved, issued, published, consumed, steps

vars == << head, tail, slotState, slotSeq, slotTicket, reserved, issued, published, consumed, steps >>

ReadyCount ==
  IF Mode = "spsc"
    THEN StateCount(slotState, "ready")
    ELSE Cardinality({ i \in Slots : MPSCReady(slotSeq, slotTicket, reserved, i) })

ClaimedCount == IF Mode = "mpsc" THEN Cardinality(reserved) ELSE 0

BusySlots ==
  IF Mode = "spsc"
    THEN { i \in Slots : slotState[i] = "ready" }
    ELSE { i \in Slots : i \in reserved \/ MPSCReady(slotSeq, slotTicket, reserved, i) }

Init ==
  /\ Mode \in Modes
  /\ Capacity \in Nat \ {0}
  /\ head = 0
  /\ tail = 0
  /\ slotState = [i \in Slots |-> "empty"]
  /\ slotSeq = [i \in Slots |-> 0]
  /\ slotTicket = [i \in Slots |-> NoneTicket]
  /\ reserved = {}
  /\ issued = 0
  /\ published = 0
  /\ consumed = 0
  /\ steps = 0

EnqSPSC ==
  /\ Mode = "spsc"
  /\ slotState[Idx(tail)] = "empty"
  /\ slotState' = [slotState EXCEPT ![Idx(tail)] = "ready"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(tail)] = tail]
  /\ tail' = tail + 1
  /\ issued' = issued + 1
  /\ published' = published + 1
  /\ UNCHANGED << head, slotSeq, reserved, consumed >>

ClaimTailMPSC ==
  /\ Mode = "mpsc"
  /\ LET i == Idx(tail) IN
     LET seq == 2 * RoundNo(tail) IN
     /\ slotSeq[i] = seq
     /\ i \notin reserved
     /\ slotTicket[i] = NoneTicket
     /\ reserved' = reserved \cup {i}
     /\ slotTicket' = [slotTicket EXCEPT ![i] = tail]
  /\ tail' = tail + 1
  /\ issued' = issued + 1
  /\ UNCHANGED << head, slotState, slotSeq, published, consumed >>

PublishClaim ==
  /\ Mode = "mpsc"
  /\ \E i \in reserved :
       LET t == slotTicket[i] IN
       /\ slotSeq[i] = 2 * RoundNo(t)
       /\ reserved' = reserved \ {i}
       /\ slotSeq' = [slotSeq EXCEPT ![i] = 2 * RoundNo(t) + 1]
       /\ UNCHANGED slotTicket
  /\ published' = published + 1
  /\ UNCHANGED << head, tail, slotState, issued, consumed >>

PopSPSC ==
  /\ Mode = "spsc"
  /\ slotState[Idx(head)] = "ready"
  /\ slotState' = [slotState EXCEPT ![Idx(head)] = "empty"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(head)] = NoneTicket]
  /\ head' = head + 1
  /\ consumed' = consumed + 1
  /\ UNCHANGED << tail, slotSeq, reserved, issued, published >>

PopMPSC ==
  /\ Mode = "mpsc"
  /\ LET i == Idx(head) IN
     LET seq == 2 * RoundNo(head) + 1 IN
     /\ i \notin reserved
     /\ slotSeq[i] = seq
     /\ slotTicket[i] = head
     /\ slotSeq' = [slotSeq EXCEPT ![i] = seq + 1]
     /\ slotTicket' = [slotTicket EXCEPT ![i] = NoneTicket]
  /\ head' = head + 1
  /\ consumed' = consumed + 1
  /\ UNCHANGED << tail, slotState, reserved, issued, published >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(EnqSPSC)
  \/ WithStep(ClaimTailMPSC)
  \/ WithStep(PublishClaim)
  \/ WithStep(PopSPSC)
  \/ WithStep(PopMPSC)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ Mode \in Modes
  /\ head \in 0..(2 * MaxSteps)
  /\ tail \in 0..(2 * MaxSteps)
  /\ slotState \in [Slots -> SlotStates]
  /\ slotSeq \in [Slots -> 0..(2 * MaxSteps + 1)]
  /\ slotTicket \in [Slots -> 0..NoneTicket]
  /\ reserved \subseteq Slots
  /\ issued \in 0..(2 * MaxSteps)
  /\ published \in 0..(2 * MaxSteps)
  /\ consumed \in 0..(2 * MaxSteps)
  /\ steps \in 0..MaxSteps

QueueAccountingInv ==
  /\ IF Mode = "spsc"
        THEN
          /\ issued = published
          /\ published = consumed + ReadyCount
          /\ tail - head = ReadyCount
        ELSE
          /\ issued = published + ClaimedCount
          /\ published = consumed + ReadyCount
          /\ tail - head = ReadyCount + ClaimedCount

BoundedOccupancyInv ==
  /\ ReadyCount + ClaimedCount <= Capacity
  /\ head <= tail

SPSCNoClaimInv ==
  Mode = "spsc" => reserved = {}

TicketPlacementInv ==
  /\ IF Mode = "spsc"
        THEN
          /\ \A i \in Slots :
               (slotState[i] = "empty") <=> (slotTicket[i] = NoneTicket)
          /\ \A i \in Slots :
               (slotState[i] = "ready") => (slotTicket[i] < tail /\ Idx(slotTicket[i]) = i)
        ELSE
          /\ \A i \in reserved :
               /\ slotTicket[i] # NoneTicket
               /\ Idx(slotTicket[i]) = i
               /\ slotSeq[i] = 2 * RoundNo(slotTicket[i])
          /\ \A i \in Slots :
               MPSCReady(slotSeq, slotTicket, reserved, i) =>
                 /\ slotTicket[i] < tail
                 /\ Idx(slotTicket[i]) = i
          /\ \A i \in Slots :
               /\ i \notin reserved
               /\ slotSeq[i] % 2 = 0
               => slotTicket[i] = NoneTicket
  /\ \A i, j \in BusySlots : i # j => slotTicket[i] # slotTicket[j]

=============================================================================
