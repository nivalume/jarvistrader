------------------------------ MODULE DepthSync ------------------------------
(* Order book synchronization of a Binance USDⓈ-M depth stream (docs/architecture.md section *)
(* 14.3; jarvis/adapter/binance/depth_sync.hpp). The exchange's book is modeled abstractly:  *)
(* a function from price to size, changed by numbered updates. The stream publishes the      *)
(* accumulated changes as events [U, u, pu, ch] (first and last update id, the previous      *)
(* event's u, the new absolute sizes); the network may lose events and the connection may   *)
(* drop. A REST snapshot [L, book] may be served at any past update id (a stale replica).    *)
(*                                                                                           *)
(* The client (the implementation) buffers events, asks for a snapshot, drops events with    *)
(* u < L, needs the first applied event to satisfy U <= L <= u and every later one to have   *)
(* pu equal to the previous u; any break sends it back to Buffering. The downstream view     *)
(* (`visible`, `out`) is what the kernel has been told through OrderBookDeltas.               *)
EXTENDS Naturals, Sequences

CONSTANTS Prices, MaxQty, MaxSeq

Books == [Prices -> 0..MaxQty]
Empty == [p \in Prices |-> 0]
NoChange == [p \in {} |-> 0]
NoSnap == [valid |-> FALSE, L |-> 0, book |-> Empty]

VARIABLES xbook, xseq, hist, pending, pendingFirst, lastPub,   \* the exchange
          connected, channel, inflight,                          \* the network
          phase, local, lastU, snapL, buffer, visible, out       \* the client

exchange == <<xbook, xseq, hist, pending, pendingFirst, lastPub>>
network == <<connected, channel, inflight>>
client == <<phase, local, lastU, snapL, buffer, visible, out>>
vars == <<exchange, network, client>>

ApplyEv(b, e) == [p \in Prices |-> IF p \in DOMAIN e.ch THEN e.ch[p] ELSE b[p]]

RECURSIVE ApplyN(_, _, _)
ApplyN(b, s, k) == IF k = 0 THEN b ELSE ApplyEv(ApplyN(b, s, k - 1), s[k])

\* The length of the longest prefix of s whose events chain (pu = previous u).
ChainLen(s) == CHOOSE k \in 1..Len(s) :
                 /\ \A i \in 2..k : s[i].pu = s[i - 1].u
                 /\ k = Len(s) \/ s[k + 1].pu # s[k].u

Init == /\ xbook \in Books
        /\ xseq = 0
        /\ hist = [i \in 0..MaxSeq |-> xbook]
        /\ pending = NoChange
        /\ pendingFirst = 0
        /\ lastPub = 0
        /\ connected = FALSE
        /\ channel = <<>>
        /\ inflight = NoSnap
        /\ phase = "Idle"
        /\ local = Empty
        /\ lastU = 0
        /\ snapL = 0
        /\ buffer = <<>>
        /\ visible = FALSE
        /\ out = Empty

\* ---- the exchange and the network ------------------------------------------------------

Update(p, v) ==
  /\ xseq < MaxSeq
  /\ v # xbook[p]
  /\ xbook' = [xbook EXCEPT ![p] = v]
  /\ xseq' = xseq + 1
  /\ hist' = [hist EXCEPT ![xseq + 1] = [xbook EXCEPT ![p] = v]]
  /\ pending' = [q \in DOMAIN pending \cup {p} |-> IF q = p THEN v ELSE pending[q]]
  /\ pendingFirst' = IF DOMAIN pending = {} THEN xseq + 1 ELSE pendingFirst
  /\ UNCHANGED <<lastPub, network, client>>

Publish ==
  /\ DOMAIN pending # {}
  /\ LET e == [U |-> pendingFirst, u |-> xseq, pu |-> lastPub, ch |-> pending]
     IN channel' = IF connected THEN Append(channel, e) ELSE channel
  /\ lastPub' = xseq
  /\ pending' = NoChange
  /\ UNCHANGED <<xbook, xseq, hist, pendingFirst, connected, inflight, client>>

Lose ==
  /\ channel # <<>>
  /\ channel' = Tail(channel)
  /\ UNCHANGED <<exchange, connected, inflight, client>>

Serve(l) ==
  /\ phase = "Requested"
  /\ ~inflight.valid
  /\ inflight' = [valid |-> TRUE, L |-> l, book |-> hist[l]]
  /\ UNCHANGED <<exchange, connected, channel, client>>

\* ---- the client ---------------------------------------------------------------------------

Enter(b, u) == /\ phase' = "Synced" /\ local' = b /\ lastU' = u /\ snapL' = snapL
               /\ buffer' = <<>> /\ visible' = TRUE /\ out' = b

Resync(buf) == /\ phase' = "Buffering" /\ local' = Empty /\ lastU' = 0 /\ snapL' = 0
               /\ buffer' = buf /\ visible' = FALSE /\ out' = Empty

Connect ==
  /\ phase = "Idle"
  /\ connected' = TRUE
  /\ phase' = "Buffering"
  /\ UNCHANGED <<exchange, channel, inflight, local, lastU, snapL, buffer, visible, out>>

Disconnect ==
  /\ phase # "Idle"
  /\ connected' = FALSE
  /\ channel' = <<>>
  /\ inflight' = NoSnap
  /\ phase' = "Idle"
  /\ local' = Empty
  /\ lastU' = 0
  /\ snapL' = 0
  /\ buffer' = <<>>
  /\ visible' = FALSE
  /\ out' = Empty
  /\ UNCHANGED exchange

Request ==
  /\ phase = "Buffering"
  /\ phase' = "Requested"
  /\ UNCHANGED <<exchange, network, local, lastU, snapL, buffer, visible, out>>

Receive ==
  /\ channel # <<>>
  /\ channel' = Tail(channel)
  /\ LET e == Head(channel) IN
     CASE phase \in {"Buffering", "Requested"} ->
            /\ buffer' = Append(buffer, e)
            /\ UNCHANGED <<phase, local, lastU, snapL, visible, out>>
       [] phase = "Validating" ->
            IF e.u < snapL THEN UNCHANGED client
            ELSE IF e.U <= snapL THEN Enter(ApplyEv(local, e), e.u)
            ELSE Resync(<<e>>)                                    \* the snapshot is stale
       [] phase = "Synced" ->
            IF e.pu = lastU
              THEN /\ local' = ApplyEv(local, e)
                   /\ lastU' = e.u
                   /\ out' = ApplyEv(local, e)
                   /\ UNCHANGED <<phase, snapL, buffer, visible>>
              ELSE Resync(<<e>>)                                  \* a gap
  /\ UNCHANGED <<exchange, connected, inflight>>

Arrive ==
  /\ phase = "Requested"
  /\ inflight.valid
  /\ inflight' = NoSnap
  /\ LET L == inflight.L
         kept == SelectSeq(buffer, LAMBDA e : e.u >= L)
     IN IF kept = <<>>
          THEN /\ phase' = "Validating" /\ local' = inflight.book /\ lastU' = L /\ snapL' = L
               /\ buffer' = <<>> /\ UNCHANGED <<visible, out>>
        ELSE IF kept[1].U > L
          THEN Resync(kept)                                       \* the snapshot is stale
        ELSE LET k == ChainLen(kept) IN
             IF k = Len(kept)
               THEN /\ Enter(ApplyN(inflight.book, kept, k), kept[k].u)
               ELSE Resync(SubSeq(kept, k + 1, Len(kept)))        \* a gap in the buffer
  /\ UNCHANGED <<exchange, connected, channel>>

Next == \/ \E p \in Prices, v \in 0..MaxQty : Update(p, v)
        \/ Publish
        \/ Lose
        \/ \E l \in 0..MaxSeq : l <= xseq /\ Serve(l)
        \/ Connect
        \/ Disconnect
        \/ Request
        \/ Receive
        \/ Arrive

Spec == Init /\ [][Next]_vars

\* ---- properties ----------------------------------------------------------------------------

TypeOK == /\ phase \in {"Idle", "Buffering", "Requested", "Validating", "Synced"}
          /\ local \in Books /\ out \in Books /\ xbook \in Books
          /\ lastU \in 0..MaxSeq /\ snapL \in 0..MaxSeq /\ visible \in BOOLEAN

\* In sync, the local book is the exchange's book as of the last applied update.
SyncedMatchesExchange == phase = "Synced" => /\ lastU <= xseq
                                              /\ local = hist[lastU]

\* After a snapshot and before the first event, the local book is the snapshot.
ValidatingHoldsSnapshot == phase = "Validating" => local = hist[snapL]

\* The kernel sees a book only while the client is in sync, and then exactly the local book.
KernelSeesOnlySynced == /\ visible => (phase = "Synced" /\ out = local)
                        /\ phase = "Synced" => visible

=============================================================================
