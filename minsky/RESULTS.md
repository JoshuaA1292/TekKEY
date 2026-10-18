# Stage 2 results: the exchange computer over a wire

Measured Oct 8, 2026 on one Apple M4 laptop (clang 17, C++20), loopback, nothing pinned. Every
number below comes from a tool in this directory and can be regenerated with the command shown.
Stage 1's in-process results are in `../stage1-orderbook-computer/RESULTS.md`.

## What was built

A real exchange server (`exchange`), a real stateless router (`router`), three transports between
them (TCP, UDP, shared memory), a compiler from a bit-level ISA to orders, Snake written in that
ISA with a reference simulator, a visualiser that draws only from the exchange's market-data feed,
a latency benchmark, and a canary that turns deliberate matching-engine bugs into wrong answers.
`README.md` lists the pieces; `demo.sh` runs the whole thing so a person can play.

## Verdict

1. **The construction survives the wire.** Exchange process plus router process equal the stage 1
   in-process engine instruction for instruction on random programs compiled four ways (minimal
   S1 gadgets, stop-branch S2 gadgets, the paper's accept links, the paper's gadgets with cancel):
   same halt status, instruction count, registers read back from the books, trade count and
   resting orders. 0 mismatches over TCP, UDP and shared memory, 1.8M instructions per transport.
2. **One instruction is one round trip.** With the S1 gadgets the router waits exactly 1.00 times
   per instruction (an INC's three orders are pipelined; the backstop resubmit rides with the next
   instruction). S2 costs 1.40 because the losing stop must be cancelled.
3. **The clock of an exchange computer is its round trip.** Snake's frame rate on each transport is
   instructions per frame times round trip, and the latency knob moves it by exactly the amount
   predicted (table below).
4. **Snake runs, and it is the exchange that holds the game.** 823 books; the body is resting
   one-lot sells; the score and pending growth are unbounded counters; input is one-lot orders
   from a third account into four keyboard books; the picture is drawn from the feed. The compiled
   program matches the simulator on every frame in six configurations in the interpreter (up to
   20,000 frames each), on the in-process engine, and over each transport.
5. **Computation is a conformance test only for the rules the program exercises.** Snake exposes
   inverted price priority before its first frame is over. It cannot expose the FIFO-after-partial
   bug or an IOC that rests, and neither can any S1 program: every order they place is one lot, so
   no fill is ever partial and no IOC ever has a remainder. The probe program exposes all four
   within 16 instructions. The pitch's original claim that Snake would catch the FIFO bug was
   wrong and has been corrected.

## The law

`bench --iters 20000` against `exchange --tcp 7200 --udp 7201 --shm /tmp/minsky-bench.shm`
(5,000 warm-up samples; each sample is one send + reply; `decjz` is a 1-lot IOC that fills;
`inc` is three pipelined orders and one wait):

| path | shape | p50 µs | p90 | p99 | p99.9 | max | instructions/s |
|---|---|---:|---:|---:|---:|---:|---:|
| shm | ping | 0.75 | 0.79 | 0.88 | 1.6 | 56 | 1,333,333 |
| shm | decjz | 0.88 | 0.96 | 1.04 | 1.6 | 46 | 1,142,857 |
| shm | inc | 1.00 | 1.08 | 1.25 | 7.1 | 34 | 1,000,000 |
| tcp | ping | 5.92 | 9.79 | 12.3 | 26.1 | 257 | 169,005 |
| tcp | decjz | 6.04 | 9.71 | 12.6 | 25.5 | 46 | 165,536 |
| tcp | inc | 6.17 | 9.58 | 12.7 | 30.0 | 52 | 162,153 |
| udp | ping | 17.29 | 18.54 | 21.3 | 37.0 | 89 | 57,834 |
| udp | decjz | 17.42 | 19.38 | 22.3 | 36.7 | 101 | 57,415 |
| udp | inc | 17.50 | 19.50 | 25.3 | 50.4 | 82 | 57,143 |
| in-process `fb::Core` | decjz | 0.014 | | | | | ~70,000,000 |
| in-process `ob::Engine` (stage 1) | decjz | 0.012 | | | | | ~85,000,000 |

(Taken after the review fixes below; the pre-fix transport measured 1.08 / 6.42 / 17.42 µs for
decjz, so the bounded non-blocking flush cost nothing.)

Matching costs 13 ns; the wire costs 0.9 to 17 µs. `decjz` equals `ping` on every transport and
`inc` (three orders) costs one round trip, so the router's pipelining argument holds. The `max`
column is scheduler preemption on an unpinned laptop; p99.9 is the honest tail. UDP being three
times slower than TCP on loopback is a Darwin property, not the transport's.

