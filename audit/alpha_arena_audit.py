#!/usr/bin/env python3
"""Audit of the Alpha Arena season-1 AI trading wallets from Hyperliquid's public info API.

Usage:
  python3 -I alpha_arena_audit.py fetch   # pulls historicalOrders + userFillsByTime for each wallet into data/
  python3 -I alpha_arena_audit.py          # runs the audit on data/

Wallets (public; five of six season-1 models; Qwen3 Max not found):
  DeepSeek: DataWallet guide. GPT-5, Gemini, Claude, Grok: reply by @quant_amann under the launch post
  by @jay_azhang (Oct 17, 2025), GPT and Gemini corroborated by a Manifold Markets post. Read-only.

Definitions:
  season window     2025-10-17 20:00 UTC to 2025-11-04 00:00 UTC
  protective order  a reduce-only trigger order of type Stop Limit / Stop Market on the closing side
  unprotected time  time inside a position episode with no protective order live for that coin/side
  zero allowance    a Stop Limit whose limit price equals its trigger price (it rests after any gap)
  partial           a triggered Stop Limit whose fills (matched by oid) total less than its size
  stop->liq         bps between the nearest live stop trigger and the liquidation mark price
Caveats: historicalOrders returns at most the 2000 most recent orders per wallet; fills retention is
limited by the venue; the API does not expose leverage per position historically, so peak gross
notional is computed from fills and last fill prices.
"""
import collections, datetime, json, os, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
API = "https://api.hyperliquid.xyz/info"
WALLETS = {
    "deepseek": "0xc20ac4dc4188660cbf555448af52694ca62b0734",
    "gpt5": "0x67293D914eAFb26878534571add81F6Bd2D9fE06",
    "gemini": "0x1b7A7D099a670256207a30dD0AE13D35f278010f",
    "claude": "0x59fA085d106541A834017b97060bcBBb0aa82869",
    "grok": "0x56D652e62998251b56C8398FB11fcFe464c08F84",
}
S0, S1 = 1760731200000, 1762214400000


