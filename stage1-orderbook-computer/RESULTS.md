# Stage 1: is an order book a computer?

Tested claim: Weinberger, *Pragmatic Information, Computation, and the Efficient Market Hypothesis*,
arXiv 2608.28803, §6.3: the Lemma, Table 2 and Appendix C (local copy: `weinberger-2608.28803.pdf`).

Two rounds: the first (E1–E9, `src/experiments.cpp`) tests the paper's construction as written.
The second (S1–S5, `src/stress.cpp`, Oct 8) asks how little the construction needs and how far it
goes. Full output: `results.txt`, `stress.txt`.

## Verdict

**Yes for an order book plus a conditional router. No for the order book alone.** After the stress
round, sharper: **one security's book is enough, the conditional can live in the exchange's stop
logic instead of the router, the router needs only "on fill at price X, submit Y", and with bounded
order counts and sizes the reachability question is PSPACE-complete in the number of securities.**

1. **The Lemma holds.** A limit order book plus a fixed, finite router that branches on the price an
   order filled at simulates any Minsky counter machine step for step. That makes it Turing complete
   when the *number of resting orders* is unbounded; each INC adds a separate 1-lot order.
   - Three compiler variants each matched a reference interpreter on every instruction of 100,000
     random programs, with 0 mismatches.
   - A filtered set of 20,000 non-trivial programs also gave 0 mismatches.
   - The engine also ran the paper's own 2N program, arithmetic programs, and Minsky's two-counter
     reduction, which needs only two books.
   - It ran the 4-state busy beaver Turing machine (107 steps, 13 ones), and the tape matched a
     direct simulation.
   - This holds using **only "on fill at price X" routing**, which is exactly the Lemma's hypothesis.
2. **The order book alone is not a computer.**
   - With no router, any finite set of orders halts within Q/2 trades (Q = total quantity submitted),
     whatever stops and cascades it contains. 20,000 random cases gave 0 violations.
   - The book supplies unbounded memory (resting quantity) and the zero test (which price the buy
     fills at). The router supplies control flow and loops.
   - The paper says this itself in footnote 19 and Note 3: exchange matching engines lack the
     conditional, and brokers' routers have it.
3. **Table 2 is ambiguous in two places, and Appendix C contradicts Table 2** (details below). None of
   this breaks the Lemma.
4. **The construction does not survive real-venue conditions.**
   - With one account, self-trade prevention stops it on the first DECJZ.
   - With two accounts it runs, but every trade is a wash trade between accounts with one owner.
   - A single trade by another participant in a register's book changes the result in 82% of runs.
5. **Theorem 6.3.1 (PSPACE-complete when bounded) is asserted, not proved, and stated in the wrong
   parameter.** The proof's last step is "such questions are, by definition, PSPACE-complete". The
   stress round supplies the missing reduction (S4) and the correct statement (below).
6. **Stress round (S1–S5).** All of the following ran lock-step against the reference interpreter
   with 0 mismatches on the same 100,000-program corpus (plus the filtered corpus) used in E3:
   - **S1** Only "on fill at price X → submit" links, no cancel, no accept, no iceberg: works, with
     resting orders = Σ registers + one backstop lot per register, constant in time.
   - **S2** No price-keyed link at all: after the probe, two stop-limit orders whose triggers
     straddle the two possible prints are submitted; the venue fires exactly one, and that order's
     own fill link continues. The branch is computed by the matching engine. BB(4) ran this way.
   - **S3** One book: asks at P are register 0, bids at P−10 are register 1. Minsky's two-counter
     reduction runs on a single security with five price levels.
   - **S4** A space-bounded Turing machine on n books each holding ≤ 2 one-lot orders. BB(4) on
     16 books; **BB(5) (47,176,870 steps) on 12,291 books, 94M instructions, 94M trades, tape
     identical, no book ever above 2 lots.**
   - **S5** Cost: 1 to 1.6 trades and 2 to 5 round trips per instruction depending on variant.
   - **Rule 110** (Turing complete, Cook 2004) runs across 96 books with the S1 primitives: 24
     generations, 6,684 trades, every row identical to a direct simulation (`src/rule110.cpp`).

## Stress round: what the construction actually needs

