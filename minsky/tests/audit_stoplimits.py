#!/usr/bin/env python3
"""Extract the Alpha Arena audit's stop-limit orders for BOUNDED_KEYS.md §8.5 test 4.

Same selection as ../../audit/alpha_arena_audit.py: one record per oid (final status preferred),
inside the season window, orderType "Stop Limit" with triggerPx > 0; zero allowance when the limit
is within 0.5 bp of the trigger. Writes ../../fixtures/audit_stoplimits.json.

Usage: python3 -I tests/audit_stoplimits.py   (from minsky/)
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "audit", "data")
S0, S1 = 1760731200000, 1762214400000
MODELS = ["deepseek", "gpt5", "gemini", "claude", "grok"]

out = []
for m in MODELS:
    raw = json.load(open(os.path.join(DATA, f"{m}_orders.json")))
    byoid = {}
    for r in raw:
        oid = r["order"]["oid"]
        if oid not in byoid or (byoid[oid]["status"] == "open" and r["status"] != "open"):
            byoid[oid] = r
    orders = [r for r in byoid.values() if S0 <= r["order"]["timestamp"] < S1]
    stops = [r for r in orders if r["order"]["orderType"] == "Stop Limit" and float(r["order"]["triggerPx"]) > 0]
    for r in stops:
        o = r["order"]
        trig, lim = float(o["triggerPx"]), float(o["limitPx"])
        out.append({"model": m, "oid": o["oid"], "coin": o["coin"], "side": "sell" if o["side"] == "A" else "buy",
                    "trigger": o["triggerPx"], "limit": o["limitPx"], "reduce_only": o["reduceOnly"],
                    "zero_allowance": abs(trig - lim) / trig * 1e4 < 0.5})
json.dump({"source": "audit/data (Alpha Arena season 1, Hyperliquid public API)",
           "selection": "orderType Stop Limit, triggerPx > 0, inside the season window, one record per oid",
           "count": len(out), "zero_allowance": sum(o["zero_allowance"] for o in out), "orders": out},
          open(os.path.join(ROOT, "fixtures", "audit_stoplimits.json"), "w"), indent=1)
print(f"{len(out)} stop-limits, {sum(o['zero_allowance'] for o in out)} with zero allowance")
