# Build A: the venue

**Language:** C++, inside `minsky/`. **Size:** two people.
**Delivers:** `policyc`, `verify`, `replay`, `venued`, and the exchange changes they need.
**Depends on Build B for:** nothing. Every input is a file or a socket message defined in
`CONTRACT.md`.

The question this build answers: **given a program, what is the worst it can do on our exchange,
and can we run it there with nobody else involved?**

---

## 1. What you own

| Component | Binary / file | Purpose |
|---|---|---|
| A1 Program format | `src/program_json.{hpp,cpp}` | load and validate CompiledProgram JSON; the shared in-memory node table |
| A2 Compiler | `build/policyc`, `src/policy.{hpp,cpp}`, `src/policyc.cpp` | MPL → CompiledProgram; static rules; deploy plan |
| A3 Verifier | `build/verify`, `src/verify.cpp` | exhaustive search over futures using `fb::Core` as the transition function |
| A4 Replayer | `build/replay`, `src/replay.cpp` | run a path on the live exchange; report loss |
| A5 Venue daemon | `build/venued`, `src/venued.cpp` | deploy, pause, cancel, state, events; one isolated sub-account per program |
| A6 Exchange changes | `src/fastbook.{hpp,cpp}` | copy constructor that re-points the `Sink` (for verifier snapshots); flatten-at-next-print helper |
| A7 Soundness suite | `tests/test_verify.cpp`, `tests/test_venued.cpp` | `BOUNDED_KEYS.md` §8.5 tests 1–5, 7 (test 6 runs at wire-up) |
| A8 Fixtures | `fixtures/*.compiled.json`, `*.result.json`, `events_v2_worst.jsonl` | produced by your binaries, checked in |

**Not yours:** keys, the budget, HTTP, Nessie, any LLM, Photon, Daytona, Chalk. If you find
yourself needing one of them, the contract is missing something. Fix the contract, not your scope.

---

## 2. Split between the two people

- **A-core:** A1 → A3 → A7 (verifier tests). Starts from **hand-written** `ex_v1.compiled.json` /
  `ex_v2.compiled.json`, so the verifier never waits for the compiler.
- **A-venue:** A6 → A2 → A5 → A4. The compiler's output must match the hand-written compiled
  fixtures byte for byte (except `source_hash`). That is the handoff test between the two of you.

---

## 3. Component details

### A1 Program format
- Parses `CONTRACT.md` §2.2.
- **Validates:**
  - parents exist and appear earlier in the deploy plan,
  - OCO groups have ≥ 2 members,
  - quantities and prices are within `fb::kMaxQty` / `fb::kMaxPrice`,
  - ≤ 64 nodes,
  - ≤ 3 instruments.
- Converts each node to a `proto::NewOrderMsg` template, with ids resolved at deploy time.

### A2 Compiler
- **Parser:** for `BOUNDED_KEYS.md` §6.1. Errors carry line and column.
- **Static rules:** §6.2. Each violation is a StaticError, exit code 2. The
  `allow < max_gap_ticks` case is a warning in the `warnings` array.
- **Mapping:** §7.
  - `all` / `half` / `position` resolve to the maximum reachable position, and the node is
    reduce-only.
  - Exits and takes without an explicit `after` become children of the entry.
- **Deploy plan:** topological generations.
- **Report:** `locked_collateral_cents` = Σ resting limit notional; stops lock nothing until they
  trigger.
- **Determinism:** node order in the output = source order; group labels `g1, g2, …` in order of
  first appearance.

### A3 Verifier
Read `BOUNDED_KEYS.md` §8 in full before writing a line.

- **State:** `(Core snapshot, node liveness, cash, position per instrument, fees paid, trades)`.
  The snapshot is a `Core` copy (A6).
- **Environment:** account 0 with STP on. The program trades in its own account. Moves are
  real `NewOrderMsg`s from account 0, so all fills, cascades, OCO effects and child releases come
  from the engine.
- **Boundaries:** every trigger and limit price in the program, plus the price at which loss
  reaches `max_loss_cents`. Recompute the loss boundary as the position changes.
- **Moves, in this fixed order** (determinism):
  1. gap down
  2. cross down
  3. cross up
  4. gap up
  5. key-holder pause
  6. key-holder cancel

  A gap jumps to the farthest print within `max_gap_ticks` that crosses at least one boundary,
  and to every intermediate boundary-crossing landing point. Each is a separate branch.
