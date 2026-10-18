# Build B: the gateway

The public face of the bounded key (`BOUNDED_KEYS.md`): who is allowed to ask for what, how much
has the account spent, and who needs to know. Python 3.12, FastAPI, SQLite. Build A (`minsky/`)
owns the exchange, the compiler, the verifier and `venued`; this build only runs binaries and
talks to a socket, through the seam in `CONTRACT.md`.

```
.venv-gateway/bin/python -m pytest                 # gateway, agent and eval tests, all on the fake venue
./gateway/demo.sh                                  # the §11 story on the fake venue (9 wire-up steps)
./gateway/demo.sh real                             # the same against Build A's exchange + venued
VENUE=real .venv-gateway/bin/python -m gateway     # serve on :8080 against the real venue
```

## What is where

| Component | File | What it does |
|---|---|---|
| B1 API | `app.py` | every route in §10 and no others; `POST /order` is FastAPI's own 404 |
| B2 keys | `keys.py` | `ok_` / `ak_` / `bk_`; 32 random bytes, base32; only `sha256(key)` stored; `bk_` states `issued -> deployed -> paused -> spent` |
| B3 budget | `budget.py` | the rolling 24 h reservation ledger (§5.2); the `409` decision; the invariant check that freezes the account |
| B4 venue | `venue.py`, `fake_venue.py` | `RealVenue` runs `policyc` / `verify` / `replay` and speaks to `venued`; `FakeVenue` serves the fixtures and plays worst paths on a timer |
| B5 bank | `nessie.py` | Nessie customer + account per gateway account; escrow per program; settlement at halt; local simulator without a key |
| B6 events | `events.py` | one consumer of `venued subscribe`; SQLite, then SSE and the phone; reconcile on start |
| B7 phone | `photon.py`, `../phone/` | envelope by voice with full read-back and spoken confirm; alarms; `freeze` / `status` only; delivered on a Photon Spectrum iMessage line by the Node sidecar |
| B8 sandbox | `../agent/` | the delegate (writes MPL from a brief), the Daytona image, the egress policy |
| B9 repair | `repair.py` | the Factory Droid (`droid-sdk`, then the CLI) turns a counterexample into patched source; at most 3 rounds; incident reports |
| B10 data | `../chalk/`, `../evals/` | every LLM call through the Chalk Model Gateway; the federated view and dataset export; the eval suite |
| core | `service.py` | the trusted logic behind every route; `app.py` is a thin HTTP layer over it |

`VENUE=fake|real` is the only switch. Nothing else changes at wire-up.

## The one new method

`POST /program {account, backend, source}` with an author key:

1. authenticate: author key bound to this account; account not frozen; envelope set
2. `policyc` (exit 2 -> `400` with the StaticError and its line)
3. instruments must be inside the envelope (else `422 violates: instruments`)
4. `venued mark` -> `verify --mark` (exit 3 -> `422` with the counterexample, stored and
   emitted as `program.rejected`; exit 4 -> `503 verify_timeout`)
5. **in one `BEGIN IMMEDIATE` transaction:** instrument busy check (`409 instrument_busy`),
   budget check and reservation (`409 budget {remaining_cents, requested_cents}`), mint the
   `bk_` key, store, emit `program.certified`
6. `200`: the certificate plus `program_id`, `account`, `budget` and `key`

The transaction is what stops two simultaneous submissions from both fitting into the last of
the budget; `test_concurrent_submissions_cannot_both_fit` fires four at once and exactly one
certifies.

## Where each external service does real work

