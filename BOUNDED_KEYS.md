# Bounded keys

*An API key whose permission is a theorem.*

Builds on `MINSKY.md`, `minsky/` (the exchange over a wire, native chaining v2) and
`stage1-orderbook-computer/` (the exact engine and the proofs). This is the product definition.
The two build specs (`BUILD_A_VENUE.md`, `BUILD_B_GATEWAY.md`) and the seam between them
(`CONTRACT.md`) implement it. Where they disagree with this file, they are wrong until this file
changes.

---

## 0. The idea, in plain words

When you give a program access to a trading account today, you give it a key. The venue asks one
question about that key: *is this caller allowed to send orders?* The answer is yes or no. If yes,
the program can do anything an order can do: buy at any price, churn the account into fees,
place a stop that will never fill, or sit on a position with no stop while it is offline. The key
cannot tell the difference between a careful strategy and a ruinous one, because the key doesn't
look at strategies. It looks at callers.

The owner of the account is asking a different question: *what is the most this program can cost
me?* No key at any venue today can answer it.

A bounded key answers it. The program never gets permission to send orders. It sends a
**program**: its whole strategy, written in a small language whose every instruction is an order the
venue already knows how to execute. The venue compiles the program into its own native orders,
runs **every future** of that compiled program on an exact copy of its own matching engine, and
answers in one of two ways:

- **a certificate**: "on every path, under these stated assumptions, this program loses at most
  $37.34, pays at most $1.35 in fees, and finishes within 2 trades", plus a key that can start,
  pause or cancel *that program and nothing else*; or
- **a counterexample**: "here is the exact sequence of fills on which it loses $40.30", which the
  author can read, replay and fix.

The worst case is a number printed before the first order rests. The program can then be handed to
an LLM agent, a copy-trading bot, a junior quant or a stranger. Whatever they do with the key, they
cannot exceed the proof.

This is possible for one reason: **an order book is a computer**. Trigger orders, parent/child
links and OCO groups are enough to express a program. Any finite set of such orders halts, so the
search over futures terminates. And our engine is exact, so the search is the real thing rather
than a simulation of it.

---

## 1. The problem with binary keys

### 1.1 What venue keys control today

| Control | What it bounds | What it does not bound |
|---|---|---|
| Read / trade / withdraw scopes | whether funds can leave the venue | anything that happens *through trading* |
| IP allowlist | who can use the key | what they do with it |
| Sub-account balance cap | how much is reachable | how much of it can be lost (100% of the cap) |
| Rate limits | how fast | whether it is harmful |
| Max order size / leverage | one order's size | the sequence of orders |

Every one of these is a check on **a caller** or **a single order**. Losses come from **sequences of
orders interacting with a moving market**, and nothing at the key layer looks at sequences.

### 1.2 What that costs (from our audit of five LLM-run wallets, `audit/`)

| Failure | Key setting that would have prevented it |
|---|---|
| 273 of 273 stop-limits placed with zero allowance: they trigger in a gap and rest unfilled | none |
| One wallet paid 13.5% of its capital in fees through churn | none |
| ~1,000 hours per wallet with an open position and no live stop | none |
| 23 liquidations across five wallets | none |

None of them were unauthorized. The key worked exactly as designed. The design was binary.

### 1.3 Why now

Agents that hold money went from a curiosity to a category within a year. Every one of them runs
on a key that answers *may it trade*, while the only real protection is the model's good
behavior, which a prompt injection, a bad update or a hallucination can remove.

---

## 2. What a bounded key is

### 2.1 Three key classes

| Class | Prefix | Held by | Can | Cannot |
|---|---|---|---|---|
| **Owner** | `ok_` | the account owner | set the envelope, fund, freeze, read everything, issue author keys | is never given to a delegate |
| **Author** | `ak_` | the delegate (agent, bot, person) | submit programs for certification, read its own programs | place, cancel or modify any order; deploy anything |
| **Program** (the bounded key) | `bk_` | whoever runs the strategy | deploy, pause, cancel **one** certified program, **once**; read its state | place an order; touch any other program; redeploy after it ends |

