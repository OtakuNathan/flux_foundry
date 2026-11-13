---- MODULE UvExecutorClose ----
EXTENDS Naturals, Sequences, FiniteSets

\* Queued dispatch / close protocol, with native send and return split apart.
\* Slots are a FIFO of claimed producer IDs; reserve and publish are separate.
\* Atomics are successful abstract transitions, not a C++ memory-model proof.
CONSTANTS Producers, Capacity, Budget
ASSUME /\ Producers # {} /\ Capacity > 0 /\ Budget > 0

ProducerStates == {"idle", "admitted", "reserved", "claimed", "published",
                   "sending", "returned", "done", "rejected"}
Active == ProducerStates \ {"idle", "done", "rejected"}
CloserStates == {"idle", "acquired", "sending", "returned", "done"}

VARIABLES pc, closing, tickets, closer, count, queue, published, accepted,
          executions, wake, owner, budget, handle, badSend
vars == <<pc, closing, tickets, closer, count, queue, published, accepted,
          executions, wake, owner, budget, handle, badSend>>

Ready == IF Len(queue) = 0 THEN FALSE ELSE Head(queue) \in published
Finished == {p \in Producers : executions[p] = 1}

Init ==
  /\ pc = [p \in Producers |-> "idle"]
  /\ closing = FALSE /\ tickets = 0 /\ closer = "idle"
  /\ count = 0 /\ queue = <<>> /\ published = {} /\ accepted = {}
  /\ executions = [p \in Producers |-> 0]
  /\ wake = FALSE /\ owner = "idle" /\ budget = 0
  /\ handle = "open" /\ badSend = FALSE

Admit(p) ==
  /\ pc[p] = "idle" /\ ~closing
  /\ pc' = [pc EXCEPT ![p] = "admitted"] /\ tickets' = tickets + 1
  /\ UNCHANGED <<closing, closer, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

RejectClosed(p) ==
  /\ pc[p] = "idle" /\ closing
  /\ pc' = [pc EXCEPT ![p] = "rejected"]
  /\ UNCHANGED <<closing, tickets, closer, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

Reserve(p) ==
  /\ pc[p] = "admitted" /\ count < Capacity
  /\ pc' = [pc EXCEPT ![p] = "reserved"] /\ count' = count + 1
  /\ accepted' = accepted \cup {p}
  /\ UNCHANGED <<closing, tickets, closer, queue, published,
                 executions, wake, owner, budget, handle, badSend>>

RejectFull(p) ==
  /\ pc[p] = "admitted" /\ count = Capacity
  /\ pc' = [pc EXCEPT ![p] = "rejected"] /\ tickets' = tickets - 1
  /\ UNCHANGED <<closing, closer, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

Claim(p) ==
  /\ pc[p] = "reserved"
  /\ pc' = [pc EXCEPT ![p] = "claimed"] /\ queue' = Append(queue, p)
  /\ UNCHANGED <<closing, tickets, closer, count, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

Publish(p) ==
  /\ pc[p] = "claimed"
  /\ pc' = [pc EXCEPT ![p] = "published"] /\ published' = published \cup {p}
  /\ UNCHANGED <<closing, tickets, closer, count, queue, accepted,
                 executions, wake, owner, budget, handle, badSend>>

\* Callback may run after this action and before SendReturn/Release.
Send(p) ==
  /\ pc[p] = "published"
  /\ pc' = [pc EXCEPT ![p] = "sending"] /\ wake' = TRUE
  /\ badSend' = badSend \/ (handle # "open")
  /\ UNCHANGED <<closing, tickets, closer, count, queue, published, accepted,
                 executions, owner, budget, handle>>

SendReturn(p) ==
  /\ pc[p] = "sending"
  /\ pc' = [pc EXCEPT ![p] = "returned"]
  /\ UNCHANGED <<closing, tickets, closer, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

Release(p) ==
  /\ pc[p] = "returned"
  /\ pc' = [pc EXCEPT ![p] = "done"] /\ tickets' = tickets - 1
  /\ UNCHANGED <<closing, closer, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

\* The close notification ticket is acquired in the same CAS as the stop bit.
RequestClose ==
  /\ ~closing
  /\ closing' = TRUE /\ tickets' = tickets + 1 /\ closer' = "acquired"
  /\ UNCHANGED <<pc, count, queue, published, accepted, executions,
                 wake, owner, budget, handle, badSend>>

CloseSend ==
  /\ closer = "acquired"
  /\ closer' = "sending" /\ wake' = TRUE
  /\ badSend' = badSend \/ (handle # "open")
  /\ UNCHANGED <<pc, closing, tickets, count, queue, published, accepted,
                 executions, owner, budget, handle>>