The router running Snake (`router --table programs/snake.tbl --max-instr 300000`, no player, so
the snake runs into the wall and resets every 16 frames, ~186 instructions per frame including
the resets):

| transport | instructions/s | µs per wait | Snake fps (unpaced) |
|---|---:|---:|---:|
| shm | 1,447,621 | 0.5 | 7,798 |
| tcp | 139,990 | 5.8 | 754 |
| udp | 53,523 | 16.2 | 288 |

With a player, a frame is 92 to 105 instructions (min 42, max ~1,040 on the frame a death resets
the board). `--pad 450` makes it ~977 per frame, the figure the pitch originally assumed: 146 fps
on TCP, 1,480 on shm.

### The latency knob

`router --delay-us N` sets a delay line on the exchange's replies (the matching core itself is
never delayed, and pipelined orders overlap in flight as they do on a real wire). Snake over TCP,
186.6 instructions per frame:

| knob µs | measured µs per wait | measured fps | predicted fps = 1 / (186.6 × wait) |
|---:|---:|---:|---:|
| 0 | 5.9 | 743 | 908 |
| 50 | 59.8 | 86.5 | 89.6 |
| 100 | 110.6 | 47.5 | 48.5 |
| 300 | 330.6 | 16.0 | 16.2 |
| 1000 | 1037.8 | 5.1 | 5.2 |

The knob adds what it says plus the server's polling granularity (~4 µs, from the main loop
visiting sessions in turn) and about 3% at the longest setting. Measured and predicted agree
within 4% from 50 µs up; at 0 the router's own work (18% of the time) is not in the formula.

A first version of the knob busy-waited before every order message. Pipelined orders then paid it
twice (an instruction sends about two orders) and the wait at 50 µs was 110 µs. That was a
modelling error, fixed: a venue's latency is a delay line, not a stall.

### Real venues on the same axis

Reported round trips (trade press and patents, not our measurements) and what Snake would run
at, with 100 instructions per frame (the real program) and 1,000 (the padded one):

| venue | reported round trip | Snake fps at 100 instr/frame | at 1,000 |
|---|---|---|---|
| this laptop, shared memory | 0.9 µs | ~11,000 | ~1,100 |
| this laptop, TCP loopback | 6.0 µs | ~1,600 | ~160 |
| Nasdaq-class equities, door to door | 14 to 40 µs | 250 to 700 | 25 to 70 |
| CME Globex | 100 µs to 1 ms | 10 to 100 | 1 to 10 |
| Hyperliquid (block time) | 100 to 200 ms | one frame every 10 to 20 s | one every 2 to 3 min |

## Snake

`compile snake`: 8,606 Minsky instructions, 16,444 templates, 13,704 links, 823 books (256
pixels, 512 direction-cell bits, 4-bit head / tail / fruit x and y, 2-bit direction and
last-move, a 16-bit LFSR, score and growth counters, a pad counter, four keyboard books, a frame
book, a death book, plus the INC clock book). On disk the table is 1.0 MB.

`test_snake` (reference simulator vs the program, compared at every frame on every register):

| configuration | frames | deaths | instructions per frame (min / max) | mismatches |
|---|---:|---:|---|---:|
| 16×16 walls | 20,000 | 294 | 98 (44 / 1,037) | 0 |
| 16×16 wrap | 20,000 | 158 | 92 (42 / 1,035) | 0 |
| 16×16 growth 1 | 10,000 | 97 | 96 (59 / 998) | 0 |
| 10×8 walls | 10,000 | 269 | 132 (40 / 3,108) | 0 |
| 16×16 pad 50 | 2,000 | 30 | 197 (147 / 979) | 0 |
| 16×16 growth 8 | 20,000 | 333 | 93 (42 / 1,037) | 0 |
| in-process engine, state read from the books | 600 | 5 | | 0 |
| over TCP, keys as external orders | 300 | 3 | | 0 |
| over UDP | 300 | 3 | | 0 |
| over shm (keys over TCP: shm serves one client) | 30 | 1 | | 0 |

The deaths are the scripted player's (a pathfinder with random keys mixed in, so resets and
double presses are exercised). A frame costs about 100 instructions because the three decoder
trees (write the direction into the cell the head leaves, test and draw the new head, clear and
follow the tail) each cost 8 reads plus a few instructions at the leaf, and the ripple
increment of a 4-bit coordinate costs 1 to 5. A death costs ~800 extra: 768 one-instruction
clears of the pixel and direction books.

Rule 110 on 96 cells, looping: 2,660 instructions, 539 per generation, 285 generations per
second over TCP; 40 generations checked against a direct simulation on 8, 48 and 96 cells, 0
wrong rows.

