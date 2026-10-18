"""T8 Leaked key: what can an attacker do with a stolen program key?

A program is certified on an account with the §5.1 envelope (fixtures/ex_v2.mpl re-based to the
mark), and its `bk_` key is handed to an attacker routine that knows every route in
BOUNDED_KEYS.md §10 (plus `POST /order`, which does not exist) and tries each one with the key,
recording the status code. Then it deploys (the one thing the key can do), tries to deploy
again, pauses, tries everything again, and after the program has ended tries everything again.

Max damage: between the deploy and the pause the worst path is driven on the venue (the entry
fills, then the gap through the stop), so the realized loss the ledger records is the most the
key could ever cost: the certified bound. Pass iff the only successful mutations are deploy
(once), pause and cancel on that one program, every other call is refused with its §10.4 code
(GET /program/{id}/state is read-only and allowed), and the realized loss <= the bound.

What is real: every call is a real HTTP request against the gateway's routes with the real key;
the loss is the venue's. The attacker's second program (to test `key_bound_to_program`) is
certified on a second account.
"""
from __future__ import annotations

import json

from ..context import Context, Result
from ..harness import rebased_ex_v2

ALLOWED_MUTATIONS = {("POST", "deploy"), ("POST", "pause"), ("POST", "cancel")}
# BOUNDED_KEYS.md §10.1: `POST /account` takes no key at all (anyone can open an account and gets its
# own owner key). With a stolen bk_ key it creates a fresh, unrelated account and touches nothing of
# the victim's, so it is recorded as an open route, not as damage.
OPEN_ROUTES = {("POST", "/account")}


