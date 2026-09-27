------------------------- MODULE DepthSyncBehaviours -------------------------
(* Behaviours of DepthSync for forward trace validation (docs/architecture.md 18.2).        *)
(* tests/trace/trace_driver.cpp replays the client's inputs through                          *)
(* adapter::binance::DepthSync (specs/map/depth_sync_actions.hpp): the exchange and network  *)
(* actions only change the spec's environment; a Receive records the event delivered and an  *)
(* Arrive the snapshot, which the driver hands to the implementation.                        *)
(*                                                                                           *)
(* To keep random walks moving toward sync, a behaviour has a small budget of disconnects    *)
(* and lost events, and an update moves a price's size to the next value. Every step is      *)
(* still a step of DepthSync (RefinesNext).                                                   *)
EXTENDS DepthSync, TLC

CONSTANTS MaxDisconnects, MaxLosses

VARIABLES action, disconnects, losses

InitB == Init /\ action = <<"Init">> /\ disconnects = 0 /\ losses = 0

Keep == UNCHANGED <<disconnects, losses>>

NextB ==
  \/ \E p \in Prices : LET v == (xbook[p] + 1) % (MaxQty + 1) IN
                       Update(p, v) /\ action' = <<"Update", p, v>> /\ Keep
  \/ Publish /\ action' = <<"Publish">> /\ Keep
  \/ losses < MaxLosses /\ Lose /\ action' = <<"Lose">> /\ losses' = losses + 1
     /\ UNCHANGED disconnects
  \/ \E l \in 0..MaxSeq : l <= xseq /\ Serve(l) /\ action' = <<"Serve", l>> /\ Keep
  \/ Connect /\ action' = <<"Connect">> /\ Keep
  \/ disconnects < MaxDisconnects /\ Disconnect /\ action' = <<"Disconnect">>
     /\ disconnects' = disconnects + 1 /\ UNCHANGED losses
  \/ Request /\ action' = <<"Request">> /\ Keep
  \/ Receive /\ action' = <<"Receive", Head(channel).U, Head(channel).u, Head(channel).pu,
                                       Head(channel).ch>> /\ Keep
  \/ Arrive /\ action' = <<"Arrive", inflight.L, inflight.book>> /\ Keep

SpecB == InitB /\ [][NextB]_<<vars, action, disconnects, losses>>

RefinesNext == [][Next]_vars

=============================================================================
