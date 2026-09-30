---------------------------- MODULE NodeLifecycle ----------------------------
(* The node lifecycle in real time (docs/architecture.md section 4.4): the transition table,  *)
(* the sync gate the driver applies after every input, the shutdown and its drain, the        *)
(* Faulted path, the strategies' on_start and on_stop, and the orders the node has open.      *)
(*                                                                                           *)
(* One input per step. Within the step the gate then moves the lifecycle until it calls for  *)
(* nothing more (jarvis/engine/sync_gate.hpp), as the driver does before the next input.     *)
(* The implementation is jarvis/backtest/driver.hpp over jarvis/engine/engine.hpp;           *)
(* tests/trace/trace_driver.cpp replays behaviours of NodeLifecycleBehaviours through it.     *)
(*                                                                                           *)
(* The account's reconciliation phase follows jarvis/execution/reconciliation.hpp: the user  *)
(* stream going up starts buffering, a snapshot syncs, the stream going down disconnects.    *)
(* Venue answers to orders arrive only while nothing is being buffered (LOCAL or SYNCED); the *)
(* Reconciliation spec covers what happens to orders across a reconnect.                     *)
EXTENDS Naturals

CONSTANTS MaxOrders

VARIABLES await,    \* live: Running waits for the user stream and order entry (await_sync)
          state,    \* the lifecycle
          phase,    \* the account's reconciliation phase
          md, oe,   \* market data and order entry connections
          stale,    \* market data up but silent for too long
          orders,   \* orders open at the venue
          mode,     \* the recorded Shutdown input's mode, NONE before one
          timedOut, \* the drain ran out of time
          kill,     \* cancels went out for open orders (the kill switch)
          starts,   \* on_start calls
          stops,    \* on_stop calls
          failure   \* what failed, once the node faulted

vars == <<await, state, phase, md, oe, stale, orders, mode, timedOut, kill, starts, stops,
          failure>>

States == {"INIT", "WIRED", "STARTING", "SYNCING", "RUNNING", "DEGRADED", "STOPPING",
           "STOPPED", "FAULTED"}
Terminal == {"STOPPED", "FAULTED"}
Phases == {"LOCAL", "DISCONNECTED", "BUFFERING", "SYNCED"}
Links == {"UNKNOWN", "UP", "DOWN"}
Modes == {"NONE", "CANCEL_ALL_THEN_EXIT", "EXIT_KEEP_ORDERS"}
Kinds == {"MARKET_DATA", "ORDER_ENTRY"}
Failures == {"PUMP", "LOG"} \* what failed: the log still records (PUMP) or nothing does (LOG)

\* ---- the sync gate (jarvis/engine/sync_gate.hpp) ------------------------------------------

Healthy(m, o, st) == m # "DOWN" /\ o # "DOWN" /\ ~st
Synced(ph) == ph = "SYNCED" \/ (ph = "LOCAL" /\ ~await)
Ready(m, o, st) == Healthy(m, o, st) /\ (~await \/ o = "UP")

GateMove(s, ph, m, o, st) ==
  CASE s = "SYNCING" ->
         IF Synced(ph) /\ Ready(m, o, st) THEN "SYNCED"
         ELSE IF ph = "DISCONNECTED" THEN "HEALTH_LOST" ELSE "NONE"
    [] s = "RUNNING" ->
         IF ph = "DISCONNECTED" \/ ph = "BUFFERING" \/ ~Healthy(m, o, st)
         THEN "HEALTH_LOST" ELSE "NONE"
    [] s = "DEGRADED" ->
         IF Healthy(m, o, st) /\ (ph = "BUFFERING" \/ Synced(ph))
         THEN "HEALTH_RESTORED" ELSE "NONE"
    [] OTHER -> "NONE"

Moved(s, reason) ==
  CASE reason = "SYNCED" -> "RUNNING"
    [] reason = "HEALTH_LOST" -> "DEGRADED"
    [] reason = "HEALTH_RESTORED" -> "SYNCING"
    [] OTHER -> s

\* The gate's moves until it calls for none (at most three: DEGRADED, SYNCING, RUNNING).
RECURSIVE Settle(_, _, _, _, _)
Settle(s, ph, m, o, st) ==
  LET reason == GateMove(s, ph, m, o, st) IN
  IF reason = "NONE" THEN s ELSE Settle(Moved(s, reason), ph, m, o, st)

\* An input changed phase, links or staleness: the gate settles, and the strategies start the
\* first time the node runs (the gate never passes through RUNNING without stopping there).
Gate(ph, m, o, st) ==
  /\ state' = Settle(state, ph, m, o, st)
  /\ starts' = IF state' = "RUNNING" /\ starts = 0 THEN 1 ELSE starts

Live == state \notin Terminal

\* ---- start (Init -> Wired -> Starting -> Syncing, then the gate or Synced) -----------------

Init == /\ await \in BOOLEAN
        /\ phase = "LOCAL"
        /\ md = "UNKNOWN"
        /\ oe = "UNKNOWN"
        /\ stale = FALSE
        /\ orders = 0
        /\ mode = "NONE"
        /\ timedOut = FALSE
        /\ kill = FALSE
        /\ state = IF await THEN "SYNCING" ELSE "RUNNING"
        /\ starts = IF await THEN 0 ELSE 1
        /\ stops = 0
        /\ failure = "NONE"

\* ---- inputs ---------------------------------------------------------------------------------

\* A market data or order entry connection went up or down (ConnectionStatus). Either change of
\* market data restarts the freshness clock.
Link(k, up) ==
  LET m == IF k = "MARKET_DATA" THEN (IF up THEN "UP" ELSE "DOWN") ELSE md
      o == IF k = "ORDER_ENTRY" THEN (IF up THEN "UP" ELSE "DOWN") ELSE oe
      st == IF k = "MARKET_DATA" THEN FALSE ELSE stale IN
  /\ Live
  /\ md' = m /\ oe' = o /\ stale' = st
  /\ Gate(phase, m, o, st)
  /\ UNCHANGED <<await, phase, orders, mode, timedOut, kill, stops, failure>>

\* The user data stream went up or down (ConnectionStatus of the user stream).
Stream(up) ==
  LET ph == IF ~up THEN "DISCONNECTED"
            ELSE IF phase \in {"LOCAL", "DISCONNECTED"} THEN "BUFFERING" ELSE phase IN
  /\ Live
  /\ phase' = ph
  /\ Gate(ph, md, oe, stale)
  /\ UNCHANGED <<await, md, oe, stale, orders, mode, timedOut, kill, stops, failure>>

\* A venue snapshot arrived (VenueSnapshot): it syncs a buffering or local account and is
\* ignored otherwise; it lists the open orders as the venue has them.
Snapshot ==
  LET ph == IF phase \in {"LOCAL", "BUFFERING"} THEN "SYNCED" ELSE phase IN
  /\ Live
  /\ phase' = ph
  /\ Gate(ph, md, oe, stale)
  /\ UNCHANGED <<await, md, oe, stale, orders, mode, timedOut, kill, stops, failure>>

\* Market data up but silent past node.market_data_stale_ms (the kernel's freshness timer).
Stale ==
  /\ Live
  /\ md = "UP" /\ ~stale
  /\ stale' = TRUE
  /\ Gate(phase, md, oe, TRUE)
  /\ UNCHANGED <<await, phase, md, oe, orders, mode, timedOut, kill, stops, failure>>

\* A market data input: fresh again.
Fresh ==
  /\ Live
  /\ stale
  /\ stale' = FALSE
  /\ Gate(phase, md, oe, FALSE)
  /\ UNCHANGED <<await, phase, md, oe, orders, mode, timedOut, kill, stops, failure>>

\* A running strategy places an order.
Submit ==
  /\ state = "RUNNING"
  /\ orders < MaxOrders
  /\ orders' = orders + 1
  /\ UNCHANGED <<await, state, phase, md, oe, stale, mode, timedOut, kill, starts, stops,
                 failure>>

\* The venue closes an order (canceled or filled). Draining, the last one ends the drain.
Answer ==
  /\ Live
  /\ orders > 0
  /\ phase \in {"LOCAL", "SYNCED"}
  /\ orders' = orders - 1
  /\ state' = IF state = "STOPPING" /\ orders' = 0 THEN "STOPPED" ELSE state
  /\ UNCHANGED <<await, phase, md, oe, stale, mode, timedOut, kill, starts, stops, failure>>

\* The node is asked to stop (a signal, --run-for, admin shutdown) with [node] shutdown = m
\* (NONE: no Shutdown input). The Shutdown input comes first; cancel_all_then_exit cancels every
\* open order and waits in Stopping for them; otherwise, or with none open, the node stops at
\* once. The strategies stop in Stopping.
Shutdown(m) ==
  /\ state \in {"SYNCING", "RUNNING", "DEGRADED"}
  /\ mode' = m
  /\ kill' = (kill \/ (m = "CANCEL_ALL_THEN_EXIT" /\ orders > 0))
  /\ state' = IF m = "CANCEL_ALL_THEN_EXIT" /\ orders > 0 THEN "STOPPING" ELSE "STOPPED"
  /\ stops' = IF starts = 1 THEN 1 ELSE 0
  /\ UNCHANGED <<await, phase, md, oe, stale, orders, timedOut, starts, failure>>

\* The drain time is up with orders still open.
Timeout ==
  /\ state = "STOPPING"
  /\ state' = "STOPPED"
  /\ timedOut' = TRUE
  /\ UNCHANGED <<await, phase, md, oe, stale, orders, mode, kill, starts, stops, failure>>

\* A step, the log, the venue or the pump failed. While the log still records, open orders are
\* canceled first (a Shutdown(cancel_all_then_exit) input) unless a shutdown already did; no
\* strategy callback follows.
Fault(f) ==
  LET cancel == f = "PUMP" /\ orders > 0 /\ mode = "NONE" IN
  /\ Live
  /\ state' = "FAULTED"
  /\ mode' = IF cancel THEN "CANCEL_ALL_THEN_EXIT" ELSE mode
  /\ kill' = (kill \/ cancel)
  /\ failure' = f
  /\ UNCHANGED <<await, phase, md, oe, stale, orders, timedOut, starts, stops>>

Next == \/ \E k \in Kinds, up \in BOOLEAN : Link(k, up)
        \/ \E up \in BOOLEAN : Stream(up)
        \/ Snapshot \/ Stale \/ Fresh \/ Submit \/ Answer \/ Timeout
        \/ \E m \in Modes : Shutdown(m)
        \/ \E f \in Failures : Fault(f)

Spec == Init /\ [][Next]_vars

\* The drain ends: under weak fairness of the timeout, Stopping always leads to Stopped (or to
\* Faulted, when something fails meanwhile).
FairSpec == Spec /\ WF_vars(Timeout)
DrainEnds == state = "STOPPING" ~> state \in Terminal

\* ---- properties -----------------------------------------------------------------------------

TypeOK == /\ await \in BOOLEAN
          /\ state \in States
          /\ phase \in Phases
          /\ md \in Links /\ oe \in Links
          /\ stale \in BOOLEAN
          /\ orders \in 0..MaxOrders
          /\ mode \in Modes
          /\ timedOut \in BOOLEAN /\ kill \in BOOLEAN
          /\ starts \in 0..1 /\ stops \in 0..1
          /\ failure \in Failures \cup {"NONE"}

\* Running means reconciled, nothing down, market data fresh, and (live) order entry up.
\* (Spelled out rather than through the gate's operators, so that it checks them.)
RunningIsReady ==
  state = "RUNNING" =>
    /\ phase = "SYNCED" \/ (phase = "LOCAL" /\ ~await)
    /\ md # "DOWN" /\ oe # "DOWN" /\ ~stale
    /\ await => oe = "UP"

\* The strategies start once, when the node first runs, and stop only after starting.
StartedWhenRunning == state = "RUNNING" => starts = 1
StopAfterStart == stops = 1 => starts = 1

\* Stopped with cancel_all_then_exit: nothing left open, unless the drain timed out.
StoppedDrained == state = "STOPPED" /\ mode = "CANCEL_ALL_THEN_EXIT" => orders = 0 \/ timedOut

\* Stopping only while orders are open under cancel_all_then_exit, their cancels sent.
StoppingDrains == state = "STOPPING" => mode = "CANCEL_ALL_THEN_EXIT" /\ orders > 0 /\ kill

\* A fault with the log still recording never leaves open orders without cancels.
FaultCancels == state = "FAULTED" /\ failure = "PUMP" /\ orders > 0 => kill

\* Stopped and Faulted are terminal.
TerminalStays == [][(state \in Terminal) => state' = state]_vars

=============================================================================
