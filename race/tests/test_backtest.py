"""Backtest mode: the clock, the candle-to-print rule, and no look-ahead on a real venue: every
print the model is told about was released before the round, and the clock is paused while the
models decide."""
from __future__ import annotations

import time

import pytest

from race import backtest as bt
from race.conductor import Conductor, load_config
from race.store import RaceStore
from race.world import World


def test_clock_and_prints():
    c = bt.MarketClock(1_000_000, speed=100.0)
    time.sleep(0.05)
    a = c.now_ms()
    assert 1_000_000 + 3_000 <= a <= 1_000_000 + 20_000
    c.pause(); time.sleep(0.05); b = c.now_ms(); assert b == c.now_ms()
    c.resume(); time.sleep(0.02); assert c.now_ms() > b
    ps = bt.prints_of_candle([1789689600, 2444.72, 2445.98, 2445.28, 2444.79])
    assert [p[1] for p in ps] == [2445.28, 2444.72, 2445.98, 2444.79]        # open, nearer extreme (low), other, close
    assert [p[0] for p in ps] == [1789689600000, 1789689615000, 1789689630000, 1789689645000]
    assert bt.parse_when("2026-09-18") == 1789689600


@pytest.fixture
def backtest(exchange, tmp_path, monkeypatch):
    monkeypatch.setenv("CHALK_SKIP_PROBE", "1")
    monkeypatch.setenv("RACE_ATTACK_EVERY", "2")
    world = World(exchange_url=exchange["url"], venued_port=exchange["venued_port"], db_path=str(tmp_path / "bt.db"))
    cond = Conductor(world, RaceStore(world.db), load_config(), mode="backtest")
    cond.start(run_feed=False, run_rounds=True)
    try:
        yield cond
    finally:
        cond.stop()
        world.close()


def test_no_look_ahead_on_a_real_venue(backtest: Conductor, monkeypatch):
    cond = backtest
    # a synthetic 30-minute window so the test needs no network: a drift up then a drop
    base = 1789689600
    candles = []
    px = 2400.0
    for i in range(30):
        px += 1.0 if i < 20 else -3.0
        candles.append([base + 60 * i, px - 0.6, px + 0.6, px - 0.2, px])
    monkeypatch.setattr(bt, "fetch_window", lambda s, e, **kw: candles)
    monkeypatch.setattr("race.conductor.fetch_window", lambda s, e, **kw: candles)
    info = cond.start_backtest(start=str(base), end=str(base + 1800), speed=300.0, round_minutes=5.0)
    assert info["prints"] == 120 and cond.mode == "backtest"
    t0 = time.time()
    while cond.finished is None and time.time() - t0 < 90:
        time.sleep(0.2)
    assert cond.finished is not None, "the window did not finish"
    assert cond.feed.status()["done"] and cond.clock.paused
    st = cond.state()
    assert st["tape"]["prints"]["n"] == 120
    rounds = cond.store.rounds(cond.epoch, limit=50, full=True)
    assert 4 <= len(rounds) <= 7                                    # a decision every 5 market minutes over 30
    for r in rounds:
        for lane in ("raw", "bounded"):
            x = r[lane]["exchanges"][0]
            # every print mentioned in MARKET lines was released at or before the round's market time
            for e in r["shared"]:
                assert e["t"] <= r["t"]
            # the model's own stamps never run ahead of the round
            assert x["t"] <= r[lane]["ended_t"]
        # the round's market time is a time on the tape, not wall time
        assert base * 1000 <= r["t"] <= (base + 1800) * 1000
        assert r["raw"]["shared_hash"] == r["bounded"]["shared_hash"]
    # the stub traded in both lanes on the replayed day and the money came from the exchange
    assert st["raw"]["trades"] >= 1 and st["raw"]["exchange_check"]["agree"]
    assert st["bounded"]["certified"] >= 1
    assert cond.finished["raw"]["equity_cents"] != cond.cfg["capital_cents"]
    # C1: the round listing carries `id` and the full lane records when asked
    assert all(r["id"] == r["round_id"] and r["raw"]["prompt_system"] for r in rounds)
    # C3: the bounded lane's bank identifiers and escrows, read from the gateway's tables, and the phone outbox
    ids = st["bounded"]["bank_ids"]
    assert ids["bank"] == cond.w.bank.name and ids["customer_id"] and ids["account_id"]
    assert len(ids["escrows"]) >= 1 and all({"program_id", "escrow_account_id", "cents", "settled", "settlement_tx_id"} <= set(e) for e in ids["escrows"])
    assert all(e["cents"] > 0 and (e["settlement_tx_id"] is None or (e["settled"] and e["settlement_tx_id"].startswith("bank_tx:"))) for e in ids["escrows"])
    with cond.w.db.connect() as c:                       # every escrow row is a real program of this account with that escrow
        for e in ids["escrows"]:
            row = c.execute("SELECT account, escrow_account_id FROM programs WHERE id=?", (e["program_id"],)).fetchone()
            assert row["account"] == st["bounded"]["account"] and row["escrow_account_id"] == e["escrow_account_id"]
    assert isinstance(st["outbox"], list) and all({"t", "to_masked", "text", "via", "delivered", "message_id", "event_type"} <= set(m) for m in st["outbox"])
    # the model and the route it takes are recorded on the window, the epoch and the comparison row; no composite label
    assert cond.route == "stub" and st["invariants"]["same_model"] == f"{cond.model} via stub"
    assert cond.backtest["model"] == cond.model and cond.backtest["route"] == "stub" and cond.backtest["eta_s"] > 0
    assert cond.backtest["label"].endswith(f" on {cond.model} via stub")
    from race.conductor import compare_rows
    row = compare_rows(cond, 3)[0]
    assert row["epoch"] == cond.epoch and row["route"] == "stub" and row["model"] == cond.model and row["label"] == cond.backtest["label"]
    assert all(x["llm"]["route"] == "stub" for r in rounds for x in r["raw"]["exchanges"])


def test_start_backtest_with_a_model_records_it(backtest: Conductor, monkeypatch):
    cond = backtest
    base = 1789689600
    candles = [[base + 60 * i, 2399.4, 2400.6, 2399.8, 2400.0] for i in range(5)]
    monkeypatch.setattr("race.conductor.fetch_window", lambda s, e, **kw: candles)
    info = cond.start_backtest(start=str(base), end=str(base + 300), speed=600.0, round_minutes=5.0,
                               label="flash crash hours on {model} via {route}", model="claude-sonnet-5-5")
    assert cond.model == "claude-sonnet-5-5" and info["model"] == "claude-sonnet-5-5" and info["route"] == "stub"
    assert info["label"] == "flash crash hours on claude-sonnet-5-5 via stub" and info["eta_s"] >= 1
    ep = cond.store.epochs(1)[0]
    assert ep["config"]["model"] == "claude-sonnet-5-5" and ep["config"]["route"] == "stub" and ep["config"]["backtest"]["model"] == "claude-sonnet-5-5"
    # the next window without a model goes back to the configured one
    info2 = cond.start_backtest(start=str(base), end=str(base + 300), speed=600.0, round_minutes=5.0)
    assert info2["model"] == cond.cfg["model"] and cond.model == cond.cfg["model"]
    assert info2["label"] == f"{base}..{base + 300} on {cond.cfg['model']} via stub"
