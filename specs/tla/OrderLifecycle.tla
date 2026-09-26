---------------------------- MODULE OrderLifecycle ----------------------------
(* One order's state machine (docs/architecture.md section 8.1). The transition relation is  *)
(* nautilus_trader's OrderStatus::transition at cd417b80 plus the edge jarvis adds,          *)
(* SUBMITTED --EXPIRED--> EXPIRED (Binance reports IOC remainders and self-trade prevention  *)
(* as EXPIRED, possibly before the order.place response). The apply-phase rules follow       *)
(* nautilus OrderCore::apply: fills decide PARTIALLY_FILLED or FILLED, a fill while an       *)
(* update or cancel is pending keeps the pending status and records PARTIALLY_FILLED as the *)
(* previous status, and MODIFY_REJECTED, CANCEL_REJECTED and a pending UPDATED restore it.   *)
(*                                                                                           *)
(* jarvis/execution/order_fsm.hpp holds the same table; tests/cpp/test_execution.cpp reads   *)
(* the set between BEGIN TRANSITIONS and END TRANSITIONS below and fails when they differ.   *)
(*                                                                                           *)
(* Deliberate simplifications, shared with the C++ code: a fill never exceeds the leaves     *)
(* quantity (nautilus tolerates overfills; Binance does not produce them), a voided fill     *)
(* reduces the filled quantity of that trade only and is never reopened, and a second        *)
(* CANCELED after a fill that followed a cancel is refused rather than absorbed.             *)
EXTENDS Naturals, FiniteSets

CONSTANTS MaxQty, TradeIds

VARIABLES status, prev, quantity, filled, fills

vars == <<status, prev, quantity, filled, fills>>

Statuses == {"INITIALIZED", "DENIED", "EMULATED", "RELEASED", "SUBMITTED", "ACCEPTED",
             "REJECTED", "CANCELED", "EXPIRED", "TRIGGERED", "PENDING_UPDATE",
             "PENDING_CANCEL", "PARTIALLY_FILLED", "FILLED", "VOIDED"}

Kinds == {"DENIED", "EMULATED", "RELEASED", "SUBMITTED", "ACCEPTED", "REJECTED", "CANCELED",
          "EXPIRED", "TRIGGERED", "PENDING_UPDATE", "PENDING_CANCEL", "MODIFY_REJECTED",
          "CANCEL_REJECTED", "UPDATED", "FILLED", "FILL_VOIDED"}

Pending == {"PENDING_UPDATE", "PENDING_CANCEL"}

\* BEGIN TRANSITIONS
Transitions == {
  <<"INITIALIZED", "DENIED", "DENIED">>,
  <<"INITIALIZED", "EMULATED", "EMULATED">>,
  <<"INITIALIZED", "RELEASED", "RELEASED">>,
  <<"INITIALIZED", "SUBMITTED", "SUBMITTED">>,
  <<"INITIALIZED", "REJECTED", "REJECTED">>,
  <<"INITIALIZED", "ACCEPTED", "ACCEPTED">>,
  <<"INITIALIZED", "CANCELED", "CANCELED">>,
  <<"INITIALIZED", "EXPIRED", "EXPIRED">>,
  <<"INITIALIZED", "TRIGGERED", "TRIGGERED">>,
  <<"INITIALIZED", "UPDATED", "INITIALIZED">>,
  <<"EMULATED", "CANCELED", "CANCELED">>,
  <<"EMULATED", "EXPIRED", "EXPIRED">>,
  <<"EMULATED", "UPDATED", "EMULATED">>,
  <<"EMULATED", "RELEASED", "RELEASED">>,
  <<"RELEASED", "SUBMITTED", "SUBMITTED">>,
  <<"RELEASED", "DENIED", "DENIED">>,
  <<"RELEASED", "CANCELED", "CANCELED">>,
  <<"RELEASED", "UPDATED", "RELEASED">>,
  <<"SUBMITTED", "PENDING_UPDATE", "PENDING_UPDATE">>,
  <<"SUBMITTED", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"SUBMITTED", "REJECTED", "REJECTED">>,
  <<"SUBMITTED", "CANCELED", "CANCELED">>,
  <<"SUBMITTED", "EXPIRED", "EXPIRED">>,
  <<"SUBMITTED", "ACCEPTED", "ACCEPTED">>,
  <<"SUBMITTED", "UPDATED", "SUBMITTED">>,
  <<"SUBMITTED", "FILLED", "FILLED">>,
  <<"ACCEPTED", "REJECTED", "REJECTED">>,
  <<"ACCEPTED", "PENDING_UPDATE", "PENDING_UPDATE">>,
  <<"ACCEPTED", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"ACCEPTED", "CANCEL_REJECTED", "ACCEPTED">>,
  <<"ACCEPTED", "CANCELED", "CANCELED">>,
  <<"ACCEPTED", "TRIGGERED", "TRIGGERED">>,
  <<"ACCEPTED", "UPDATED", "ACCEPTED">>,
  <<"ACCEPTED", "EXPIRED", "EXPIRED">>,
  <<"ACCEPTED", "FILLED", "FILLED">>,
  <<"ACCEPTED", "FILL_VOIDED", "ACCEPTED">>,
  <<"CANCELED", "FILLED", "FILLED">>,
  <<"CANCELED", "FILL_VOIDED", "CANCELED">>,
  <<"CANCELED", "UPDATED", "CANCELED">>,
  <<"PENDING_UPDATE", "REJECTED", "REJECTED">>,
  <<"PENDING_UPDATE", "ACCEPTED", "ACCEPTED">>,
  <<"PENDING_UPDATE", "CANCELED", "CANCELED">>,
  <<"PENDING_UPDATE", "EXPIRED", "EXPIRED">>,
  <<"PENDING_UPDATE", "TRIGGERED", "TRIGGERED">>,
  <<"PENDING_UPDATE", "SUBMITTED", "PENDING_UPDATE">>,
  <<"PENDING_UPDATE", "PENDING_UPDATE", "PENDING_UPDATE">>,
  <<"PENDING_UPDATE", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"PENDING_UPDATE", "MODIFY_REJECTED", "PENDING_UPDATE">>,
  <<"PENDING_UPDATE", "UPDATED", "PENDING_UPDATE">>,
  <<"PENDING_UPDATE", "FILLED", "FILLED">>,
  <<"PENDING_UPDATE", "FILL_VOIDED", "PENDING_UPDATE">>,
  <<"PENDING_CANCEL", "REJECTED", "REJECTED">>,
  <<"PENDING_CANCEL", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"PENDING_CANCEL", "MODIFY_REJECTED", "PENDING_CANCEL">>,
  <<"PENDING_CANCEL", "CANCEL_REJECTED", "PENDING_CANCEL">>,
  <<"PENDING_CANCEL", "CANCELED", "CANCELED">>,
  <<"PENDING_CANCEL", "EXPIRED", "EXPIRED">>,
  <<"PENDING_CANCEL", "ACCEPTED", "ACCEPTED">>,
  <<"PENDING_CANCEL", "UPDATED", "PENDING_CANCEL">>,
  <<"PENDING_CANCEL", "FILLED", "FILLED">>,
  <<"PENDING_CANCEL", "FILL_VOIDED", "PENDING_CANCEL">>,
  <<"TRIGGERED", "REJECTED", "REJECTED">>,
  <<"TRIGGERED", "PENDING_UPDATE", "PENDING_UPDATE">>,
  <<"TRIGGERED", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"TRIGGERED", "CANCELED", "CANCELED">>,
  <<"TRIGGERED", "EXPIRED", "EXPIRED">>,
  <<"TRIGGERED", "FILLED", "FILLED">>,
  <<"TRIGGERED", "UPDATED", "TRIGGERED">>,
  <<"TRIGGERED", "FILL_VOIDED", "TRIGGERED">>,
  <<"PARTIALLY_FILLED", "PENDING_UPDATE", "PENDING_UPDATE">>,
  <<"PARTIALLY_FILLED", "PENDING_CANCEL", "PENDING_CANCEL">>,
  <<"PARTIALLY_FILLED", "CANCELED", "CANCELED">>,
  <<"PARTIALLY_FILLED", "EXPIRED", "EXPIRED">>,
  <<"PARTIALLY_FILLED", "FILLED", "FILLED">>,
  <<"PARTIALLY_FILLED", "ACCEPTED", "ACCEPTED">>,
  <<"PARTIALLY_FILLED", "UPDATED", "PARTIALLY_FILLED">>,
  <<"PARTIALLY_FILLED", "FILL_VOIDED", "PARTIALLY_FILLED">>,
  <<"FILLED", "FILL_VOIDED", "VOIDED">>,
  <<"FILLED", "UPDATED", "FILLED">>,
  <<"EXPIRED", "FILL_VOIDED", "EXPIRED">>,
  <<"EXPIRED", "UPDATED", "EXPIRED">>,
  <<"VOIDED", "FILL_VOIDED", "VOIDED">>
}
\* END TRANSITIONS

Allowed(s, k) == \E t \in Statuses : <<s, k, t>> \in Transitions
Target(s, k) == CHOOSE t \in Statuses : <<s, k, t>> \in Transitions

Leaves == quantity - filled

\* previous_status is saved on every transition except the rejections, and not while an
\* update or cancel is already pending (so repeated requests keep the pre-pending status).
SavePrev(k) == IF k \in {"MODIFY_REJECTED", "CANCEL_REJECTED"} \/ status \in Pending
               THEN prev ELSE status

Init == /\ status = "INITIALIZED"
        /\ prev = "NONE"
        /\ quantity \in 1..MaxQty
        /\ filled = 0
        /\ fills = [t \in {} |-> 0]

\* Events without quantities: the table decides, except the rejections and a pending UPDATED.
Plain(k) ==
  /\ k \notin {"FILLED", "FILL_VOIDED", "UPDATED"}
  /\ Allowed(status, k)
  /\ \/ k \notin {"MODIFY_REJECTED", "CANCEL_REJECTED"}
     \/ k = "CANCEL_REJECTED" /\ status # "PENDING_CANCEL"
     \/ prev # "NONE"
  /\ prev' = SavePrev(k)
  /\ status' = CASE k = "MODIFY_REJECTED" /\ status # "PENDING_CANCEL" -> prev
               []   k = "CANCEL_REJECTED" /\ status = "PENDING_CANCEL" -> prev
               []   OTHER -> Target(status, k)
  /\ UNCHANGED <<quantity, filled, fills>>

Updated(q) ==
  /\ Allowed(status, "UPDATED")
  /\ q > filled
  /\ prev' = SavePrev("UPDATED")
  /\ status' = IF status \in Pending /\ prev # "NONE" THEN prev ELSE Target(status, "UPDATED")
  /\ quantity' = q
  /\ UNCHANGED <<filled, fills>>

Fill(t, q) ==
  /\ Allowed(status, "FILLED")
  /\ t \notin DOMAIN fills
  /\ q \in 1..Leaves
  /\ filled' = filled + q
  /\ fills' = [x \in DOMAIN fills \cup {t} |-> IF x = t THEN q ELSE fills[x]]
  /\ IF filled + q >= quantity
       THEN status' = "FILLED" /\ prev' = SavePrev("FILLED")
     ELSE IF status = "CANCELED"
       THEN status' = "CANCELED" /\ prev' = SavePrev("FILLED")
     ELSE IF status \in Pending
       THEN status' = status /\ prev' = "PARTIALLY_FILLED"
     ELSE status' = "PARTIALLY_FILLED" /\ prev' = SavePrev("FILLED")
  /\ UNCHANGED quantity

Void(t, v) ==
  /\ Allowed(status, "FILL_VOIDED")
  /\ t \in DOMAIN fills
  /\ v \in 1..fills[t]
  /\ filled' = filled - v
  /\ fills' = [fills EXCEPT ![t] = @ - v]
  /\ prev' = SavePrev("FILL_VOIDED")
  /\ status' = CASE status \in {"CANCELED", "EXPIRED", "VOIDED", "TRIGGERED"} \cup Pending -> status
               []   status = "FILLED" -> "VOIDED"
               []   OTHER -> IF filled - v = 0 THEN "ACCEPTED" ELSE "PARTIALLY_FILLED"
  /\ UNCHANGED quantity

Next == \/ \E k \in Kinds : Plain(k)
        \/ \E q \in 1..MaxQty : Updated(q)
        \/ \E t \in TradeIds, q \in 1..MaxQty : Fill(t, q)
        \/ \E t \in TradeIds, v \in 1..MaxQty : Void(t, v)

Spec == Init /\ [][Next]_vars

TypeOK == /\ status \in Statuses
          /\ prev \in Statuses \cup {"NONE"}
          /\ quantity \in 1..MaxQty
          /\ filled \in 0..MaxQty

FilledWithinQuantity == filled <= quantity

FilledIsSumOfFills ==
  LET Sum[S \in SUBSET DOMAIN fills] ==
        IF S = {} THEN 0 ELSE LET x == CHOOSE y \in S : TRUE IN fills[x] + Sum[S \ {x}]
  IN filled = Sum[DOMAIN fills]

\* The fill that completes an order always makes it FILLED, whatever it was before.
FullMeansFilled == filled = quantity => status = "FILLED"

\* While an update or cancel is pending there is always a status to return to.
PendingHasPrevious == status \in Pending => prev \in Statuses

=============================================================================
