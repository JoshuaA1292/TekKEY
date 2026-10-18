"""An ordinary API key.

This is a direct session to the exchange over its own wire protocol (minsky/src/proto.hpp):
fixed-size little-endian records over TCP, one Ack per order, Fills as they happen, Control
round trips for queries. It is exactly what `venued` and `replay` use from C++ (bk::WirePort in
minsky/src/env.hpp), written in Python so the race's raw lane can be what a binary key really is:
orders that are simply executed. Nothing here is simulated; every call is a round trip to the
running exchange process.

Two things are also here because the raw lane and the tape need them and they must match the
venue byte for byte:
  * `print_one`: a print at price P as the environment accounts make it (env.hpp `print_one`),
    so the live ETH tape lands on the book with the same mechanics the verifier assumes;
  * the journal readers (`fill_count`, `fill_at`), the single source of truth `venued` uses for
    a program's money, used the same way for the raw account's.
"""
from __future__ import annotations

import itertools
import socket
import struct
import threading
import time
from dataclasses import dataclass

# ---- proto.hpp -------------------------------------------------------------------------------
NEW_ORDER, CANCEL, REDUCE, CONTROL = 1, 2, 3, 4
ACK, FILL, CONTROL_REPLY = 16, 17, 18
SIZES = {1: 56, 2: 16, 3: 24, 4: 24, 16: 24, 17: 48, 18: 24, 32: 32, 33: 24, 34: 8, 35: 48, 36: 8, 37: 24}

BUY, SELL = 0, 1
LIMIT, MARKET, IOC, STOP_MARKET, STOP_LIMIT = 0, 1, 2, 3, 4
OCO_CANCEL, OCO_REDUCE = 0, 1

ACK_STATUS = {0: "rested", 1: "filled", 2: "done", 3: "rejected", 4: "cancelled", 5: "not_found", 6: "reduced",
              7: "expired", 8: "held", 9: "activated", 10: "group_cancelled", 11: "group_reduced",
              12: "parent_cancelled", 13: "finished"}
ORDER_STATE = {0: "gone", 1: "resting", 2: "pending", 3: "queued", 4: "held"}

# ControlOp
PING, LAST_PRICE, POSITION, CASH = 0, 7, 8, 9
ORDER_INFO, DEPTH_THROUGH, FILL_COUNT, FILL_INFO = 18, 19, 20, 21
SET_MARK, MARK = 15, 17

_NEW = struct.Struct("<BBBBB3xIIqqqQIB3x")
_CANCEL = struct.Struct("<B3xIQ")
_CONTROL = struct.Struct("<BB2xIqq")
_ACK = struct.Struct("<BB2xIQq")
_FILL = struct.Struct("<BB2xIQIIqqq")
_CTRL_REPLY = struct.Struct("<BB2xIqq")
assert _NEW.size == 56 and _CANCEL.size == 16 and _CONTROL.size == 24 and _ACK.size == 24 and _FILL.size == 48

ENV_SWEEP = 0          # E1: the environment's sweeping side (bk::EnvAccounts.e1)
ENV_LIQUIDITY = 254    # E2: the environment's liquidity side (bk::EnvAccounts.e2)
ENV_DEPTH = 1 << 30    # bk::kEnvDepth


def unsolicited(status: int) -> bool:
    return status == 7 or status >= 9


class ExchangeError(Exception):
    pass


@dataclass
class Ack:
    status: int
    order_id: int
    remaining: int

    @property
    def name(self) -> str:
        return ACK_STATUS.get(self.status, str(self.status))

    @property
    def ok(self) -> bool:
        return self.status in (0, 1, 2, 8)


@dataclass
class Fill:
    """A fill as the exchange reports it on the session (type 17)."""
    is_maker: int
    tag: int
    order_id: int
    book: int
    price: int
    qty: int
    remaining: int


@dataclass
class JournalFill:
    """One entry of an account's fill journal (FillInfo), the same fields bk::WirePort::JFill reads."""
    order_id: int
    price: int
    qty: int
    book: int
    maker: bool
    side: int          # BUY | SELL of the order that filled