| ID | Question | Result |
|---|---|---|
| S1 | Do the Lemma's hypotheses suffice *as written*? Only fill-price-keyed submit links; no cancel, no accept link, no iceberg | Yes. One 1-lot backstop per register is placed at start and re-submitted by the zero branch's own link. 244 gadget cases exact; 100,000 random + 20,000 filtered programs: 0 mismatches, 0 garbage violations. A loop whose register stays in {0,1} has 1 resting order after 10⁶ instructions |
| S2 | Can the exchange do the branching? Router has no price test | Yes. DECJZ = probe IOC (prints at P or P+2), then a sell stop-limit (trigger P, limit P−1) and a buy stop-limit (trigger P+2, limit P+2). Exactly one fires; its identity-keyed fill link cancels the other and continues. 0 mismatches on the full corpus; mul, divmod and BB(4) exact. Requires a stop whose trigger is already crossed to fire on submission |
| S3 | Is one security enough? | Yes. Register 0 = ask quantity at 1000, register 1 = bid quantity at 990, backstops at 1001/989, clock trade at 995. Both S1 and S2 gadgets exact on 976 cases; 50,000 random 2-register programs each: 0 mismatches. 2N, add, absdiff, mul ran via Minsky's 2-counter reduction on one book |
| S4 | What happens with bounded books? | A TM with n tape cells compiles to n one-bit registers (head position in the control state; a destructive DECJZ reads a cell, an INC rewrites it). 3,000 random TMs: compiler vs direct tape 0 mismatches, book vs reference 0 mismatches, no book above 2 lots. BB(4): 16 books, 182 instructions. BB(5): 12,291 books, 159,785-instruction router, 94,333,336 instructions, 188,683,060 work items, 4,098 ones, tape identical, peak 2 lots per book, 20 s |
| S5 | What does computation cost? | Trades per instruction: 0.4–0.6 (paper, accept link), 1.0 (S1), 1.4–1.6 (S2). Router round trips per instruction: 1.4–1.6, 1.9–2.2, 4.3–4.9. BB(4) via three counters is ~1.2M round trips, about 3 hours at 10 ms each; via the S4 bit layout, 392 |

All S1–S4 runs use two accounts and self-trade prevention in expire-maker mode (E8). Everything
passes under AddressSanitizer and UndefinedBehaviorSanitizer (`make sanitize`). The engine tests
grew to 80,626 checks (lot tracking added for S4).

### The corrected theorem

The paper's Theorem 6.3.1 says the questions are "PSPACE-complete in m⁺", with the proof ending
"such questions are, by definition, PSPACE-complete". Two things are wrong: being simulable by a
linear bounded automaton gives membership, not hardness; and with a *fixed* number of securities and
bounded order counts and sizes the state space is polynomial, so reachability is in P (RESULTS P4).
The parameter that matters is the number of securities.

