"""T5 Real tape: do real ETH prints ever beat a certificate?

The tape (lab/tape.py) is the latest slice of 60 Coinbase ETH-USD minute closes in ticks. The
live book is first walked to the slice's first print (environment prints only, inside the gap
model), so every program is written and certified against that print as its mark, and the
slice is then played at its real prices: RealVenue `replay --attach` puts the prints on the
live book the deployed program rests in; FakeVenue has no matching engine, so there the tape's
fills are modelled by `lab.tape.TapeBook` and the rows say `fills: modelled`.

Programs: with no model live (route `stub`) a fixed set of 10 simple programs around the mark
(longs and shorts, stops sized to the §5.1 envelope). With a model live, the 50 briefs' clean
programs written by the delegate; the briefs quote absolute prices around 3010, so in that
variant the tape is played offset to 3010 (the gaps, which are what a certificate is about,
are unchanged) and `evidence.offset_ticks` says so.

One program at a time: certify, deploy, play the slice, wait for the halt or cancel at the end
of the slice (the venue flattens on the next print), then compare the ledger's realized loss
with the certified bound. Assumption violations are prints whose gap exceeds the envelope's
`max_gap_ticks`; the slice is still played. Breaches are split into "assumptions held" (no
violation in the slice: must be 0 for pass) and "outside the model" (reported, not a failure).
"""
from __future__ import annotations

from ..context import Context, Result
from ..harness import MARK, fixed_programs, load_briefs, public, run_delegate
from ..tape import TapeBook, prints_of_path


