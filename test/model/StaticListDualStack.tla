---- MODULE StaticListDualStack ----
EXTENDS Naturals, Sequences, FiniteSets

\* Closer-to-code abstract model for utility/static_stack.h:
\* - explicit tagged heads: head_ / free_
\* - explicit per-node next tag (nodes[i].next)
\* - successful pop_from_list / append_to_list linearization steps
\* - transient reservations between pop and append (push path / pop path)
\*
\* Still abstracted:
\* - CAS retry loops/backoff (we model only successful state transitions)
\* - object construction/destruction payloads
\* - packed-bit encoding of tag value (modeled as record {seq,off})
\* - seq wraparound (bounded by MaxSteps in this TLC model)

CONSTANTS Capacity, MaxSteps

Nodes == 0..(Capacity - 1)
NoneOff == Capacity
MaxSeq == MaxSteps + 1

TagOffs == 0..NoneOff
TagSeqs == 0..MaxSeq

MkTag(s, o) == [seq |-> s, off |-> o]
EmptyTag == MkTag(0, NoneOff)

IsEmptyTag(t) == t.off = NoneOff
TagOff(t) == t.off
TagSeq(t) == t.seq

SeqContainsNoDup(s) == \A i, j \in 1..Len(s) : i # j => s[i] # s[j]
PresentInSeq(n, s) == \E i \in 1..Len(s) : s[i] = n

RECURSIVE PathNodes(_, _, _)
PathNodes(tag, next, k) ==
  IF k = 0 \/ IsEmptyTag(tag) THEN
    << >>
  ELSE
    << TagOff(tag) >> \o PathNodes(next[TagOff(tag)], next, k - 1)

RECURSIVE PathTagsMatch(_, _, _, _)
PathTagsMatch(tag, next, seqCurr, k) ==
  IF k = 0 \/ IsEmptyTag(tag) THEN
    TRUE
  ELSE
    /\ tag.seq = seqCurr[TagOff(tag)]
    /\ PathTagsMatch(next[TagOff(tag)], next, seqCurr, k - 1)

VARIABLES headTag,
          freeTag,
          nextTag,
          resPushTag,
          resPopTag,
          seqCurr,
          pushPublishes,
          popPublishes,
          steps

vars ==
  << headTag, freeTag, nextTag, resPushTag, resPopTag, seqCurr,
     pushPublishes, popPublishes, steps >>

HeadNodes == PathNodes(headTag, nextTag, Capacity)
FreeNodes == PathNodes(freeTag, nextTag, Capacity)

HeadCount == Len(HeadNodes)
FreeCount == Len(FreeNodes)
ResPushCount == IF IsEmptyTag(resPushTag) THEN 0 ELSE 1
ResPopCount == IF IsEmptyTag(resPopTag) THEN 0 ELSE 1

Init ==
  /\ Capacity \in Nat \ {0}
  /\ headTag = EmptyTag
  /\ freeTag = MkTag(0, 0)
  /\ nextTag =
       [n \in Nodes |->
          IF n = Capacity - 1 THEN EmptyTag ELSE MkTag(0, n + 1)]
  /\ resPushTag = EmptyTag
  /\ resPopTag = EmptyTag
  /\ seqCurr = [n \in Nodes |-> 0]
  /\ pushPublishes = 0
  /\ popPublishes = 0
  /\ steps = 0

\* push()/emplace(): pop_from_list(free_)
AcquireFromFree ==
  /\ IsEmptyTag(resPushTag)
  /\ ~IsEmptyTag(freeTag)
  /\ LET t == freeTag IN
     /\ resPushTag' = t
     /\ freeTag' = nextTag[TagOff(t)]
  /\ UNCHANGED << headTag, nextTag, resPopTag, seqCurr, pushPublishes, popPublishes >>

\* push()/emplace(): append_to_list(head_, make_seq(seq+1, offset))
PublishToHead ==
  /\ ~IsEmptyTag(resPushTag)
  /\ LET t == resPushTag IN
     LET n == TagOff(t) IN
     LET s == TagSeq(t) IN
     /\ s < MaxSeq
     /\ nextTag' = [nextTag EXCEPT ![n] = headTag]
     /\ seqCurr' = [seqCurr EXCEPT ![n] = s + 1]
     /\ headTag' = MkTag(s + 1, n)
     /\ resPushTag' = EmptyTag
  /\ pushPublishes' = pushPublishes + 1
  /\ UNCHANGED << freeTag, resPopTag, popPublishes >>

