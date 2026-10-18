"""The ordinary-key client against a real exchange: prints move the last price the way the
environment's prints do, a resting order fills at its own price, the journal reports it, and the
Python ledger agrees with venued's accounting on the worked example's worst path (CONTRACT §11:
3734 cents) to the cent."""
from __future__ import annotations

import json
from pathlib import Path

from gateway.venue import RealVenue

from race import exchange_client as xc
from race.ledger import Fees, Ledger

ROOT = Path(__file__).resolve().parents[2]
FIX = ROOT / "fixtures"


def test_fee_formula_matches_contract_examples():
    f = Fees()
    assert f.fee(2964, 1, 100, maker=False) == 104      # BOUNDED_KEYS §11.2: exit fee at 2964, 3.5 bp
    assert f.fee(3000, 1, 100, maker=True) == 30        # entry fee at 3000, 1 bp
    assert f.fee(13429, 1, 100, maker=False) == 471     # CONTRACT A8: the flatten at 13,429 pays $4.71


def test_print_and_resting_fill(exchange):
    env = xc.Session(exchange["url"], name="env")
    raw = xc.Session(exchange["url"], name="raw")
    book = 0
    assert env.last(book) is None
    xc.print_one(env, book, 3010)
    assert env.last(book) == 3010
    base = raw.fill_count(250)
    # the raw account rests a bid below the print; a down print through it fills it at its own price
    a = raw.submit(250, xc.BUY, xc.LIMIT, book, 2, price=3000)
    assert a.name == "rested"
    xc.print_one(env, book, 3005)
    assert raw.order_state(a.order_id) == "resting"
    xc.print_one(env, book, 2999)
    assert env.last(book) == 2999
    assert raw.order_state(a.order_id) == "gone"
    assert raw.position(250, book) == 2
    n = raw.fill_count(250)
    assert n == base + 1
    jf = raw.fill_at(250, n - 1)
    assert (jf.order_id, jf.price, jf.qty, jf.maker, jf.side) == (a.order_id, 3000, 2, True, xc.BUY)
    led = Ledger(100)
    led.fill(jf.side, jf.price, jf.qty, jf.maker)
    assert led.pos == 2 and led.cash_cents == -600_000 and led.fees_cents == 60
    assert led.loss(2999) == 200 + 60            # 2 lots x $1 adverse + the maker fee
    # a market order is an OCO pair that fills at the next print (the venue's own flatten shape)
    oco = 0x60000001
    s1 = raw.submit(250, xc.SELL, xc.STOP_MARKET, book, 2, trigger=2998, reduce_only=True, oco=oco)
    s2 = raw.submit(250, xc.SELL, xc.LIMIT, book, 2, price=2999, reduce_only=True, oco=oco)
    assert s1.ok and s2.ok
    xc.print_one(env, book, 3001)
    assert raw.position(250, book) == 0
    assert raw.order_state(s1.order_id) == "gone" and raw.order_state(s2.order_id) == "gone"
    env.close()
    raw.close()


def test_ledger_matches_venued_on_the_worked_example(exchange):
    """Deploy ex_v2 through venued, play its certified worst path with our prints, and compare
    venued's state (the venue's accounting) with our ledger over the same journal."""
    env = xc.Session(exchange["url"], name="env")
    ctl = xc.Session(exchange["url"], name="ctl")
    venue = RealVenue(port=exchange["venued_port"])
    compiled = json.loads((FIX / "ex_v2.compiled.json").read_text())
    result = json.loads((FIX / "ex_v2.result.json").read_text())
    path = result["worst_path"]
    xc.print_one(env, 0, 3010)                            # the live book has a print before deployment (A11)
    dep = venue.deploy(1, 1, compiled, 0)
    sub = int(dep["sub_account"])
    base = ctl.fill_count(sub)
    for step in path:
        if step.get("env") == "print":
            xc.print_one(env, 0, int(step["at"]))
        elif step.get("env") == "gap":
            xc.print_one(env, 0, int(step["to"]))
    st = venue.state(1)
    led = Ledger(100)
    for i in range(base, ctl.fill_count(sub)):
        f = ctl.fill_at(sub, i)
        led.fill(f.side, f.price, f.qty, f.maker)
    last = ctl.last(0)
    assert st["status"] == "halted"
    assert led.trades == st["trades"] == 2
    assert led.fees_cents == st["fees_cents"]
    assert led.realized_loss() == st["realized_loss_cents"] == result["bound"]["max_loss_cents"] == 3734
    assert led.loss(last) == st["mtm_loss_cents"]
    env.close()
    ctl.close()
