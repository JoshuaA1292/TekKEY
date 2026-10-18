"""The milestone that cannot slip: ex_v1 -> 422, ex_v2 -> 200, resubmit -> 409, all on the fake."""
import json
import threading

from .conftest import H, src


def submit(client, acc, name_or_src):
    s = src(name_or_src) if "\n" not in name_or_src else name_or_src
    return client.post("/program", json={"account": acc["account"], "backend": "minsky", "source": s}, headers=H(acc["ak"]))


def test_v1_rejected_v2_certified_resubmit_409(client, account):
    r = submit(client, account, "ex_v1")
    assert r.status_code == 422, r.text
    body = r.json()
    assert body["status"] == "rejected" and body["violates"] == "max_loss_cents"
    assert body["limit"] == 4000 and body["value_on_path"] == 4030
    assert body["counterexample"][-1] == {"event": "mark", "position": 1, "loss_cents": 4030}

    r = submit(client, account, "ex_v2")
    assert r.status_code == 200, r.text
    cert = r.json()
    assert cert["status"] == "certified"
    assert cert["bound"] == {"max_loss_cents": 3734, "max_fees_cents": 136, "max_exposure_lots": 1, "halts_within_trades": 2}
    assert cert["key"].startswith("bk_")
    assert cert["budget"] == {"remaining_before_cents": 4000, "remaining_after_cents": 266,
                              "remaining_fees_after_cents": 364, "remaining_trades_after": 58}
    assert cert["locked_collateral_cents"] == 300000

    r = submit(client, account, "ex_v2")
    assert r.status_code == 409, r.text
    assert r.json()["error"] in ("budget", "instrument_busy")


def test_static_error_400_with_line(client, account):
    r = submit(client, account, "ex_noallow")
    assert r.status_code == 400
    assert r.json()["status"] == "static_error" and r.json()["line"] == 3
    assert "allow" in r.json()["message"]


def test_no_order_route(client, account):
    for key in (account["ok"], account["ak"]):
        r = client.post("/order", json={"side": "buy", "qty": 1, "price": 3000}, headers=H(key))
        assert r.status_code == 404


def test_full_lifecycle_budget_and_bank(client, account, venue, app):
    acc = account["account"]
    gw = app.state.gw
    before_bal = client.get(f"/account/{acc}", headers=H(account["ok"])).json()["balance_cents"]
    cert = submit(client, account, "ex_v2").json()
    pid, bk = cert["program_id"], cert["key"]

    r = client.post(f"/program/{pid}/deploy", headers=H(bk))
    assert r.status_code == 200, r.text
    assert r.json()["fund_cents"] == 300000 + 3734
    assert set(r.json()["order_ids"]) == {"entry", "sl", "tp"}
    st = client.get(f"/program/{pid}/state", headers=H(bk)).json()
    assert st["status"] == "deployed" and "entry" in st["live_nodes"]     # venued may list held children too

    # drive the worst path and let the bus apply the halt
    venue.drive(pid)
    _wait(lambda: client.get(f"/program/{pid}", headers=H(account["ak"])).json()["status"] == "halted")
    p = client.get(f"/program/{pid}", headers=H(account["ak"])).json()
    assert p["realized"] == {"loss_cents": 3734, "fees_cents": 134, "trades": 2}

    view = client.get(f"/account/{acc}", headers=H(account["ok"])).json()
    assert view["budget"]["remaining_loss_cents"] == 4000 - 3734 == 266
    assert view["balance_cents"] == before_bal - 3734            # Nessie moved by exactly the realized loss
    assert view["live_programs"] == []

    # the turned delegate resubmits: certified at 3734 but only 266 left
    r = submit(client, account, "ex_v2")
    assert r.status_code == 409 and r.json()["error"] == "budget"
    assert r.json()["remaining_cents"] == 266 and r.json()["requested_cents"] == 3734
    types = [e["type"] for e in view["alarms"]]
    assert "budget.low" in types and "program.halted" in types

    # key is spent
    r = client.post(f"/program/{pid}/deploy", headers=H(bk))
    assert r.status_code == 403 and r.json()["error"] == "key_spent"
    assert client.get(f"/program/{pid}/state", headers=H(bk)).status_code == 200


def test_concurrent_submissions_cannot_both_fit(client, account):
    """Two simultaneous submissions against a budget that holds one: exactly one certifies."""
    results = []

    def go():
        results.append(submit(client, account, "ex_v2").status_code)

    ts = [threading.Thread(target=go) for _ in range(4)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert sorted(results) == [200, 409, 409, 409], results


def _wait(pred, timeout=5.0):
    import time
    t0 = time.time()
    while time.time() - t0 < timeout:
        if pred():
            return
        time.sleep(0.05)
    assert pred(), "condition not met in time"