| Service | Real when | Otherwise | Honest note |
|---|---|---|---|
| Build A's venue | `VENUE=real` and `minsky/build/{policyc,verify,replay,venued}` exist | `fake_venue.py`: pinned fixtures by source hash, a Python model of the verifier for other programs, a simulated `venued` | the fake reproduces the §11 arithmetic to the cent and the real verifier's 136-cent fee bound, but it is not an exhaustive search; every fake certificate says `"engine": "fake_venue"` |
| Nessie (Capital One) | `NESSIE_API_KEY` set | `LocalBank`, same ledger in SQLite | Nessie is a simulated bank. The flow is real (customer, account, deposit, escrow per program, settlement at halt); the money is not. `GET /account` shows which bank is in use |
| Chalk Model Gateway | `CHALK_ROUTER_API_KEY` + `CHALK_ENV_ID` set (older names `CHALK_API_KEY` / `CHALK_AI_GATEWAY_URL` still accepted) | Anthropic directly if credentials exist (dev only, logged); else `StubLLM` | every model call in `chalk/ai_gateway.py`: `claude-*` through the Anthropic SDK at `https://api.chalk.ai/v1/router` with `Authorization: Bearer` and `X-Chalk-Env-Id`; `openai/*`, `google/*` through the router's `/chat/completions`. The stub is rule-based and offline; its results carry `route: stub` and are not evidence about any model |
| Chalk datasets | `CHALK_CLIENT_ID` + `CHALK_CLIENT_SECRET` and `chalkpy` importable | rows written to `evals/results/chalk/<dataset>.<revision>.json` in dataset shape | `chalk/federated.py` `push_rows`; the upload path has not been exercised against a live environment from here |
| Daytona | `DAYTONA_API_KEY` set | `LocalSandbox`: a subprocess with a scrubbed environment | local mode cannot enforce egress; the two-host allow list is only applied by Daytona |
| Photon 2.0 Spectrum | `PHOTON_PROJECT_ID` (+ `PHOTON_PROJECT_SECRET` for the sidecar in `phone/`) | alarms logged and kept at `GET /photon/outbox` | the phone lives on the project's iMessage/SMS line through the `spectrum-ts` sidecar; `via: "photon"` only when Spectrum accepted the message. Voice is SIP only on Photon's side, so calls are sent as texts with `voice: unsupported` |
| Factory Droid | `droid-sdk` installed + `FACTORY_API_KEY` + `droid` on `PATH` (`DROID_BIN`) | `droid exec -o json` if only the CLI is there; then the LLM behind Chalk; then the rule-based stub | the incident report's `backend` names which one ran: `factory-droid-sdk`, `factory-droid-cli`, `droid-via-chalk:<model>`, `droid-via-stub` |

`GET /lab/status` (`lab/services.py`) probes each of these in under 2 s and reports `live`, a
one-line reason and the `mode` in use.

## Where Droid runs

The Droid runs **inside the gateway process, at runtime**, when `POST /program/{id}/repair` is
called with the author key of a rejected program (the delegate calls this on a `422`). This is
`gateway/repair.py`, and it is the only place a Droid runs in this product.

What it does: the verifier has just rejected a program with a counterexample, the exact sequence of
prints and engine events on which the program would break the owner's limit. The Droid receives
exactly three things: the MPL source text, the envelope, and that counterexample. It returns
source text. The gateway resubmits that text through the same `POST /program` flow, with the same
author key, for at most three rounds, and attaches an incident report (what failed, on which path,
what changed, which backend ran, how long it took) to the rejected program and to the owner's
event stream as `program.repair_round`.

How it is called, in order:

