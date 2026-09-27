-------------------------- MODULE MatchingBehaviours --------------------------
(* Behaviours of Matching for forward trace validation (docs/architecture.md 18.2).         *)
(* tests/trace/trace_driver.cpp replays them through backtest::SimulatedExchange with the    *)
(* QueuePosition fill model; `action` holds the step's action and its parameter              *)
(* (specs/map/matching_actions.hpp).                                                         *)
EXTENDS Matching, TLC

VARIABLE action

InitB == Init /\ action = <<"Init">>

NextB ==
  \/ Rest /\ action' = <<"Rest">>
  \/ \E v \in 1..MaxLevel : Take(v) /\ action' = <<"Take", v>>
  \/ \E s \in 1..MaxTrade : TradeAt(s) /\ action' = <<"TradeAt", s>>
  \/ \E s \in 1..MaxTrade : Through(s) /\ action' = <<"Through", s>>
  \/ \E n \in 0..MaxLevel : Level(n) /\ action' = <<"Level", n>>
  \/ Gone /\ action' = <<"Gone">>
  \/ \E v \in 1..MaxLevel : Cross(v) /\ action' = <<"Cross", v>>

SpecB == InitB /\ [][NextB]_<<vars, action>>

RefinesNext == [][Next]_vars

=============================================================================