The API has **no order endpoint**. `POST /order` returns `404`. Orders are something the venue
derives from a certified program, never something a caller sends.

### 2.2 The one new method

```
POST /program   { account, backend, source }
  → 200  certificate + bk_ key
  → 422  counterexample (the first path that breaks the envelope)
  → 409  certified, but the account's remaining budget can't cover it
  → 400  static error, with line number
```

Everything else in this document exists to make that one call honest.

---

## 3. What is guaranteed

**Guarantee.** For a certified program, on every path the verifier explores, under the
assumptions printed on its certificate, including **every pause or cancel the key holder could
issue at any moment**:

- realized plus mark-to-market loss never exceeds `bound.max_loss_cents`;
- fees never exceed `bound.max_fees_cents`;
- position never exceeds `bound.max_exposure_lots`;
- the program halts within `bound.halts_within_trades` trades.

**Account-level guarantee.** Over any rolling 24-hour window, the account's total loss across all
programs, past and live, never exceeds the envelope's `max_loss_cents` (§5.2). This is what
stops a turned delegate from draining the account one certified program at a time.

**Not guaranteed.**
- Profit.
- Behavior of other participants outside the stated model (`assumes`).
- Venue rules other than the ones our engine implements. The canary (`minsky/src/canary.cpp`)
  tests those separately.
- Anything if the **owner** key leaks.

---

## 4. Threat model

| Threat | With a binary key | With a bounded key |
|---|---|---|
| Delegate offline when the market moves | position unprotected until it returns | the program is resting inside the venue; it runs with nobody connected |
| Delegate turned (prompt injection, bad update, hallucination) writes a draining strategy | executes | it can only submit programs; a draining program gets `422` with its path |
| Delegate turned, submits *many* certified programs in a row, losing within each bound | n/a | the rolling budget (§5.2): once spent, every new program gets `409` |
| Correct-looking program with a hidden path to ruin (zero-allowance stop-limit) | rests unfilled in a gap; liquidation | `422` with the gap path shown |
| Churn into fees | allowed | `max_fees_cents` per program and per window |
| Key holder pauses or cancels at the worst moment | n/a | pause and cancel are adversarial moves inside the verifier's search; the bound already includes them |
| A bounded key leaks | attacker trades freely | attacker can deploy, pause or cancel one certified program, once. That's it. |
| Loss in one program triggers liquidation in another | possible under cross margin | each program runs in its own isolated sub-account; bounds add |
| Venue changes its matching rules | silent | out of scope for the certificate; covered by the canary |

Out of scope: owner-key compromise, venue insolvency, a venue lying about its fills.

---

## 5. The envelope and the budget

### 5.1 The envelope (set by the owner, per account)

```json
{
  "contract_version": 1,
  "account": 3,
  "window": "24h_rolling",
  "max_loss_cents": 4000,
  "max_fees_cents": 500,
  "max_trades": 60,
  "max_exposure_lots": 2,
  "instruments": ["ETH"],
  "require_live_stop": true,
  "env": {
    "max_gap_ticks": 25,
    "fill": "full_at_print",
    "fee_bps": { "maker": 1.0, "taker": 3.5 },
    "trigger_ref": "last"
  }
}
```

`env` is the model of everyone else, and it belongs to the owner on purpose: **the owner chooses
how pessimistic the proof is.** `require_live_stop` means every reachable state with a nonzero
position has a live or held exit node.

### 5.2 The budget

Every program's certified bound is a *reservation* against the account's window:

```
remaining = max_loss_cents
          − Σ realized losses of programs that ended inside the window
          − Σ certified max_loss of programs still live
```

- A new certificate is issued a key only if `bound.max_loss_cents ≤ remaining`; otherwise `409`.
- When a program ends, its reservation is replaced by its realized loss, which is never more than
  the reservation.