CloseSendReturn ==
  /\ closer = "sending" /\ closer' = "returned"
  /\ UNCHANGED <<pc, closing, tickets, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

CloseRelease ==
  /\ closer = "returned" /\ closer' = "done" /\ tickets' = tickets - 1
  /\ UNCHANGED <<pc, closing, count, queue, published, accepted,
                 executions, wake, owner, budget, handle, badSend>>

StartCallback ==
  /\ handle = "open" /\ owner = "idle" /\ wake
  /\ wake' = FALSE /\ owner' = "drain" /\ budget' = Budget
  /\ UNCHANGED <<pc, closing, tickets, closer, count, queue, published,
                 accepted, executions, handle, badSend>>

Drain ==
  /\ owner = "drain" /\ budget > 0 /\ Ready
  /\ executions' = [executions EXCEPT ![Head(queue)] = @ + 1]
  /\ queue' = Tail(queue) /\ count' = count - 1 /\ budget' = budget - 1
  /\ UNCHANGED <<pc, closing, tickets, closer, published, accepted,
                 wake, owner, handle, badSend>>

EndDrain ==
  /\ owner = "drain" /\ (budget = 0 \/ ~Ready)
  /\ owner' = "check"
  /\ UNCHANGED <<pc, closing, tickets, closer, count, queue, published,
                 accepted, executions, wake, budget, handle, badSend>>

CheckClose ==
  /\ owner = "check"
  /\ owner' = "idle"
  /\ IF closing /\ tickets = 0 /\ count = 0
        THEN /\ handle' = "closing" /\ wake' = FALSE
        ELSE /\ handle' = handle /\ wake' = (wake \/ Ready \/ closing)
  /\ UNCHANGED <<pc, closing, tickets, closer, count, queue, published,
                 accepted, executions, budget, badSend>>

NativeClosed ==
  /\ handle = "closing" /\ handle' = "closed"
  /\ UNCHANGED <<pc, closing, tickets, closer, count, queue, published,
                 accepted, executions, wake, owner, budget, badSend>>

ProducerProgress(p) == Reserve(p) \/ RejectFull(p) \/ Claim(p) \/ Publish(p)
                       \/ Send(p) \/ SendReturn(p) \/ Release(p)
CloserProgress == CloseSend \/ CloseSendReturn \/ CloseRelease
OwnerProgress == StartCallback \/ Drain \/ EndDrain \/ CheckClose \/ NativeClosed
Next == RequestClose \/ CloserProgress \/ OwnerProgress
        \/ (\E p \in Producers : Admit(p) \/ RejectClosed(p) \/ ProducerProgress(p))

\* Fairness means admitted producers/closer and the host loop keep running.
\* No assumption that close is requested: progress is conditional on closing.
Spec == Init /\ [][Next]_vars
        /\ (\A p \in Producers : WF_vars(ProducerProgress(p)))
        /\ WF_vars(CloserProgress) /\ WF_vars(OwnerProgress)

TypeInv ==
  /\ pc \in [Producers -> ProducerStates] /\ closer \in CloserStates
  /\ closing \in BOOLEAN /\ tickets \in 0..(Cardinality(Producers) + 1)
  /\ count \in 0..Capacity /\ queue \in Seq(Producers)
  /\ published \subseteq Producers /\ accepted \subseteq Producers
  /\ executions \in [Producers -> 0..1]
  /\ wake \in BOOLEAN /\ owner \in {"idle", "drain", "check"}
  /\ budget \in 0..Budget /\ handle \in {"open", "closing", "closed"}
  /\ badSend \in BOOLEAN

TicketAccounting == tickets = Cardinality({p \in Producers : pc[p] \in Active})
                              + (IF closer \in {"acquired", "sending", "returned"} THEN 1 ELSE 0)
QueueAccounting ==
  /\ count = Len(queue) + Cardinality({p \in Producers : pc[p] = "reserved"})
  /\ count = Cardinality(accepted \ Finished)
  /\ Len(queue) = Cardinality({queue[i] : i \in 1..Len(queue)})
  /\ Finished \subseteq published /\ published \subseteq accepted
NoPrematureClose == handle # "open" =>
  /\ closing /\ tickets = 0 /\ count = 0 /\ owner = "idle"
  /\ accepted = Finished /\ closer = "done"
  /\ \A p \in Producers : pc[p] \notin Active
NoSendAfterClose == ~badSend
ShutdownCompletes == closing ~> (handle = "closed")
=============================================================================
