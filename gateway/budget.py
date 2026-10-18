"""B3: the rolling-window reservation ledger (BOUNDED_KEYS.md §5.2).

    remaining = max_loss_cents
              - sum of realized losses of programs that ended inside the window
              - sum of certified max_loss of programs still live

Fees and trades are budgeted the same way; exposure is instantaneous. The invariant
realized-in-window <= max_loss_cents is checked on every write: a violation is a bug in the
system, not a user error. It is logged loudly and the account is frozen.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass

from . import config
from .db import now_ms
from .errors import ApiError

log = logging.getLogger("gateway.budget")

LIVE_STATUSES = ("certified", "deployed", "paused")


@dataclass
class Remaining:
    loss: int
    fees: int
    trades: int
    exposure: int
    realized_loss: int
    reserved_loss: int

    def as_dict(self) -> dict:
        return {
            "remaining_loss_cents": self.loss,
            "remaining_fees_cents": self.fees,
            "remaining_trades": self.trades,
            "remaining_exposure_lots": self.exposure,
            "realized_loss_in_window_cents": self.realized_loss,
            "reserved_by_live_programs_cents": self.reserved_loss,
        }


def _sum(conn, sql, args):
    row = conn.execute(sql, args).fetchone()
    return tuple(int(v or 0) for v in row)


def remaining(conn, account: int, envelope: dict, now: int | None = None) -> Remaining:
    now = now if now is not None else now_ms()
    since = now - config.WINDOW_MS
    r_loss, r_fees, r_trades = _sum(
        conn,
        "SELECT SUM(cents_loss), SUM(cents_fees), SUM(trades) FROM ledger "
        "WHERE account=? AND kind='realize' AND t>=?",
        (account, since),
    )
    # Reservations of programs still live: a reserve row with no release row.
    v_loss, v_fees, v_trades, v_exp = _sum(
        conn,
        "SELECT SUM(l.cents_loss), SUM(l.cents_fees), SUM(l.trades), SUM(l.exposure) FROM ledger l "
        "WHERE l.account=? AND l.kind='reserve' AND NOT EXISTS "
        "(SELECT 1 FROM ledger r WHERE r.program_id=l.program_id AND r.kind='release')",
        (account,),
    )
    return Remaining(
        loss=int(envelope["max_loss_cents"]) - r_loss - v_loss,
        fees=int(envelope["max_fees_cents"]) - r_fees - v_fees,
        trades=int(envelope["max_trades"]) - r_trades - v_trades,
        exposure=int(envelope["max_exposure_lots"]) - v_exp,
        realized_loss=r_loss,
        reserved_loss=v_loss,
    )


def check_and_reserve(conn, account: int, program_id: int, bound: dict, envelope: dict,
                      now: int | None = None) -> tuple[Remaining, Remaining]:
    """Inside the caller's BEGIN IMMEDIATE. Raises 409 budget if the bound does not fit."""
    now = now if now is not None else now_ms()
    before = remaining(conn, account, envelope, now)
    req_loss = int(bound["max_loss_cents"])
    req_fees = int(bound["max_fees_cents"])
    req_trades = int(bound["halts_within_trades"])
    req_exp = int(bound["max_exposure_lots"])
    violated = None
    if req_loss > before.loss:
        violated = ("max_loss_cents", before.loss, req_loss)
    elif req_fees > before.fees:
        violated = ("max_fees_cents", before.fees, req_fees)
    elif req_trades > before.trades:
        violated = ("max_trades", before.trades, req_trades)
    elif req_exp > before.exposure:
        violated = ("max_exposure_lots", before.exposure, req_exp)
    if violated:
        name, rem, req = violated
        raise ApiError(409, "budget", violates=name, remaining_cents=before.loss,
                       requested_cents=req_loss, remaining=rem, requested=req,
                       budget=before.as_dict())
    conn.execute(
        "INSERT INTO ledger(account, program_id, kind, cents_loss, cents_fees, trades, exposure, t) "
        "VALUES (?,?,?,?,?,?,?,?)",
        (account, program_id, "reserve", req_loss, req_fees, req_trades, req_exp, now),
    )
    after = remaining(conn, account, envelope, now)
    _check_invariant(conn, account, envelope, after)
    return before, after


def release_and_realize(conn, account: int, program_id: int, envelope: dict,
                        realized_loss: int, fees: int, trades: int, now: int | None = None) -> Remaining:
    """On program.halted (or cancel): replace the reservation with the realized loss, max(0, loss)."""
    now = now if now is not None else now_ms()
    already = conn.execute(
        "SELECT 1 FROM ledger WHERE program_id=? AND kind='release'", (program_id,)
    ).fetchone()
    if already:
        return remaining(conn, account, envelope, now)
    res = conn.execute(
        "SELECT cents_loss, cents_fees, trades, exposure FROM ledger WHERE program_id=? AND kind='reserve'",
        (program_id,),
    ).fetchone()
    if res is None:
        log.error("ledger: halt for program %s with no reservation", program_id)
        res = {"cents_loss": 0, "cents_fees": 0, "trades": 0, "exposure": 0}
    realized_loss = max(0, int(realized_loss))
    if realized_loss > int(res["cents_loss"]):
        # The certificate said this could not happen. Record it anyway, loudly, then freeze.
        log.critical("LEDGER INVARIANT: program %s realized %s > reserved %s",
                     program_id, realized_loss, res["cents_loss"])
    conn.execute(
        "INSERT INTO ledger(account, program_id, kind, cents_loss, cents_fees, trades, exposure, t) "
        "VALUES (?,?,?,?,?,?,?,?)",
        (account, program_id, "release", int(res["cents_loss"]), int(res["cents_fees"]),
         int(res["trades"]), int(res["exposure"]), now),
    )
    conn.execute(
        "INSERT INTO ledger(account, program_id, kind, cents_loss, cents_fees, trades, exposure, t) "
        "VALUES (?,?,?,?,?,?,?,?)",
        (account, program_id, "realize", realized_loss, max(0, int(fees)), max(0, int(trades)), 0, now),
    )
    after = remaining(conn, account, envelope, now)
    _check_invariant(conn, account, envelope, after)
    return after


def _check_invariant(conn, account: int, envelope: dict, rem: Remaining) -> None:
    if rem.realized_loss > int(envelope["max_loss_cents"]):
        log.critical(
            "LEDGER INVARIANT VIOLATED account=%s realized_in_window=%s max_loss=%s -- freezing account",
            account, rem.realized_loss, envelope["max_loss_cents"],
        )
        conn.execute("UPDATE accounts SET frozen=1 WHERE id=?", (account,))
        conn.execute(
            "INSERT INTO events(account, program_id, type, payload_json, t) VALUES (?,?,?,?,?)",
            (account, None, "invariant.violated",
             '{"contract_version":1,"type":"invariant.violated","realized_in_window_cents":%d,"max_loss_cents":%d}'
             % (rem.realized_loss, int(envelope["max_loss_cents"])), now_ms()),
        )
