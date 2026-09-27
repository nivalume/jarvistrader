----------------------- MODULE OrderLifecycleBehaviours -----------------------
(* Behaviours of OrderLifecycle for forward trace validation (docs/architecture.md 18.2).   *)
(* tools/tla/behaviours.py runs TLC in simulation mode on this module and writes each        *)
(* behaviour to a text file that tests/trace/trace_driver.cpp replays through the OMS.       *)
(*                                                                                           *)
(*   action   the step's action and its parameters, as in specs/map/order_lifecycle_actions  *)
(*   plain    the kinds k for which Plain(k) was enabled in the state the step left; the     *)
(*            driver checks that the OMS refuses every other kind there                      *)
(*                                                                                           *)
(* NextB has one disjunct per disjunct of OrderLifecycle!Next; RefinesNext checks that every *)
(* step is a Next step, and the driver fails when an action never appears.                   *)
EXTENDS OrderLifecycle, TLC

VARIABLES action, plain

PlainKinds == Kinds \ {"FILLED", "FILL_VOIDED", "UPDATED"}
Enabled == {k \in PlainKinds : ENABLED Plain(k)}

InitB == Init /\ action = <<"Init">> /\ plain = {}

NextB ==
  /\ plain' = Enabled
  /\ \/ \E k \in Kinds : Plain(k) /\ action' = <<"Plain", k>>
     \/ \E q \in 1..MaxQty : Updated(q) /\ action' = <<"Updated", q>>
     \/ \E t \in TradeIds, q \in 1..MaxQty : Fill(t, q) /\ action' = <<"Fill", t, q>>
     \/ \E t \in TradeIds, v \in 1..MaxQty : Void(t, v) /\ action' = <<"Void", t, v>>

SpecB == InitB /\ [][NextB]_<<vars, action, plain>>

RefinesNext == [][Next]_vars

=============================================================================