def post(body):
    req = urllib.request.Request(API, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def fetch():
    os.makedirs(DATA, exist_ok=True)
    for m, a in WALLETS.items():
        json.dump(post({"type": "historicalOrders", "user": a}), open(f"{DATA}/{m}_orders.json", "w"))
        json.dump(post({"type": "userFillsByTime", "user": a, "startTime": 1760572800000, "endTime": 1762300800000,
                        "aggregateByTime": False}), open(f"{DATA}/{m}_fills.json", "w"))
        print("fetched", m)


def ts(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, datetime.UTC).strftime("%m-%d %H:%M:%S")


def audit():
    rows = []
    for m, wallet in WALLETS.items():
        wallet = wallet.lower()
        raw = json.load(open(f"{DATA}/{m}_orders.json"))
        fills = [f for f in json.load(open(f"{DATA}/{m}_fills.json")) if S0 <= f["time"] < S1]
        byoid = {}
        for r in raw:  # one record per oid, preferring the final status
            oid = r["order"]["oid"]
            if oid not in byoid or (byoid[oid]["status"] == "open" and r["status"] != "open"):
                byoid[oid] = r
        orders = [r for r in byoid.values() if S0 <= r["order"]["timestamp"] < S1]
        filled = collections.defaultdict(float)
        for f in fills:
            filled[f["oid"]] += float(f["sz"])

        def life(r):
            end = r["statusTimestamp"] if r["status"] != "open" else float("inf")
            return r["order"]["timestamp"], end

        fees = sum(float(f["fee"]) for f in fills)
        pnl = sum(float(f["closedPnl"]) for f in fills)
        liq = [f for f in fills if f.get("liquidation") and f["liquidation"]["liquidatedUser"].lower() == wallet]
        stops = [r for r in orders if r["order"]["orderType"] == "Stop Limit" and float(r["order"]["triggerPx"]) > 0]
        zero = sum(1 for r in stops if abs(float(r["order"]["triggerPx"]) - float(r["order"]["limitPx"])) /
                   float(r["order"]["triggerPx"]) * 1e4 < 0.5)
        trig = {r["order"]["oid"] for r in raw if r["status"] == "triggered" and r["order"]["orderType"] == "Stop Limit"
                and S0 <= r["order"]["timestamp"] < S1}
        partial = sum(1 for oid in trig if filled.get(oid, 0.0) < float(byoid[oid]["order"]["origSz"]) * 0.999)
        protective = [r for r in orders if r["order"]["reduceOnly"] and r["order"]["orderType"].startswith("Stop")
                      and (r["order"]["isTrigger"] or r["order"]["triggerCondition"] == "Triggered")]
        by_coin = collections.defaultdict(list)
        for f in fills:
            by_coin[f["coin"]].append(f)
        episodes = unprot_eps = 0
        unprot_h = held_h = 0.0
        for coin, fl in by_coin.items():
            fl.sort(key=lambda f: f["time"])
            p, start = 0.0, None
            for f in fl:
                q = float(f["sz"]) * (1 if f["side"] == "B" else -1)
                before, p = p, round(p + q, 8)
                if before == 0 and p != 0:
                    start = f["time"]
                if before != 0 and (p == 0 or (before > 0) != (p > 0)) and start is not None:
                    end = f["time"]
                    episodes += 1
                    held_h += (end - start) / 3.6e6
                    side = "A" if before > 0 else "B"
                    segs = sorted(life(r) for r in protective if r["order"]["coin"] == coin and r["order"]["side"] == side)
                    cur, unc = start, 0
                    for s, e in segs:
                        if e < cur:
                            continue
                        if s > cur:
                            unc += min(s, end) - cur
                        cur = max(cur, min(e, end))
                        if cur >= end:
                            break
                    if cur < end:
                        unc += end - cur
                    unprot_h += unc / 3.6e6
                    unprot_eps += unc > 300_000
                    start = f["time"] if p != 0 else None
        pos, last, peak = {}, {}, 0.0
        for f in sorted(fills, key=lambda f: f["time"]):
            pos[f["coin"]] = round(pos.get(f["coin"], 0) + float(f["sz"]) * (1 if f["side"] == "B" else -1), 8)
            last[f["coin"]] = float(f["px"])
            peak = max(peak, sum(abs(pos[c]) * last[c] for c in pos))
        liqrows = []
        for f in liq:
            T, mark = f["time"], float(f["liquidation"]["markPx"])
            side = "A" if f["dir"].startswith("Close Long") else "B"
            live = [r for r in protective if r["order"]["coin"] == f["coin"] and r["order"]["side"] == side
                    and life(r)[0] <= T <= life(r)[1] + 1000 and float(r["order"]["triggerPx"]) > 0]
            d = min(abs(float(r["order"]["triggerPx"]) - mark) / mark * 1e4 for r in live) if live else None
            liqrows.append((ts(T), f["coin"], float(f["closedPnl"]), d))
        rows.append((m, len(fills), fees, pnl, len(liq), sum(x[2] for x in liqrows), len(stops), zero, len(trig), partial,
                     episodes, unprot_eps, unprot_h, held_h, peak, liqrows))
    print("Alpha Arena season 1, five of six wallets, Hyperliquid public API, window 2025-10-17 20:00 to 2025-11-04 00:00 UTC")
    hdr = ("model", "fills", "fees$", "fee%", "closedPnL$", "liqs", "liqPnL$", "stopLim", "zeroAllow", "trig", "partial",
           "episodes", "unprotEp", "unprot_h", "held_h", "peakGross$")
    print(" ".join(f"{h:>9s}" for h in hdr))
    for r in rows:
        m, nf, fees, pnl, nl, lp, ns, z, nt, pa, ep, ue, uh, hh, pk, _ = r
        print(" ".join(f"{x:>9}" for x in (m, nf, f"{fees:.0f}", f"{fees/100:.1f}", f"{pnl:.0f}", nl, f"{lp:.0f}", ns, z, nt, pa,
                                             ep, ue, f"{uh:.0f}", f"{hh:.0f}", f"{pk:.0f}")))
    print("\nliquidations: time, coin, closedPnl, bps from nearest live stop trigger to liquidation mark")
    for r in rows:
        for lt, coin, p, d in r[-1]:
            print(f"  {r[0]:9s} {lt} {coin:5s} {p:9.2f}  {('%.0f bps' % d) if d is not None else 'NO LIVE STOP'}")


if __name__ == "__main__":
    fetch() if len(sys.argv) > 1 and sys.argv[1] == "fetch" else audit()
