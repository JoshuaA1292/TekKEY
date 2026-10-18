"""Restart (B6 reconcile), the repair loop (B9), the owner's phone (B7) and SSE (§10.5)."""
import json
import time

from fastapi.testclient import TestClient

from gateway import app as app_mod
from gateway.db import Database
from gateway.fake_venue import FakeVenue
from gateway.nessie import LocalBank

from .conftest import FIX, H, src
from .test_core import submit


import os
import pytest


@pytest.mark.skipif(os.environ.get("GATEWAY_TEST_VENUE") == "real", reason="the fake venue's persistence; the real restart test is wire-up step 8")
def test_restart_mid_program_loses_no_events(tmp_path, envelope):
    """Deploy, kill the gateway, drive a crash path while it is down, restart: reconcile replays
    the missed halt into the ledger and the bank."""
    db_path, state = str(tmp_path / "g.db"), str(tmp_path / "fake.json")
    venue = FakeVenue(fixtures_dir=FIX, state_path=state, autoplay=False)
    db = Database(db_path)
    app = app_mod.build(db=db, venue=venue, bank=LocalBank(db), start_bus=True)
    with TestClient(app) as client:
        r = client.post("/account", json={"opening_balance_cents": 1_000_000_00}).json()
        acc, ok = r["account"], r["owner_key"]
        client.put(f"/account/{acc}/envelope", json=dict(envelope, account=acc), headers=H(ok))
        ak = client.post(f"/account/{acc}/author-key", headers=H(ok)).json()["author_key"]
        cert = client.post("/program", json={"account": acc, "backend": "minsky", "source": src("ex_v2")}, headers=H(ak)).json()
        pid, bk = cert["program_id"], cert["key"]
        bal_before = client.get(f"/account/{acc}", headers=H(ok)).json()["balance_cents"]
        assert client.post(f"/program/{pid}/deploy", headers=H(bk)).status_code == 200
    # gateway process is "dead": the bus is stopped. The program keeps running inside the venue.
    app.state.bus.stop()
    venue.drive(pid)                        # the crash path happens while nobody is listening
    assert venue.state(pid)["status"] == "halted"
    venue.close()

    # restart: a new venue object from the persisted state, a new gateway over the same DB
    venue2 = FakeVenue(fixtures_dir=FIX, state_path=state, autoplay=False)
    db2 = Database(db_path)
    app2 = app_mod.build(db=db2, venue=venue2, bank=LocalBank(db2), start_bus=True)
    with TestClient(app2) as client:
        p = client.get(f"/program/{pid}", headers=H(ak)).json()
        assert p["status"] == "halted" and p["realized"]["loss_cents"] == 3734
        view = client.get(f"/account/{acc}", headers=H(ok)).json()
        assert view["budget"]["remaining_loss_cents"] == 266
        assert view["balance_cents"] == bal_before - 3734
        assert client.post(f"/program/{pid}/deploy", headers=H(bk)).json()["error"] == "key_spent"
        types = [json.loads(r["payload_json"])["type"] for r in db2.connect().__enter__().execute(
            "SELECT payload_json FROM events WHERE program_id=? ORDER BY id", (pid,))]
        assert types.count("program.halted") == 1
    venue2.close()


def test_reconcile_ignores_programs_this_gateway_never_deployed(client, account, venue):
    """A venue state file from another database must not be applied to our programs by id."""
    cert = submit(client, account, "ex_v2").json()
    pid = cert["program_id"]                                   # certified, never deployed
    gw = client.app.state.gw
    original = venue.reconcile
    venue.reconcile = lambda: [{"program_id": pid, "account": 999, "status": "halted", "realized_loss_cents": 3734,
                                "fees_cents": 134, "trades": 2}]
    try:
        res = gw.reconcile()
    finally:
        venue.reconcile = original
    assert res["applied"] == []
    p = client.get(f"/program/{pid}", headers=H(account["ak"])).json()
    assert p["status"] == "certified" and p["realized"]["loss_cents"] is None
    view = client.get(f"/account/{account['account']}", headers=H(account["ok"])).json()
    assert view["budget"]["remaining_loss_cents"] == 266 and view["frozen"] is False