\* pop(): pop_from_list(head_)
AcquireFromHead ==
  /\ IsEmptyTag(resPopTag)
  /\ ~IsEmptyTag(headTag)
  /\ LET t == headTag IN
     /\ resPopTag' = t
     /\ headTag' = nextTag[TagOff(t)]
  /\ UNCHANGED << freeTag, nextTag, resPushTag, seqCurr, pushPublishes, popPublishes >>

\* pop(): append_to_list(free_, seq)
PublishToFree ==
  /\ ~IsEmptyTag(resPopTag)
  /\ LET t == resPopTag IN
     LET n == TagOff(t) IN
     /\ nextTag' = [nextTag EXCEPT ![n] = freeTag]
     /\ freeTag' = t
     /\ resPopTag' = EmptyTag
  /\ popPublishes' = popPublishes + 1
  /\ UNCHANGED << headTag, resPushTag, seqCurr, pushPublishes >>

WithStep(A) ==
  /\ steps < MaxSteps
  /\ A
  /\ steps' = steps + 1

Next ==
  \/ WithStep(AcquireFromFree)
  \/ WithStep(PublishToHead)
  \/ WithStep(AcquireFromHead)
  \/ WithStep(PublishToFree)

Spec == Init /\ [][Next]_vars

AllNodesPartitioned ==
  \A n \in Nodes :
      (IF PresentInSeq(n, HeadNodes) THEN 1 ELSE 0)
    + (IF PresentInSeq(n, FreeNodes) THEN 1 ELSE 0)
    + (IF ~IsEmptyTag(resPushTag) /\ TagOff(resPushTag) = n THEN 1 ELSE 0)
    + (IF ~IsEmptyTag(resPopTag) /\ TagOff(resPopTag) = n THEN 1 ELSE 0)
    = 1

TypeInv ==
  /\ Capacity \in Nat \ {0}
  /\ headTag \in [seq : TagSeqs, off : TagOffs]
  /\ freeTag \in [seq : TagSeqs, off : TagOffs]
  /\ nextTag \in [Nodes -> [seq : TagSeqs, off : TagOffs]]
  /\ resPushTag \in [seq : TagSeqs, off : TagOffs]
  /\ resPopTag \in [seq : TagSeqs, off : TagOffs]
  /\ seqCurr \in [Nodes -> TagSeqs]
  /\ pushPublishes \in 0..MaxSteps
  /\ popPublishes \in 0..MaxSteps
  /\ steps \in 0..MaxSteps

ListShapeInv ==
  /\ SeqContainsNoDup(HeadNodes)
  /\ SeqContainsNoDup(FreeNodes)
  /\ HeadCount <= Capacity
  /\ FreeCount <= Capacity

PartitionInv ==
  /\ AllNodesPartitioned
  /\ HeadCount + FreeCount + ResPushCount + ResPopCount = Capacity

AccountingInv ==
  /\ pushPublishes = popPublishes + HeadCount + ResPopCount

SeqTagBoundInv ==
  /\ \A n \in Nodes : seqCurr[n] <= pushPublishes

ReservationExclusionInv ==
  /\ ~(~IsEmptyTag(resPushTag) /\ ~IsEmptyTag(resPopTag) /\ TagOff(resPushTag) = TagOff(resPopTag))
  /\ ~(~IsEmptyTag(resPopTag) /\ PresentInSeq(TagOff(resPopTag), HeadNodes))
  /\ ~(~IsEmptyTag(resPushTag) /\ PresentInSeq(TagOff(resPushTag), FreeNodes))

TagConsistencyInv ==
  /\ PathTagsMatch(headTag, nextTag, seqCurr, Capacity + 1)
  /\ PathTagsMatch(freeTag, nextTag, seqCurr, Capacity + 1)
  /\ (~IsEmptyTag(resPushTag) => TagSeq(resPushTag) = seqCurr[TagOff(resPushTag)])
  /\ (~IsEmptyTag(resPopTag) => TagSeq(resPopTag) = seqCurr[TagOff(resPopTag)])

=============================================================================