class Test:
    id = "leak"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        env = ctx.envelope_40()
        victim = ctx.new_account(env)
        other = ctx.new_account(env)
        mark = ctx.mark()
        status, cert = ctx.submit(victim, rebased_ex_v2(mark))
        if status != 200:
            ctx.log("the victim's program did not certify", http=status, body=str(cert)[:300])
            return res.verdict("max damage", None, "cents", False)
        pid, stolen, bound = cert["program_id"], cert["key"], int(cert["bound"]["max_loss_cents"])
        res.evidence.setdefault("certificates", {})[str(pid)] = {k: v for k, v in cert.items() if k != "key"}
        status2, cert2 = ctx.submit(other, rebased_ex_v2(mark))
        other_pid = cert2.get("program_id") if status2 == 200 else None
        ctx.log("program certified; its bk_ key is now in the attacker's hands", program_id=pid, bound_cents=bound,
                other_program_id=other_pid)
        H = ctx.H(stolen)
        a = victim["account"]
        matrix = [
            ("POST", "/account", "create account", None),
            ("PUT", f"/account/{a}/envelope", "set envelope", dict(env, max_loss_cents=10_000_00)),
            ("POST", f"/account/{a}/author-key", "mint author key", None),
            ("POST", f"/account/{a}/freeze", "freeze", None),
            ("GET", f"/account/{a}", "read account", None),
            ("GET", f"/account/{a}/events?once=1", "owner events", None),
            ("POST", "/program", "submit a program", {"account": a, "backend": "minsky", "source": rebased_ex_v2(mark)}),
            ("GET", f"/program/{pid}", "read program", None),
            ("GET", f"/program/{pid}/counterexample", "read counterexample", None),
            ("POST", f"/program/{pid}/repair", "repair", None),
            ("POST", "/order", "place an order", {"instrument": "ETH", "side": "buy", "qty": 100, "price": mark}),
            ("GET", f"/program/{pid}/state", "state", None),
        ]
        if other_pid is not None:
            matrix.insert(0, ("POST", f"/program/{other_pid}/deploy", "deploy another program", None))
            matrix.insert(1, ("POST", f"/program/{other_pid}/cancel", "cancel another program", None))
        unexpected = []

        def attempt(phase: str, method: str, path: str, what: str, body=None) -> dict:
            r = ctx.http.request(method, path, json=body, headers=H)
            try:
                j = r.json()
            except ValueError:
                j = {}
            open_route = (method, path.split("?")[0]) in OPEN_ROUTES
            allowed = method == "GET" or open_route or (
                (method, path.rsplit("/", 1)[-1]) in ALLOWED_MUTATIONS and f"/program/{pid}/" in path)
            row = {"phase": phase, "method": method, "path": path.split("?")[0], "what": what, "status": r.status_code,
                   "error": j.get("error") if isinstance(j, dict) else None, "mutation": method != "GET" and not open_route,
                   "succeeded": r.status_code < 300, "allowed": allowed, "open_route": open_route,
                   "server_error": r.status_code >= 500}
            if open_route and r.status_code < 300:
                row["note"] = "unauthenticated by design (§10.1): a fresh account for the caller, nothing of the victim's"
            if r.status_code < 300 and not allowed:
                unexpected.append(row)
            ctx.row(row)
            return row

        def sweep(phase: str):
            for m, p, w, b in matrix:
                attempt(phase, m, p, w, b)

        sweep("before deploy")
        d1 = attempt("deploy", "POST", f"/program/{pid}/deploy", "deploy (the key's one power)")
        d2 = attempt("deploy again", "POST", f"/program/{pid}/deploy", "deploy a second time")
        # max damage: the worst path. First the entry fills (3 moves: settle print, gap, fill)...
        ctx.drive(pid, steps=3)
        pz = attempt("pause", "POST", f"/program/{pid}/pause", "pause (exits stay)")
        sweep("after pause")
        # ...then the gap through the stop: the program halts with the realized loss the certificate bounded.
        ctx.drive(pid)
        prog = ctx.wait_ended(pid, victim["ak"], timeout_s=15)
        if prog["status"] not in ("halted", "cancelled"):
            prog = ctx.settle(pid, stolen, victim["ak"])
        c1 = attempt("after halt", "POST", f"/program/{pid}/cancel", "cancel after the program ended")
        sweep("after halt")
        d3 = attempt("after halt", "POST", f"/program/{pid}/deploy", "deploy after halt")
        realized = int(prog["realized"]["loss_cents"] or 0)
        view = ctx.account_view(victim)
        successes = [r for r in res.rows if r["succeeded"] and r["mutation"]]
        ctx.metric("calls", len(res.rows))
        ctx.metric("successful_mutations", [f"{r['phase']}: {r['method']} {r['path']} -> {r['status']}" for r in successes])
        ctx.metric("unexpected_successes", len(unexpected))
        ctx.metric("open_routes_used", sorted({f"{r['method']} {r['path']}" for r in res.rows if r.get("open_route") and r["succeeded"]}))
        ctx.metric("server_errors", [f"{r['phase']}: {r['method']} {r['path']} -> {r['status']}" for r in res.rows if r.get("server_error")])
        ctx.metric("codes", sorted({f"{r['status']} {r['error'] or ''}".strip() for r in res.rows}))
        ctx.metric("deploy_once", d1["status"] == 200 and d2["status"] == 403 and d2["error"] == "key_spent")
        ctx.metric("pause", pz["status"])
        ctx.metric("post_order", next((r["status"] for r in res.rows if r["path"] == "/order"), None))
        ctx.metric("realized_loss_cents", realized)
        ctx.metric("certified_bound_cents", bound)
        ctx.metric("damage_equals_bound", realized == bound)
        ctx.metric("final_status", prog["status"])
        ctx.metric("remaining_cents", view["budget"]["remaining_loss_cents"])
        ctx.metric("frozen", view.get("frozen"))
        ctx.metric("envelope_intact", int(view["envelope"]["max_loss_cents"]) == int(env["max_loss_cents"]))
        res.evidence.update({"program_ids": [pid] + ([other_pid] if other_pid else []), "account": a,
                             "refusals_table": "BOUNDED_KEYS.md §10.4"})
        ok = (not unexpected and d1["status"] == 200 and d2["status"] == 403 and d3["status"] == 403 and
              realized <= bound and prog["status"] in ("halted", "cancelled") and not view.get("frozen")
              and int(view["envelope"]["max_loss_cents"]) == int(env["max_loss_cents"]))
        ctx.log(f"max damage {realized}c against a certified bound of {bound}c; {len(successes)} successful mutations, "
                f"{len(unexpected)} unexpected", successes=[f"{r['method']} {r['path']}" for r in successes])
        return res.verdict("max damage", realized, f"cents of {bound} certified", ok)
