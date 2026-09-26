------------------------------- MODULE SeqOrder -------------------------------
(* M0 placeholder spec. The core thread assigns a strictly increasing seq to the events it   *)
(* dequeues from several SPSC rings (docs/architecture.md section 5.2). It is the smallest  *)
(* end-to-end check of the formal CI job until the real specs arrive in M3.                 *)
EXTENDS Naturals, Sequences

CONSTANTS Rings, PerRing

VARIABLES produced, rings, log

vars == <<produced, rings, log>>

Init == /\ produced = [r \in Rings |-> 0]
        /\ rings = [r \in Rings |-> <<>>]
        /\ log = <<>>

Produce(r) == /\ produced[r] < PerRing
              /\ produced' = [produced EXCEPT ![r] = @ + 1]
              /\ rings' = [rings EXCEPT ![r] = Append(@, <<r, produced[r] + 1>>)]
              /\ UNCHANGED log

Dequeue(r) == /\ rings[r] # <<>>
              /\ log' = Append(log, [seq |-> Len(log) + 1, event |-> Head(rings[r])])
              /\ rings' = [rings EXCEPT ![r] = Tail(@)]
              /\ UNCHANGED produced

Next == \E r \in Rings : Produce(r) \/ Dequeue(r)

Spec == Init /\ [][Next]_vars /\ \A r \in Rings : WF_vars(Dequeue(r))

SeqContiguous == \A i \in 1..Len(log) : log[i].seq = i

NoDuplicates == \A i, j \in 1..Len(log) : i # j => log[i].event # log[j].event

PerRingFifo == \A i, j \in 1..Len(log) :
                 (i < j /\ log[i].event[1] = log[j].event[1]) => log[i].event[2] < log[j].event[2]

Drained == <>[](\A r \in Rings : rings[r] = <<>>)
===============================================================================
