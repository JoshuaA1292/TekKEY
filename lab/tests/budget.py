"""T6 The window: can a turned delegate drain the account one certified program at a time?

BOUNDED_KEYS.md §8.5 test 6, through the gateway. One account with the §5.1 envelope
(fixtures/envelope_40.json: $40 per rolling 24 h window). The program is fixtures/ex_v2.mpl, the
worked example that certifies at $37.34, with its entry re-based to the current mark (at the
fixture's mark of 3010 it is the fixture byte for byte).

Loop: POST /program (certify) -> deploy with the bk_ key -> drive the certificate's worst path
on the venue (RealVenue: `replay --attach`; FakeVenue: scripted playback) -> wait for
program.halted through the ledger -> certify again. The loop ends when the gateway answers
`409 budget`. Pass iff the realized total in the window (read from GET /account) never exceeds
`max_loss_cents` and the 409 arrives (within 8 programs).

What is real: the certificate, the key, the deploy, the halt, the ledger and the bank movement.
Each program's loss is whatever the venue realized on its worst path, which is at most the bound.
"""
from __future__ import annotations

from ..context import Context, Result
from ..harness import rebased_ex_v2

MAX_PROGRAMS = 8


class Test:
    id = "budget"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        env = ctx.envelope_40()
        acc = ctx.new_account(env)
        max_loss = int(env["max_loss_cents"])
        balance0 = ctx.balance(acc)
        ctx.log("account opened", account=acc["account"], max_loss_cents=max_loss, bank=ctx.bank.name,
                balance_cents=balance0, venue=ctx.venue.name)
        realized_total = 0
        got_409 = None
        n_certified = 0
        cumulative = []
        pids = []
        for i in range(1, MAX_PROGRAMS + 1):
            mark = ctx.mark()
            src = rebased_ex_v2(mark)
            status, body = ctx.submit(acc, src)
            if status == 409 and body.get("error") == "budget":
                got_409 = {"remaining_cents": body.get("remaining_cents"), "requested_cents": body.get("requested_cents"),
                           "program_id": body.get("program_id")}
                ctx.row({"i": i, "mark": mark, "http": 409, "error": "budget", "remaining_cents": body.get("remaining_cents"),
                         "requested_cents": body.get("requested_cents"), "program_id": body.get("program_id"),
                         "certified_bound_cents": (body.get("certificate") or {}).get("bound", {}).get("max_loss_cents")})
                ctx.log(f"program {i}: 409 budget, remaining {body.get('remaining_cents')} < requested {body.get('requested_cents')}")
                if body.get("program_id") is not None:
                    pids.append(body["program_id"])
                break
            if status != 200:
                ctx.row({"i": i, "mark": mark, "http": status, "error": body.get("error") or body.get("status"), "body": body})
                ctx.log(f"program {i}: unexpected {status}", body=str(body)[:300])
                break
            pid, bk, bound = body["program_id"], body["key"], body["bound"]["max_loss_cents"]
            pids.append(pid)
            res.evidence.setdefault("certificates", {})[str(pid)] = {k: v for k, v in body.items() if k != "key"}
            n_certified += 1
            dstatus, dbody = ctx.deploy(pid, bk)
            if dstatus != 200:
                ctx.row({"i": i, "program_id": pid, "http": 200, "deploy": dstatus, "error": dbody.get("error")})
                ctx.log(f"program {i}: deploy failed {dstatus}", body=dbody)
                break
            ctx.drive(pid)                                    # the certificate's worst path
            p = ctx.wait_ended(pid, acc["ak"], timeout_s=15)
            if p["status"] not in ("halted", "cancelled"):
                p = ctx.settle(pid, bk, acc["ak"])
            realized = int(p["realized"]["loss_cents"] or 0)
            realized_total += realized
            cumulative.append([i, realized_total])
            view = ctx.account_view(acc)
            ctx.row({"i": i, "mark": mark, "http": 200, "program_id": pid, "bound_cents": bound, "deploy": dstatus,
                     "final_status": p["status"], "realized_cents": realized, "fees_cents": p["realized"]["fees_cents"],
                     "trades": p["realized"]["trades"], "realized_in_window_cents": view["budget"]["realized_loss_in_window_cents"],
                     "remaining_cents": view["budget"]["remaining_loss_cents"], "balance_cents": view.get("balance_cents")})
            ctx.log(f"program {i} ({pid}): certified {bound}c, halted {p['status']} with realized {realized}c; "
                    f"window {view['budget']['realized_loss_in_window_cents']}c of {max_loss}c, remaining {view['budget']['remaining_loss_cents']}c")
        view = ctx.account_view(acc)
        in_window = int(view["budget"]["realized_loss_in_window_cents"])
        balance1 = ctx.balance(acc)
        ctx.metric("programs_certified", n_certified)
        ctx.metric("programs_until_409", n_certified if got_409 else None)
        ctx.metric("realized_total_cents", realized_total)
        ctx.metric("realized_in_window_cents", in_window)
        ctx.metric("max_loss_cents", max_loss)
        ctx.metric("remaining_cents", view["budget"]["remaining_loss_cents"])
        ctx.metric("got_409", got_409 is not None)
        ctx.metric("refused", got_409)
        ctx.metric("bank", ctx.bank.name)
        ctx.metric("bank_delta_cents", (balance1 - balance0) if balance0 is not None and balance1 is not None else None)
        ctx.metric("frozen", view.get("frozen"))
        ctx.series("realized_cumulative_cents", cumulative)
        res.evidence.update({"account": acc["account"], "program_ids": pids, "envelope": env})
        ok = got_409 is not None and in_window <= max_loss and realized_total <= max_loss and not view.get("frozen")
        return res.verdict("programs until 409", n_certified, f"program{'s' if n_certified != 1 else ''}", ok)