**Theorem.** Fix a matching engine with price-time priority, limit and IOC orders, and a router
with fill-price-keyed submit links (the Lemma's hypotheses). Given n securities, a router table of
size polynomial in n, and the constraint that no book ever holds more than two one-lot orders,
deciding whether a given order is ever executed is PSPACE-complete in n.

*Membership.* The state (n books with ≤ 2 lots each, the finite router table, the work queue, which
is bounded by the table size) has polynomially many bits, and the system is deterministic, so
reachability is decided by running it for at most 2^{poly(n)} steps in polynomial space.

*Hardness.* Deciding whether a deterministic Turing machine M accepts input w within n tape cells
(n in unary) is PSPACE-complete. `tm_to_bits` maps (M, w, n) to a router program with O(|Q|·n)
instructions over n registers that stay in {0, 1}: cell i is the resting quantity at P in book i;
the control state (q, i) is a block of instructions; a DECJZ on register i reads the cell
destructively, and an INC rewrites a 1. S1's layout puts exactly one backstop lot per book, so
each book holds at most 2 lots, and the whole construction uses only the Lemma's primitives. M
accepts within n cells iff the HALT template of the accepting block is reached, i.e. iff a
particular order is executed. The reduction is polynomial, and S4 executes it: BB(4) and BB(5) ran
exactly this way, and 3,000 random machines agreed with a direct simulation. ∎

The same argument with the S2 gadgets gives the theorem for a router with *no* price test, provided
the venue offers stop-limit orders. For a fixed number of securities the problem is in P, so the
hardness genuinely comes from many small books, not from one deep one.

### A taxonomy of order types by computational power

| Venue offers | Router does | Power | Evidence |
|---|---|---|---|
| limit + IOC | on fill at price → submit | Turing complete; one security suffices | S1, S3 |
| limit + IOC + stop-limit | on fill → submit / cancel, no price test | Turing complete | S2 |
| limit, market, IOC, stops | nothing | bounded: halts within Q/2 trades | E7 |
| any of the above, ≤ 2 lots per book, n securities | as above | PSPACE-complete in n | S4 |

### Caveats specific to the stress round

- **Already-crossed stops.** S2 submits a stop whose trigger is already crossed and needs it to fire
  at once. The engine does this; Hyperliquid's documentation does not say what its API does, and a
  `badTriggerPxRejected` status plus a UI error for TP/SL that would execute immediately suggest
  rejection. The alternative (place stops before the probe) fails because the previous print leaves
  the book's last price on one side of every usable trigger. This must be checked on a testnet
  before S2 is claimed for a named venue.
- **S2 uses cancel.** The losing stop must be removed, so S2 needs on-fill → cancel as well as
  submit. S1 needs neither cancel nor stops. They are different minimal sets, not one.
- **Single book + S2** works only because the fired orders are stop-*limit* orders whose limit price
  excludes the other register's level; stop-market orders would hit the best level and destroy the
  other counter.
- **Wall-clock.** S5's round-trip conversion assumes one router action per venue round trip and no
  batching; it is an order of magnitude, not a measurement.

## Issues in the paper

| # | Where | Issue | Evidence |
|---|---|---|---|
| P1 | Table 2, INC | **Ambiguous.** The text says "Place a resting SELL … at P_i, then place order k when order is executed". Read as "when *that sell* fills", every INC deadlocks: only a later DECJZ ever buys it, and the router never reaches one. Read as "once placed", it works unmodified. The strict "fill" reading also works if INC adds a self-cross of one lot in a separate clock book. | E1 (a) stalls for every N ≥ 1; E1 (b) and (c) are exact; E3 shows 0 mismatches for both |
| P2 | Table 2, DECJZ | **Leaks, as tabulated.** A fresh backstop sell at P_i+θ is placed on every DECJZ, and nothing removes it on the non-zero branch. Book size then grows with running time instead of register size. That breaks Theorem 6.3.1's premise that order counts are bounded. Note 1 calls the backstop an "iceberg" order: one persistent large backstop would not leak. Cancelling it on the non-zero branch also fixes it. | E6: a loop whose register stays in {0,1} has 500,000 resting orders after 10⁶ instructions, and 0 with the fix |
| P3 | Appendix C vs Tables 2/3 | **Contradiction.** Appendix C says DECJZ "decrements … and [goes to] I_k unless the register *subsequently* holds zero". The gadget and Table 3 use the standard rule: test, then decrement. They disagree at register = 1. Under Appendix C's wording, Table 3 computes 2N−2, contradicting its own note that the loop runs "exactly N times". | E2: the gadget matches the standard rule on 20,000/20,000 random programs and Appendix C on 13,087/20,000 |
| P4 | Thm 6.3.1 proof | **Not a proof, and the wrong parameter.** Being simulable by a linear bounded automaton gives membership in PSPACE, not hardness. With a fixed number of books and bounded sizes the state space is polynomial and reachability is in P. The correct statement is PSPACE-complete *in the number of securities* with per-book size bounded by a constant; the reduction uses one bit per book, not one deep register (see "The corrected theorem"). | S4 supplies the reduction and runs it |

The paper's claim that Turing completeness survives "arbitrary orders for other securities" holds,
but by construction rather than as a finding: books share no state and foreign orders fire no links.
E9a is only a sanity check.

## Experiments (`src/experiments.cpp`; full output in `results.txt`)

| ID | Question | Result |
|---|---|---|
| E1 | Does Table 2's INC run? | (a) wait for the INC sell to fill: deadlock for every N ≥ 1. (b) continue on accept: exact. (c) fill-only links with a clock self-cross: exact |
| E2 | Which DECJZ semantics does the gadget implement? | Standard Minsky. Appendix C disagrees at register = 1, and Table 3 gives 2N−2 under it |
| E3 | Does the book run programs step for step? | 3 variants × 100,000 random programs × up to 2,000 steps: **0 mismatches**. Registers are read from the resting quantity in the books. Honest coverage: 52% of those random programs visit ≤ 2 instructions. A **filtered corpus** of 20,000 programs (≥ 4 distinct instructions, ≥ 20 non-zero and ≥ 3 zero DECJZ, kept from 292,961 generated) ran 39.8M instructions per variant, including 10.6M non-zero DECJZ: 0 mismatches in all variants |
| E4 | Is each gadget correct in isolation? | 15,128 cases (register 0..60 as one order or as one-lot orders, leftover backstop lots 0..60, with and without cleanup): 0 wrong. Each case checks the branch, the register, leftover backstop lots, trades, and that other books are untouched |
| E5 | Real programs | 2N, add, \|a−b\|, 17×23 and 100 div/mod 7 were all exact. Minsky's 2-counter reduction ran on **two books**. BB(2) and **BB(4)** ran via TM → 3 counters → orders (578,702 instructions, 285,585 trades), with the tape identical to a direct simulation |
| E6 | Backstop leak | The book grows linearly with time under the gadget as written, and stays constant with cleanup |
| E7 | Book without a router | 20,000 random order sets (all order types, stop cascades, STP on and off): all halt, 0 violations of trades ≤ Q/2 |
| E8 | Self-trade prevention | With one account, both STP modes stall on the first DECJZ, and expire-maker (Hyperliquid's documented mode) cancels the whole register. Two accounts work in all modes |
| E9 | Other participants | (a) other securities: no effect, by construction. (b) one 1-lot market buy in a register's book: 12,337 of 14,976 runs (82%) computed something else |

The engine tests (`src/tests.cpp`, 626 checks) cover:
- price-time priority, partial fills and IOC
- stop cascades, stop-limit orders resting after trigger, and buy stops firing on intermediate prints
- reduce-only, and one-cancels-other in cancel mode vs reduce mode (the spec's bracket bug)
- the STP modes
- determinism replay (500 seeds × 300 events) and conservation of positions and cash
- regressions for two issues the reviewer found

Everything passes under AddressSanitizer and UndefinedBehaviorSanitizer (`make sanitize`).

**Independent review.** A separate adversarial review agent ran a white-box fuzzer: 3,000 seeds with
invariant checks after every step, under ASan/UBSan. It found no engine bug affecting any result. It
re-verified the 2-counter reduction on 3,000 random programs and the TM compiler on 2,000 random
Turing machines. Its two small engine findings are fixed:
- The state hash ignored queued work.
- An accept link fired for a reduce-only order that was rejected.

It also found the clock construction behind E1 (c). The reviewer's corrections are reflected above.

## Why 0 mismatches is evidence, not just absence of a counterexample

The argument is an induction over steps.

- **Base case (E4).** Each gadget, started from a book whose register level holds v lots and whose
  backstop level holds g lots, takes the right branch. It leaves the register at max(v−1, 0) or v+1
  and touches nothing else.
- **The cases are complete.** The IOC buy has limit P_i+θ and the router places no resting bids, so
  the buy can only touch the best ask:
  - if v > 0, the front lot at P_i;
  - otherwise, the oldest lot at P_i+θ.

  So the outcome depends only on whether v > 0, and every v ≥ 1 behaves as v = 1..60 does.
- **Composition (E3, E5).** The router is a finite table keyed by template and fill price, so after
  each gadget the next one starts from a book of the same shape. The random lock-step runs exercise
  this composition, including how link actions are ordered.

With unbounded order counts this simulates every Minsky machine. Two counters suffice (Minsky 1967),
and E5 runs that reduction on the book. Hence the system is Turing complete.

## What this means for the Minsky build (spec of Oct 7)

1. **INC needs a continuation.** The spec's on-fill-only format can express it with the clock
   self-cross (E1 c), at the cost of one extra book and two orders per INC. The alternative is an
   on-accept link, which is simpler but adds a primitive the paper doesn't assume. Pick one and say
   which on stage.
2. **Cancel the backstop on the DECJZ non-zero branch**, or use one persistent iceberg backstop.
   Otherwise every loop iteration leaves a new order behind. The checker's canonical states then
   never repeat, so deduplication fails (P2).
3. **Use standard Minsky semantics** in the reference interpreter, and don't quote Appendix C (P3).
4. **Model STP, and say so.** Under Hyperliquid's rules the router's sells and buys need two
   addresses (E8). Say plainly that this is a demonstration of computation, not something to deploy:
   every trade is a wash trade.
5. **On stage, say "order book plus router"**, not "the order book is a computer". The book is the
   memory and the comparator, and the router is the control unit. Without links the book always
   halts (E7). That is also why the spec's stop-only goal can only give bounded programs.
6. **A bounded checker is the only possible kind, and now the hardness is precise.** Unbounded
   sizes: undecidable, even on one security (S3). Bounded sizes: PSPACE-complete in the number of
   instruments (S4). The compiler is the lower bound and the checker is the upper bound; that is the
   link between the two halves of the spec.
7. **The research goal has an answer.** Stops alone give bounded programs (E7). Stops plus a router
   that only chains unconditionally on fills give Turing completeness (S2). The conditional moves
   into the exchange. State the fire-on-submit assumption.
8. **Demo on one book.** S3's single-security layout is the most striking 30-second demo: five
   price levels of one instrument running 2N.

See `../DIRECTIONS.md` for what this means for the hackathon plan.

Not tested: the stop-only construction (the spec's research goal), PSPACE-hardness, and real-venue
rules beyond STP. Order types that reprice without trading (pegged, trailing) are not modelled, so
the E7 halting result covers this engine's five order types only.

## Reproduce

```
make run        # engine tests, then E1-E9 -> results.txt (~2 min)
make stress     # S1-S5 including BB(5) -> stress.txt (~3.5 min; --quick or --no-bb5 to shorten)
make rule110    # Rule 110 on 96 books, 24 generations, checked against a direct simulation
make sanitize   # ASan+UBSan: tests + reduced E and S suites
```
