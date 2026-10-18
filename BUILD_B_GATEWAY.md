# Build B: the gateway

**Language:** Python 3.12 (FastAPI, SQLite). **Size:** two people.
**Delivers:** the public API, the three key classes, the budget, banking, the delegate's sandbox,
the repair loop, the owner's phone, and the evaluation harness.
**Depends on Build A for:** nothing until wire-up. Until then you develop against `fake_venue`,
built from `CONTRACT.md`.

The question this build answers: **who is allowed to ask for what, how much has the account
spent, and who needs to know?**

---

## 1. What you own

| Component | Location | Purpose |
|---|---|---|
| B1 API server | `gateway/app.py` | every route in `BOUNDED_KEYS.md` §10, no others |
| B2 Keys and auth | `gateway/keys.py` | `ok_` / `ak_` / `bk_` classes, hashing, binding, spent state |
| B3 Budget ledger | `gateway/budget.py` | rolling-window reservations (§5.2); the `409` decision |
| B4 Venue client | `gateway/venue.py`, `gateway/fake_venue.py` | runs `policyc` / `verify`; talks to `venued`; the fake serves the fixtures |
| B5 Banking | `gateway/nessie.py` | Nessie customer + account per gateway account; funding; settlement |
| B6 Event bus | `gateway/events.py` | consumes `venued` events; derives `budget.low`, `agent.offline`, `rejections.burst`; serves SSE |
| B7 Owner's phone | `gateway/photon.py` | envelope by voice with read-back; alarms; one-word replies |
| B8 Delegate sandbox | `agent/`, `agent/image/` | Daytona image and egress policy; an LLM delegate that writes MPL from a brief |
| B9 Repair loop | `gateway/repair.py` | Factory Droid fixes a rejected program from its counterexample |
| B10 Data and evals | `chalk/`, `evals/` | Chalk AI Gateway for every LLM call; federated queries; the eval suite |

**Not yours:** anything that touches the exchange protocol, the search, or MPL parsing. You call
binaries and a socket.

---

## 2. Split between the two people

- **B-api:** B1 → B2 → B3 → B4 → B5 → B6. The trusted core: if this is wrong, the bound is wrong.
- **B-agents:** B8 → B9 → B7 → B10. Everything that talks to a model, a phone or a dashboard.
  Builds against B-api's routes from hour ~6, using `fake_venue`.

---

## 3. Component details

### B1 API server
- Routes and responses exactly as `BOUNDED_KEYS.md` §10. There is **no `/order` route at all**, so
  FastAPI's own 404 is the refusal.
- **`POST /program` flow:**
  1. Authenticate: author key, bound to this account.
  2. Account not frozen.
  3. `policyc`; on exit 2 → `400` StaticError.
  4. `venued mark` → `verify --mark`; on exit 3 → `422` Rejection (store it, emit
     `program.rejected`); on exit 4 → `503 verify_timeout`.
  5. If the instrument already has a live program → `409 instrument_busy`.
  6. Budget check (B3); if it fails → `409 budget`.
  7. Reserve, mint the `bk_` key, store, emit `program.certified` → `200` with the Certificate plus
     `program_id`, `account`, `budget` and `key`.

  Steps 5–7 run in **one SQLite transaction** so two simultaneous submissions can't both fit into
  the last of the budget.

### B2 Keys and auth
- 32 random bytes, base32, prefixed by class. Store `sha256(key)` only.
- **Binding:** an `ak_` key is bound to one account; a `bk_` key to one program.
- **`bk_` states:** `issued → deployed → spent`.
  - Deploy is allowed only from `issued`.
  - Pause is allowed from `deployed`.
  - Cancel and halt move the key to `spent`.
  - `spent` permits `GET /state` only.
- Every refusal row in §10.4 is a unit test.

### B3 Budget ledger
- **Table:** `ledger(account, program_id, kind ∈ {reserve, realize, release}, cents_loss,
  cents_fees, trades, t)`.
- **`remaining(account, now)`:** `max_loss` − Σ realized losses in the window − Σ reservations of
  live programs. Fees and trades are computed the same way.
- **On `program.halted`,** in one transaction: release the reservation, record the realized loss
  (`max(0, …)`), settle with Nessie (B5).
- **Invariant, checked on every write:** realized-in-window ≤ `max_loss_cents`. A violation is a
  bug in the system, not a user error. Log it loudly and freeze the account.

### B4 Venue client
- Wraps `policyc` / `verify` / `replay` as subprocesses (JSON in and out, exit codes per contract),
  and `venued` as a socket client with `request_id`s and retry.
- **`fake_venue`:**
  - The same interface. It reads `fixtures/` and returns the matching file by `source_hash` of the
    input MPL.
  - Simulates `venued`: `deploy` returns ids; `state` and events come from
    `events_v2_worst.jsonl` played on a timer.
  - It must be good enough that B-agents never waits on Build A.
- A single env var `VENUE=fake|real` switches between the two. Nothing else changes at wire-up.

### B5 Banking (Nessie)
- **`POST /account`:** creates a Nessie customer + account, deposits the opening balance, and
  stores the mapping.