- **Profits do not replenish the budget.** Realized loss is counted as `max(0, loss)`.
- `max_fees_cents` and `max_trades` are budgeted the same way.
- `max_exposure_lots` is instantaneous: the sum of live programs' exposure bounds must fit.

### 5.3 Composition

Each program runs in its own **isolated sub-account** on the exchange, so one program's loss cannot
cause another's liquidation, and certificates add. At most one live program per (account,
instrument); programs sharing an instrument would have to be certified jointly, which this version
refuses (`409 instrument_busy`).

---

## 6. The policy language (MPL)

Small, typed and finite. It describes a decision tree over price paths whose nodes are
venue-native orders. It cannot loop except through explicit, counted re-arms. **The grammar is the
guardrail: what MPL cannot express, the venue cannot be asked to run.**

### 6.1 Grammar

```
program   := header node+
header    := "instrument" IDENT ("," IDENT)*  "entry" side qty "at" price_expr
node      := IDENT ":" action [ "when" cond ] [ "after" IDENT ] [ "oco" IDENT ("," IDENT)* ] [ "rearm" INT ]
action    := "exit"  qty_expr "at" price_expr "allow" ticks           -- stop-limit with allowance
           | "exit"  qty_expr "market"                                 -- stop-market
           | "take"  qty_expr "at" price_expr                          -- limit, reduce-only
           | "scale" side qty "at" price_expr                          -- add to position (limit)
           | "hedge" IDENT side qty "at" price_expr                    -- order in another instrument
           | "halt"
cond      := "price" cmp price_expr | "fill" IDENT | "position" cmp qty_expr
cmp       := "<=" | ">="
price_expr:= price | "entry" ("+"|"-") ticks | "entry" ("+"|"-") bps "bps"
qty_expr  := qty | "all" | "half" | "position"
side      := "buy" | "sell"
```

### 6.2 Static rules (checked before compilation; violations are `400` with a line number)

- Every `exit … at` has `allow ≥ 1`. (This rule alone rejects every stop-limit in the audit.)
- Exit and take nodes without an explicit `after` are children of the entry: held until it fills.
- Every node is reachable from the entry.
- `rearm` counts are finite; total nodes after expansion ≤ 64.
- At most 3 instruments. The search is exponential in instruments (stage 1, S4), and the product
  says so rather than hiding it.
- Quantities are integer lots and prices integer ticks.
- **Warning, not error:** `allow < env.max_gap_ticks` on a stop-limit. The verifier will almost
  certainly find a gap path; the warning tells the author why before they wait for it.

---

## 7. Compilation

Every MPL node becomes a native order on our exchange (`fb::Core`, v2 chaining):

| MPL | Native order |
|---|---|
| `entry buy q at p` | Limit `qty=q`, `price=p` |
| `exit q at p allow t when price <= x` | StopLimit, `trigger=x`, limit `p − t` (sell) / `p + t` (buy), reduce-only |
| `exit q market when …` | StopMarket, reduce-only |
| `take q at p` | Limit, reduce-only |
| `scale`, `hedge` | Limit in the named book |
| `after N` | `parent = N`; held until N fills completely |
| `oco A, B` | shared OCO group, mode `OcoReduce` |
| `rearm K` | K chained copies, each a child of the previous copy's exit |
| `halt` | no order; marks a terminal node for the verifier |
| `all` / `half` / `position` | the maximum reachable position (or half of it), reduce-only so it can never overshoot |

The compiler also outputs a **deploy plan** (parents before children, one generation per round
trip) and a report: `nodes_total`, `native`, `relayed`, `locked_collateral_cents`. On our exchange
every node is native (`relayed = 0`), so after deployment the program runs inside the venue
with no external process involved.

---

## 8. Verification

A bounded model checker whose transition function **is the matching engine**. There is no
abstraction of the matching rules: every transition is `fb::Core` processing a real message.

### 8.1 The environment (everyone else)

Prices move as a sequence of **prints**. The model, stated on every certificate:

