"""T9 Speed: how long is a proof?

Programs of 3, 6 and 12 nodes (after rearm expansion, as `policyc` reports them) are compiled
and verified against a generous envelope so each one certifies; the headline is the verifier's
own `explored.ms` for the 12-node program, with wall-clock milliseconds alongside. Each program
is verified three times and the median is reported (`verify` is deterministic; only the time
moves). The verifier is called directly through the venue client.

Node counts:
  3   entry, stop, take
  6   entry, stop, take, scale-in, the scale-in's stop and take
  12  the 6-node program with the first stop re-armed 3 times and the second re-armed twice,
      plus a partial take (BOUNDED_KEYS.md §7: `rearm K` is K chained copies)

Instruments: instruments.json is Build A's and lists what the exchange trades. Two- and
three-instrument programs (a hedge leg per extra instrument) are generated only for instruments
that file lists; with ETH alone the test reports one instrument and says why.
"""
from __future__ import annotations

import statistics
import time

from ..context import Context, Result

ENVELOPE = {"contract_version": 1, "window": "24h_rolling", "max_loss_cents": 50000, "max_fees_cents": 5000,
            "max_trades": 60, "max_exposure_lots": 4, "require_live_stop": True,
            "env": {"max_gap_ticks": 25, "fill": "full_at_print", "fee_bps": {"maker": 1.0, "taker": 3.5}, "trigger_ref": "last"}}
EXTRA_INSTRUMENTS = ("BTC", "SOL")


class Test:
    id = "speed"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        mark = ctx.mark()
        known = ctx.instruments()
        extras = [i for i in EXTRA_INSTRUMENTS if i in known]
        instrument_sets = [["ETH"]] + [["ETH"] + extras[:k] for k in range(1, len(extras) + 1)]
        if len(instrument_sets) == 1:
            ctx.log("instruments.json lists only ETH: BTC and SOL are not instruments on this exchange, so the "
                    "2- and 3-instrument series are not generated", instruments=sorted(known))
        marks = {i: (mark if i == "ETH" else ctx.mark(i)) for s in instrument_sets for i in s}
        ok = True
        for ins in instrument_sets:
            env = dict(ENVELOPE, instruments=list(ins), account=0)
            series = []
            for target in (3, 6, 12):
                src = _program(target, mark, ins, marks)
                code, comp = ctx.venue.compile(src, env["env"])
                if code != 0:
                    ctx.row({"instruments": len(ins), "nodes_requested": target, "compiled": False, "error": comp})
                    ok = False
                    continue
                nodes = int(comp["report"]["nodes_total"])
                wall, engine, status, result = [], [], None, None
                for _ in range(3):
                    t0 = time.perf_counter()
                    vcode, result = ctx.venue.verify(comp, env, mark, 30000)
                    wall.append((time.perf_counter() - t0) * 1000)
                    engine.append(int((result.get("explored") or {}).get("ms", 0)))
                    status = {0: "certified", 3: "rejected", 4: "timeout"}.get(vcode, "error")
                ms = int(statistics.median(engine))
                row = {"instruments": len(ins), "nodes_requested": target, "nodes": nodes, "status": status,
                       "verify_ms": ms, "wall_ms": round(statistics.median(wall), 1),
                       "states": (result.get("explored") or {}).get("states"), "paths": (result.get("explored") or {}).get("paths"),
                       "depth": (result.get("explored") or {}).get("depth"),
                       "bound_cents": (result.get("bound") or {}).get("max_loss_cents"),
                       "violates": result.get("violates"), "engine": result.get("engine"), "source": src}
                ctx.row(row)
                series.append([nodes, ms])
                ctx.log(f"{len(ins)} instrument(s), {nodes} nodes: {status} in {ms} ms (wall {row['wall_ms']} ms)")
                if status != "certified":
                    ok = False
                    ctx.log(f"{nodes}-node program did not certify: {result.get('violates')} {result.get('value_on_path')}")
            name = f"{len(ins)} instrument" + ("s" if len(ins) > 1 else "")
            ctx.series(name, series)
        rows12 = [r for r in res.rows if r.get("nodes_requested") == 12 and r.get("instruments") == 1]
        headline = rows12[0]["verify_ms"] if rows12 else None
        ctx.metric("instrument_counts", [len(s) for s in instrument_sets])
        ctx.metric("ms_12_nodes", headline)
        ctx.metric("ms_by_nodes", {str(r["nodes"]): r["verify_ms"] for r in res.rows if r.get("instruments") == 1 and "verify_ms" in r})
        ctx.metric("engine", next((r.get("engine") for r in res.rows if r.get("engine")), None))
        res.evidence.update({"envelope": ENVELOPE, "mark": mark, "instruments_available": sorted(known)})
        return res.verdict("ms to certificate (12 nodes)", headline, "ms", ok and headline is not None)


def _program(nodes: int, mark: int, ins: list[str], marks: dict[str, int]) -> str:
    entry = mark - 10
    lines = [f"instrument {', '.join(ins)}", f"entry buy 1 at {entry}"]
    if nodes == 3:
        lines += ["sl: exit all at entry - 12 allow 25 when price <= entry - 12  oco tp",
                  "tp: take all at entry + 30  oco sl"]
    elif nodes == 6:
        lines += ["sl: exit all at entry - 12 allow 25 when price <= entry - 12  oco tp",
                  "tp: take all at entry + 30  oco sl",
                  "sc: scale buy 1 at entry - 5",
                  "sl2: exit all at entry - 12 allow 25 when price <= entry - 12 after sc  oco tp2",
                  "tp2: take all at entry + 30 after sc  oco sl2"]
    else:
        lines += ["sl: exit all at entry - 12 allow 25 when price <= entry - 12  oco tp, tp1  rearm 3",
                  "tp1: take half at entry + 15  oco sl",
                  "tp: take all at entry + 30  oco sl",
                  "sc: scale buy 1 at entry - 5",
                  "sl2: exit all at entry - 12 allow 25 when price <= entry - 12 after sc  oco tp2  rearm 2",
                  "tp2: take all at entry + 30 after sc  oco sl2"]
    # one hedge leg per extra instrument, released by the entry fill (counts as a node each)
    for k, other in enumerate(ins[1:], 1):
        lines.append(f"h{k}: hedge {other} sell 1 at {marks.get(other, mark) + 5}")
    return "\n".join(lines) + "\n"