def parse_url(url: str) -> tuple[str, int]:
    u = url.strip()
    if u.startswith("tcp://"):
        u = u[6:]
    host, _, port = u.rpartition(":")
    return host or "127.0.0.1", int(port)


class Session:
    """One connection to the exchange. Thread-safe: every public call holds the session lock for
    its whole round trip, so a session may be shared, but the race gives each role its own."""

    def __init__(self, url: str = "tcp://127.0.0.1:7000", timeout: float = 5.0, name: str = ""):
        self.url, self.timeout, self.name = url, timeout, name
        self.host, self.port = parse_url(url)
        self._lock = threading.RLock()
        self._tags = itertools.count(1)
        self._buf = b""
        self.other: list = []            # unsolicited records seen on this session (fills, acks), capped
        self.sock: socket.socket | None = None
        self.connect()

    # ---- transport ---------------------------------------------------------------------------
    def connect(self) -> None:
        with self._lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None
            self._buf = b""
            s = socket.create_connection((self.host, self.port), timeout=self.timeout)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(self.timeout)
            self.sock = s
            self._buf = b""

    def close(self) -> None:
        with self._lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                finally:
                    self.sock = None

    def _send(self, payload: bytes) -> None:
        if self.sock is None:
            self.connect()
        try:
            self.sock.sendall(payload)
        except OSError as e:
            self.sock = None
            raise ExchangeError(f"exchange connection lost: {e}")

    def _record(self, deadline: float) -> bytes:
        """The next whole record from the stream."""
        assert self.sock is not None
        while True:
            if self._buf:
                t = self._buf[0]
                n = SIZES.get(t)
                if n is None:
                    self._buf = b""
                    raise ExchangeError(f"unknown record type {t} on the wire; stream desynchronised")
                if len(self._buf) >= n:
                    rec, self._buf = self._buf[:n], self._buf[n:]
                    return rec
            left = deadline - time.monotonic()
            if left <= 0:
                raise ExchangeError("exchange did not answer")
            self.sock.settimeout(min(left, self.timeout))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                raise ExchangeError("exchange did not answer")
            if not chunk:
                raise ExchangeError("exchange connection closed")
            self._buf += chunk

    def _note_other(self, rec: bytes) -> None:
        if rec[0] == FILL:
            _, mk, tag, oid, book, _pad, px, q, rem = _FILL.unpack(rec)
            self.other.append(Fill(mk, tag, oid, book, px, q, rem))
        elif rec[0] == ACK:
            _, st, tag, oid, rem = _ACK.unpack(rec)
            self.other.append(("ack", st, tag, oid, rem))
        if len(self.other) > 10000:
            del self.other[:5000]

    def _wait_ack(self, tag: int) -> Ack:
        deadline = time.monotonic() + self.timeout
        while True:
            rec = self._record(deadline)
            if rec[0] == ACK:
                _, st, rtag, oid, rem = _ACK.unpack(rec)
                if rtag == tag and not unsolicited(st):
                    return Ack(st, oid, rem)
            self._note_other(rec)

    def _wait_reply(self, tag: int) -> tuple[int, int]:
        deadline = time.monotonic() + self.timeout
        while True:
            rec = self._record(deadline)
            if rec[0] == CONTROL_REPLY:
                _, _op, rtag, arg, arg2 = _CTRL_REPLY.unpack(rec)
                if rtag == tag:
                    return arg, arg2
            self._note_other(rec)

    def poll(self) -> None:
        """Drain anything already queued (fills, unsolicited acks) without blocking."""
        with self._lock:
            assert self.sock is not None
            self.sock.setblocking(False)
            try:
                while True:
                    try:
                        chunk = self.sock.recv(65536)
                    except BlockingIOError:
                        break
                    if not chunk:
                        raise ExchangeError("exchange connection closed")
                    self._buf += chunk
            finally:
                self.sock.settimeout(self.timeout)
            while self._buf and len(self._buf) >= SIZES.get(self._buf[0], 1 << 30):
                n = SIZES[self._buf[0]]
                rec, self._buf = self._buf[:n], self._buf[n:]
                self._note_other(rec)

    # ---- orders ------------------------------------------------------------------------------
    def submit(self, account: int, side: int, otype: int, book: int, qty: int, price: int = 0, trigger: int = 0,
               reduce_only: bool = False, parent: int = 0, oco: int = 0, oco_mode: int = OCO_REDUCE) -> Ack:
        with self._lock:
            tag = next(self._tags) & 0xFFFFFFFF or 1
            self._send(_NEW.pack(NEW_ORDER, account & 0xFF, side, otype, 1 if reduce_only else 0, tag, book,
                                 int(price), int(trigger), int(qty), int(parent), int(oco), oco_mode))
            return self._wait_ack(tag)

    def cancel(self, order_id: int) -> Ack:
        with self._lock:
            tag = next(self._tags) & 0xFFFFFFFF or 1
            self._send(_CANCEL.pack(CANCEL, tag, int(order_id)))
            return self._wait_ack(tag)

    # ---- queries -----------------------------------------------------------------------------
    def control(self, op: int, arg: int = 0, arg2: int = 0) -> tuple[int, int]:
        with self._lock:
            tag = next(self._tags) & 0xFFFFFFFF or 1
            self._send(_CONTROL.pack(CONTROL, op, tag, int(arg), int(arg2)))
            return self._wait_reply(tag)

    def ping(self) -> bool:
        self.control(PING)
        return True

    def last(self, book: int) -> int | None:
        arg, any_ = self.control(LAST_PRICE, book)
        return int(arg) if any_ else None

    def depth_through(self, book: int, side: int, price: int) -> int:
        return self.control(DEPTH_THROUGH, book | (side << 32), price)[0]

    def position(self, account: int, book: int) -> int:
        return self.control(POSITION, account | (book << 32))[0]

    def cash(self, account: int) -> int:
        """The exchange's own cash counter for the account, in price units x lots (ticks x lots)."""
        return self.control(CASH, account)[0]

    def order_state(self, order_id: int) -> str:
        return ORDER_STATE.get(self.control(ORDER_INFO, order_id)[0], "gone")

    def fill_count(self, account: int) -> int:
        return self.control(FILL_COUNT, account)[0]

    def fill_at(self, account: int, i: int) -> JournalFill:
        a1, a2 = self.control(FILL_INFO, account | (i << 8), 0)
        b1, b2 = self.control(FILL_INFO, account | (i << 8), 1)
        return JournalFill(order_id=int(a1), price=int(a2), qty=int(b1), book=int(b2 & 0xFFFFFFFF),
                           maker=bool((b2 >> 32) & 1), side=int((b2 >> 33) & 1))

    def set_mark(self, book: int, price: int) -> None:
        self.control(SET_MARK, book, price)


