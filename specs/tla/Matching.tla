------------------------------- MODULE Matching -------------------------------
(* The simulated venue's queue-position fill model for one buy order at price p              *)
(* (docs/architecture.md section 12.3; jarvis/backtest/matching/sim_exchange.hpp). `level` is *)
(* the market's size at p on the bid side, `ahead` the part of it queued before the order.   *)
(*                                                                                           *)
(*   Rest       the order arrives when nothing is offered at p and joins the back of the level *)
(*   Take(v)    it arrives when v is offered at p: a post-only order is rejected (-5022),     *)
(*              any other takes up to v as a taker and rests the remainder at the back       *)
(*   TradeAt(s) sellers trade s at p: the queue ahead goes first, the rest fills the order    *)
(*   Through(s) sellers trade s below p: the order fills up to s                              *)
(*   Level(n)   the level becomes n: a decrease shrinks the queue ahead in proportion        *)
(*   Gone       the market leaves p (the best bid moves below it): nothing is ahead any more  *)
(*   Cross(v)   v is offered at p while the order rests: it fills up to v as a maker          *)
EXTENDS Naturals

CONSTANTS MaxQty, MaxLevel, MaxTrade

VARIABLES phase, qty, filled, ahead, level, postOnly, taker, lastFill

vars == <<phase, qty, filled, ahead, level, postOnly, taker, lastFill>>

Min(a, b) == IF a < b THEN a ELSE b
Leaves == qty - filled

Init == /\ phase = "NEW"
        /\ qty \in 1..MaxQty
        /\ filled = 0
        /\ ahead = 0
        /\ level \in 0..MaxLevel
        /\ postOnly \in BOOLEAN
        /\ taker = 0
        /\ lastFill = "NONE"

\* A maker fill of q.
MakerFill(q, kind) ==
  /\ q > 0
  /\ filled' = filled + q
  /\ phase' = IF filled + q = qty THEN "DONE" ELSE "RESTING"
  /\ lastFill' = kind

Rest ==
  /\ phase = "NEW"
  /\ phase' = "RESTING"
  /\ ahead' = level
  /\ UNCHANGED <<qty, filled, level, postOnly, taker, lastFill>>

Take(v) ==
  /\ phase = "NEW"
  /\ IF postOnly
       THEN /\ phase' = "DONE"
            /\ UNCHANGED <<filled, ahead, taker, lastFill>>
       ELSE /\ filled' = Min(qty, v)
            /\ taker' = Min(qty, v)
            /\ phase' = IF v >= qty THEN "DONE" ELSE "RESTING"
            /\ ahead' = IF v >= qty THEN 0 ELSE level
            /\ lastFill' = "TAKE"
  /\ UNCHANGED <<qty, level, postOnly>>

TradeAt(s) ==
  /\ phase = "RESTING"
  /\ IF s <= ahead
       THEN /\ ahead' = ahead - s
            /\ UNCHANGED <<phase, filled, lastFill>>
       ELSE /\ ahead' = 0
            /\ MakerFill(Min(Leaves, s - ahead), "AT")
  /\ UNCHANGED <<qty, level, postOnly, taker>>

Through(s) ==
  /\ phase = "RESTING"
  /\ MakerFill(Min(Leaves, s), "THROUGH")
  /\ UNCHANGED <<qty, ahead, level, postOnly, taker>>

Level(n) ==
  /\ phase \in {"NEW", "RESTING"}
  /\ n # level
  /\ level' = n
  /\ ahead' = IF phase = "RESTING" /\ n < level THEN (ahead * n) \div level ELSE ahead
  /\ UNCHANGED <<phase, qty, filled, postOnly, taker, lastFill>>

Gone ==
  /\ phase = "RESTING"
  /\ level > 0
  /\ ahead' = 0
  /\ level' = 0
  /\ UNCHANGED <<phase, qty, filled, postOnly, taker, lastFill>>

Cross(v) ==
  /\ phase = "RESTING"
  /\ MakerFill(Min(Leaves, v), "CROSS")
  /\ UNCHANGED <<qty, ahead, level, postOnly, taker>>

Next == \/ Rest
        \/ \E v \in 1..MaxLevel : Take(v)
        \/ \E s \in 1..MaxTrade : TradeAt(s) \/ Through(s)
        \/ \E n \in 0..MaxLevel : Level(n)
        \/ Gone
        \/ \E v \in 1..MaxLevel : Cross(v)

Spec == Init /\ [][Next]_vars

TypeOK == /\ phase \in {"NEW", "RESTING", "DONE"}
          /\ filled \in 0..MaxQty
          /\ ahead \in 0..MaxLevel
          /\ level \in 0..MaxLevel

FilledWithinQuantity == filled <= qty

\* A post-only order never takes liquidity.
PostOnlyNeverTakes == postOnly => taker = 0

\* At its own price the order fills only once nothing is ahead of it.
QueueFirst == lastFill = "AT" => ahead = 0

\* The queue ahead is part of the level.
AheadWithinLevel == phase = "RESTING" => ahead <= level

\* The queue ahead never grows while the order rests.
AheadNeverGrows == [][phase = "RESTING" /\ phase' = "RESTING" => ahead' <= ahead]_vars

=============================================================================
