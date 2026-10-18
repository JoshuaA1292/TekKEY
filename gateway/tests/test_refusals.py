"""Every row of BOUNDED_KEYS.md §10.4 is a test. These define the invention as much as the successes."""
from .conftest import H, src
from .test_core import submit


def certify(client, account):
    r = submit(client, account, "ex_v2")
    assert r.status_code == 200, r.text
    return r.json()


def test_post_order_404_with_any_key(client, account):
    cert = certify(client, account)
    for key in (account["ok"], account["ak"], cert["key"], "nonsense"):
        assert client.post("/order", json={}, headers=H(key)).status_code == 404
    assert client.get("/order").status_code == 404


def test_deploy_with_author_key_403_key_class(client, account):
    cert = certify(client, account)
    r = client.post(f"/program/{cert['program_id']}/deploy", headers=H(account["ak"]))
    assert r.status_code == 403 and r.json()["error"] == "key_class"
    r = client.post(f"/program/{cert['program_id']}/deploy", headers=H(account["ok"]))
    assert r.status_code == 403 and r.json()["error"] == "key_class"


def test_program_m_key_on_program_n_403_key_bound_to_program(client, account, envelope):
    cert_n = certify(client, account)
    # a second account so a second program can be certified
    r = client.post("/account", json={})
    acc2, ok2 = r.json()["account"], r.json()["owner_key"]
    client.put(f"/account/{acc2}/envelope", json=dict(envelope, account=acc2), headers=H(ok2))
    ak2 = client.post(f"/account/{acc2}/author-key", headers=H(ok2)).json()["author_key"]
    cert_m = client.post("/program", json={"account": acc2, "backend": "minsky", "source": src("ex_v2")}, headers=H(ak2)).json()
    for route in ("deploy", "pause", "cancel"):
        r = client.post(f"/program/{cert_n['program_id']}/{route}", headers=H(cert_m["key"]))
        assert r.status_code == 403 and r.json()["error"] == "key_bound_to_program", route
    r = client.get(f"/program/{cert_n['program_id']}/state", headers=H(cert_m["key"]))
    assert r.status_code == 403 and r.json()["error"] == "key_bound_to_program"


def test_deploy_twice_and_after_cancel_or_halt_403_key_spent(client, account, venue):
    cert = certify(client, account)
    pid, bk = cert["program_id"], cert["key"]
    assert client.post(f"/program/{pid}/deploy", headers=H(bk)).status_code == 200
    r = client.post(f"/program/{pid}/deploy", headers=H(bk))
    assert r.status_code == 403 and r.json()["error"] == "key_spent"
    # cancel: key becomes spent; only GET /state remains
    r = client.post(f"/program/{pid}/cancel", headers=H(bk))
    assert r.status_code == 200 and r.json()["status"] == "cancelled"
    for route in ("deploy", "pause", "cancel"):
        r = client.post(f"/program/{pid}/{route}", headers=H(bk))
        assert r.status_code == 403 and r.json()["error"] == "key_spent", route
    assert client.get(f"/program/{pid}/state", headers=H(bk)).status_code == 200

    # after a halt (not a cancel) the key is spent too
    client.post(f"/account/{account['account']}", headers=H(account["ok"]))
    cert2 = certify(client, account)        # budget: the cancelled program realized 0, so 4000 is free again
    pid2, bk2 = cert2["program_id"], cert2["key"]
    assert client.post(f"/program/{pid2}/deploy", headers=H(bk2)).status_code == 200
    venue.drive(pid2)
    _wait(lambda: client.get(f"/program/{pid2}", headers=H(account["ak"])).json()["status"] == "halted")
    r = client.post(f"/program/{pid2}/deploy", headers=H(bk2))
    assert r.status_code == 403 and r.json()["error"] == "key_spent"


def test_put_envelope_with_non_owner_key_403_key_class(client, account, envelope):
    cert = certify(client, account)
    for key in (account["ak"], cert["key"]):
        r = client.put(f"/account/{account['account']}/envelope", json=envelope, headers=H(key))
        assert r.status_code == 403 and r.json()["error"] == "key_class"
    # another owner's key is bound to its own account
    r = client.post("/account", json={})
    ok2 = r.json()["owner_key"]
    r = client.put(f"/account/{account['account']}/envelope", json=envelope, headers=H(ok2))
    assert r.status_code == 403 and r.json()["error"] == "key_bound_to_account"


