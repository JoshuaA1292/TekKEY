"""T7 Unplugged: if the agent dies mid-program, who protects the position?

Live only with a real Daytona sandbox (`DAYTONA_API_KEY`) and a gateway URL the sandbox can
reach (`LAB_PUBLIC_URL` or `GATEWAY_URL`). `agent.sandbox.LocalSandbox` is a local subprocess
with a scrubbed environment; killing it is not unplugging a real sandbox, so without the key the
run is `skipped` with the reason and nothing is faked.

When live: `SandboxManager.create` builds the delegate image on Daytona with the two-host egress
policy, `run_delegate` executes `python -m agent.delegate` inside it against the public gateway
URL (the delegate holds the author key only), the certified program is deployed with its bk_
key, the sandbox is DELETED, and only then is the certificate's worst path driven on the venue.
The program must halt within its bound with nobody connected (venued state, the ledger). The
owner's phone: the gateway's Photon adapter sends `program.halted`; its outbox entry carries
`delivered` and `via`, and the headline is the delivery latency in ms, or "phone not live".
"""
from __future__ import annotations

import os
import time
import uuid

from gateway.db import now_ms

from ..context import Context, Result
from ..harness import MARK, load_briefs


class Test:
    id = "unplugged"

    def run(self, ctx: Context) -> Result:
        res = ctx.result
        if not os.environ.get("DAYTONA_API_KEY"):
            res.note = "LocalSandbox (a local subprocess) does not count as unplugging a real sandbox"
            return res.skip("needs DAYTONA_API_KEY")
        gateway_url = os.environ.get("LAB_PUBLIC_URL") or os.environ.get("GATEWAY_URL")
        if not gateway_url:
            return res.skip("needs LAB_PUBLIC_URL (a gateway URL the Daytona sandbox can reach)")
        try:
            from agent.sandbox import SandboxManager
        except ImportError as e:
            return res.skip(f"agent.sandbox unavailable: {e}")
        manager = SandboxManager()
        if manager.backend != "daytona":
            return res.skip("sandbox backend is local, not Daytona")
        brief = load_briefs()[ctx.param("brief", 0)]
        if abs(ctx.mark() - MARK) > 100:
            ctx.walk_mark_to(MARK)
        phone = os.environ.get("PHOTON_OWNER_NUMBER") or None
        acc = ctx.new_account(brief["envelope"], owner_phone=phone)
        chalk_url = os.environ.get("CHALK_AI_GATEWAY_URL") or None
        delegate_id = f"lab-{uuid.uuid4().hex[:8]}"
        t0 = time.perf_counter()
        try:
            handle = manager.create(delegate_id, acc["ak"], gateway_url, chalk_url)
        except Exception as e:
            return res.skip(f"Daytona refused to create the sandbox: {type(e).__name__}: {str(e)[:160]}")
        ctx.log("sandbox created on Daytona", sandbox_id=handle.sandbox_id, egress=getattr(handle, "egress", "?"),
                create_ms=int((time.perf_counter() - t0) * 1000))
        res.evidence["sandbox"] = {"id": handle.sandbox_id, "egress": getattr(handle, "egress", None)}
        ok, detail = manager.probe_reachability(handle, gateway_url.rstrip("/") + "/healthz")
        ctx.log("egress probe from inside the sandbox", target=gateway_url, ok=ok, detail=detail)
        res.evidence["egress_probe"] = {"target": gateway_url, "ok": ok, "detail": detail}
        if not ok:
            manager.delete(handle)
            res.note = ("Daytona tiers 1 and 2 allow outbound traffic to essential services only (package registries, git "
                        "hosts, model providers) and refuse sandbox-level allow lists; the gateway is not reachable from "
                        "the sandbox, so the delegate cannot submit from inside it. Needs Daytona tier 3, or a gateway "
                        "hosted on a domain Daytona allows.")
            return res.skip(f"Daytona tier policy blocks egress to the gateway ({detail})")
        try:
            result = manager.run_delegate(handle, brief["brief"], account=acc["account"], envelope=acc["envelope"],
                                          timeout=ctx.param("timeout_s", 600))
        except Exception as e:
            manager.delete(handle)
            ctx.log("delegate failed inside the sandbox", error=repr(e))
            return res.verdict("alarm delivered", None, "ms", False)
        pid, key = result.get("program_id"), result.get("key")
        ctx.log("delegate finished inside the sandbox", status=result.get("status"), program_id=pid, attempts=result.get("attempts"))
        if result.get("status") != "certified" or not key:
            manager.delete(handle)
            ctx.row({"program_id": pid, "status": result.get("status"), "sandbox": handle.sandbox_id, "note": "no certificate to deploy"})
            ctx.metric("certified", False)
            return res.verdict("alarm delivered", None, "ms", False)
        prog = ctx.program(pid, acc["ak"])
        res.evidence.setdefault("certificates", {})[str(pid)] = prog.get("result")
        bound = int(prog["result"]["bound"]["max_loss_cents"])
        dstatus, dbody = ctx.deploy(pid, key)
        deployed_t = now_ms()
        ctx.log("program deployed with its bk_ key", http=dstatus, sub_account=dbody.get("sub_account"))
        t1 = time.perf_counter()
        manager.delete(handle)
        ctx.log("sandbox DELETED: nobody is connected", delete_ms=int((time.perf_counter() - t1) * 1000))
        ctx.drive(pid)                                  # the worst path, with the agent gone
        p = ctx.wait_ended(pid, acc["ak"], timeout_s=20)
        halted_t = now_ms()
        realized = int(p["realized"]["loss_cents"] or 0)
        venue_state = ctx.venue.state(pid)
        receipt = None
        if ctx.photon is not None:
            for m in list(ctx.photon.outbox):
                if m.get("event_type") == "program.halted" and int(m.get("t", 0)) >= deployed_t and str(pid) in str(m.get("text")):
                    receipt = m
        delivered = bool(receipt and receipt.get("delivered"))
        alarm_ms = (int(receipt["t"]) - int(p.get("ended_t") or halted_t)) if receipt else None
        ctx.row({"program_id": pid, "sandbox": handle.sandbox_id, "backend": handle.backend, "bound_cents": bound,
                 "realized_cents": realized, "status": p["status"], "venue_status": venue_state.get("status"),
                 "phone": receipt.get("via") if receipt else None, "delivered": delivered, "alarm_ms": alarm_ms})
        ctx.metric("certified", True)
        ctx.metric("realized_loss_cents", realized)
        ctx.metric("bound_cents", bound)
        ctx.metric("halted_unattended", p["status"] in ("halted", "cancelled"))
        ctx.metric("photon_delivered", delivered)
        ctx.metric("photon_via", receipt.get("via") if receipt else None)
        ctx.metric("photon_receipt_ms", alarm_ms if delivered else None)
        res.evidence.update({"program_ids": [pid], "sandbox_id": handle.sandbox_id, "photon_receipt_ms": alarm_ms if delivered else None,
                             "photon_outbox_entry": receipt})
        ok = p["status"] in ("halted", "cancelled") and realized <= bound
        if delivered:
            return res.verdict("alarm delivered", alarm_ms, "ms", ok)
        return res.verdict("alarm delivered", "phone not live", "", ok)
