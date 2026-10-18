"""The program-side money, as `venued` keeps it (minsky/src/env.hpp `bk::Ledger` and
`bk::Fees`), in Python, so the raw lane's account is measured with the same arithmetic as the
bounded lane's: fee per fill = ceil(price_cents x qty x bps / 10000), maker 1.0 bp / taker 3.5 bp
by default, average-cost realized loss, loss = -(cash + position x mark) + fees."""
from __future__ import annotations

from dataclasses import dataclass, field


def bps100(bps: float) -> int:
    return int(round(bps * 100))


@dataclass
class Fees:
    maker_bps100: int = 100
    taker_bps100: int = 350

    def fee(self, price_ticks: int, qty: int, tick_cents: int, maker: bool) -> int:
        bps = self.maker_bps100 if maker else self.taker_bps100
        n = abs(int(price_ticks)) * int(tick_cents) * int(qty) * int(bps)
        return (n + 999_999) // 1_000_000


@dataclass
class Ledger:
    tick_cents: int = 100
    fees: Fees = field(default_factory=Fees)
    cash_cents: int = 0
    fees_cents: int = 0
    trades: int = 0
    max_exposure: int = 0
    pos: int = 0
    cost: int = 0          # cents paid for the open position (signed like pos)

    def fill(self, side: int, px: int, q: int, maker: bool) -> int:
        """side: 0 buy, 1 sell. Returns the fee charged."""
        fee = self.fees.fee(px, q, self.tick_cents, maker)
        notional = px * q * self.tick_cents
        d = q if side == 0 else -q
        self.cash_cents += -notional if side == 0 else notional
        self.fees_cents += fee
        self.trades += 1
        p, c = self.pos, self.cost
        if p == 0 or (p > 0) == (d > 0):
            c += d * px * self.tick_cents
            p += d
        else:
            close = min(abs(d), abs(p))
            c -= c * close // abs(p)
            p += d
            if p != 0 and (p > 0) == (d > 0):
                c = p * px * self.tick_cents
            if p == 0:
                c = 0
        self.pos, self.cost = p, c
        self.max_exposure = max(self.max_exposure, abs(p))
        return fee

    def loss(self, mark: int | None) -> int:
        v = self.cash_cents + (self.pos * mark * self.tick_cents if mark is not None else self.cost)
        return -v + self.fees_cents

    def realized_loss(self) -> int:
        return -(self.cash_cents + self.cost) + self.fees_cents

    def avg_cost(self) -> int | None:
        return None if self.pos == 0 else abs(self.cost) // abs(self.pos)