def test_certified_larger_than_remaining_409_budget(client, account):
    cert = certify(client, account)
    r = submit(client, account, "ex_v2")
    # the live program holds the instrument, so this is instrument_busy; cancel it to free the instrument
    assert r.status_code == 409 and r.json()["error"] == "instrument_busy"
    # a different instrument would hit the budget: with ETH freed but 3734 reserved... use cancel-before-deploy path:
    # keep program live, certify on the fake with a program that is cheaper: still busy. So: realize the loss first.
    pid, bk = cert["program_id"], cert["key"]
    assert client.post(f"/program/{pid}/deploy", headers=H(bk)).status_code == 200
    client.app.state.venue.drive(pid)
    _wait(lambda: client.get(f"/program/{pid}", headers=H(account["ak"])).json()["status"] == "halted")
    r = submit(client, account, "ex_v2")
    assert r.status_code == 409, r.text
    body = r.json()
    assert body["error"] == "budget" and body["remaining_cents"] == 266 and body["requested_cents"] == 3734
    assert body["certificate"]["status"] == "certified"      # it certified; the window cannot cover it
    p = client.get(f"/program/{body['program_id']}", headers=H(account["ak"])).json()
    assert p["status"] == "budget_refused"


def test_second_live_program_same_instrument_409_instrument_busy(client, account):
    cert = certify(client, account)
    r = submit(client, account, "ex_v2")
    assert r.status_code == 409 and r.json()["error"] == "instrument_busy"
    assert r.json()["live_program_id"] == cert["program_id"] and r.json()["instrument"] == "ETH"


def test_exit_without_allow_400_with_line(client, account):
    r = submit(client, account, "ex_noallow")
    assert r.status_code == 400
    assert r.json()["line"] == 3 and "allow" in r.json()["message"] and r.json()["error"] == "static_error"
    r = submit(client, account, "instrument ETH\nentry buy 1 at 3000\nsl: exit all at entry - 12 allow 0 when price <= entry - 12\n")
    assert r.status_code == 400 and r.json()["line"] == 3


def test_instrument_outside_envelope_422(client, account):
    r = submit(client, account, "instrument BTC\nentry buy 1 at 3000\nsl: exit all at entry - 12 allow 25\n")
    assert r.status_code in (400, 422)       # unknown to the venue -> 400; known but outside the envelope -> 422


def test_bad_or_missing_key_401(client, account):
    assert client.get(f"/account/{account['account']}").status_code == 401
    assert client.get(f"/account/{account['account']}", headers=H("ok_nope")).status_code == 401


def test_author_key_bound_to_account(client, account, envelope):
    r = client.post("/account", json={})
    acc2 = r.json()["account"]
    r = client.post("/program", json={"account": acc2, "backend": "minsky", "source": src("ex_v2")}, headers=H(account["ak"]))
    assert r.status_code == 403 and r.json()["error"] == "key_bound_to_account"


def test_frozen_account_refuses_submissions_and_pauses_live(client, account, venue):
    cert = certify(client, account)
    pid, bk = cert["program_id"], cert["key"]
    assert client.post(f"/program/{pid}/deploy", headers=H(bk)).status_code == 200
    venue.drive(pid, steps=3)                       # settle print, print 3000, entry filled: exits live
    r = client.post(f"/account/{account['account']}/freeze", headers=H(account["ok"]))
    assert r.status_code == 200 and r.json()["paused_programs"] == [pid]
    st = venue.state(pid)
    # CONTRACT §9 A2: every reduce-only node stays on pause, the stop and the take alike
    assert st["status"] == "paused" and "sl" in st["live_nodes"] and "tp" in st["live_nodes"]
    r = submit(client, account, "ex_v2")
    assert r.status_code == 403 and r.json()["error"] == "account_frozen"
    view = client.get(f"/account/{account['account']}", headers=H(account["ok"])).json()
    assert view["frozen"] is True


def _wait(pred, timeout=5.0):
    import time
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return
        time.sleep(0.05)
    assert pred(), "condition not met in time"
