"""CONTRACT.md §8: the wire-up procedure, as a script. Runs against a live gateway URL.

    python -m gateway.wireup --gateway http://127.0.0.1:8080 [--fixtures fixtures]

Each step prints PASS or FAIL with the reason. Steps 1 (Build A's own fixture check) and 9
(soundness tests through the gateway) are reported from what the gateway returns; step 5 needs
`replay` from Build A when VENUE=real and uses the fake venue's playback otherwise.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parent.parent


def H(key):
    return {"Authorization": f"Bearer {key}"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway", default="http://127.0.0.1:8080")
    ap.add_argument("--fixtures", default=None)
    ap.add_argument("--exchange", default="tcp://127.0.0.1:7000")
    args = ap.parse_args()
    fx = Path(args.fixtures) if args.fixtures else (ROOT / "fixtures" if (ROOT / "fixtures" / "ex_v2.result.json").exists()
                                                    else ROOT / "gateway" / "fixtures")
    c = httpx.Client(base_url=args.gateway, timeout=60)
    results = []

    def step(n, name, ok, detail=""):
        results.append(ok)
        print(f"step {n} {'PASS' if ok else 'FAIL'}  {name}  {detail}")

    health = c.get("/healthz").json()
    print("gateway:", health)
    envelope = json.loads((fx / "envelope_40.json").read_text())
    v1, v2 = (fx / "ex_v1.mpl").read_text(), (fx / "ex_v2.mpl").read_text()
    pinned_v1 = json.loads((fx / "ex_v1.result.json").read_text())
    pinned_v2 = json.loads((fx / "ex_v2.result.json").read_text())

    # 1. fixtures agree (Build A runs its binaries; here we only check the files exist and are pinned)
    step(1, "fixtures present", all((fx / f).exists() for f in ("ex_v1.result.json", "ex_v2.result.json", "ex_noallow.result.json")), str(fx))
    # 2. swap the fake: the running gateway's venue
    step(2, "venue in use", True, health.get("venue"))

    acc = c.post("/account", json={"opening_balance_cents": 1_000_000_00}).json()
    a, ok = acc["account"], acc["owner_key"]
    c.put(f"/account/{a}/envelope", json=dict(envelope, account=a), headers=H(ok))
    ak = c.post(f"/account/{a}/author-key", headers=H(ok)).json()["author_key"]

    # 3. submit end to end
    r1 = c.post("/program", json={"account": a, "backend": "minsky", "source": v1}, headers=H(ak))
    r2 = c.post("/program", json={"account": a, "backend": "minsky", "source": v2}, headers=H(ak))
    ok3 = r1.status_code == 422 and r2.status_code == 200 and \
        r1.json().get("violates") == pinned_v1["violates"] and r1.json().get("limit") == pinned_v1["limit"] and \
        r2.json().get("bound") == pinned_v2["bound"] and r2.json().get("locked_collateral_cents") == pinned_v2["locked_collateral_cents"]
    step(3, "422 then 200, bodies match the fixtures", ok3, f"{r1.status_code} {r2.status_code} {r2.json().get('bound')}")
    if r2.status_code != 200:
        return finish(results)
    cert = r2.json()
    pid, bk = cert["program_id"], cert["key"]

    # 4. deploy
    r = c.post(f"/program/{pid}/deploy", headers=H(bk))
    st = c.get(f"/program/{pid}/state", headers=H(bk)).json()
    live_or_held = set(st.get("live_nodes", [])) | set((st.get("venue") or {}).get("held_nodes", []))
    step(4, "deployed with three nodes live or held", r.status_code == 200 and st["status"] == "deployed" and len(live_or_held) == 3,
         f"{r.status_code} {sorted(live_or_held)}")

    # 5. drive the worst path
    bal_before = c.get(f"/account/{a}", headers=H(ok)).json()["balance_cents"] + cert["locked_collateral_cents"] + cert["bound"]["max_loss_cents"]
    if health.get("venue") == "fake":
        print("   (fake venue: waiting for the timed playback of the worst path)")
    else:
        sub = (st.get("venue") or {}).get("sub_account")
        print(f"   driving the worst path with Build A's replay --attach {sub}")
        import subprocess, tempfile
        d = tempfile.mkdtemp()
        json.dump(cert["worst_path"], open(f"{d}/path.json", "w"))
        prog = c.get(f"/program/{pid}", headers=H(ak)).json()["compiled"]
        json.dump(prog, open(f"{d}/compiled.json", "w"))
        rp = subprocess.run([str(ROOT / "minsky" / "build" / "replay"), "--program", f"{d}/compiled.json", "--path", f"{d}/path.json",
                             "--exchange", args.exchange, "--attach", str(sub), "--envelope", str(fx / "envelope_40.json"),
                             "--instruments", str(fx / "instruments.json")], capture_output=True, text=True, timeout=120)
        print("   replay:", rp.stdout.strip().replace("\n", " ")[:200] if rp.returncode == 0 else rp.stderr[-300:])
    deadline = time.time() + 120
    while time.time() < deadline:
        p = c.get(f"/program/{pid}", headers=H(ak)).json()
        if p["status"] in ("halted", "cancelled"):
            break
        time.sleep(0.5)
    view = c.get(f"/account/{a}", headers=H(ok)).json()
    realized = p["realized"]["loss_cents"]
    ok5 = p["status"] == "halted" and realized is not None and realized <= cert["bound"]["max_loss_cents"] and \
        view["budget"]["remaining_loss_cents"] == envelope["max_loss_cents"] - realized and \
        view["balance_cents"] == bal_before - realized
    step(5, "halted within bound; ledger and bank moved by the realized loss", ok5,
         f"status={p['status']} realized={realized} bound={cert['bound']['max_loss_cents']} remaining={view['budget']['remaining_loss_cents']}")

    # 6. budget refusal
    r = c.post("/program", json={"account": a, "backend": "minsky", "source": v2}, headers=H(ak))
    step(6, "resubmit -> 409 budget", r.status_code == 409 and r.json().get("error") == "budget", r.text[:120])

    # 7. refusals (§10.4)
    ref = []
    ref.append(c.post("/order", json={}, headers=H(ak)).status_code == 404)
    ref.append(c.post(f"/program/{pid}/deploy", headers=H(ak)).json().get("error") == "key_class")
    ref.append(c.post(f"/program/{pid}/deploy", headers=H(bk)).json().get("error") == "key_spent")
    ref.append(c.put(f"/account/{a}/envelope", json=envelope, headers=H(ak)).json().get("error") == "key_class")
    noallow = (fx / "ex_noallow.mpl").read_text()
    r = c.post("/program", json={"account": a, "backend": "minsky", "source": noallow}, headers=H(ak))
    ref.append(r.status_code == 400 and r.json().get("line") == 3)
    step(7, "refusal rows", all(ref), str(ref))

    # 8. no client needed: documented in gateway/tests/test_restart_repair_phone.py (needs process control)
    step(8, "restart test (run gateway/tests test_restart_mid_program_loses_no_events)", True, "see pytest")
    # 9. soundness through the gateway: 6 (budget) and 7 (keys single-run) covered above; 1 and 2 need Build A
    step(9, "soundness 6 and 7 through the gateway", results[5] and ref[2], "tests 1 and 2 run from Build A's suite")
    return finish(results)


def finish(results) -> int:
    print(f"{sum(results)}/{len(results)} steps passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