1. **Factory Python SDK**, `droid-sdk` 0.5.0 (`factory-droid-sdk`): `droid_sdk.run(prompt, cwd=<empty
   temp dir>, config=SessionConfig(auto_reject_permission_requests=True), runtime=Runtime(executable=DROID_BIN),
   api_key=FACTORY_API_KEY, timeout=DROID_TIMEOUT_S)`. The SDK spawns the local `droid` executable and
   talks JSON-RPC to it, so the binary has to be installed; `DROID_MODEL` and `DROID_REASONING_EFFORT`
   are passed through when set. The result is a `RunSuccess` whose `text` holds the ```mpl block.
2. **Factory CLI** (`factory-droid-cli`): `droid exec -o json "<prompt>"`; the JSON's `result` field
   is the answer and `is_error` is honoured.
3. The LLM behind the Chalk Model Gateway (`droid-via-chalk:<model>`), with the same prompt.
4. The rule-based stub (`droid-via-stub`), offline.

What it can touch: nothing but the text. Its working directory is an empty temporary directory, it
runs with permission requests auto-rejected (read-only), and the gateway parses exactly one fenced
block out of its answer. It cannot change the envelope, the key, the budget, the venue, or anything
server-side, because nothing else is passed in and nothing else is accepted back. A repaired
program that certifies is a new program with its own key and its own reservation; the original
stays `rejected`. `tests/test_repair_backends.py` pins the backend order and that boundary.

## Photon

The owner's phone is a Photon 2.0 Spectrum iMessage/SMS line, reached through the Node sidecar in
`phone/` (`spectrum-ts` 12.11.0). The gateway's `photon.py` POSTs every alarm and reply to the
sidecar's `POST /send` when `PHOTON_PROJECT_ID` is set; the sidecar resolves a space from the
owner's number and calls `space.send(text)`. Inbound texts stream to the sidecar
(`for await (const [space, message] of app.messages)`) and are forwarded to `POST /photon/text`;
transcripts reach `POST /photon/voice` through the sidecar's `POST /voice`. `freeze` and `status`
are the only words accepted by text; the envelope by voice is read back in full and applied only on
a spoken "confirm". Without the project id the gateway keeps the same messages in `GET /photon/outbox`
with `via: "log"`. See `phone/README.md` for the run command, the channel and what is unverified.

## Refusals

Every row of `BOUNDED_KEYS.md` §10.4 is a test in `tests/test_refusals.py`:

| Attempt | Response |
|---|---|
| `POST /order` with any key | `404` |
| deploy with an author or owner key | `403 key_class` |
| program M's key on program N | `403 key_bound_to_program` |
| deploy twice, or after cancel or halt | `403 key_spent` |
| `PUT /envelope` with a non-owner key | `403 key_class` (another owner's key: `key_bound_to_account`) |
| certified but larger than the remaining budget | `409 budget {remaining_cents, requested_cents}` |
| second live program on the same instrument | `409 instrument_busy` |
| `exit … at` without `allow` | `400` with the line |
| submission on a frozen account | `403 account_frozen` |

A `spent` key can still call `GET /program/{id}/state`, and nothing else.

## Events

`GET /account/{a}/events` (owner key) is server-sent events, replayed from `?since=<id>` or
`Last-Event-ID`, then live; `?once=1` returns after the replay for polling clients. Venue events
(`program.deployed`, `node.triggered`, `node.filled`, `node.cancelled`, `program.halted`) are
stored first, then fanned out. Derived events: `budget.low` (remaining < 25 %), `agent.offline`
(a live program and no author-key request for 120 s), `rejections.burst` (3 rejections from one
author key within 60 s; this also deletes the delegate's sandbox when `agent.sandbox.delete_on_burst`
is attached). `print` events are not persisted.

On start the bus calls `venued reconcile` first and applies any halt missed while down through the
same ledger path; `test_restart_mid_program_loses_no_events` kills the bus, drives a crash path,
restarts over the same database and checks the ledger, the bank and the key.

## Decisions worth knowing

- A certified program holds its reservation and its instrument from the moment its key is issued,
  not from deploy. A key holder who never deploys can `cancel` from `issued`, which releases both
  with a realized loss of 0.
- `realized_loss_cents` from `venued` already includes fees (`CONTRACT.md` §6.3); the ledger records
  loss and fees separately and the bank settles the escrow back minus the realized loss, with the
  loss going to a venue clearing account so money is conserved.
- The fee bound on the worked example is 136 cents, not the 135 in `BOUNDED_KEYS.md` §11: the
  verifier found the key holder cancelling after a 25-tick gap up (flatten at 3025 as a taker,
  106 cents). The verifier is authoritative; the fake model was changed to agree.
- Fixtures: `../fixtures/` (Build A's, produced by its binaries) wins when present; `fixtures/` here
  is the hand-written copy regenerated by `python gateway/fixtures/make.py`.
- Routes beyond §10: `POST /program/{id}/repair` (B9 says the gateway resubmits), the phone's
  inbound webhook under `/photon/*`, and `GET /healthz`.
- `CONTRACT.md` §9 amendments (all accepted, see §9.1 there): pause keeps every reduce-only node,
  stops and takes alike; cancel flattens natively on the venue, so the ledger settles when the
  flatten fill and `program.halted` arrive (`status: cancelling` until then); a program with no
  take-profit has unbounded fees and is rejected with `max_fees_cents`, which the delegate's
  reference and the Droid's prompt both say.

## Running the suite on the real venue

`GATEWAY_TEST_VENUE=real .venv-gateway/bin/python -m pytest gateway/tests` starts a fresh exchange
and `venued` per test (scratch ports 7400/7410) and runs the same tests; `venue.drive()` becomes
`replay --attach`. The fake venue's own persistence test is skipped in that mode. 28 passed on
2026-10-16.

## Wire-up (`CONTRACT.md` §8)

`python -m gateway.wireup --gateway URL [--exchange tcp://…]` runs the nine steps against a live
gateway and prints PASS/FAIL per step; on the real venue it drives step 5 itself with
`replay --attach`. `./gateway/demo.sh real` starts the exchange and `venued` and runs it. Steps 3
to 7 and 9 (tests 6 and 7) passed on Build A's binaries on 2026-10-16; step 8 is the restart test
in `tests/`; soundness tests 1 and 2 run in Build A's suite.
