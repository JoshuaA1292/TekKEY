# Build A: the venue (bounded keys)

The question this build answers: **given a program, what is the worst it can do on our exchange,
and can we run it there with nobody else involved?** Spec: `../BUILD_A_VENUE.md`,
`../BOUNDED_KEYS (1).md`, `../CONTRACT.md` (see §9 there for the amendments this build proposes).

```
make                    # everything into build/ (clang++, C++20, no dependencies beyond POSIX)
make test-bk            # BOUNDED_KEYS §8.5 tests 1-5 and 7, determinism, performance, fixtures
make fixtures           # regenerate every fixture Build A owns (CONTRACT §7) from the binaries
```

## The pipeline, on the worked example

```
$ build/policyc --instruments ../fixtures/instruments.json --env ../fixtures/envelope_40.json < ../fixtures/ex_v1.mpl
  -> CompiledProgram (3 native orders, deploy plan, warning: allow 5 < max_gap_ticks 25)
$ build/verify --program ex_v1.compiled.json --envelope envelope_40.json --mark 3010      # exit 3
  -> rejected, max_loss_cents 4030 > 4000:
     print 3010 | gap 3010 -> 3000, entry fills | gap 3000 -> 2982, sl triggers, rests (limit 2983 above print)
     | gap 2982 -> 2960 | mark: long 1, loss 4030
$ build/verify --program ex_v2.compiled.json ...                                           # exit 0
  -> certified: max_loss 3734, max_fees 136, exposure 1, halts within 2 trades;
     worst path: gap 3010 -> 2989 (entry fills at 3000), gap 2989 -> 2964 (sl fills at 2964)
$ build/replay --program ... --path ex_v2.result.json --exchange tcp://127.0.0.1:7000      # live exchange
  -> loss 3734, fees 134, trades 2, matches_verifier: true
$ build/venued    # deploy ex_v2, drive the worst path: node.filled entry, node.triggered sl,
                  # node.filled sl 2964, program.halted 3734 / 134 / 2   (../fixtures/events_v2_worst.jsonl)
```

The exchange must run with `--accounts 255` (accounts 0 and 254 are the environment, 253 is
replay's scratch, 1-252 are programs).

## What is where

| File | What |
|---|---|
| `src/json.hpp`, `src/sha256.hpp` | dependency-free JSON (insertion-ordered, byte-stable output) and SHA-256 |
| `src/program_json.*` | A1: CompiledProgram load + validation, instruments, envelope, fee arithmetic (`ceil`, bps in hundredths) |
| `src/policy.*`, `src/policyc.cpp` | A2: MPL lexer, parser, static rules (§6.2), compiler (§7), deploy plan |
| `src/env.hpp` | the environment as exchange messages (the print procedure), the native flatten, the program ledger, and one `Port` interface implemented in-process (`CorePort`) and over the wire (`WirePort`) |
| `src/verifier.*`, `src/verify.cpp` | A3: the search; the transition function is `fb::Core` itself |
| `src/replayer.*`, `src/replay.cpp` | A4: a path on a live exchange, measured from the exchange's own fill journal |
| `src/venued.cpp` | A5: the venue daemon (CONTRACT §6) |
| `src/fastbook.hpp`, `src/matcher.hpp`, `src/proto.hpp` | A6: `Core(const Core&, Sink*)` snapshots; a per-account fill journal and four read-only control ops (OrderInfo, DepthThrough, FillCount, FillInfo). Matching semantics unchanged: `test_fastbook`'s 1.85M hash-identity checks against stage 1 still pass |
| `tests/test_verify.cpp`, `tests/test_venued.cpp` | A7: the soundness suite |
| `../fixtures/` | A8: every fixture produced by these binaries |

## How the proof works

**One message sequence, two engines.** A print at price P is real orders from two environment
accounts: E2 rests unlimited liquidity at P, E1 sends an IOC sized to sweep every program order the
print trades through (resting orders fill at their own price) plus one lot against E2 (the print).
Stops fired and children released by those trades are processed by the engine as aggressors against
E2's liquidity at P. The verifier runs this on a snapshot `fb::Core`; `replay` and `venued` run the
identical code over TCP. Test 3 checks the two give the same fills in 200 of 200 random runs.

**Finite search over an infinite market.** Only boundaries matter: every live trigger and limit,
the price where the loss would pass the envelope, and two fee points (below). Landing points are
each boundary and its neighbours. Moves, in fixed order: gap down, cross down, cross up, gap up (per
instrument), key-holder pause, key-holder cancel. Gaps land on every boundary-crossing point within
`max_gap_ticks` and on the farthest print. States are memoized on the canonical book hashes plus the
program's own ledger and node states (not `Core::hash()`, which also hashes the environment's
accounts and would never close a price cycle). Metrics are functions of the state, so the bound is a
maximum over reachable states. A shallow iterative-deepening probe runs first; reported
counterexamples and worst paths are shortest paths.

**What the fuzz found.** Soundness test 1 (random prints anywhere within the gap, not only at
boundaries) beat early certificates' fee bound by 1-2 cents on about 1 in 2,000 paths. Fills at
market-determined prices (a flatten after a rally, a hedge released while its own book is far
away) pay fees on notional that nothing in the model caps. The fix is the fee sentinel: a landing
point at the lowest print where one lot would break the fee budget (or the top of the price domain),
so the search walks there and finds the market-priced fill if one exists. After it: 0 of 27,000.

## Results (`make test-bk`)

| Check | Result |
|---|---|
| fixtures: policyc == compiled fixtures, verify == result fixtures, paths replay | pass, in-process and over TCP |
| 2. every counterexample / worst_path replays to the cent | 80 random programs (≈20 certified, ≈60 rejected), 2 exchanges |
| 1. fuzz never beats a certificate | 10,000 sequences in-process + 10,000 over TCP; 0 beat the bound; ~150 steps land exactly on max_loss (the bound is tight) |
| 5. gaps at the edge | 2,500 + 2,500; 0 beat the bound |
| 1, three instruments | 2,000 + 2,000 sequences across ETH/BTC/SOL; 0 beat the bound |
| 3. compiled equals drawn | 200 / 200 identical (node, price, qty, maker, fee) |
| 4. the audit fails | 253 / 253 static errors; with `allow 0` forced, 253 / 253 gap paths; with `allow = max_gap_ticks`, 253 / 253 certified |
| determinism | 100 runs identical except `explored.ms` |
| 7. single-run at the venue, kill -9 + reconcile | `test_venued` 87 / 87 |
| performance | 3 nodes 0.2-2 ms; 12 nodes 20-100 ms; 3 instruments: rejection in ~20 ms, certification with no fee cap 11 s (1.9M states; target 10 s, missed by ~1 s) |

## Limits, stated plainly

- `full_at_print` is optimistic about depth; an order that enters on the side opposite a print's
  direction fills at its own limit, never better (env.hpp header).
- An absolute fee cap binds on winning paths too: a long with no take-profit is rejected for fees
  (CONTRACT §9 A8). Delegates should always write a take-profit.
- `halt` nodes are markers; they do not cancel anything (nothing native can).
- `venued` killed between submitting a deployment's orders and persisting them leaves those orders
  orphaned on the exchange (no venue op lists orders by account).
- The Hyperliquid relay backend (stretch) is not built.
