"""T2 First pass: can a model write a program that certifies, unaided?

50 briefs (evals/data/briefs.json) go through `agent.delegate.Delegate` with the run's LLM
route, one fresh account per brief with the brief's envelope. The delegate holds an author key
only; every submission is a real `POST /program` and a `422` hands off to the gateway's repair
route (the Droid, up to 3 rounds). Everything reported is read back from the gateway's
`programs` table: whether the FIRST program certified (first pass), how many repair rounds sit
behind the final certificate (the parent_program_id chain), and the bound's tightness
(certificate max_loss_cents / envelope max_loss_cents).

The oracle is `verify`; pass means every brief got a definitive answer from the gateway with no
harness error. On the stub route the run finishes `completed`, not `passed`: the stub is a
rule-based stand-in and its numbers are not evidence about any model.
"""
from __future__ import annotations

import statistics

from ..context import Context, Result
from ..harness import MARK, load_briefs, public, run_delegate


class Test:
    id = "firstpass"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        briefs = load_briefs()[: ctx.param("n", 50)]
        if abs(ctx.mark() - MARK) > 100:
            w = ctx.walk_mark_to(MARK)
            ctx.log(f"book walked to the briefs' mark: {w['from']} -> {w['to']}")
        mark = ctx.mark()
        ctx.log(f"{len(briefs)} briefs through the delegate", model=ctx.model, route=ctx.route, mark=mark, venue=ctx.venue.name)
        rows = []
        for i, b in enumerate(briefs, 1):
            r = run_delegate(ctx, b, None, mark=mark)
            r["tags"] = b.get("tags")
            r["envelope_max_loss_cents"] = b["envelope"]["max_loss_cents"]
            r["tightness"] = (r["bound"]["max_loss_cents"] / b["envelope"]["max_loss_cents"]) if r["bound"] else None
            r["bound_cents"] = r["bound"]["max_loss_cents"] if r["bound"] else None
            r["http"] = r["first_http"]
            r["outcome"] = "certified first pass" if r["first_http"] == 200 else (
                f"certified after {r['rounds']} droid round(s)" if r["certified"] else (r["first_status"] or r["delegate_status"]))
            rows.append(r)
            ctx.row(public(r))
            if r["program_id"] is not None:
                prog = ctx.program(r["program_id"], r["author_key"])
                key = "certificates" if r["certified"] else "counterexamples"
                res.evidence.setdefault(key, {})[str(r["program_id"])] = prog.get("result")
            if r["tightness"] is not None:
                ctx.series("tightness", [[i, round(r["tightness"], 4)]])
            if r["rounds"] is not None:
                ctx.series("droid_rounds", [[i, r["rounds"]]])
            ctx.log(f"{b['id']}: first {r['first_http']}, {r['outcome']}, bound {r['bound_cents']}c"
                    + (f" (error: {r['error']})" if r["error"] else ""))
        n = len(rows)
        first = [r for r in rows if r["first_http"] == 200]
        cert = [r for r in rows if r["certified"]]
        rounds = [r["rounds"] for r in cert]
        tight = [r["tightness"] for r in cert]
        errors = [r for r in rows if r["error"] or r["first_http"] is None]   # the model never answered (transport): not a verdict on the brief
        answered = [r for r in rows if r not in errors]
        dist: dict[str, int] = {}
        for x in rounds:
            dist[str(x)] = dist.get(str(x), 0) + 1
        ctx.metric("n_briefs", n)
        ctx.metric("n_first_pass", len(first))
        ctx.metric("n_answered", len(answered))
        ctx.metric("certification_rate", (len(first) / len(answered)) if answered else 0.0)   # over briefs the model answered
        ctx.metric("n_certified_final", len(cert))
        ctx.metric("droid_rounds_mean", statistics.mean(rounds) if rounds else None)
        ctx.metric("droid_rounds_median", statistics.median(rounds) if rounds else None)
        ctx.metric("droid_rounds_distribution", dict(sorted(dist.items())))
        ctx.metric("bound_tightness_mean", statistics.mean(tight) if tight else None)
        ctx.metric("bound_tightness_min", min(tight) if tight else None)
        ctx.metric("bound_tightness_max", max(tight) if tight else None)
        ctx.metric("final_status_counts", _counts(r["delegate_status"] for r in rows))
        ctx.metric("harness_errors", len(errors))
        ctx.metric("harness_error_reasons", [str(r["error"])[:160] for r in errors][:10])
        ctx.metric("llm", rows[0]["llm"] if rows else None)
        res.evidence.update({"program_ids": [r["program_id"] for r in rows if r["program_id"] is not None],
                             "dataset": "evals/data/briefs.json", "mark": mark})
        pct = round(100.0 * len(first) / len(answered), 1) if answered else 0.0
        if errors:
            res.note = (f"{len(errors)} of {n} briefs never reached the model (provider error, see metrics.harness_error_reasons) and are "
                        f"excluded from the rate; the run passes only if at most 10% were lost")
        if ctx.stub:
            res.status = "completed"
            res.note = "stub route: the rule-based stand-in never reads a brief as a model would; not evidence about any model"
            return res.verdict("certified first pass", pct, "%", None)
        return res.verdict("certified first pass", pct, "%", len(errors) <= max(0, n // 10))


def _counts(it) -> dict:
    out: dict[str, int] = {}
    for x in it:
        out[str(x)] = out.get(str(x), 0) + 1
    return out
