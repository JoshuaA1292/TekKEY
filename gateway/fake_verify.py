"""The fake venue's model of `verify` (BOUNDED_KEYS.md §8), in Python, for development only.

It is NOT the exhaustive search over an exact engine that Build A ships. It computes the same
quantities for the common program shapes (entry, scale-ins, stop-limit / stop-market exits,
takes, rearms) using the §8.1 environment: worst trigger print is one tick past the trigger,
then a gap of max_gap_ticks; a triggered stop-limit fills at the print only if its limit
allows; fees are rounded up per fill. On the two worked fixtures (§11) it reproduces the hand
arithmetic to the cent. Every certificate it emits says `"engine": "fake_venue"`.
"""
from __future__ import annotations

import math
import time


def fee(price_ticks: int, qty: int, tick_cents: int, bps: float) -> int:
    return math.ceil(price_ticks * tick_cents * qty * bps / 10000)


def verify(compiled: dict, envelope: dict, mark: int, instruments_cfg: dict, program_id: int | None = None) -> tuple[int, dict]:
    """Returns (exit_code, body): 0 + Certificate, or 3 + Rejection."""
    t0 = time.perf_counter()
    env = envelope["env"]
    gap = int(env.get("max_gap_ticks", 0))
    maker = float(env["fee_bps"]["maker"])
    taker = float(env["fee_bps"]["taker"])
    nodes = {n["id"]: n for n in compiled["nodes"]}
    entry = nodes["entry"]
    ins = entry["instrument"]
    tick = int(instruments_cfg[ins]["tick_cents"])
    d = 1 if entry["side"] == "buy" else -1            # adverse direction: down for a long

    def adverse(p: int, ticks: int) -> int:
        return max(1, p - d * ticks)

    def crossed(trigger: int, print_: int) -> bool:   # has the print reached the trigger?
        return d * (trigger - print_) >= 0

    def limit_fills(limit: int, print_: int) -> bool:  # long: sell limit <= print
        return d * (print_ - limit) >= 0

    # ---- entry fill ---------------------------------------------------------------------
    entry_px = entry["price"]
    crosses_mark = d * (entry_px - mark) >= 0            # a buy above the mark is an aggressor
    e_fill = mark if crosses_mark else entry_px
    e_fee = fee(e_fill, entry["qty"], tick, taker if crosses_mark else maker)
    fills = [("entry", e_fill, entry["qty"], e_fee)]
    path = [{"env": "print", "at": mark}]                 # the settle print after deployment (CONTRACT §9 A6)
    if e_fill != mark:
        path.append({"env": "gap" if abs(e_fill - mark) > 1 else "print", **({"from": mark, "to": e_fill} if abs(e_fill - mark) > 1 else {"at": e_fill})})
    path += [
            {"event": "filled", "node": "entry", "qty": entry["qty"], "price": e_fill, "fee_cents": e_fee}]

    # ---- scale-ins (worst case: all of them fill, on the main instrument) ------------------
    scales = [n for n in compiled["nodes"] if n["kind"] == "limit" and not n["reduce_only"]
              and n["id"] != "entry" and n["instrument"] == ins and n["side"] == entry["side"]]
    pos = entry["qty"]
    for s in scales:
        f = fee(s["price"], s["qty"], tick, maker)
        fills.append((s["id"], s["price"], s["qty"], f))
        path.append({"env": "print", "at": s["price"]})
        path.append({"event": "filled", "node": s["id"], "qty": s["qty"], "price": s["price"], "fee_cents": f})
        pos += s["qty"]
    max_exposure = pos
    other_legs = [n for n in compiled["nodes"] if n["kind"] == "limit" and not n["reduce_only"] and n["instrument"] != ins]

    exits = [n for n in compiled["nodes"] if n["kind"] in ("stop_limit", "stop_market") and n["reduce_only"]
             and n["instrument"] == ins and "#" not in n["id"]]
    takes = [n for n in compiled["nodes"] if n["kind"] == "limit" and n["reduce_only"] and n["instrument"] == ins
             and "#" not in n["id"]]
    rearm_mult = 1 + max([0] + [int(n["id"].split("#")[1]) for n in compiled["nodes"] if "#" in n["id"]])

    cost = sum(p * q for _, p, q, _ in fills) * tick       # notional paid (long) / received (short)
    fees_so_far = sum(f for *_, f in fills)
    trades = len(fills)
    realized_loss = 0                                       # cents, from exits that filled

    def loss_at(print_: int, position: int) -> int:
        """Realized + mark-to-market loss (cents) with `position` still open at `print_`."""
        return realized_loss + fees_so_far + d * (cost - print_ * position * tick) if position else realized_loss + fees_so_far

    # ---- the adverse path: gap past each stop in turn --------------------------------------
    last_print = e_fill
    remaining = list(exits)
    exit_fills: list[tuple[str, int, int, int]] = []
    rejection = None
    position = pos
    # cost of the *remaining* position after partial exits: keep average cost per lot
    avg_cost = cost // pos if pos else 0
    # the path must stay within the gap: print one tick before the first trigger, then gap
    while position > 0:
        if not remaining:
            break
        top = min(remaining, key=lambda e: -d * e["trigger"])     # the trigger reached first
        pre = adverse(top["trigger"], -1)                           # one tick before the trigger
        if pre != last_print:
            path.append({"env": "print", "at": pre})
            last_print = pre
        landing = adverse(pre, gap) if gap > 0 else top["trigger"]
        path.append({"env": "gap", "to": landing} if gap > 0 else {"env": "print", "at": landing})
        triggered = [e for e in remaining if crossed(e["trigger"], landing)]
        triggered.sort(key=lambda e: -d * e["trigger"])
        for e in triggered:
            remaining.remove(e)
            if e["kind"] == "stop_market":
                q = min(e["qty"], position)
                f = fee(landing, q, tick, taker)
                path.append({"event": "triggered", "node": e["id"], "limit": None})
                path.append({"event": "filled", "node": e["id"], "qty": q, "price": landing, "fee_cents": f})
                realized_loss += d * (avg_cost - landing * tick) * q
                fees_so_far += f
                trades += 1
                position -= q
                exit_fills.append((e["id"], landing, q, f))
            else:
                path.append({"event": "triggered", "node": e["id"], "limit": e["price"]})
                if limit_fills(e["price"], landing):
                    q = min(e["qty"], position)
                    f = fee(landing, q, tick, taker)
                    path.append({"event": "filled", "node": e["id"], "qty": q, "price": landing, "fee_cents": f})
                    realized_loss += d * (avg_cost - landing * tick) * q
                    fees_so_far += f
                    trades += 1
                    position -= q
                    exit_fills.append((e["id"], landing, q, f))
                else:
                    reason = "limit above print" if d == 1 else "limit below print"
                    path.append({"event": "rests", "node": e["id"], "reason": reason})
            if position <= 0:
                break
        last_print = landing
        # Was the envelope already broken at this landing?
        if loss_at(landing, position) > int(envelope["max_loss_cents"]):
            path.append({"event": "mark", "position": position, "loss_cents": loss_at(landing, position)})
            rejection = ("max_loss_cents", loss_at(landing, position))
            break
    if rejection is None and position > 0:
        # Nothing left to protect the position: the price walks to the loss boundary.
        limit_cents = int(envelope["max_loss_cents"])
        # smallest adverse move where loss > limit
        need = limit_cents - loss_at(last_print, position) + 1
        ticks = max(1, math.ceil(need / (position * tick)))
        target = adverse(last_print, ticks)
        p = last_print
        while True:
            p = adverse(p, min(gap or 1, abs(target - p)) if gap else 1)
            path.append({"env": "print", "at": p})
            if loss_at(p, position) > limit_cents or p == target:
                break
        path.append({"event": "mark", "position": position, "loss_cents": loss_at(p, position)})
        rejection = ("max_loss_cents", loss_at(p, position))

    worst_loss = loss_at(last_print, position) if rejection is None else rejection[1]
    if rejection is None:
        path.append({"event": "mark", "position": 0, "loss_cents": worst_loss})

    # ---- the key holder's moves: cancel right after entry, flattened at the next (gapped) print
    flat_px = adverse(e_fill, gap)
    cancel_loss = d * (e_fill - flat_px) * entry["qty"] * tick + e_fee + fee(flat_px, entry["qty"], tick, taker)
    # fees are worst when the cancel lands after a gap in the *favourable* direction (a higher print
    # for a long): the flatten is a taker fill at that price. The real verifier finds this path.
    flat_up = adverse(e_fill, -gap)
    if takes:
        nearest_take = min((t["price"] for t in takes), key=lambda p_: d * (e_fill - p_))
        if d * (nearest_take - flat_up) <= 0:          # the gap would cross the take: it fills instead
            flat_up = nearest_take - d * 1 if d * (nearest_take - e_fill) < 0 else flat_up
    cancel_fees = e_fee + max(fee(flat_px, entry["qty"], tick, taker), fee(flat_up, entry["qty"], tick, taker))

    # ---- fees: the no-gap stop path, the take path and the cancel path ------------------------
    base_fees = sum(f for *_, f in fills)
    stop_fees = [base_fees + fee(e["trigger"], min(e["qty"], pos), tick, taker) for e in exits]
    take_fees = [base_fees + fee(t["price"], min(t["qty"], pos), tick, maker) for t in takes]
    all_exit_fees = [fees_so_far] + stop_fees + take_fees + [cancel_fees, base_fees + fee(e_fill, pos, tick, taker)]
    max_fees = max(all_exit_fees) * rearm_mult
    max_trades = max(trades, len(fills) + len([e for e in exits]) if exits else trades, len(fills) + (1 if takes else 0)) * rearm_mult
    max_loss = max(worst_loss, cancel_loss) * rearm_mult if rejection is None else worst_loss

    explored = {"states": 0, "paths": 0, "depth": len(path), "ms": int((time.perf_counter() - t0) * 1000)}
    assumes = {"max_gap_ticks": gap, "fill": env.get("fill", "full_at_print"), "fee_bps": env["fee_bps"],
               "trigger_ref": env.get("trigger_ref", "last"), "margin": "isolated_per_program"}
    checked = {k: envelope[k] for k in ("max_loss_cents", "max_fees_cents", "max_trades", "max_exposure_lots")}

    violation = None
    rally = None
    if rejection is None and not takes and exits:
        # CONTRACT §9 A8: with no upper exit nothing stops a rally; the key holder's flatten after a rally
        # pays a fee on that notional, so fees are unbounded. The real verifier answers 422 max_fees_cents.
        max_fees_env = int(envelope["max_fees_cents"])
        q = entry["qty"]
        p_rally = int((max_fees_env - e_fee) * 10000 / (tick * q * taker)) + 1
        while fee(p_rally, q, tick, taker) + e_fee <= max_fees_env:
            p_rally += 1
        rally_fee = fee(p_rally, q, tick, taker)
        rally_path = [{"env": "print", "at": mark}]
        if e_fill != mark:
            rally_path.append({"env": "gap", "from": mark, "to": e_fill} if abs(e_fill - mark) > 1 else {"env": "print", "at": e_fill})
        rally_path += [{"event": "filled", "node": "entry", "qty": q, "price": e_fill, "fee_cents": e_fee},
                       {"env": "print", "at": p_rally + d}, {"keyholder": "cancel"},
                       {"event": "filled", "node": "flatten", "qty": q, "price": p_rally, "fee_cents": rally_fee},
                       {"event": "mark", "position": 0, "loss_cents": -d * (p_rally - e_fill) * q * tick + e_fee + rally_fee}]
        rejection_fees = ("max_fees_cents", e_fee + rally_fee, rally_path)
    else:
        rejection_fees = None
    if rejection is not None:
        violation = ("max_loss_cents", int(envelope["max_loss_cents"]), rejection[1])
    elif max_loss > int(envelope["max_loss_cents"]):
        violation = ("max_loss_cents", int(envelope["max_loss_cents"]), max_loss)
    elif rejection_fees is not None:
        violation = ("max_fees_cents", int(envelope["max_fees_cents"]), rejection_fees[1])
        path = rejection_fees[2]
    elif max_fees > int(envelope["max_fees_cents"]):
        violation = ("max_fees_cents", int(envelope["max_fees_cents"]), max_fees)
    elif max_trades > int(envelope["max_trades"]):
        violation = ("max_trades", int(envelope["max_trades"]), max_trades)
    elif max_exposure > int(envelope["max_exposure_lots"]):
        violation = ("max_exposure_lots", int(envelope["max_exposure_lots"]), max_exposure)
    elif envelope.get("require_live_stop") and not exits:
        violation = ("require_live_stop", 1, 0)

    if violation:
        name, limit, value = violation
        body = {"contract_version": 1, "status": "rejected", "violates": name, "limit": limit,
                "value_on_path": value, "counterexample": path, "explored": explored,
                "engine": "fake_venue"}
        if program_id is not None:
            body["program_id"] = program_id
        return 3, body

    body = {
        "contract_version": 1,
        "status": "certified",
        "bound": {"max_loss_cents": max_loss, "max_fees_cents": max_fees,
                  "max_exposure_lots": max_exposure, "halts_within_trades": max_trades},
        "assumes": assumes,
        "envelope_checked": checked,
        "worst_path": path,
        "explored": explored,
        "nodes": {"total": compiled["report"]["nodes_total"], "native": compiled["report"]["native"],
                  "relayed": compiled["report"]["relayed"]},
        "locked_collateral_cents": compiled["report"]["locked_collateral_cents"],
        "engine": "fake_venue",
    }
    if other_legs:
        body["warnings"] = ["fake_venue: legs in other instruments are not modelled"]
    if program_id is not None:
        body["program_id"] = program_id
    return 0, body