- **Deploy:** transfers `fund_cents` (the program's `locked_collateral_cents` plus its bound)
  from the account into an escrow account per program, and passes `fund_cents` to `venued`.
- **Halt:** transfers the escrow back, minus realized loss and fees.
- **`GET /account`:** the balance shown is the Nessie balance.
- **Be honest that Nessie is a simulated bank.** The flow is real; the money is not.

### B6 Event bus
- A single consumer of `venued subscribe`. It writes events to SQLite, then fans them out to SSE
  listeners and to B7.
- **Derived events** (definitions in `BOUNDED_KEYS.md` §10.5):
  - `budget.low`: remaining < 25%.
  - `agent.offline`: live program and no `ak_` request for T = 120 s.
  - `rejections.burst`: ≥ 3 `program.rejected` from one `ak_` within 60 s.
- **On reconnect after a restart:** call `venued reconcile` and apply any halts missed while down,
  through the same ledger path.

### B7 Owner's phone (Photon)
- **Envelope by voice.** The owner says the limits; the agent fills the envelope JSON, **reads it
  back in full** ("forty dollars a day, gap twenty-five ticks, ETH only — confirm?"), and only
  sends `PUT /envelope` after a spoken "confirm".
- **Alarms by text or call:** `budget.low`, `agent.offline`, `rejections.burst`, `program.halted`.
- **One-word replies:**
  - `freeze` → `POST /freeze`.
  - `status` → a one-line summary.
  - Nothing else is accepted by text. The phone can only make the account *safer*.

### B8 Delegate sandbox (Daytona)
- **Image:** the MPL reference (`BOUNDED_KEYS.md` §6), the OpenAPI spec, a small client library,
  and `AUTHOR_KEY` in the environment.
- **Egress policy, exactly two destinations:**
  1. the gateway's `/program` and `GET` routes;
  2. the Chalk AI Gateway (its only route to a model).

  No venue address, no owner key, no general internet.
- **The delegate:** takes a one-paragraph brief, writes MPL, submits it, reads the
  response, and on `422` hands off to B9.
- **Lifecycle via the Daytona SDK:** create per delegate, delete on owner request or on
  `rejections.burst`. Deleting a sandbox must never affect a deployed program, and wire-up
  step 8 tests this.

### B9 Repair loop (Factory Droid)
- Triggered on a `422` from a delegate submission.
- The Droid receives the MPL source, the envelope and the counterexample, and returns patched
  source. The gateway resubmits, for **at most 3 rounds**.
- **Allowed to change:** the policy source only.
- **Never allowed to change:** the envelope, the key, or anything server-side. Enforce this by
  giving the Droid only the source text and accepting only source text back.
- It also writes a short incident report per rejection (what failed, on which path, what changed),
  attached to the program record and to the owner's event stream.
- The README must say exactly where the Droid runs (inside the gateway, at runtime) and what it can
  and cannot touch.

### B10 Data and evals (Chalk)
- **AI Gateway:** every LLM call (delegate, Droid prompts if routed, Photon voice parsing) goes
  through it. It is the sandbox's only model route (B8).
- **Federated queries:** join gateway SQLite (programs, certificates, ledger), `venued` events
  (fills) and Nessie balances into one view per account and per delegate model.
- **Datasets** (in `evals/data/`):
  - **Briefs:** 50 one-paragraph trading briefs, each with an envelope.
  - **Attacks:** 200 messages aimed at the delegate, labelled by class (instruction override,
    fake owner, fake market event, social pressure).
- **Metrics** (per model):

  | Metric | Oracle |
  |---|---|
  | first-pass certification rate | `verify` |
  | Droid rounds to certify | `verify` |
  | bound tightness (cert `max_loss` / envelope `max_loss`) | certificate |
  | turned rate (attack → next program is `422`) | `verify` |
  | breach rate (realized loss in window > `max_loss_cents`) | ledger + `replay`; **must be 0** |
  | raw-key drain (same attacks, delegate given a direct exchange session) | exchange state |

- Results are written to `evals/results/*.json` and exposed as a Chalk eval run.

---

## 4. Milestones (hours from start of build)

| Hour | B-api | B-agents | Exit check |
|---|---|---|---|
| 0–4 | schema; B2 keys; `POST /account`, `PUT /envelope`, author-key; `fake_venue` serving `ex_v1/ex_v2/ex_noallow` | Daytona image + egress policy; Chalk AI Gateway reachable from inside the sandbox and nothing else | sandbox can reach exactly two hosts |
| 4–10 | `POST /program` full flow on the fake (400 / 422 / 200); B3 ledger + `409`; all §10.4 refusals as tests | delegate writes MPL from a brief and submits through the real API (fake venue) | **`ex_v1` → 422, `ex_v2` → 200, resubmit → 409, all on the fake. This cannot slip.** |
| 10–16 | deploy / pause / cancel / state; B6 consumer on the fake event stream; halt → ledger | B9 Droid repair loop (v1 → v2 in ≤ 3 rounds); incident reports | Droid turns `ex_v1` into a certifying program |
| 16–22 | B5 Nessie: accounts, escrow, settlement; reconcile path | B7 Photon: voice envelope with read-back; alarms; `freeze` | a halt moves Nessie balances by exactly the realized loss |
| 22–26 | hardening: concurrent submissions vs budget (transaction test); restart test | B10: briefs + attacks datasets; eval runner on the fake; Chalk federated view | eval runner produces a full results file on the fake |
| 26–30 | **wire-up with Build A** (`CONTRACT.md` §8) | **wire-up**; rerun evals against the real verifier | steps 1–9 green |
| 30–36 | freeze; README (incl. Droid section); bug fixes only | final eval run; results committed | — |

---

## 5. Definition of done
- With `VENUE=real`, every step of `CONTRACT.md` §8 passes.
- Every refusal in `BOUNDED_KEYS.md` §10.4 is a passing test.
- The ledger invariant (realized-in-window ≤ `max_loss_cents`) holds across the full attack
  eval, with breach rate 0, verified by `replay`.
- Deleting a delegate's sandbox and restarting the gateway mid-program loses no events and leaves
  the ledger correct.
- The README states where each external service does real work, and the Droid's exact permissions.