- **Gap.** Consecutive prints differ by at most `max_gap_ticks`.
- **Fill (`full_at_print`).** At each print, unlimited size trades at that price.
  - An order triggered *by* that print is an aggressor at the print. It fills at the print price if
    its limit allows, otherwise it rests.
  - A resting order fills at its own price when a later print trades through it.
- **Fees.** `fee_bps`, rounded **up** to the cent on every fill.
- **Triggers.** Evaluated on `last` print (or `mark`, if the envelope says so).

### 8.2 The moves

At every state the verifier branches over:

1. **Environment, cross:** the next print lands on the next boundary up or down.
2. **Environment, gap:** the next print jumps up to `max_gap_ticks` past one or more boundaries,
   with no prints between. This is the move that defeats a stop-limit whose allowance is too small.
3. **Key holder, pause:** cancel untriggered non-exit nodes; keep exits.
4. **Key holder, cancel:** cancel everything and flatten with a market exit at the next print.

The **boundaries** are every trigger and limit price in the compiled program, plus the price at which
loss would cross the envelope. Between two boundaries nothing in the program can change, so prices
between them are not explored. This is sound because triggers and limits are the only
price-dependent rules in the engine.

### 8.3 Search

- **Method:** depth-first, with memoization on `Core::hash()` (canonical, independent of order ids)
  combined with the node-liveness vector, **exact** fees paid and trades so far. Fees are never
  bucketed: merging states with different fees could under-report `max_fees`.
- **Per path:** the verifier tracks realized + mark-to-market loss, fees, max |position| and trades.
- **Termination:** a finite resting set with no relayed nodes halts within Q/2 trades (stage 1,
  E7). `rearm K` is finite by construction.
- **Determinism:** the move order is fixed, so the same input always gives the same output.
- **Cost:** for one instrument, about 12 nodes and roughly 24 boundaries, it is about 10⁵–10⁶
  engine steps at roughly 1 µs each, so under a second.

### 8.4 Output

- **Certified:** the maximum of each metric over all paths, plus `worst_path`, the path that
  attains `max_loss`. That path shows the bound is tight, not just an upper limit.
- **Rejected:** the first path on which any metric exceeds the envelope, as environment moves and
  engine events, ending at the violation.

### 8.5 Soundness, as tests (failing any of these is a red build)

1. **Fuzz never beats a certificate.** 10,000 random move sequences within the model, run against
   a certified program on the real exchange over TCP. Realized loss ≤ bound on every run.
2. **Every counterexample replays.** Each rejection's path, run on the exchange, reproduces the
   reported loss to the cent.
3. **Compiled equals drawn.** The `test_wire` harness extended to compiled programs: the
   in-process engine and the exchange over the wire produce the same fills and the same final
   state.
4. **The audit fails.** Every one of the 273 zero-allowance stop-limits is rejected by the static
   check. With `allow 0` forced through, the verifier finds the gap path.
5. **Gaps at the edge.** The fuzz from test 1 is re-run with every gap exactly `max_gap_ticks`.
6. **The budget holds.** A loop of certify → deploy → worst path → halt → certify gets `409` as
   soon as `remaining` falls below the next bound. The realized total never exceeds
   `max_loss_cents` in the window.
7. **Keys are single-run.** Deploying a program key a second time, after halt or after cancel,
   returns `403 key_spent`.

---

## 9. Certificate and rejection

### 9.1 Certificate

```json
{
  "contract_version": 1,
  "status": "certified",
  "program_id": 7,
  "account": 3,
  "bound": { "max_loss_cents": 3734, "max_fees_cents": 135, "max_exposure_lots": 1, "halts_within_trades": 2 },
  "assumes": { "max_gap_ticks": 25, "fill": "full_at_print", "fee_bps": { "maker": 1.0, "taker": 3.5 },
               "trigger_ref": "last", "margin": "isolated_per_program" },
  "envelope_checked": { "max_loss_cents": 4000, "max_fees_cents": 500, "max_trades": 60, "max_exposure_lots": 2 },
  "worst_path": [ "…environment moves and engine events…" ],
  "explored": { "states": 0, "paths": 0, "depth": 0, "ms": 0 },
  "nodes": { "total": 3, "native": 3, "relayed": 0 },
  "locked_collateral_cents": 300000,
  "engine": "fastbook v2 @ <git sha>",
  "budget": { "remaining_before_cents": 4000, "remaining_after_cents": 266 },
  "key": "bk_7f3a…"
}
```

