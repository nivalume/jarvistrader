----------------------- MODULE ReconciliationBehaviours -----------------------
(* Behaviours of Reconciliation for forward trace validation (docs/architecture.md 18.2).   *)
(* tests/trace/trace_driver.cpp replays them through the kernel's reconciler, the engine and  *)
(* the driver's sync gate (specs/map/reconciliation_actions.hpp): the venue's actions build   *)
(* the driver's copy of the venue, a Deliver hands the kernel the message as a venue order    *)
(* event, SnapshotTaken records the snapshot and Reconcile hands it to the kernel.            *)
(*                                                                                            *)
(* To keep random walks moving toward sync, a behaviour has a small budget of disconnects.    *)
(* Every step is still a step of Reconciliation (RefinesNext).                                 *)
EXTENDS Reconciliation, TLC

CONSTANTS MaxDisconnects

VARIABLES action, disconnects

InitB == Init /\ action = <<"Init">> /\ disconnects = 0

Keep == UNCHANGED disconnects

NextB ==
  \/ \E o \in Orders : Open(o) /\ action' = <<"Open", o>> /\ Keep
  \/ \E o \in Orders : Fill(o) /\ action' = <<"Fill", o>> /\ Keep
  \/ \E o \in Orders : Cancel(o) /\ action' = <<"Cancel", o>> /\ Keep
  \/ disconnects < MaxDisconnects /\ Disconnect /\ action' = <<"Disconnect">>
     /\ disconnects' = disconnects + 1
  \/ Connect /\ action' = <<"Connect">> /\ Keep
  \/ \E m \in chan, keep \in BOOLEAN :
       Deliver(m, keep) /\ action' = <<"Deliver", m.o, m.n, m.st, m.f, m.t, keep>> /\ Keep
  \/ RequestSnapshot /\ action' = <<"RequestSnapshot">> /\ Keep
  \/ SnapshotTaken /\ action' = <<"SnapshotTaken", xtime, xst, xf>> /\ Keep
  \/ Reconcile /\ action' = <<"Reconcile">> /\ Keep

SpecB == InitB /\ [][NextB]_<<vars, action, disconnects>>

RefinesNext == [][Next]_vars

=============================================================================
