"""The StubLLM's brain: rule-based answers for the three prompt shapes Build B sends
(delegate brief -> MPL, droid repair -> MPL, phone transcript -> envelope JSON)."""
from __future__ import annotations

import json
import math
import re

WORDS = {"zero": 0, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8,
         "nine": 9, "ten": 10, "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14, "fifteen": 15,
         "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19, "twenty": 20, "thirty": 30,
         "forty": 40, "fifty": 50, "sixty": 60, "seventy": 70, "eighty": 80, "ninety": 90, "hundred": 100,
         "thousand": 1000}


def words_to_int(s: str) -> int | None:
    total = cur = 0
    seen = False
    for w in re.split(r"[\s-]+", s.lower()):
        if w in ("and",):
            continue
        if w not in WORDS:
            return total + cur if seen else None
        seen = True
        v = WORDS[w]
        if v == 100:
            cur = max(1, cur) * 100
        elif v == 1000:
            total += max(1, cur) * 1000
            cur = 0
        else:
            cur += v
    return total + cur if seen else None


def _num(pattern: str, text: str, default=None):
    m = re.search(pattern, text, re.I)
    if not m:
        return default
    g = m.group(1)
    try:
        return int(float(g.replace(",", "")))
    except ValueError:
        return words_to_int(g) or default


def _ctx(user: str) -> dict:
    """The gateway's prompts embed a JSON context block; read it if present."""
    m = re.search(r"<context>\s*(\{.*?\})\s*</context>", user, re.S)
    if m:
        try:
            return json.loads(m.group(1))
        except ValueError:
            pass
    return {}


def respond(system: str, user: str) -> str:
    if "TASK: repair" in user:
        return repair(user)
    if "TASK: envelope" in user:
        return envelope(user)
    return write_program(user)


def write_program(user: str) -> str:
    ctx = _ctx(user)
    env = ctx.get("envelope", {})
    gap = int(env.get("env", {}).get("max_gap_ticks", 25))
    max_loss = int(env.get("max_loss_cents", 4000))
    instrument = (env.get("instruments") or ["ETH"])[0]
    tick = int(ctx.get("tick_cents", 100))
    mark = int(ctx.get("mark", 3010))
    brief = ctx.get("brief", user)
    side = "sell" if re.search(r"\b(short|sell)\b", brief, re.I) else "buy"
    qty = _num(r"(\d+)\s*(?:lot|lots|eth)\b", brief, 1) or 1
    entry = _num(r"(?:at|near|around|entry(?: at)?)\s*\$?(\d{3,6})\b", brief, None) or (mark - 10 if side == "buy" else mark + 10)
    stop_ticks = _num(r"stop\D{0,20}?(\d+)\s*(?:ticks?|dollars?|\$)", brief, None)
    take_ticks = _num(r"(?:target|take[- ]profit|tp)\D{0,20}?(\d+)\s*(?:ticks?|dollars?|\$)", brief, 30) or 30
    # The stop must keep the worst gap path inside the envelope: (k - 1 + gap) * qty * tick + fees <= max_loss
    fees = math.ceil(entry * tick * qty * 4.5 / 10000) + 2
    k_max = (max_loss - fees) // (qty * tick) + 1 - gap
    if stop_ticks is None or stop_ticks > k_max:
        stop_ticks = max(1, k_max)
    sign = "-" if side == "buy" else "+"
    cmp_ = "<=" if side == "buy" else ">="
    tsign = "+" if side == "buy" else "-"
    return (f"```mpl\ninstrument {instrument}\nentry {side} {qty} at {entry}\n"
            f"sl: exit all at entry {sign} {stop_ticks} allow {gap} when price {cmp_} entry {sign} {stop_ticks}   oco tp\n"
            f"tp: take all at entry {tsign} {take_ticks}   oco sl\n```\n")


