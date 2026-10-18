"""T1 The audit: do the stop-limits five LLM wallets actually placed survive the static check?

Data: fixtures/audit_stoplimits.json, 253 zero-allowance stop-limit orders placed by five LLM
wallets in Alpha Arena season 1 (Hyperliquid public API). Every one has `limit == trigger`.

Each order becomes one MPL program on the ETH instrument. The audit's prices are dollars on BTC,
BNB, SOL and so on; the point of the test is the allowance, not the level, so each order is
mapped into ETH tick space around the current mark:

    allow_ticks = |trigger - limit| in ETH ticks     (0 for all 253: zero allowance)
    sell stop (protects a long):  entry buy 1 at mark - 9,  exit all at entry - 3 allow A when price <= entry - 3
    buy stop (protects a short):  entry sell 1 at mark + 9, exit all at entry + 3 allow A when price >= entry + 3
    plus a take 20 ticks the other way (every program needs one, CONTRACT.md §9 A8)

Three phases, each real:
  1. static: `policyc` must reject `allow 0` with a line number (400). Expected 253 / 253.
  2. forced: `policyc --unsafe-allow-zero` lets it through and `verify` must find the gap path
     (422 max_loss_cents). RealVenue only: the fake venue has no such flag, so this phase is
     recorded as skipped there.
  3. repaired: with `allow = env.max_gap_ticks` the same program must certify. Expected 253 / 253.
The envelope is BOUNDED_KEYS.md §5.1 (fixtures/envelope_40.json). The verifier is called
directly through the venue client (no account is needed: this is a question about programs).
"""
from __future__ import annotations

import json
import subprocess
from collections import Counter

from gateway import config

from ..context import Context, Result, FIXTURES

STOP_TICKS = 3        # the stop sits 3 ticks inside the entry: a tight protective exit
ENTRY_OFF = 9         # the entry rests 9 ticks from the mark


class Test:
    id = "audit"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        audit = json.loads((FIXTURES / "audit_stoplimits.json").read_text())
        orders = audit["orders"]
        env = ctx.envelope_40()
        gap = int(env["env"]["max_gap_ticks"])
        tick = ctx.tick_cents("ETH")
        mark = ctx.mark()
        real = ctx.venue.name == "real"
        ctx.log("audit loaded", source=audit.get("source"), count=len(orders), mark=mark, max_gap_ticks=gap, venue=ctx.venue.name)
        if not real:
            ctx.log("forced phase skipped: the fake venue has no --unsafe-allow-zero (RealVenue only)")

        counts = Counter()
        by_model: dict[str, Counter] = {}
        for i, o in enumerate(orders):
            allow = int(round(abs(float(o["trigger"]) - float(o["limit"])) * 100 / tick))
            src0 = _program(mark, o["side"], allow)
            row = {"i": i + 1, "model": o["model"], "coin": o["coin"], "oid": o["oid"], "side": o["side"],
                   "trigger_usd": o["trigger"], "limit_usd": o["limit"], "allow_ticks": allow}
            bm = by_model.setdefault(o["model"], Counter())
            bm["n"] += 1
            # 1. static
            code, body = ctx.venue.compile(src0, env["env"])
            row["static"] = 400 if code == 2 else (200 if code == 0 else 500)
            row["static_message"] = body.get("message") if code == 2 else None
            if code == 2:
                counts["rejected"] += 1
                bm["rejected"] += 1
            # 2. forced through (real venue)
            if real:
                forced = _force_compile(src0)
                if forced is None:
                    row["forced"] = "compile_failed"
                else:
                    vcode, vres = ctx.venue.verify(forced, env, mark, config.VERIFY_TIMEOUT_MS)
                    if vcode == 3:
                        row["forced"] = 422
                        row["forced_violates"] = vres.get("violates")
                        row["forced_value_on_path"] = vres.get("value_on_path")
                        row["forced_gap"] = _widest_gap(vres.get("counterexample", []))
                        counts["gap_path"] += 1
                        bm["gap_path"] += 1
                    else:
                        row["forced"] = {0: 200, 4: "timeout"}.get(vcode, "error")
            else:
                row["forced"] = "skipped"
            # 3. repaired with allow = max_gap_ticks
            code, comp = ctx.venue.compile(_program(mark, o["side"], gap), env["env"])
            if code == 0:
                vcode, vres = ctx.venue.verify(comp, env, mark, config.VERIFY_TIMEOUT_MS)
                row["repaired"] = {0: 200, 3: 422, 4: "timeout"}.get(vcode, "error")
                if vcode == 0:
                    row["repaired_bound_cents"] = vres["bound"]["max_loss_cents"]
                    row["repaired_ms"] = vres.get("explored", {}).get("ms")
                    counts["certified"] += 1
                    bm["certified"] += 1
                    res.series.setdefault("bound_cents", []).append([i + 1, vres["bound"]["max_loss_cents"]])
                else:
                    row["repaired_violates"] = vres.get("violates")
            else:
                row["repaired"] = "compile_failed"
            ctx.row(row)
            if (i + 1) % 50 == 0 or i + 1 == len(orders):
                ctx.log(f"{i + 1}/{len(orders)}: rejected {counts['rejected']}, gap path {counts['gap_path']}, "
                        f"certified {counts['certified']}")

        n = len(orders)
        ctx.metric("count", n)
        ctx.metric("rejected", counts["rejected"])
        ctx.metric("gap_path_found", counts["gap_path"] if real else None)
        ctx.metric("certified_with_allow_max_gap", counts["certified"])
        ctx.metric("max_gap_ticks", gap)
        ctx.metric("forced_phase", "real" if real else "skipped: fake venue has no --unsafe-allow-zero")
        ctx.metric("by_model", {m: dict(c) for m, c in by_model.items()})
        ctx.series("bound_cents", res.series.pop("bound_cents", []))
        res.evidence.update({"source": audit.get("source"), "selection": audit.get("selection"), "mark": mark,
                             "mapping": "allow = |trigger - limit| in ETH ticks around the current mark; see the docstring"})
        ok = counts["rejected"] == n and counts["certified"] == n and (not real or counts["gap_path"] == n)
        return res.verdict("rejected", counts["rejected"], f"/ {n}", ok)


def _program(mark: int, side: str, allow: int) -> str:
    if side == "sell":                                   # a sell stop protects a long
        return (f"instrument ETH\nentry buy 1 at {mark - ENTRY_OFF}\n"
                f"sl: exit all at entry - {STOP_TICKS} allow {allow} when price <= entry - {STOP_TICKS}  oco tp\n"
                f"tp: take all at entry + 20  oco sl\n")
    return (f"instrument ETH\nentry sell 1 at {mark + ENTRY_OFF}\n"
            f"sl: exit all at entry + {STOP_TICKS} allow {allow} when price >= entry + {STOP_TICKS}  oco tp\n"
            f"tp: take all at entry - 20  oco sl\n")


def _force_compile(source: str) -> dict | None:
    """policyc --unsafe-allow-zero: the compiler's test-only flag that lets `allow 0` through so
    the verifier can show it catches the same orders."""
    p = subprocess.run([str(config.MINSKY_BUILD / "policyc"), "--backend", "minsky", "--instruments",
                        str(config.instruments_path()), "--unsafe-allow-zero"], input=source, capture_output=True, text=True)
    if p.returncode != 0:
        return None
    return json.loads(p.stdout)


def _widest_gap(path: list) -> int:
    return max((abs(int(s["to"]) - int(s["from"])) for s in path if s.get("env") == "gap" and "from" in s), default=0)