class Test:
    id = "tape"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        tape = ctx.tape
        if tape is None:
            return res.skip("no tape: Coinbase unreachable and nothing cached under lab/data/")
        env = ctx.envelope_40()
        fake = ctx.venue.name != "real"
        use_briefs = ctx.model_live and ctx.param("source", "auto") != "fixed"
        base = MARK if use_briefs else tape.prints[0]
        offset = base - tape.prints[0]
        ctx.log(f"tape: {tape.source}{' (cached)' if tape.cached else ''}, {len(tape.prints)} prints {tape.from_t}..{tape.to_t}, "
                f"first {tape.prints[0]} last {tape.prints[-1]} max gap {tape.max_gap}",
                offset_ticks=offset, venue=ctx.venue.name, fills="venue" if not fake else "modelled (fake venue)")
        ctx.series("price", [[i, p + offset] for i, p in enumerate(tape.prints)])
        ctx.series("gap_ticks", [[i, b - a] for i, (a, b) in enumerate(zip(tape.prints, tape.prints[1:]), 1)])
        walk = ctx.walk_mark_to(base)
        ctx.log(f"book walked to the slice's first print: {walk['from']} -> {walk['to']} in {walk['steps']} moves")

        if use_briefs:
            items = [{"id": b["id"], "brief": b, "envelope": b["envelope"]} for b in load_briefs()[: ctx.param("n", 50)]]
        else:
            items = [dict(p, envelope=env) for p in fixed_programs(base, env, ctx.tick_cents(), n=ctx.param("n", 10))]
        path = tape.path(mark=base)
        counts = {"certified": 0, "deployed": 0, "filled": 0, "held": 0, "outside": 0, "exposed": 0}
        pids = []
        for k, item in enumerate(items, 1):
            gap = int(item["envelope"]["env"]["max_gap_ticks"])
            violations = tape.violations(gap)
            exposed = bool(violations)
            if use_briefs:
                r = run_delegate(ctx, item["brief"], None, mark=base)
                acc = {"account": r["account"], "ok": r["owner_key"], "ak": r["author_key"]}
                row = {"id": item["id"], "program_id": r["program_id"], "http": r["first_http"], "rounds": r["rounds"],
                       "source": r["source"], "max_gap_ticks": gap}
                if not r["certified"]:
                    row.update(status="not certified", filled=False)
                    ctx.row(row)
                    continue
                pid, bk, bound = r["program_id"], r["key"], int(r["bound"]["max_loss_cents"])
            else:
                acc = ctx.new_account(item["envelope"])
                status, body = ctx.submit(acc, item["source"])
                row = {"id": item["id"], "side": item["side"], "entry": item["entry"], "stop_ticks": item["stop_ticks"],
                       "http": status, "program_id": body.get("program_id"), "source": item["source"], "max_gap_ticks": gap}
                if status != 200:
                    row.update(status=body.get("error") or body.get("status"), violates=body.get("violates"), filled=False)
                    res.evidence.setdefault("counterexamples", {})[str(body.get("program_id"))] = body
                    ctx.row(row)
                    ctx.log(f"{item['id']}: not certified ({status} {body.get('violates') or body.get('error')})")
                    continue
                pid, bk, bound = body["program_id"], body["key"], int(body["bound"]["max_loss_cents"])
                res.evidence.setdefault("certificates", {})[str(pid)] = {k_: v for k_, v in body.items() if k_ != "key"}
            counts["certified"] += 1
            pids.append(pid)
            dstatus, dbody = ctx.deploy(pid, bk)
            if dstatus != 200:
                row.update(status=f"deploy {dstatus}", error=dbody.get("error"), filled=False)
                ctx.row(row)
                continue
            counts["deployed"] += 1
            play = path
            if fake:
                prog = ctx.program(pid, acc["ak"])
                book = TapeBook(ctx.tick_cents(), item["envelope"]["env"]["fee_bps"]["maker"], item["envelope"]["env"]["fee_bps"]["taker"])
                play = book.run_program(prog["compiled"], path)
            out = ctx.drive(pid, play) or {}
            p, how = ctx.ended_or_settle(pid, bk, acc["ak"])
            ended_by = "halt" if how == "halt" else "cancel at end of slice"
            realized = int(p["realized"]["loss_cents"] or 0)
            trades = int(p["realized"]["trades"] or 0)
            breach = realized > bound
            if trades:
                counts["filled"] += 1
            if exposed:
                counts["exposed"] += 1
            if breach:
                counts["outside" if exposed else "held"] += 1
            row.update(status=p["status"], ended_by=ended_by, bound_cents=bound, realized_cents=realized,
                       fees_cents=p["realized"]["fees_cents"], trades=trades, filled=trades > 0, exposed=exposed,
                       violations=len(violations), breach=breach,
                       breach_class=("outside the model" if exposed else "assumptions held") if breach else None,
                       fills="venue" if not fake else "modelled", replay_position=out.get("position") if isinstance(out, dict) else None)
            ctx.row(row)
            ctx.series("realized_cents", [[k, realized]])
            ctx.log(f"{item['id']} ({pid}): bound {bound}c, {trades} trade(s), realized {realized}c, {p['status']} by {ended_by}"
                    + (" [exposed to a gap outside the model]" if exposed else "") + (" BREACH" if breach else ""))
            back = ctx.walk_mark_to(base)
            if back["steps"]:
                ctx.log(f"book walked back {back['from']} -> {back['to']} for the next program")

        viol_env = tape.violations(int(env["env"]["max_gap_ticks"]))
        ctx.metric("programs", len(items))
        ctx.metric("certified", counts["certified"])
        ctx.metric("deployed", counts["deployed"])
        ctx.metric("filled", counts["filled"])
        ctx.metric("breaches_assumptions_held", counts["held"])
        ctx.metric("breaches_outside_model", counts["outside"])
        ctx.metric("programs_exposed_to_violation", counts["exposed"])
        ctx.metric("assumption_violations", len(viol_env))
        ctx.metric("max_gap_ticks", int(env["env"]["max_gap_ticks"]))
        ctx.metric("max_gap_seen", tape.max_gap)
        ctx.metric("tape_source", tape.source)
        ctx.metric("tape_cached", tape.cached)
        ctx.metric("fills", "venue" if not fake else "modelled by lab.tape.TapeBook (fake venue)")
        res.evidence.update({"tape": {"instrument": tape.instrument, "from": tape.from_t, "to": tape.to_t, "source": tape.source,
                                      "cached": tape.cached, "file": tape.file, "n": len(tape.prints)},
                             "offset_ticks": offset, "violations": viol_env, "program_ids": pids, "walk": walk,
                             "played_prints": prints_of_path(path)})
        if tape.cached:
            res.note = "tape: cached"
        ok = counts["held"] == 0 and counts["deployed"] > 0
        return res.verdict("breaches (assumptions held)", counts["held"], f"of {counts['deployed']} programs", ok)