# ---- the environment: a print, exactly as env.hpp makes one -------------------------------------
def print_one(s: Session, book: int, P: int, mark_mode: bool = False) -> int:
    """A print at P on `book` by the two environment accounts (env.hpp `print_one`): E2 rests
    liquidity at P, E1 sweeps everything resting at or through P with an IOC of n+1 lots so the
    last trade is at P, stops fired on the way fill against E2's liquidity, E2's remainder is
    cancelled. Returns the number of rounds run (1 normally)."""
    with s._lock:
        last = s.last(book)

        def one_pass(down: bool) -> int:
            swept = BUY if down else SELL
            rounds = 0
            while rounds < 4:
                n = s.depth_through(book, swept, P)
                if rounds > 0 and n == 0:
                    break
                liq = s.submit(ENV_LIQUIDITY, swept, LIMIT, book, ENV_DEPTH, price=P)
                s.submit(ENV_SWEEP, SELL if down else BUY, IOC, book, n + 1, price=P)
                if mark_mode:
                    s.set_mark(book, P)
                if liq.status == 0:
                    s.cancel(liq.order_id)
                rounds += 1
            return rounds

        if last is not None and P > last:
            return one_pass(False)
        if last is not None and P < last:
            return one_pass(True)
        return one_pass(True) + one_pass(False)
