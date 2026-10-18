# Minsky stage 2: the exchange computer over a wire

A matching engine in one process, a stateless order router in another, a wire between them, and
Snake compiled into orders. Every instruction of the program is one order round trip, so the
frame rate is the latency benchmark. Stage 1 (`../stage1-orderbook-computer/`) proved the
construction in-process; this stage makes it physical and measurable. Results and the measured
numbers are in `RESULTS.md`; the pitch is `../PITCH.md`.

## Pieces

| Process / file | What it is |
|---|---|
| `src/fastbook.*` | The matching core: price-time priority, limit / market / IOC / stop-market / stop-limit, self-trade prevention, reduce-only, stop cascades. Flat layout: preallocated order pool (id = slot ‖ generation), per-book dense price window with an occupancy bitmap (best level is a `ctz`), intrusive FIFO per level, no hashing on the hot path. Four deliberate bugs behind flags (the canary). Semantics identical to stage 1's reference engine, proved by hash-equality after every event on random flow (`tests/test_fastbook.cpp`). |
| `src/proto.hpp` | Fixed-size little-endian records: NewOrder, Cancel, Reduce, Control → Ack, Fill, ControlReply; feed records Level, Trade, BookReset, Stats, Gap, Hello. |
| `src/transport.*` | One channel interface over three transports: `tcp://` (NODELAY, non-blocking, busy-polled), `udp://` (records packed into datagrams), `shm://` (two SPSC rings of 64-byte slots in an mmapped file, no syscalls). Nothing sleeps. |
| `src/matcher.hpp` | The exchange's message logic over the core, independent of sockets, plus an in-process channel (the zero-latency point on the law, used by the canary). |
| `src/exchange.cpp` | The server. One matching thread (price-time priority is serial per instrument) listening on any mix of transports at once; one feed thread fed from a lock-free ring so a slow visualiser never touches the core's clock; the latency knob is a per-session delay line on replies. |
| `src/table.*` | The router table on disk: templates, fill/accept links, initial actions, instruction markers; and the text layout file the visualiser reads. |
| `src/router.*` | Weinberger's "smart order router" made literal: a fixed table, no counters, no program counter. Pipelines sends and waits only when the next action needs a reply: exactly one wait per instruction for the minimal gadgets. Same queue semantics as the stage 1 engine. |
| `src/bitasm.hpp` | A bit ISA over one-bit registers (one book each): read, write, copy, ripple increment / decrement, equality, parity, a decoder tree, two-way branch; compiled to Minsky INC / DECJZ. |
| `src/snake.*` | Snake on the bit ISA: 256 pixel books, 512 direction-cell bits (the body is a linked list on the grid), 4-bit head / tail / fruit coordinates, a 16-bit LFSR for fruit, unbounded counters for score and pending growth, four keyboard books, a frame book and a death book. Plus the reference simulator the program is checked against, bit for bit. |
| `src/programs.*` | Rule 110, looping forever, for the "wall" demo. |
| `src/viz.cpp` | Terminal visualiser and keyboard client. Draws only from the exchange's feed and a static layout file, never from program state. Arrow keys are 1-lot orders into the keyboard books; `+`/`-` turn the latency knob; `m` cycles the engine mutations. |
| `src/bench.cpp` | The latency law: round trip per transport and per instruction shape, p50/p99/p99.9, the knob sweep, and the in-process points. |
| `src/canary.cpp` | Computation as a conformance test: each mutation × each program → instructions until the run diverges from the correct engine, with the rule coverage of each program. |
| `src/compile_main.cpp`, `src/router_main.cpp` | Tools: compile a program to a table; run a table against an exchange. |

## Build and run

```
make                 # everything into build/ (clang++, C++20, no dependencies beyond POSIX)
make test            # the tests that need no server
make test-all        # everything, against an exchange the target starts on 7000-7002
./demo.sh            # exchange + Snake router + visualiser; play with the arrow keys
./demo.sh --fps 0    # unpaced: the frame rate is the latency law
./demo.sh --transport shm
./demo.sh --rule110  # the Rule 110 wall
```

By hand:

```
./build/exchange --tcp 7000 --udp 7001 --shm /tmp/minsky.shm --feed 7002
./build/compile snake --out programs/snake.tbl --layout programs/snake.layout [--pad N] [--wrap]
./build/router --table programs/snake.tbl --connect tcp://127.0.0.1:7000 --reset [--max-fps 12] [--delay-us N]
./build/viz --layout programs/snake.layout
./build/bench --iters 50000       # needs an exchange on 7200/7201//tmp/minsky-bench.shm
./build/canary --programs 1500
```

## Tests

| Test | What it proves |
|---|---|
| `build/test_fastbook` | The wire core equals the stage 1 engine: canonical hash after every event on random order flow (all order types, stops, cascades, reduce-only, cancels; window and far prices; three STP modes), 1.58M checks; reduce-in-place, the STP cancel report, each mutation on a scenario built to show it; pool growth and stale ids. Clean under ASan/UBSan. |
| `build/test_wire --connect URL` | Exchange process + router process over the transport equal the in-process engine, instruction for instruction, on random programs compiled four ways (S1 minimal, S2 stop-branch, the paper's accept links, the paper's gadgets with cancel): same halt status, instruction count, registers read back from the books, trade count and resting orders. 0 mismatches on TCP, UDP and shm. |
| `build/test_snake [--wire URL] [--keys URL]` | Snake equals the reference simulator on every frame: in the interpreter (six configurations, up to 20,000 frames each), on the in-process engine with the state read from the books, and over the wire with keys injected as external orders. |
| `build/test_programs` | Rule 110 looping against a direct simulation; table and layout files round-trip exactly. |
| `build/test_server URL` | Server robustness against a live exchange: a malformed byte closes only its session, a passive session receives its maker fills and STP expiries, a client that stops reading does not wedge the server, acks report real remainders, repeated reconnects. |
| `build/test_viz` | The visualiser's feed state against a live exchange: pixels, score, fruit / head / tail bit decode, fps from the frame book, death flash, knob and mutation display, snapshot consistency, reconnect. |

The exchange is a real server and the router a real client; nothing in the demo is simulated
except the latency knob, which is a delay line on the exchange's replies and is labelled as
such on screen.
