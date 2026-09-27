---------------------------- MODULE Reconciliation ----------------------------
(* Reconciliation of the local order and position state with the venue (docs/architecture.md *)
(* section 15). The venue is a process whose orders change over time: an order opens, fills   *)
(* one unit at a time (trade <<o, n>>), and finishes (filled or canceled). Every change takes  *)
(* the venue's next time and emits a user data stream message carrying the order's status and *)
(* fill count after it. The network reorders and duplicates messages while the stream is up   *)
(* and loses what is in flight, or sent, while it is down. A snapshot can be taken at any      *)
(* moment and shows the venue as of its time T_s.                                             *)
(*                                                                                            *)
(* The client subscribes first and buffers, then asks for the snapshot, sets its state from   *)
(* it, applies the buffered messages newer than T_s, and only then trades. In Synced it       *)
(* applies messages as they come: a trade is counted once (by id), a status only when newer   *)
(* than the last one applied to that order. The local position is kept as a counter, so a     *)
(* double count would show. A disconnect sends it back to the start.                          *)
EXTENDS Naturals, FiniteSets

CONSTANTS Orders, MaxFill

Statuses == {"none", "open", "done"}
Trades == Orders \X (1..MaxFill)
Messages == [o : Orders, n : 0..MaxFill, st : Statuses, f : 0..MaxFill, t : Nat]
NoSnap == [valid |-> FALSE, ts |-> 0, st |-> [o \in Orders |-> "none"],
           f |-> [o \in Orders |-> 0]]

VARIABLES xtime, xst, xf,                       \* the venue: clock, status, fills per order
          up, chan,                              \* the stream connection, messages in flight
          phase, buffer, snap,                   \* the client's session
          lst, lf, lts, lpos, seen, trading      \* the client's state

venue == <<xtime, xst, xf>>
net == <<up, chan>>
session == <<phase, buffer, snap>>
local == <<lst, lf, lts, lpos, seen, trading>>
vars == <<venue, net, session, local>>

Sum(f) == LET S[s \in SUBSET Orders] ==
                IF s = {} THEN 0 ELSE LET o == CHOOSE x \in s : TRUE IN f[o] + S[s \ {o}]
          IN S[Orders]

Init == /\ xtime = 0 /\ xst = [o \in Orders |-> "none"] /\ xf = [o \in Orders |-> 0]
        /\ up = FALSE /\ chan = {}
        /\ phase = "Disconnected" /\ buffer = {} /\ snap = NoSnap
        /\ lst = [o \in Orders |-> "none"] /\ lf = [o \in Orders |-> 0]
        /\ lts = [o \in Orders |-> 0] /\ lpos = 0 /\ seen = {} /\ trading = "Halted"

\* ---- the venue and the network ----------------------------------------------------------

\* A change to order o: its new status and fill count, emitted as a message when the stream
\* is up (a message sent while it is down is lost).
Change(o, st, f, n) ==
  /\ xtime' = xtime + 1
  /\ xst' = [xst EXCEPT ![o] = st]
  /\ xf' = [xf EXCEPT ![o] = f]
  /\ chan' = IF up THEN chan \cup {[o |-> o, n |-> n, st |-> st, f |-> f, t |-> xtime + 1]}
             ELSE chan
  /\ UNCHANGED <<up, session, local>>

Open(o) == xst[o] = "none" /\ Change(o, "open", 0, 0)
Fill(o) == /\ xst[o] = "open" /\ xf[o] < MaxFill
           /\ Change(o, IF xf[o] + 1 = MaxFill THEN "done" ELSE "open", xf[o] + 1, xf[o] + 1)
Cancel(o) == xst[o] = "open" /\ Change(o, "done", xf[o], 0)

\* The stream drops: in-flight messages are lost and the session starts over, halted.
Disconnect ==
  /\ up
  /\ up' = FALSE /\ chan' = {}
  /\ phase' = "Disconnected" /\ buffer' = {} /\ snap' = NoSnap /\ trading' = "Halted"
  /\ UNCHANGED <<venue, lst, lf, lts, lpos, seen>>

\* ---- the client ---------------------------------------------------------------------------

\* A message applied to the local state: a trade once, a status only when newer.
ApplyMsg(m, s) ==
  LET fresh == m.n > 0 /\ <<m.o, m.n>> \notin s.seen
      newer == m.t > s.lts[m.o]
  IN [lst |-> IF newer THEN [s.lst EXCEPT ![m.o] = m.st] ELSE s.lst,
      lts |-> IF newer THEN [s.lts EXCEPT ![m.o] = m.t] ELSE s.lts,
      lf |-> IF fresh THEN [s.lf EXCEPT ![m.o] = @ + 1] ELSE s.lf,
      lpos |-> IF fresh THEN s.lpos + 1 ELSE s.lpos,
      seen |-> IF fresh THEN s.seen \cup {<<m.o, m.n>>} ELSE s.seen]