### 9.2 Rejection

```json
{
  "contract_version": 1,
  "status": "rejected",
  "program_id": 8,
  "violates": "max_loss_cents",
  "limit": 4000,
  "value_on_path": 4030,
  "counterexample": [
    { "env": "print", "at": 3000 }, { "event": "filled", "node": "entry", "qty": 1, "price": 3000, "fee_cents": 30 },
    { "env": "print", "at": 2989 },
    { "env": "gap",   "to": 2964 }, { "event": "triggered", "node": "sl", "limit": 2983 },
                                    { "event": "rests", "node": "sl", "reason": "limit above print" },
    { "env": "print", "at": 2960 }, { "event": "mark", "position": 1, "loss_cents": 4030 }
  ]
}
```

---

## 10. API

HTTP + JSON. `Authorization: Bearer <key>`. Keys are random 32-byte tokens; the server stores
only a hash.

### 10.1 Owner (`ok_`)

| Method | Path | Result |
|---|---|---|
| `POST` | `/account` | creates exchange account + Nessie account, funds it → `{account, owner_key}` |
| `PUT` | `/account/{a}/envelope` | sets the envelope (§5.1) |
| `POST` | `/account/{a}/author-key` | → `{author_key}` |
| `POST` | `/account/{a}/freeze` | every live program is paused down to its exits; new submissions refused |
| `GET` | `/account/{a}` | balance, envelope, budget remaining, live programs, alarms |

### 10.2 Author (`ak_`)

| Method | Path | Result |
|---|---|---|
| `POST` | `/program` | `200` certificate + key · `422` counterexample · `409` budget / instrument busy · `400` static error |
| `GET` | `/program/{id}` | the certificate or rejection |
| `GET` | `/program/{id}/counterexample` | the path, replayable on the exchange |

### 10.3 Program key (`bk_`)

| Method | Path | Result |
|---|---|---|
| `POST` | `/program/{id}/deploy` | orders rest → `{order_ids, sub_account, locked_collateral_cents}`; once only |
| `POST` | `/program/{id}/pause` | untriggered non-exit nodes cancelled; exits stay |
| `POST` | `/program/{id}/cancel` | everything cancelled, position flattened; key becomes spent |
| `GET` | `/program/{id}/state` | live nodes, position, loss vs bound, fees vs bound, trades vs bound, status |

There is no resume in this version. To continue after a pause, submit a new program, which is
certified from scratch against the remaining budget.

### 10.4 Refusals (these define the invention as much as the successes)

| Attempt | Response |
|---|---|
| `POST /order` with any key | `404` (the route does not exist) |
| deploy with an author key | `403 key_class` |
| any call on program N with program M's key | `403 key_bound_to_program` |
| deploy a second time, or after cancel or halt | `403 key_spent` |
| `PUT /envelope` with a non-owner key | `403 key_class` |
| certified program larger than the remaining budget | `409 budget` with `{remaining_cents, requested_cents}` |
| second live program on the same instrument | `409 instrument_busy` |
| `exit … at` without `allow` | `400 line N: exit requires allow ≥ 1 tick` |
| relayed nodes when the envelope forbids relay | `422 relay_forbidden` |

### 10.5 Events (`GET /account/{a}/events`, server-sent)

| Event | When |
|---|---|
| `program.certified` / `program.rejected` | after each submission (rejected includes the author key id and the violated bound) |
| `program.deployed` | orders rest |
| `node.triggered` / `node.filled` | in the order they happen |
| `program.halted` | the program ended; carries realized loss, fees and trades |
| `budget.low` | remaining < 25% of `max_loss_cents` |
| `agent.offline` | a live program and no author-key request for T seconds |
| `rejections.burst` | ≥ 3 rejections from one author key in a minute (the signature of a turned agent) |