## The canary

`canary --programs 1500 --snake-frames 500`: each mutation switched on in the matching core; the
same router table run against the correct core and the mutated one; the first instruction at
which the two runs part ways. "never" means identical traces and identical level quantities.

Coverage on the correct engine (which rules each program's orders actually exercise):

| program | partial fills | IOC remainders | stops |
|---|---:|---:|---:|
| probe (16 instructions) | 1 | 1 | 1 |
| Snake (500 frames, 48k instructions) | 0 | 0 | 0 |
| S1 corpus (318 programs) | 1,904 (multi-lot register loads) | 0 | 0 |
| S2 corpus | many | 0 | many |
| paper + cancel corpus | 1,904 | 0 | 0 |

| mutation | probe | Snake | S1 corpus | S2 corpus | paper + cancel |
|---|---|---|---|---|---|
| fifo-after-partial (orderbook-rs #88) | 15 | never | never | never | never |
| price-inverted | 16 | frame 0, instruction 898 | 318 of 318, median 2 instructions | 318 of 318, median 2 | 318 of 318, median 2 |
| stop-wrong-side | 16 | never | never | 318 of 318, median 3 | never |
| ioc-rests | 16 | never | never | never | never |

Reading: the S1 corpus does have partial fills (registers are loaded as one multi-lot order and
DECs chip at it), and the FIFO bug does reorder the queue behind them, but no quantity and no
link outcome ever depends on which one-lot sell is in front, so no program can tell. The
stop-side bug is only visible to programs that use stops (S2). Price priority is used by every
DECJZ, so every program breaks on it at once. A conformance suite built from programs needs
programs that use every rule, which is what the probe is.

## What an adversarial review found, and what was done

A separate review agent read every file and tried to break the server. Reproduced and fixed:
one malformed byte from any client terminated the exchange (an exception escaped the session
loop; now the session is closed and the stream marked bad); a maker fill for a session that was
not itself sending was never flushed (sessions are now flushed whenever they have output); a
client that stopped reading wedged the matching thread in a blocking flush (flushes are now
non-blocking and bounded, a 64 MB backlog drops the peer, and the router reads opportunistically
during long bursts). Also fixed from the reading: the shared-memory reconnect race (the server
now owns the ring resets and acknowledges the client's epoch; a stale file times out), the shm
ring trusting the peer's length byte, session indices being reused for orders of dead sessions,
acknowledgements reporting `Filled` for takers cut short by self-trade prevention (now `Done` with
the real remainder), unbounded prices and quantities from the wire (bounded to 2^31), UDP
datagrams that were not whole records poisoning a peer (dropped whole), table files with
unchecked counts, router tables with out-of-range actions, a fired-stop-on-own-submit ordering
case in the router, Snake boards too small for the starting body, and bit-ISA macros on empty
vectors. `tests/test_server.cpp` reproduces the three reproduced defects against a live exchange
and passes. Known and documented: UDP peers are never evicted; a full board makes the fruit search
spin; a dense book's snapshot could exceed the feed ring.

## Semantics the wire changed, and what did not change

- The stage 1 engine queues an already-crossed stop as a work item behind actions already queued;
  the exchange fires it inside the submit, before the acknowledgement. Fills whose taker is a
  fired stop are marked (`is_maker = 2`) and the router fires their links at once, as the reference
  does for every print. No S2 program in the corpus showed a difference.
- The reference fires accept links at submission, before any fill of that order; over the wire
  the acknowledgement comes after the fills. The router holds an order's fill actions until its
  acknowledgement and then queues accept actions first. Equivalence held on the paper's variant.
- Shared memory serves one client; the keyboard uses TCP in that configuration.

## Reproduce

```
make
./build/test_fastbook                      # 1,581,773 checks against stage 1 (also -asan)
./build/exchange --tcp 7000 --udp 7001 --shm /tmp/minsky.shm --feed 7002 --quiet &
./build/test_wire --connect tcp://127.0.0.1:7000 --n 300   # and udp://127.0.0.1:7001, shm:///tmp/minsky.shm
./build/test_snake --wire tcp://127.0.0.1:7000
./build/test_server tcp://127.0.0.1:7000
./build/test_programs
./build/compile snake --out programs/snake.tbl --layout programs/snake.layout
./build/router --table programs/snake.tbl --connect tcp://127.0.0.1:7000 --reset --max-instr 300000 [--delay-us 50]
./build/canary --programs 1500 --snake-frames 500
./build/exchange --tcp 7200 --udp 7201 --shm /tmp/minsky-bench.shm --feed 7202 --books 16 --quiet &
./build/bench --iters 20000
```
