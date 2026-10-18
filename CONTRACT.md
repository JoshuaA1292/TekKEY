# Contract: the seam between Build A and Build B

Build A (the venue, C++) and Build B (the gateway, Python) meet **only** at the interfaces in this
file. Each build can be finished and tested on its own against this file, then wired together at
the end.

**Rules**
- Every JSON object carries `"contract_version": 1`.
- Changing anything here requires both builds to agree, in writing, in this file, before code
  changes.
- Fields not listed here must be ignored by the receiver, never rejected.

---

## 1. Units and identifiers

| Quantity | Unit | Type |
|---|---|---|
| price | ticks | int64 |
| quantity | lots | int64 |
| money (loss, fees, collateral, balance) | cents | int64 |
| fee per fill | `ceil(price_cents × qty × bps / 10000)` | int64 |
| time | Unix milliseconds | int64 |

- **Instrument parameters** come from `instruments.json`, a shared file owned by Build A:
  `{"ETH": {"tick_cents": 100, "lot_desc": "1 ETH"}}`.
- **`program_id`** is assigned by Build B (int64, unique). Build A never invents program ids.
- **`account`** is the gateway account id (int). Build A maps each **program** to its own exchange
  sub-account (`sub_account`, 1–254; 0 is reserved for the verifier's environment).

---

## 2. Data types

### 2.1 Envelope (as Build B passes it to `verify`)
Exactly `BOUNDED_KEYS.md` §5.1. Build B strips budget state before passing it, because the verifier
is **stateless**: it checks a program against the envelope's limits. The budget check is Build
B's job (§5.2).

### 2.2 CompiledProgram (output of `policyc`, input to `verify` and `venued`)

```json
{
  "contract_version": 1,
  "backend": "minsky",
  "source_hash": "sha256:…",
  "instruments": ["ETH"],
  "nodes": [
    { "id": "entry", "kind": "limit",      "instrument": "ETH", "side": "buy",  "qty": 1, "price": 3000,
      "trigger": null, "reduce_only": false, "parent": null,    "oco": null, "terminal": false },
    { "id": "sl",    "kind": "stop_limit", "instrument": "ETH", "side": "sell", "qty": 1, "price": 2963,
      "trigger": 2988, "reduce_only": true,  "parent": "entry", "oco": "g1", "terminal": false },
    { "id": "tp",    "kind": "limit",      "instrument": "ETH", "side": "sell", "qty": 1, "price": 3030,
      "trigger": null, "reduce_only": true,  "parent": "entry", "oco": "g1", "terminal": false }
  ],
  "deploy_plan": [["entry"], ["sl", "tp"]],
  "report": { "nodes_total": 3, "native": 3, "relayed": 0, "locked_collateral_cents": 300000 },
  "warnings": []
}
```

- **`kind`** is one of `limit | stop_limit | stop_market | halt`.
- **`oco`** is a group label. The compiler emits `OcoReduce` mode for every group.

### 2.3 Certificate and Rejection
Exactly `BOUNDED_KEYS.md` §9.1 and §9.2, with these differences:
- `verify` does **not** emit `program_id`, `account`, `budget` or `key`. Build B adds them.
- Path entries are one of:
  - `{"env": "print"|"gap", ...}`
  - `{"keyholder": "pause"|"cancel"}`
  - `{"event": "filled"|"triggered"|"rests"|"cancelled"|"mark", ...}`

### 2.4 StaticError

```json
{ "contract_version": 1, "status": "static_error", "line": 3, "col": 5, "message": "exit requires allow >= 1 tick" }
```

---

## 3. CLI: `policyc`

```
build/policyc --backend minsky --instruments instruments.json --env env.json  < source.mpl  > out.json
```

- **stdin:** MPL source.
- **stdout:** a CompiledProgram, or a StaticError.
- **`--env`:** only the `env` object (used for the `allow < max_gap_ticks` warning).
- **Exit codes:** `0` compiled · `2` static error · `1` internal error (stderr has details).
- **Deterministic:** the same input gives byte-identical output.

## 4. CLI: `verify`

```
build/verify --program compiled.json --envelope envelope.json --mark 3010 [--timeout-ms 10000]  > out.json
```

- **stdout:** a Certificate (without key or budget) or a Rejection.
- **Exit codes:** `0` certified · `3` rejected · `4` timeout (stdout is `{"status": "timeout"}`) ·
  `1` internal error.
- **Deterministic:** the move order is fixed, so the same input always produces the same output.
  Only `explored.ms` may differ between runs.

## 5. CLI: `replay`

```
build/replay --program compiled.json --path path.json --exchange tcp://127.0.0.1:7000  > out.json
```

- Runs a path (a counterexample or a `worst_path`) on the live exchange in a scratch sub-account.
- **stdout:** `{"contract_version": 1, "loss_cents": …, "fees_cents": …, "trades": …, "matches_verifier": true|false}`
- Build B uses it in evals. Build A uses it in soundness tests.

---

## 6. Daemon: `venued`

`venued` owns every exchange session. Build B never speaks the exchange protocol.

### 6.1 Transport
TCP `127.0.0.1:7010`, newline-delimited JSON, one request → one response. Every request carries
a `request_id`. Repeating a request id returns the original response without re-executing it
(idempotent).

### 6.2 Requests

| `op` | Request fields | Response |
|---|---|---|
| `deploy` | `program_id`, `account`, `compiled` (CompiledProgram), `fund_cents` | `{ok, sub_account, order_ids: {node: id}, locked_collateral_cents}` |
| `pause` | `program_id` | `{ok, cancelled: [node]}` |
| `cancel` | `program_id` | `{ok, cancelled: [node], flatten_fill: {qty, price, fee_cents} \| null}` |
| `state` | `program_id` | `{ok, status, live_nodes, position, realized_loss_cents, mtm_loss_cents, fees_cents, trades, last_print}` |
| `mark` | `instrument` | `{ok, last_print}` (Build B passes this as `--mark` to `verify`) |
| `reconcile` | — | `{ok, programs: [state…]}` (after a restart, rebuilt from exchange state) |
| `subscribe` | — | the connection switches to a one-way event stream (§6.3) |

**Errors:** `{ok: false, error: "unknown_program" | "already_deployed" | "not_live" | "exchange_down", detail}`.

**`status`** is one of `deployed | paused | halted | cancelled`.

### 6.3 Events (on a `subscribe` connection; one JSON object per line, ordered per program)

```json
{"contract_version":1,"type":"node.triggered","program_id":7,"node":"sl","print":2964,"t":…}
{"contract_version":1,"type":"node.filled","program_id":7,"node":"sl","qty":1,"price":2964,"fee_cents":104,"t":…}
{"contract_version":1,"type":"program.halted","program_id":7,"realized_loss_cents":3734,"fees_cents":134,"trades":2,"t":…}
{"contract_version":1,"type":"print","instrument":"ETH","price":2964,"t":…}
```

- `realized_loss_cents` is `max(0, loss)` at halt.
- After `program.halted` or a successful `cancel`, `venued` refuses every further op on that
  program except `state`.

---

## 7. Golden fixtures (`fixtures/`, owned jointly)

These files are the integration test. Build A's tests must **produce** them; Build B's fake venue
**serves** them.

| File | Contents | Pinned fields |
|---|---|---|
| `instruments.json` | §1 | all |
| `envelope_40.json` | `BOUNDED_KEYS.md` §5.1 | all |
| `ex_v1.mpl` | §11.1 source (allow 5) | — |
| `ex_v1.compiled.json` | compiled v1 | all, except `source_hash` |
| `ex_v1.result.json` | Rejection | `status`, `violates`, `limit`; the path must replay to a value > limit |
| `ex_v2.mpl` | §11.2 source (allow 25) | — |
| `ex_v2.compiled.json` | compiled v2 (§2.2 above) | all, except `source_hash` |
| `ex_v2.result.json` | Certificate | `status`, `bound.*`, `nodes.*`, `locked_collateral_cents` |
| `ex_noallow.mpl` | v1 without `allow` | — |
| `ex_noallow.result.json` | StaticError | `status`, `line` |
| `events_v2_worst.jsonl` | the event stream of v2 running its `worst_path` | types and order; numbers |

**Rule:** if Build A's real verifier disagrees with a pinned number, the two builds sit down and
decide whether the bug is in the verifier, the compiler or the hand arithmetic. Then the fixture
is updated in one commit that both sides sign off on.

---

## 8. Wire-up procedure (run in order; each step has a pass condition)

1. **Fixtures agree.** Build A runs `policyc` + `verify` on every `fixtures/*.mpl`.
   *Pass:* pinned fields match.
2. **Swap the fake.** Build B points its venue client at the real binaries and at `venued`
   instead of `fake_venue`.
   *Pass:* Build B's full test suite stays green.
3. **Submit end to end.** `POST /program` with `ex_v1.mpl`, then `ex_v2.mpl`.
   *Pass:* `422` then `200`; the body equals the fixture plus Build B's added fields.
4. **Deploy.** `POST /program/{id}/deploy` with the program key.
   *Pass:* `venued state` shows `deployed`, with three nodes live or held.
5. **Drive the worst path.** Use `replay --path` with the certificate's `worst_path` against the
   deployed program's sub-account.
   *Pass:* `program.halted` arrives with `realized_loss_cents` ≤ `bound.max_loss_cents`; Build B's
   ledger shows remaining = `max_loss_cents` − realized; Nessie balance moved by the same amount.
6. **Budget refusal.** Resubmit `ex_v2.mpl`.
   *Pass:* `409 budget`.
7. **Refusals.** Run every row of `BOUNDED_KEYS.md` §10.4.
   *Pass:* each returns exactly its listed code.
8. **No client needed.** Deploy a fresh program on a fresh account, delete the delegate's sandbox,
   kill Build B's gateway process, drive a crash path, then restart the gateway.
   *Pass:* `venued reconcile` shows the program halted within its bound; the gateway replays the
   missed events into the ledger.
9. **Soundness under integration.** Run `BOUNDED_KEYS.md` §8.5 tests 1, 2, 6 and 7 through the
   gateway rather than directly.
   *Pass:* all green.

---

## 9. Amendments proposed by Build A (implemented on A's side; need Build B's sign-off)

Each item below is what Build A's binaries do today, with the reason. Nothing here removes a field
or changes a pinned fixture value except A9.

| # | Change | Why |
|---|---|---|
| A1 | **Accounts on the exchange.** The exchange runs with `--accounts 255`. Programs get sub-accounts **1–252** (not 1–254); **253** is `replay`'s scratch account; **0 and 254** are the environment. | Self-trade prevention stops one account trading with itself, so a print needs two environment accounts. |
| A2 | **`pause`** cancels every node that is **not reduce-only** (entry, scale, hedge: the nodes that can open exposure), resting or held. Every reduce-only node stays, stops **and takes**. BOUNDED_KEYS §8.2 and §10.3 ("non-exit nodes cancelled; exits stay") should say this. | With takes cancelled, a paused long has no upper exit, and its fee exposure is unbounded (A8): `ex_v2` itself would stop certifying. The verifier and `venued` use the same rule. |
| A3 | **`cancel` flattens natively**: a reduce-only OCO pair, a stop-market one tick through the last print and a limit at the last print. The position closes on the next print in either direction with no client connected. `flatten_fill` is `null` unless the pair crossed when placed (new field `flatten_pending: true`), and the fill arrives later as `node.filled` with `"node": "flatten"`, then `program.halted` (status stays `cancelled`). | §8.2 says "flatten with a market exit at the next print"; a market order sent between prints has no liquidity, and the whole point is that nothing outside the venue has to be alive. The verifier searches exactly this. |
| A4 | **New `venued` error `bad_request`** (malformed request, bad CompiledProgram, unknown instrument or op). If all 252 sub-accounts are live, deploy answers `exchange_down` with detail `"no free sub-account"`. | §6.2's list has no code for these. |
| A5 | **Extra response fields** (receivers ignore unknown fields): `deploy` echoes `program_id`; `state`/`reconcile` entries add `program_id`, `account`, `sub_account`, `positions {instr: lots}`; `cancel` adds `flatten_pending`. | Multi-instrument programs; reconcile entries must say which program they are. |
| A6 | **Path entries.** Gaps carry their origin: `{"env": "gap", "from": 3000, "to": 2982}`. With more than one instrument, env entries carry `"instrument"`. Events: `triggered` has `print` and `limit` (`null` for stop-market); `rests` has `reason`. Every path starts with one `{"env": "print", "at": MARK}` per instrument (the settle print after deployment) and ends with `{"event": "mark", "position", "loss_cents"}`. | `replay` needs the move list back; the final mark is what `matches_verifier` compares. |
| A7 | **Rejections can name** `max_loss_cents`, `max_fees_cents`, `max_trades`, `max_exposure_lots`, `require_live_stop` or `instruments` (the program trades an instrument the envelope does not allow; `limit` is the allowed list, `value_on_path` the instrument, `counterexample` empty). | §9.2 shows only `max_loss_cents`. |
| A8 | **Fee exposure is part of the proof.** A fill at a market-determined price (a stop at the print, a flatten, a hedge released while its own book is elsewhere) pays a fee on that price's notional, and nothing in the model stops a rally. A long with **no upper exit** (no take-profit), or a hedge leg released by another instrument's fill, therefore has **unbounded** fees: `verify` answers `422 max_fees_cents` with the rally path (`ex_v2` without its take: "entry fills at 3000; ETH prints 13,430; the key holder cancels; the flatten sells at 13,429 and pays $4.71", total fees 501 > 500, while the program is $10,424 in profit). The cap is absolute cents, so it binds even on winning paths; if that is not the product we want, the fix is in the envelope's definition, not the verifier. **Build B: tell delegates (and the Droid) that every program needs a take-profit**; it is the most common repair after `allow`. | Found by soundness test 1: without it, 1 in ~2,000 random paths beat a certificate's fee bound by 1–2 cents. With it, 0 in 27,000. |
| A9 | **`ex_v2.result.json`: `bound.max_fees_cents` is 136, not 135.** The key holder can cancel while the price sits at 3029 (just under the take-profit); the flatten's stop then fills at 3028 as a taker (106 cents). Other bound fields match §11.2 (3734 / 1 / 2). | The verifier is authoritative (BOUNDED_KEYS §11 note). The hand figure ignored the key holder's moves. |
| A10 | **The audit has 253 zero-allowance stop-limits, not 273** (21 + 33 + 143 + 22 + 34 in `audit/audit.txt`; `fixtures/audit_stoplimits.json`). All 253 are static errors; forced through with `allow 0` the verifier finds the gap path for all 253; with `allow = max_gap_ticks` all 253 certify. | BOUNDED_KEYS §1.2, §6.2, §8.5 test 4 say 273. |
| A11 | **Deployment in the model.** The verifier deploys into a book that already has a last print (one print at each mark, as `venued` deploys into the live book), then the settle print at the mark that opens every path. An order marketable at deployment fills at its own limit, never better than the print. | Same message sequence as the live venue. |
| A12 | **Tool conveniences.** `policyc`, `verify` and `replay` find `instruments.json` via `--instruments`, `$BK_INSTRUMENTS`, `./`, `./fixtures/`, `../fixtures/`. `verify --mark` takes one price or `NAME=P,NAME=P`. `replay --attach SUB [--journal-base N]` plays only the environment's moves against a program `venued` already deployed (wire-up step 5); its output adds `expected_loss_cents`, `position`, `sub_account`, `books`. | Wire-up steps 5 and 8. |

### 9.1 Build B's response (2026-10-16)

All twelve accepted. What changed on B's side, per item (nothing else needed):

| # | Build B |
|---|---|
| A1, A4, A11, A12 | No change. `VenueError` carries any error string; `replay --attach` is what `gateway.wireup` and the test suite's real mode use for step 5. |
| A2 | `fake_venue.pause` now keeps every reduce-only node (stops and takes) and cancels the rest; `test_frozen_account_refuses_submissions_and_pauses_live` asserts the take stays. |
| A3 | `POST /program/{id}/cancel` no longer settles synchronously: the key is spent at once, `cancel_requested_t` is recorded, and the ledger waits for `node.filled "flatten"` + `program.halted` (or `reconcile`) through the same path. The response carries `flatten_pending` and `status: cancelling` until then. The fake reports the same shape. |
| A5, A6, A7 | Receivers ignore unknown fields; the fake's paths now open with the settle print at the mark and the gateway's own `instruments` rejection uses `value_on_path: <instrument>`. |
| A8 | The delegate's MPL reference, the Droid's prompt and the stub repairer all say every program needs a take-profit; the fake verifier rejects a program without an upper exit with `422 max_fees_cents` and the rally path (ex_v2 without its take: flatten at 13,429, fees 501). |
| A9 | `gateway/fixtures/ex_v2.result.json` is 136 and the fake model includes the cancel-after-rally-print path. |
| A10 | Noted for `BOUNDED_KEYS.md`; the eval suite does not depend on the count. |

Verified against A's binaries on 2026-10-16: `./gateway/demo.sh real` passes wire-up steps 1 to 9
(5 via `replay --attach`), and `GATEWAY_TEST_VENUE=real pytest gateway/tests` runs B's suite on a
fresh exchange + `venued` per test: 28 passed, 1 skipped (the fake's own persistence test).
Two things A should know: `venued state.live_nodes` lists held children as live (B's tests accept
either); and `mark` answers `last_print: null` before any print, so B uses `DEFAULT_MARK` (3010)
until the venue has printed.

### 9.2 Amendment proposed by Build B: raw orders for the control group (needs Build A's sign-off)

The lab's headline test (`lab/CONTRACT.md` T4, "the race") runs the same model, on the same live
book and the same tape, twice: once with a bounded key through the gateway, once with a **binary
key**: a plain session where orders are just placed. Today the binary-key side has to be
simulated, which weakens the comparison. Build B asks `venued` for four ops that give a
sub-account with no program, no verifier and no budget, so the control group trades on the real
exchange:

| `op` | Request fields | Response |
|---|---|---|
| `raw_open` | `account`, `fund_cents` | `{ok, sub_account}` (allocated like a program's, funded with `fund_cents`) |
| `raw_order` | `sub_account`, `instrument`, `side`, `qty`, `kind` ∈ `limit\|market\|stop_limit\|stop_market`, `price` (null for market), `trigger` (null unless stop), `reduce_only` | `{ok, order_id}` |
| `raw_cancel` | `sub_account`, `order_id` | `{ok}` |
| `raw_state` | `sub_account` | `{ok, position, positions, cash_cents, fees_cents, trades, open_orders:[{order_id, kind, side, qty, price, trigger}], liquidated: bool}` |

Rules: a raw sub-account is liquidated (everything cancelled, position flattened on the next
print, `liquidated: true`) when its mark-to-market loss reaches `fund_cents`; fills appear on the
event stream as `node.filled` with `"program_id": null, "sub_account": N, "node": "raw:<order_id>"`;
`replay --attach` plays the tape onto the live book exactly as for a program, so both sides see
the same prints. Build B's `gateway/venue.py` already carries the client side
(`raw_open/raw_order/raw_cancel/raw_state`) and the lab labels the raw side `simulated` until
`venued` answers these ops.