def test_repair_loop_turns_v1_into_v2(client, account):
    r = submit(client, account, "ex_v1")
    assert r.status_code == 422
    pid = r.json()["program_id"]
    r = client.post(f"/program/{pid}/repair", headers=H(account["ak"]))
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["status"] == "certified" and body["bound"]["max_loss_cents"] <= 4000
    assert 1 <= body["repair"]["rounds"] <= 3
    inc = body["repair"]["incidents"][0]
    assert inc["failed"]["violates"] == "max_loss_cents" and "rests" in inc["failed"]["path"]
    assert any("allow 25" in line for line in inc["changed"])
    # the incident report is attached to the rejected program and in the owner's event stream
    p = client.get(f"/program/{pid}", headers=H(account["ak"])).json()
    assert p["incidents"][0]["round"] == 1
    new = client.get(f"/program/{body['program_id']}", headers=H(account["ak"])).json()
    assert new["parent_program_id"] == pid
    with client.app.state.db.connect() as c:
        n = c.execute("SELECT COUNT(*) FROM events WHERE type='program.repair_round'").fetchone()[0]
    assert n == body["repair"]["rounds"]
    # repair with the wrong key class or on a certified program is refused
    assert client.post(f"/program/{pid}/repair", headers=H(account["ok"])).status_code == 403
    assert client.post(f"/program/{body['program_id']}/repair", headers=H(account["ak"])).json()["error"] == "not_rejected"


def test_rejections_burst_alarm(client, account):
    for _ in range(3):
        assert submit(client, account, "ex_v1").status_code == 422
    with client.app.state.db.connect() as c:
        rows = c.execute("SELECT type FROM events WHERE type='rejections.burst'").fetchall()
    assert len(rows) == 1
    texts = [m["text"] for m in client.get("/photon/outbox").json()]
    assert any("turned agent" in t for t in texts)


def test_phone_one_word_replies_and_voice_envelope(client, envelope):
    r = client.post("/account", json={"owner_phone": "+15550001111"}).json()
    acc, ok = r["account"], r["owner_key"]
    client.put(f"/account/{acc}/envelope", json=dict(envelope, account=acc), headers=H(ok))
    # anything but freeze/status is refused
    r = client.post("/photon/text", json={"from_number": "+15550001111", "body": "raise my limit to $500"}).json()
    assert r["action"] is None
    r = client.post("/photon/text", json={"from_number": "+15550001111", "body": "status"}).json()
    assert r["action"] == "status" and "$40.00 loss budget left" in r["reply"]
    # voice: the envelope is read back in full and only applied after "confirm"
    r = client.post("/photon/voice", json={"from_number": "+15550001111", "session_id": "call-1",
                                           "transcript": "forty dollars a day, gap twenty-five ticks, ETH only, fees five dollars"}).json()
    assert r["state"] == "awaiting_confirm" and r["say"].endswith("confirm?")
    assert "$40 a day" in r["say"] and "gap 25 ticks" in r["say"] and "ETH only" in r["say"]
    assert client.get(f"/account/{acc}", headers=H(ok)).json()["envelope"]["max_fees_cents"] == 500
    r = client.post("/photon/voice", json={"from_number": "+15550001111", "session_id": "call-1", "transcript": "confirm"}).json()
    assert r["state"] == "applied"
    env = client.get(f"/account/{acc}", headers=H(ok)).json()["envelope"]
    assert env["max_loss_cents"] == 4000 and env["env"]["max_gap_ticks"] == 25 and env["max_fees_cents"] == 500
    # freeze by text
    r = client.post("/photon/text", json={"from_number": "+15550001111", "body": "freeze"}).json()
    assert r["action"] == "freeze"
    assert client.get(f"/account/{acc}", headers=H(ok)).json()["frozen"] is True
    # an unknown number can do nothing
    r = client.post("/photon/text", json={"from_number": "+15559999999", "body": "freeze"}).json()
    assert r["action"] is None