def repair(user: str) -> str:
    ctx = _ctx(user)
    src = ctx.get("source", "")
    env = ctx.get("envelope", {})
    gap = int(env.get("env", {}).get("max_gap_ticks", 25))
    rej = ctx.get("rejection", {})
    violates = rej.get("violates")
    tick = int(ctx.get("tick_cents", 100))
    lines = src.rstrip("\n").split("\n")
    out = []
    m_entry = re.search(r"entry\s+(buy|sell)\s+(\d+)\s+at\s+(\d+)", src)
    qty = int(m_entry.group(2)) if m_entry else 1
    entry = int(m_entry.group(3)) if m_entry else 3000
    max_loss = int(env.get("max_loss_cents", 4000))
    fees = math.ceil(entry * tick * qty * 4.5 / 10000) + 2
    k_max = max(1, (max_loss - fees) // (qty * tick) + 1 - gap)
    has_exit = any(re.search(r":\s*exit\b", l) for l in lines)
    for l in lines:
        m = re.search(r"^(\s*\w+\s*:\s*exit\s+\S+\s+at\s+entry\s*([+-])\s*(\d+))(\s+allow\s+(\d+))?(.*)$", l)
        if m and violates in ("max_loss_cents", "require_live_stop", None):
            k = int(m.group(3))
            allow = int(m.group(5)) if m.group(5) else 0
            new_allow = max(allow, gap)
            new_k = min(k, k_max)
            rest = m.group(6)
            rest = re.sub(r"(entry\s*[+-]\s*)\d+", lambda mm: mm.group(1) + str(new_k), rest, count=1)
            head = re.sub(r"(at\s+entry\s*[+-]\s*)\d+", lambda mm: mm.group(1) + str(new_k), m.group(1), count=1)
            out.append(f"{head} allow {new_allow}{rest}")
        elif violates == "max_trades" and re.search(r"\brearm\s+\d+", l):
            out.append(re.sub(r"\brearm\s+\d+", "rearm 0", l))
        elif violates == "max_exposure_lots" and re.search(r":\s*scale\b", l):
            continue
        else:
            out.append(l)
    has_take = any(re.search(r":\s*take\b", l) for l in out)
    if not has_take and m_entry and violates in ("max_fees_cents", None):
        side = m_entry.group(1)
        tsign = "+" if side == "buy" else "-"
        out.append(f"tp: take all at entry {tsign} 30")
    if not has_exit and m_entry:
        side = m_entry.group(1)
        sign, cmp_ = ("-", "<=") if side == "buy" else ("+", ">=")
        out.append(f"sl: exit all at entry {sign} {k_max} allow {gap} when price {cmp_} entry {sign} {k_max}")
    return "```mpl\n" + "\n".join(out) + "\n```\n"


def envelope(user: str) -> str:
    ctx = _ctx(user)
    text = ctx.get("transcript", user)
    current = ctx.get("current") or {}
    dollars = _num(r"\$\s*(\d[\d,]*)\s*(?:a|per)?\s*day", text) or _num(r"([a-z\- ]+?)\s+dollars?\s+(?:a|per)\s+day", text)
    if dollars is None:
        dollars = _num(r"(?:lose|loss|risk)\D{0,15}?\$?(\d[\d,]*)", text)
    gap = _num(r"gap\D{0,10}?(\d+)", text) or _num(r"gap\s+([a-z\- ]+?)\s+ticks", text)
    fees = _num(r"fees?\D{0,15}?\$?(\d[\d,]*)", text)
    trades = _num(r"(\d+)\s*trades", text)
    lots = _num(r"(\d+)\s*lots?", text)
    instruments = re.findall(r"\b(ETH|BTC|SOL)\b", text.upper())
    env = {
        "max_loss_cents": dollars * 100 if dollars is not None else current.get("max_loss_cents", 4000),
        "max_fees_cents": fees * 100 if fees is not None else current.get("max_fees_cents", 500),
        "max_trades": trades if trades is not None else current.get("max_trades", 60),
        "max_exposure_lots": lots if lots is not None else current.get("max_exposure_lots", 2),
        "instruments": sorted(set(instruments)) or current.get("instruments", ["ETH"]),
        "require_live_stop": True,
        "env": {"max_gap_ticks": gap if gap is not None else current.get("env", {}).get("max_gap_ticks", 25),
                "fill": "full_at_print", "fee_bps": {"maker": 1.0, "taker": 3.5}, "trigger_ref": "last"},
    }
    return "```json\n" + json.dumps(env) + "\n```\n"
