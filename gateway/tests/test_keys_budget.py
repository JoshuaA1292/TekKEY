"""B2 and B3 unit tests: key hashing and binding; the rolling-window ledger and its invariant."""
import json

import pytest

from gateway import budget, keys
from gateway.db import Database, now_ms
from gateway.errors import ApiError


def test_keys_are_hashed_and_bound(db):
    with db.tx() as c:
        plaintext, kid = keys.issue(c, "ak", account=7)
    assert plaintext.startswith("ak_") and len(plaintext) > 50
    with db.connect() as c:
        row = c.execute("SELECT * FROM keys WHERE id=?", (kid,)).fetchone()
    assert row["hash"] == keys.hash_key(plaintext) and plaintext not in json.dumps(dict(row))
    info = keys.authenticate(db, f"Bearer {plaintext}")
    assert info.cls == "ak" and info.account == 7 and info.state == "active"
    with pytest.raises(ApiError) as e:
        keys.authenticate(db, "Bearer ak_wrong")
    assert e.value.status == 401
    with pytest.raises(ApiError) as e:
        keys.require_owner(info, 7)
    assert e.value.code == "key_class"
    with pytest.raises(ApiError) as e:
        keys.require_author(info, 8)
    assert e.value.code == "key_bound_to_account"


def test_bk_state_machine(db):
    with db.tx() as c:
        bk, kid = keys.issue(c, "bk", account=1, program_id=5)
    info = keys.authenticate(db, f"Bearer {bk}")
    assert info.state == "issued" and info.program_id == 5
    keys.require_program_key(info, 5, ("issued",))
    with pytest.raises(ApiError) as e:
        keys.require_program_key(info, 6, ("issued",))
    assert e.value.code == "key_bound_to_program"
    with db.tx() as c:
        keys.set_state(c, kid, "spent")
    info = keys.authenticate(db, f"Bearer {bk}")
    with pytest.raises(ApiError) as e:
        keys.require_program_key(info, 5, ("issued", "deployed", "paused"))
    assert e.value.code == "key_spent"
    keys.require_program_key(info, 5, ("spent",))          # GET /state only


def test_ledger_window_and_reservations(db, envelope):
    now = now_ms()
    with db.tx() as c:
        c.execute("INSERT INTO accounts(id, bank, frozen, created_t, envelope_json) VALUES (1,'local-sim',0,?,?)", (now, json.dumps(envelope)))
        before, after = budget.check_and_reserve(c, 1, 10, {"max_loss_cents": 3000, "max_fees_cents": 100,
                                                            "halts_within_trades": 2, "max_exposure_lots": 1}, envelope, now)
        assert before.loss == 4000 and after.loss == 1000 and after.exposure == 1
        with pytest.raises(ApiError) as e:
            budget.check_and_reserve(c, 1, 11, {"max_loss_cents": 1001, "max_fees_cents": 1, "halts_within_trades": 1,
                                                "max_exposure_lots": 1}, envelope, now)
        assert e.value.code == "budget" and e.value.extra["remaining_cents"] == 1000
        # halt: realized 2500 replaces the 3000 reservation; profits never replenish
        rem = budget.release_and_realize(c, 1, 10, envelope, realized_loss=2500, fees=50, trades=2, now=now)
        assert rem.loss == 1500 and rem.realized_loss == 2500
        rem2 = budget.release_and_realize(c, 1, 10, envelope, 2500, 50, 2, now)     # idempotent
        assert rem2.loss == 1500
        # the window rolls: a day later the realized loss no longer counts
        later = budget.remaining(c, 1, envelope, now + 24 * 3600 * 1000 + 1)
        assert later.loss == 4000
        # a negative (profitable) realization counts as 0
        budget.check_and_reserve(c, 1, 12, {"max_loss_cents": 1000, "max_fees_cents": 10, "halts_within_trades": 1,
                                            "max_exposure_lots": 1}, envelope, now)
        rem3 = budget.release_and_realize(c, 1, 12, envelope, realized_loss=-700, fees=5, trades=1, now=now)
        assert rem3.realized_loss == 2500 and rem3.loss == 1500


def test_invariant_violation_freezes_account(db, envelope, caplog):
    now = now_ms()
    with db.tx() as c:
        c.execute("INSERT INTO accounts(id, bank, frozen, created_t, envelope_json) VALUES (1,'local-sim',0,?,?)", (now, json.dumps(envelope)))
        budget.check_and_reserve(c, 1, 10, {"max_loss_cents": 3000, "max_fees_cents": 100, "halts_within_trades": 2,
                                            "max_exposure_lots": 1}, envelope, now)
        # the venue reports more than the certificate allowed: a bug in the system, not a user error
        budget.release_and_realize(c, 1, 10, envelope, realized_loss=4500, fees=0, trades=2, now=now)
        frozen = c.execute("SELECT frozen FROM accounts WHERE id=1").fetchone()["frozen"]
        ev = c.execute("SELECT type FROM events WHERE account=1").fetchall()
    assert frozen == 1 and any(r["type"] == "invariant.violated" for r in ev)
    assert "INVARIANT" in caplog.text