\* The buffered messages newer than the snapshot, applied oldest first (the implementation sorts
\* them by the venue's time); older ones are already in the snapshot.
RECURSIVE ApplyAll(_, _)
ApplyAll(ms, s) ==
  IF ms = {} THEN s
  ELSE LET m == CHOOSE x \in ms : \A y \in ms : x.t <= y.t
       IN ApplyAll(ms \ {m}, ApplyMsg(m, s))

Connect ==
  /\ ~up /\ phase = "Disconnected"
  /\ up' = TRUE /\ phase' = "Buffering"
  /\ UNCHANGED <<venue, chan, buffer, snap, local>>

\* Delivery of one message; `keep` leaves a copy in flight (a duplicate comes later).
Deliver(m, keep) ==
  /\ up /\ m \in chan
  /\ chan' = IF keep THEN chan ELSE chan \ {m}
  /\ IF phase = "Synced"
       THEN LET s == ApplyMsg(m, [lst |-> lst, lts |-> lts, lf |-> lf, lpos |-> lpos,
                                  seen |-> seen])
            IN /\ lst' = s.lst /\ lts' = s.lts /\ lf' = s.lf /\ lpos' = s.lpos
               /\ seen' = s.seen /\ UNCHANGED buffer
       ELSE /\ buffer' = buffer \cup {m}
            /\ UNCHANGED <<lst, lts, lf, lpos, seen>>
  /\ UNCHANGED <<venue, up, phase, snap, trading>>

RequestSnapshot ==
  /\ phase = "Buffering"
  /\ phase' = "Snapshotting"
  /\ UNCHANGED <<venue, net, buffer, snap, local>>

\* The venue answers: its state at this moment, T_s = its clock.
SnapshotTaken ==
  /\ phase = "Snapshotting" /\ ~snap.valid
  /\ snap' = [valid |-> TRUE, ts |-> xtime, st |-> xst, f |-> xf]
  /\ UNCHANGED <<venue, net, phase, buffer, local>>

\* Set from the snapshot, then the buffered messages newer than T_s; trading resumes.
Reconcile ==
  /\ phase = "Snapshotting" /\ snap.valid
  /\ LET base == [lst |-> snap.st, lts |-> [o \in Orders |-> snap.ts], lf |-> snap.f,
                  lpos |-> Sum(snap.f),
                  seen |-> {tr \in Trades : tr[2] <= snap.f[tr[1]]}]
         s == ApplyAll({m \in buffer : m.t > snap.ts}, base)
     IN /\ lst' = s.lst /\ lts' = s.lts /\ lf' = s.lf /\ lpos' = s.lpos /\ seen' = s.seen
  /\ phase' = "Synced" /\ buffer' = {} /\ trading' = "Active"
  /\ UNCHANGED <<venue, net, snap>>

Next == \/ \E o \in Orders : Open(o) \/ Fill(o) \/ Cancel(o)
        \/ Disconnect \/ Connect
        \/ \E m \in chan, keep \in BOOLEAN : Deliver(m, keep)
        \/ RequestSnapshot \/ SnapshotTaken \/ Reconcile

Spec == Init /\ [][Next]_vars

\* ---- properties -------------------------------------------------------------------------

TypeOK == /\ xst \in [Orders -> Statuses] /\ xf \in [Orders -> 0..MaxFill]
          /\ chan \subseteq Messages /\ buffer \subseteq Messages
          /\ phase \in {"Disconnected", "Buffering", "Snapshotting", "Synced"}
          /\ lst \in [Orders -> Statuses] /\ lf \in [Orders -> 0..MaxFill]
          /\ seen \subseteq Trades /\ trading \in {"Halted", "Active"}

\* Nothing is traded before the state is reconciled.
HaltedUntilSynced == phase # "Synced" => trading = "Halted"

\* No trade is counted twice: the position counter equals the distinct trades counted.
CountedOnce == /\ lpos = Cardinality(seen)
               /\ \A o \in Orders : lf[o] = Cardinality({tr \in seen : tr[1] = o})

\* The local state is never ahead of the venue.
NoPhantom == phase = "Synced" => \A tr \in seen : tr[2] <= xf[tr[1]]

\* Synced, with every message delivered, the local state is the venue's: no fill and no open
\* order missed.
Converged == phase = "Synced" /\ chan = {} =>
               /\ lst = xst /\ lf = xf /\ lpos = Sum(xf)

\* A bound on the venue's clock for model checking (each order changes at most MaxFill + 2 times).
Bounded == xtime <= Cardinality(Orders) * (MaxFill + 2)
=============================================================================
