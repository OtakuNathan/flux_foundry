---- MODULE SpmcDequeState ----
EXTENDS Naturals, FiniteSets

\* Abstracts spmc_deque owner/thief slot-state protocol:
\* empty / private / shared / claimed

CONSTANTS Capacity, MaxSteps

SlotStates == {"empty", "private", "shared", "claimed"}
Slots == 0..(Capacity - 1)
NoneTicket == 2 * MaxSteps + 1
Idx(x) == x % Capacity
RoundNo(x) == x \div Capacity
CountState(slots, st) == Cardinality({ i \in Slots : slots[i] = st })

VARIABLES h, t, slots, slotTicket, slotRound, ownerPops, thiefPops, ownerPushes, steps

vars == << h, t, slots, slotTicket, slotRound, ownerPops, thiefPops, ownerPushes, steps >>

PrivateCount == CountState(slots, "private")
SharedCount == CountState(slots, "shared")
ClaimedCount == CountState(slots, "claimed")
BusySlots == { i \in Slots : slots[i] # "empty" }
VisibleCount == PrivateCount + SharedCount
OccupiedCount == VisibleCount + ClaimedCount

Init ==
  /\ h = 0
  /\ t = 0
  /\ slots = [i \in Slots |-> "empty"]
  /\ slotTicket = [i \in Slots |-> NoneTicket]
  /\ slotRound = [i \in Slots |-> 0]
  /\ ownerPops = 0
  /\ thiefPops = 0
  /\ ownerPushes = 0
  /\ steps = 0

OwnerPush ==
  /\ slots[Idx(t)] = "empty"
  /\ LET prev == t - 1 IN
     /\ slots' =
         IF t > h /\ slots[Idx(prev)] = "private"
            THEN [ [slots EXCEPT ![Idx(prev)] = "shared"] EXCEPT ![Idx(t)] = "private" ]
            ELSE [ slots EXCEPT ![Idx(t)] = "private" ]
     /\ slotTicket' = [slotTicket EXCEPT ![Idx(t)] = t]
     /\ slotRound' = [slotRound EXCEPT ![Idx(t)] = RoundNo(t)]
  /\ t' = t + 1
  /\ ownerPushes' = ownerPushes + 1
  /\ UNCHANGED << h, ownerPops, thiefPops >>

OwnerPopPrivate ==
  /\ t > h
  /\ slots[Idx(t - 1)] = "private"
  /\ slotTicket[Idx(t - 1)] = t - 1
  /\ LET t1 == t - 1 IN
     LET slots1 == [slots EXCEPT ![Idx(t - 1)] = "empty"] IN
     LET slots2 ==
         IF t1 > h /\ slots1[Idx(t1 - 1)] = "shared"
            THEN [slots1 EXCEPT ![Idx(t1 - 1)] = "private"]
            ELSE slots1
     IN
       /\ slots' = slots2
       /\ slotTicket' = [slotTicket EXCEPT ![Idx(t - 1)] = NoneTicket]
       /\ slotRound' = [slotRound EXCEPT ![Idx(t - 1)] = RoundNo(t1)]
       /\ t' = t1
  /\ ownerPops' = ownerPops + 1
  /\ UNCHANGED << h, thiefPops, ownerPushes >>

\* Owner can also pop a shared back slot if it wins the CAS race.
OwnerPopSharedWin ==
  /\ t > h
  /\ slots[Idx(t - 1)] = "shared"
  /\ slotTicket[Idx(t - 1)] = t - 1
  /\ LET t1 == t - 1 IN
     LET slots1 == [slots EXCEPT ![Idx(t - 1)] = "empty"] IN
     LET slots2 ==
         IF t1 > h /\ slots1[Idx(t1 - 1)] = "shared"
            THEN [slots1 EXCEPT ![Idx(t1 - 1)] = "private"]
            ELSE slots1
     IN
       /\ slots' = slots2
       /\ slotTicket' = [slotTicket EXCEPT ![Idx(t - 1)] = NoneTicket]
       /\ slotRound' = [slotRound EXCEPT ![Idx(t - 1)] = RoundNo(t1)]
       /\ t' = t1
  /\ ownerPops' = ownerPops + 1
  /\ UNCHANGED << h, thiefPops, ownerPushes >>

ThiefClaimFront ==
  /\ h < t
  /\ slots[Idx(h)] = "shared"
  /\ slotTicket[Idx(h)] = h
  /\ slotRound[Idx(h)] = RoundNo(h)
  /\ slots' = [slots EXCEPT ![Idx(h)] = "claimed"]
  /\ h' = h + 1
  /\ UNCHANGED << t, slotTicket, slotRound, ownerPops, thiefPops, ownerPushes >>

ThiefFinishFront ==
  /\ \E i \in Slots : slots[i] = "claimed"
  /\ \E i \in Slots :
       /\ slots[i] = "claimed"
       /\ slots' = [slots EXCEPT ![i] = "empty"]
       /\ slotTicket' = [slotTicket EXCEPT ![i] = NoneTicket]
       /\ slotRound' = [slotRound EXCEPT ![i] = slotRound[i] + 1]
  /\ thiefPops' = thiefPops + 1
  /\ UNCHANGED << h, t, ownerPops, ownerPushes >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(OwnerPush)
  \/ WithStep(OwnerPopPrivate)
  \/ WithStep(OwnerPopSharedWin)
  \/ WithStep(ThiefClaimFront)
  \/ WithStep(ThiefFinishFront)

Spec == Init /\ [][Next]_vars

TypeInv ==
  /\ h \in 0..(2 * MaxSteps)
  /\ t \in 0..(2 * MaxSteps)
  /\ slots \in [Slots -> SlotStates]
  /\ slotTicket \in [Slots -> 0..NoneTicket]
  /\ slotRound \in [Slots -> 0..(2 * MaxSteps)]
  /\ ownerPops \in 0..MaxSteps
  /\ thiefPops \in 0..MaxSteps
  /\ ownerPushes \in 0..MaxSteps
  /\ steps \in 0..MaxSteps

AccountingInv ==
  /\ t - h = VisibleCount
  /\ ownerPushes = ownerPops + thiefPops + VisibleCount + ClaimedCount

BoundedInv ==
  /\ OccupiedCount <= Capacity
  /\ h <= t
  /\ PrivateCount <= 1

BackPrivatePlacementInv ==
  /\ (PrivateCount = 1) => slots[Idx(t - 1)] = "private"

ClaimOnlyFromSharedInv ==
  /\ ClaimedCount <= Capacity

TicketPlacementInv ==
  /\ \A i \in Slots :
       (slots[i] = "empty") <=> (slotTicket[i] = NoneTicket)
  /\ \A i \in Slots :
       (slots[i] # "empty") =>
         /\ slotTicket[i] # NoneTicket
         /\ Idx(slotTicket[i]) = i
         /\ slotRound[i] = RoundNo(slotTicket[i])
  /\ \A i, j \in BusySlots : i # j => slotTicket[i] # slotTicket[j]

FrontBackTicketInv ==
  /\ (t > h /\ slots[Idx(t - 1)] = "private") => slotTicket[Idx(t - 1)] = t - 1
  /\ (h < t /\ slots[Idx(h)] \in {"shared", "claimed"}) => slotTicket[Idx(h)] <= h
  /\ \A i \in Slots : slotRound[i] <= ownerPushes

=============================================================================
