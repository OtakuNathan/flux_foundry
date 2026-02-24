---- MODULE MpmcQueueSeq ----
EXTENDS Naturals, FiniteSets

\* Abstracts mpmc_queue slot sequence protocol into slot-round + state machine.

CONSTANTS Capacity, MaxSteps

SlotStates == {"empty", "claimedProd", "full", "claimedCons"}
Slots == 0..(Capacity - 1)
NoneTicket == 2 * MaxSteps + 1

Idx(x) == x % Capacity
RoundNo(x) == x \div Capacity
CountState(slots, st) == Cardinality({ i \in Slots : slots[i] = st })

VARIABLES head, tail, slotState, slotRound, slotTicket, issued, published, consumed, steps

vars == << head, tail, slotState, slotRound, slotTicket, issued, published, consumed, steps >>

FullCount == CountState(slotState, "full")
ClaimedProdCount == CountState(slotState, "claimedProd")
ClaimedConsCount == CountState(slotState, "claimedCons")
BusySlots == { i \in Slots : slotState[i] # "empty" }

Init ==
  /\ head = 0
  /\ tail = 0
  /\ slotState = [i \in Slots |-> "empty"]
  /\ slotRound = [i \in Slots |-> 0]
  /\ slotTicket = [i \in Slots |-> NoneTicket]
  /\ issued = 0
  /\ published = 0
  /\ consumed = 0
  /\ steps = 0

ProdClaim ==
  /\ slotState[Idx(tail)] = "empty"
  /\ slotRound[Idx(tail)] = RoundNo(tail)
  /\ slotState' = [slotState EXCEPT ![Idx(tail)] = "claimedProd"]
  /\ slotTicket' = [slotTicket EXCEPT ![Idx(tail)] = tail]
  /\ tail' = tail + 1
  /\ issued' = issued + 1
  /\ UNCHANGED << head, slotRound, published, consumed >>

ProdPublish ==
  /\ \E i \in Slots : slotState[i] = "claimedProd"
  /\ \E i \in Slots :
       /\ slotState[i] = "claimedProd"
       /\ slotState' = [slotState EXCEPT ![i] = "full"]
       /\ UNCHANGED slotTicket
  /\ published' = published + 1
  /\ UNCHANGED << head, tail, slotRound, issued, consumed >>

ConsClaim ==
  /\ slotState[Idx(head)] = "full"
  /\ slotRound[Idx(head)] = RoundNo(head)
  /\ slotTicket[Idx(head)] = head
  /\ slotState' = [slotState EXCEPT ![Idx(head)] = "claimedCons"]
  /\ head' = head + 1
  /\ UNCHANGED << tail, slotRound, slotTicket, issued, published, consumed >>

ConsFinish ==
  /\ \E i \in Slots : slotState[i] = "claimedCons"
  /\ \E i \in Slots :
       /\ slotState[i] = "claimedCons"
       /\ slotState' = [slotState EXCEPT ![i] = "empty"]
       /\ slotRound' = [slotRound EXCEPT ![i] = slotRound[i] + 1]
       /\ slotTicket' = [slotTicket EXCEPT ![i] = NoneTicket]
  /\ consumed' = consumed + 1
  /\ UNCHANGED << head, tail, issued, published >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(ProdClaim)
  \/ WithStep(ProdPublish)
  \/ WithStep(ConsClaim)
  \/ WithStep(ConsFinish)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ head \in 0..(2 * MaxSteps)
  /\ tail \in 0..(2 * MaxSteps)
  /\ slotState \in [Slots -> SlotStates]
  /\ slotRound \in [Slots -> 0..(2 * MaxSteps)]
  /\ slotTicket \in [Slots -> 0..NoneTicket]
  /\ issued \in 0..(2 * MaxSteps)
  /\ published \in 0..(2 * MaxSteps)
  /\ consumed \in 0..(2 * MaxSteps)
  /\ steps \in 0..MaxSteps

AccountingInv ==
  /\ issued = published + ClaimedProdCount
  /\ published = consumed + FullCount + ClaimedConsCount
  /\ tail - head = FullCount + ClaimedProdCount

BoundedInv ==
  /\ FullCount + ClaimedProdCount + ClaimedConsCount <= Capacity
  /\ head <= tail

RoundBoundInv ==
  \A i \in Slots : slotRound[i] <= consumed

TicketPlacementInv ==
  /\ \A i \in Slots :
       (slotState[i] = "empty") <=> (slotTicket[i] = NoneTicket)
  /\ \A i \in Slots :
       (slotState[i] # "empty") =>
         /\ slotTicket[i] # NoneTicket
         /\ slotTicket[i] < tail
         /\ Idx(slotTicket[i]) = i
         /\ slotRound[i] = RoundNo(slotTicket[i])
  /\ \A i, j \in BusySlots : i # j => slotTicket[i] # slotTicket[j]

=============================================================================