- **Fill model `full_at_print`:**
  - The environment posts unlimited liquidity at the print price, then sweeps it.
  - Orders triggered by that print are aggressors against it.
  - Resting orders fill at their own price when a print trades through them.
- **Memo key:** `Core::hash()` ⊕ hash(liveness) ⊕ hash(exact fees) ⊕ trades. Never bucket fees.
- **Metrics per path:** loss = −(cash change) + position × (entry-relative mark loss) + fees;
  exposure = max |position|; trades = program fills.
- **Output:**
  - The first violation found → Rejection (exit 3).
  - Otherwise, the maxima over all paths, plus the `worst_path` that attains `max_loss` →
    Certificate (exit 0).
- **Timeout:** honor `--timeout-ms` (exit 4).
- **Performance target:** the 3-node fixture in < 50 ms; a 12-node, 1-instrument program in < 1 s;
  3 instruments in < 10 s.

### A4 Replayer
- Spins up a scratch sub-account on the live exchange over TCP.
- Deploys the compiled program, then plays the path's environment and key-holder moves as real
  messages from account 0.
- Reports loss, fees, trades, and `matches_verifier` (equality with the path's final value).

### A5 Venue daemon
- **Protocol:** `CONTRACT.md` §6 exactly. One exchange session per sub-account.
- **Sub-accounts:** each program gets the next free sub-account (1–254), funded with `fund_cents`.
  This is how margin is isolated per program.
- **Deploy:** executes the deploy plan generation by generation, waiting for acks to resolve parent
  ids. On partial failure it cancels what was placed and returns `exchange_down`.
- **Accounting:** computes per-program realized loss, fees and trades from fills, and emits events.
- **Halt detection:** no live or held nodes and position 0 → `program.halted`.
- **Cancel:** cancels nodes, flattens with a market exit, emits fills, then `program.halted`
  (status `cancelled`).
- **Restart:** `reconcile` rebuilds state from the exchange's order and fill state. Programs keep
  running on the exchange whether or not `venued` is up.
- **Stretch: backend `hyperliquid-testnet`.**
  - Native entry + one TP + one SL (`normalTpsl`).
  - Deeper nodes are relayed: a stateless per-program table driven by the fill stream.
  - The certificate must then carry `relayed > 0`.

### A6 Exchange changes
- `Core` copy constructor that re-points the `Sink` to a null or capturing sink.
- `Core::hash()` must already be canonical and independent of order ids. Add a test if it isn't
  covered.
- Nothing else in the engine changes. If the verifier seems to need an engine change, raise it
  first: an engine change can invalidate the hash-identity results.

---

## 4. Milestones (hours from start of build)

| Hour | A-core | A-venue | Exit check |
|---|---|---|---|
| 0–4 | A1 loader; hand-write `ex_v1/ex_v2.compiled.json`; verifier skeleton with move generation | A6 copy ctor + test; `venued` socket skeleton answering `mark` and `state` with stubs | loader round-trips both fixtures |
| 4–10 | full search, metrics, memo, Rejection output | MPL parser + static rules; `ex_noallow` → StaticError | **`verify ex_v1` → 422-shaped Rejection; `verify ex_v2` → Certificate. This cannot slip.** |
| 10–16 | `worst_path`; determinism test (100 runs, identical output) | compiler mapping + deploy plan; output matches the hand-written fixtures | `policyc ex_v2.mpl` == `ex_v2.compiled.json` |
| 16–22 | soundness tests 1, 2, 5 (fuzz, replay, gap edge) | `venued` deploy/pause/cancel/state/events/reconcile; per-program sub-accounts; A4 replayer | `replay worst_path` matches the certificate to the cent |
| 22–26 | test 4 (audit's 273 stop-limits); performance on a 12-node program | test 7 (single-run at the venue: `already_deployed`, `not_live`); `events_v2_worst.jsonl` fixture | all fixtures checked in |
| 26–30 | **wire-up with Build B** (`CONTRACT.md` §8) | **wire-up** | steps 1–9 green |
| 30–36 | freeze; bug fixes only; stretch only if green | Hyperliquid relay (stretch) | — |

---

## 5. Definition of done
- Every fixture in `CONTRACT.md` §7 is produced by your binaries, and pinned fields match.
- `BOUNDED_KEYS.md` §8.5 tests 1–5 and 7 pass in CI.
- `venued` survives a kill −9 during a live program, and `reconcile` reports the correct state
  afterwards.
- Each binary has a `--help` that documents its contract section.
