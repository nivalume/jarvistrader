----------------------------- MODULE TradingState -----------------------------
(* TradingState and the order rate limit (docs/architecture.md sections 10.2 and 10.4).       *)
(*                                                                                           *)
(* The effective state is the strictest of three parts: the base, which the post-trade       *)
(* monitors only tighten and admin commands set; the sync hold (Halted while reconciling);   *)
(* the degraded hold (Reducing while the node is Degraded). The rate limit counts admitted   *)
(* orders in fixed windows; a window ends with Tick.                                         *)
(*                                                                                           *)
(* jarvis/risk/trading_state.hpp holds the same tables; tests/cpp/test_risk.cpp reads the    *)
(* sets between BEGIN/END TRIGGERS and BEGIN/END MATRIX and checks the C++ code against them *)
(* for every state.                                                                          *)
EXTENDS Naturals

CONSTANTS Limit, MaxWindows

VARIABLES base, syncing, degraded, used, window, lastBase, lastTrigger, haltedAdmit

vars == <<base, syncing, degraded, used, window, lastBase, lastTrigger, haltedAdmit>>

States == {"ACTIVE", "REDUCING", "HALTED"}
Rank(s) == CASE s = "ACTIVE" -> 0 [] s = "REDUCING" -> 1 [] OTHER -> 2
Stricter(a, b) == IF Rank(a) >= Rank(b) THEN a ELSE b

Triggers == {"SYNC_STARTED", "SYNCED", "DEGRADED", "RECOVERED", "SOFT_LIMIT", "HARD_LIMIT",
             "ADMIN_HALT", "ADMIN_REDUCE", "ADMIN_RESUME"}
Admin == {"ADMIN_HALT", "ADMIN_REDUCE", "ADMIN_RESUME"}
Commands == {"OPEN", "REDUCE", "MODIFY", "MODIFY_UP", "CANCEL"}

\* What each trigger does: <<trigger, part, value>>. part "base+" tightens the base to at least
\* value; "base=" sets it; "sync" and "degraded" set the holds.
\* BEGIN TRIGGERS
TriggerTable == {
  <<"SYNC_STARTED", "sync", TRUE>>,
  <<"SYNCED", "sync", FALSE>>,
  <<"DEGRADED", "degraded", TRUE>>,
  <<"RECOVERED", "degraded", FALSE>>,
  <<"SOFT_LIMIT", "base+", "REDUCING">>,
  <<"HARD_LIMIT", "base=", "HALTED">>,
  <<"ADMIN_HALT", "base=", "HALTED">>,
  <<"ADMIN_REDUCE", "base=", "REDUCING">>,
  <<"ADMIN_RESUME", "base=", "ACTIVE">>
}
\* END TRIGGERS

\* Which commands each effective state admits.
\* BEGIN MATRIX
Matrix == {
  <<"ACTIVE", "OPEN">>, <<"ACTIVE", "REDUCE">>, <<"ACTIVE", "MODIFY">>,
  <<"ACTIVE", "MODIFY_UP">>, <<"ACTIVE", "CANCEL">>,
  <<"REDUCING", "REDUCE">>, <<"REDUCING", "MODIFY">>, <<"REDUCING", "CANCEL">>,
  <<"HALTED", "CANCEL">>
}
\* END MATRIX

Effective == IF syncing THEN "HALTED"
             ELSE IF degraded THEN Stricter(base, "REDUCING") ELSE base

Init == /\ base \in States
        /\ syncing = FALSE
        /\ degraded = FALSE
        /\ used = 0
        /\ window = 0
        /\ lastBase = base
        /\ lastTrigger = "NONE"
        /\ haltedAdmit = FALSE

Trigger(t) ==
  LET row == CHOOSE r \in TriggerTable : r[1] = t IN
  /\ lastBase' = base
  /\ lastTrigger' = t
  /\ base' = CASE row[2] = "base+" -> Stricter(base, row[3])
              []   row[2] = "base=" -> row[3]
              []   OTHER -> base
  /\ syncing' = IF row[2] = "sync" THEN row[3] ELSE syncing
  /\ degraded' = IF row[2] = "degraded" THEN row[3] ELSE degraded
  /\ UNCHANGED <<used, window, haltedAdmit>>

\* A command the gate admits: the state allows it and (orders only) the window has room.
\* Cancels do not count toward the rate limit.
Admit(c) ==
  /\ <<Effective, c>> \in Matrix
  /\ c = "CANCEL" \/ used < Limit
  /\ used' = IF c = "CANCEL" THEN used ELSE used + 1
  /\ haltedAdmit' = (haltedAdmit \/ (Effective = "HALTED" /\ c # "CANCEL"))
  /\ lastBase' = base
  /\ lastTrigger' = "NONE"
  /\ UNCHANGED <<base, syncing, degraded, window>>

Tick ==
  /\ window < MaxWindows
  /\ window' = window + 1
  /\ used' = 0
  /\ lastBase' = base
  /\ lastTrigger' = "NONE"
  /\ UNCHANGED <<base, syncing, degraded, haltedAdmit>>

Next == \/ \E t \in Triggers : Trigger(t)
        \/ \E c \in Commands : Admit(c)
        \/ Tick

Spec == Init /\ [][Next]_vars

TypeOK == /\ base \in States
          /\ syncing \in BOOLEAN
          /\ degraded \in BOOLEAN
          /\ used \in 0..Limit
          /\ window \in 0..MaxWindows

\* Only admin commands loosen the base.
OnlyAdminLoosens == Rank(base) < Rank(lastBase) => lastTrigger \in Admin

\* Nothing but cancels passes while Halted.
HaltedAdmitsOnlyCancels == ~haltedAdmit

\* The window never admits more orders than the limit (and the count is never negative).
WithinLimit == used <= Limit

\* Reconciling always halts; a degraded node never opens.
SyncHalts == syncing => Effective = "HALTED"
DegradedReduces == degraded => Effective # "ACTIVE"

=============================================================================
