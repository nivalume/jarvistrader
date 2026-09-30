----------------------- MODULE NodeLifecycleBehaviours -----------------------
(* Behaviours of NodeLifecycle for forward trace validation (docs/architecture.md 18.2).    *)
(* tests/trace/trace_driver.cpp replays them through the real-time backtest::Driver over an  *)
(* Engine: each action becomes the inputs, clock moves or failures of                        *)
(* specs/map/node_lifecycle_actions.hpp, and after each one the driver's state must match.   *)
(* A stop or a fault ends a behaviour, so they wait until `n` steps have been taken (Warmup);  *)
(* otherwise most random behaviours would end within a few steps.                          *)
EXTENDS NodeLifecycle, TLC

CONSTANTS Warmup

VARIABLES action, n

InitB == Init /\ action = <<"Init">> /\ n = 0

NextActions ==
  \/ \E k \in Kinds, up \in BOOLEAN : Link(k, up) /\ action' = <<"Link", k, up>>
  \/ \E up \in BOOLEAN : Stream(up) /\ action' = <<"Stream", up>>
  \/ Snapshot /\ action' = <<"Snapshot">>
  \/ Stale /\ action' = <<"Stale">>
  \/ Fresh /\ action' = <<"Fresh">>
  \/ Submit /\ action' = <<"Submit">>
  \/ Answer /\ action' = <<"Answer">>
  \/ Timeout /\ action' = <<"Timeout">>
  \/ n >= Warmup /\ \E m \in Modes : Shutdown(m) /\ action' = <<"Shutdown", m>>
  \/ n >= Warmup /\ \E f \in Failures : Fault(f) /\ action' = <<"Fault", f>>

NextB ==
  /\ n' = n + 1
  /\ NextActions

SpecB == InitB /\ [][NextB]_<<vars, action, n>>

RefinesNext == [][Next]_vars

=============================================================================