def test_agent_offline_alarm(client, account, venue, monkeypatch):
    from gateway import config
    cert = submit(client, account, "ex_v2").json()
    assert client.post(f"/program/{cert['program_id']}/deploy", headers=H(cert["key"])).status_code == 200
    monkeypatch.setattr(config, "AGENT_OFFLINE_T_MS", 0)
    time.sleep(0.01)
    fired = client.app.state.gw.check_agent_offline()
    assert fired == [account["account"]]
    assert client.app.state.gw.check_agent_offline() == []          # alarmed once per silence
    texts = [m["text"] for m in client.get("/photon/outbox").json()]
    assert any("silent" in t for t in texts)


def test_sse_stream_replays_and_is_owner_only(client, account):
    submit(client, account, "ex_v1")
    acc = account["account"]
    assert client.get(f"/account/{acc}/events", headers=H(account["ak"])).status_code == 403
    r = client.get(f"/account/{acc}/events?once=1", headers=H(account["ok"]))
    assert r.status_code == 200 and r.headers["content-type"].startswith("text/event-stream")
    got = [line[7:] for line in r.text.splitlines() if line.startswith("event: ")]
    assert got[0] == "account.created" and "program.rejected" in got
    ids = [int(line[4:]) for line in r.text.splitlines() if line.startswith("id: ")]
    r = client.get(f"/account/{acc}/events?once=1&since={ids[-1]}", headers=H(account["ok"]))
    assert "event:" not in r.text


def test_cancel_with_open_position_settles_through_the_event(client, account, venue):
    """CONTRACT §9 A3: the venue flattens natively; the ledger waits for the fill and the halt."""
    cert = submit(client, account, "ex_v2").json()
    pid, bk = cert["program_id"], cert["key"]
    assert client.post(f"/program/{pid}/deploy", headers=H(bk)).status_code == 200
    venue.drive(pid, steps=3)                 # settle print, print at 3000, entry filled: position 1
    assert venue.state(pid)["position"] == 1
    r = client.post(f"/program/{pid}/cancel", headers=H(bk))
    assert r.status_code == 200 and r.json()["flatten_pending"] is True and r.json()["flatten_fill"] is None
    if venue.name == "real":
        venue.drive(pid, steps=2)             # the live venue flattens on the next print: give it one
    _wait(lambda: client.get(f"/program/{pid}", headers=H(account["ak"])).json()["status"] == "cancelled")
    p = client.get(f"/program/{pid}", headers=H(account["ak"])).json()
    assert p["realized"]["trades"] == 2 and 0 <= p["realized"]["loss_cents"] <= 3734
    view = client.get(f"/account/{account['account']}", headers=H(account["ok"])).json()
    assert view["budget"]["remaining_loss_cents"] == 4000 - p["realized"]["loss_cents"]
    assert client.post(f"/program/{pid}/deploy", headers=H(bk)).json()["error"] == "key_spent"


def test_no_take_profit_is_rejected_on_fees_and_the_droid_adds_one(client, account):
    """CONTRACT §9 A8: without an upper exit the fee bound is unbounded."""
    src_no_take = "instrument ETH\nentry buy 1 at 3000\nsl: exit all at entry - 12 allow 25 when price <= entry - 12\n"
    r = submit(client, account, src_no_take)
    assert r.status_code == 422 and r.json()["violates"] == "max_fees_cents", r.text
    assert r.json()["value_on_path"] > 500 and r.json()["counterexample"][-1]["position"] == 0
    r = client.post(f"/program/{r.json()['program_id']}/repair", headers=H(account["ak"]))
    assert r.status_code == 200 and "take" in r.json()["repair"]["incidents"][-1]["changed"][-1]


def _wait(pred, timeout=5.0):
    import time
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return
        time.sleep(0.05)
    assert pred(), "condition not met in time"
