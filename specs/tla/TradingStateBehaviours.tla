------------------------ MODULE TradingStateBehaviours ------------------------
(* Behaviours of TradingState for forward trace validation (docs/architecture.md 18.2).     *)
(* tests/trace/trace_driver.cpp replays them through risk::RiskEngine.                       *)
(*                                                                                           *)
(*   action      the step's action and its parameter (specs/map/trading_state_actions.hpp)   *)
(*   admissible  the commands Admit(c) accepted in the state the step left; the driver       *)
(*               checks that the gates deny every other order and modify there              *)
EXTENDS TradingState, TLC

VARIABLES action, admissible

Admissible == {c \in Commands : ENABLED Admit(c)}

InitB == Init /\ action = <<"Init">> /\ admissible = {}

NextB ==
  /\ admissible' = Admissible
  /\ \/ \E t \in Triggers : Trigger(t) /\ action' = <<"Trigger", t>>
     \/ \E c \in Commands : Admit(c) /\ action' = <<"Admit", c>>
     \/ Tick /\ action' = <<"Tick">>

SpecB == InitB /\ [][NextB]_<<vars, action, admissible>>

RefinesNext == [][Next]_vars

=============================================================================