---

## 11. Worked example

**Units:** 1 lot = 1 ETH, 1 tick = $1. Envelope as in §5.1: $40 per window, gap ≤ 25 ticks.
The mark when submitted is 3010.

### 11.1 First attempt: rejected

```
instrument ETH
entry buy 1 at 3000
sl: exit all at entry - 12 allow 5 when price <= entry - 12   oco tp
tp: take all at entry + 30                                    oco sl
```

- **The stop:** trigger 2988, limit 2983.
- **The path:** the entry fills at 3000, the last print is 2989, then the market gaps 25 ticks to
  2964.
- **What happens:** the stop triggers, but its limit (2983) is above the print, so it rests. The
  next print, 2960, marks the loss at (3000 − 2960) × $1 + $0.30 entry fee = **$40.30**.
- **Result:** `422`, with that path. The compiler had already warned that allow 5 < gap 25.

### 11.2 Second attempt: certified

The only change is `allow 25`, which moves the limit to 2963. The worst trigger print is still 2964,
and 2964 ≥ 2963, so the stop fills at 2964.

- Price loss: (3000 − 2964) = $36.00
- Entry fee: 3000 × 1 bp, rounded up = $0.30
- Exit fee: 2964 × 3.5 bp, rounded up = $1.04
- **Worst loss: $37.34** → `max_loss_cents: 3734`

The highest fees fall on the no-gap path: $0.30 + (2988 × 3.5 bp = $1.05) = $1.35, so
`max_fees_cents: 135`. The program makes at most 2 trades. A key holder who cancels at the worst
moment is flattened at the next print, which cannot be worse than 2964, so the bound is unchanged.

Result: `200`, the certificate in §9.1, and `bk_7f3a…`. Budget: $40.00 → $2.66 remaining while
the program is live.

### 11.3 Third attempt: the turned delegate

Suppose program 7 runs, hits its worst path and halts with a realized loss of $37.34. The delegate,
now turned, submits the identical program again.

- It certifies at $37.34, but only $2.66 is left in the window.
- `409 budget {remaining_cents: 266, requested_cents: 3734}`.
- The owner's phone gets `budget.low`.

The account cannot lose more than $40 today, no matter what the delegate submits.

> These figures are what the model predicts by hand. The verifier's output is authoritative: if it
> disagrees, either the model, the compiler or this arithmetic is wrong, and that is a bug to find,
> not to round away.

---

## 12. Limits, stated plainly

- **The bound is conditional.** It depends on the gap, the fill model and the fee schedule, all
  printed on the certificate. This is "a worst case under stated assumptions", which is what every
  risk system offers. The difference is that this one prints its assumptions.
- **`full_at_print` is optimistic about depth.** It assumes unlimited size trades at every print. A
  pessimistic partial-fill model is the next version.
- **Three instruments is the ceiling.** The search is PSPACE-complete in instruments, and the
  product says so.
- **On a venue with shallow native chaining** (for example Hyperliquid's two levels), deeper nodes
  are relayed by a stateless table in our gateway. The certificate carries `relayed > 0`, and in that
  case the key's semantics are enforced by our gateway, not by the venue.

---

## 13. Glossary

- **Bounded key:** a key that can deploy, pause or cancel one certified program, once.
- **Envelope:** the owner's limits plus the model of other participants that the proof depends on.
- **Budget:** the envelope's loss, fee and trade limits as a rolling 24-hour reservation ledger.
- **Certificate:** the verifier's bounds, assumptions, worst path and search statistics for one
  program.
- **Counterexample:** the first path on which a program breaks its envelope.
- **Native node:** a program node the venue executes itself.
- **Relayed node:** a node a stateless table submits after a fill, on venues whose native chaining
  is too shallow.
