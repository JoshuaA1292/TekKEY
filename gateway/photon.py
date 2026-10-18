"""B7: the owner's phone. Alarms out; `freeze` / `status` in; the envelope by voice with a full
read-back and a spoken "confirm" before anything is written.

Photon 2.0 Spectrum is reached through the Node sidecar in phone/ (spectrum-ts on the project's
iMessage/SMS line): when PHOTON_PROJECT_ID is set, `send()` POSTs `{to, text, call}` to
PHONE_SIDECAR_URL (default http://127.0.0.1:8787) and records `via: "photon"` only when the sidecar
reports the message was accepted by Spectrum. Without PHOTON_PROJECT_ID, or when the sidecar is
down, outbound messages are logged and kept in memory (`outbox`, via "log") so the demo and the
tests can see what the phone would have received; nothing is reported as delivered. Voice calls:
Spectrum voice is SIP only and the SDK has no call control yet, so `call=True` sends the text and
the record carries `voice: "unsupported"` from the sidecar. Inbound messages arrive from the
sidecar on the webhook routes mounted in app.py under /photon.
The phone can only make the account *safer*: by text it accepts exactly `freeze` and `status`.
"""
from __future__ import annotations

import json
import logging
import os
import re
from collections import deque

import httpx

from .photon_beta import PhotonBeta, PhotonError, is_beta_project

from chalk import ai_gateway

from . import config
from .db import now_ms
from .errors import ApiError
from .service import Gateway

log = logging.getLogger("gateway.photon")

ALARM_TYPES = {"budget.low", "agent.offline", "rejections.burst", "program.halted", "invariant.violated"}
CALL_TYPES = {"rejections.burst", "invariant.violated"}          # these ring; the rest are texts

ENVELOPE_SYSTEM = """You turn an account owner's spoken risk limits into an envelope JSON. Return ONLY a ```json
block with keys max_loss_cents, max_fees_cents, max_trades, max_exposure_lots, instruments (list), require_live_stop,
env {max_gap_ticks, fill: "full_at_print", fee_bps: {maker: 1.0, taker: 3.5}, trigger_ref: "last"}. Dollars become
cents. Keep any limit the owner did not mention at its current value from the context."""


PHONE_SIDECAR_URL = os.environ.get("PHONE_SIDECAR_URL", "http://127.0.0.1:8787")
PHOTON_PROJECT_ID = os.environ.get("PHOTON_PROJECT_ID", "")


class Photon:
    def __init__(self, gw: Gateway, sidecar_url: str | None = None, project_id: str | None = None,
                 http: httpx.Client | None = None):
        self.gw = gw
        self.sidecar_url = (sidecar_url if sidecar_url is not None else PHONE_SIDECAR_URL).rstrip("/")
        self.project_id = project_id if project_id is not None else PHOTON_PROJECT_ID
        self.outbox: deque = deque(maxlen=200)
        self.http = http or httpx.Client(timeout=10)
        # Photon 2.0 Beta projects (pho_prj_...) are driven directly over REST (gateway/photon_beta.py);
        # stable projects go through the spectrum-ts sidecar in phone/.
        self.beta = None
        self.line: dict = {}
        self.webhook_secret: str | None = None
        secret = os.environ.get("PHOTON_PROJECT_SECRET", "")
        if is_beta_project(self.project_id) and secret and http is None:
            self.beta = PhotonBeta(self.project_id, secret)
        gw.listeners.append(self.on_event)

    def setup(self, public_url: str | None, owner_number: str | None) -> dict:
        """Assign the project's shared line to the owner's handle and point a webhook at this gateway."""
        info = {"beta": bool(self.beta), "line": None, "webhook": None}
        if not self.beta:
            return info
        try:
            if owner_number:
                info["line"] = self.beta.ensure_line(owner_number)
                self.line = {"owner": owner_number, "agent_number": info["line"]}
            if public_url:
                wh = self.beta.ensure_webhook(public_url.rstrip("/") + "/photon/webhook")
                key = f"photon_webhook_secret:{wh.get('destinationId')}"
                secret = wh.get("signingSecret")
                with self.gw.db.connect() as c:
                    row = c.execute("SELECT v FROM meta WHERE k=?", (key,)).fetchone()
                    if not secret and row:
                        secret = row["v"]
                    if not secret and wh.get("destinationId"):
                        try:
                            secret = self.beta.rotate_webhook_secret(wh["destinationId"])
                        except PhotonError as e:
                            log.warning("photon: could not rotate the webhook secret: %s", e)
                    if secret and not row:
                        c.execute("INSERT OR REPLACE INTO meta(k, v) VALUES (?, ?)", (key, secret))
                self.webhook_secret = secret
                info["webhook"] = {k: v for k, v in wh.items() if k != "signingSecret"}
                info["webhook"]["signing_secret_held"] = bool(secret)
        except PhotonError as e:
            info["error"] = str(e)
            log.error("photon setup: %s", e)
        return info

    def handle_webhook(self, raw_body: bytes, headers: dict) -> dict:
        """Photon Beta webhook: a message.received from the owner becomes a one-word command."""
        if not self.beta:
            return {"ok": False, "error": "not a beta project"}
        verified = self.beta.verify_signature(self.webhook_secret, raw_body, headers)
        try:
            event = json.loads(raw_body.decode() or "{}")
        except ValueError:
            return {"ok": False, "error": "bad json"}
        parsed = self.beta.parse_event(event)
        if parsed is None:
            return {"ok": True, "ignored": event.get("type")}
        self.beta.remember_inbound(parsed["message"])
        self._last_inbound = parsed
        res = self.handle_text(parsed["handle"] or "", parsed["text"])
        return {"ok": True, "signature_verified": verified, "handle": parsed["handle"], "action": res.get("action"),
                "reply": res.get("reply"), "delivered": res.get("delivered"), "via": res.get("via")}

    @property
    def live(self) -> bool:
        """True when outbound goes to Photon (Beta REST or the Spectrum sidecar) rather than the log."""
        return bool(self.project_id)

    # ---- outbound ------------------------------------------------------------------
    def on_event(self, ev: dict) -> None:
        if ev.get("type") not in ALARM_TYPES:
            return
        account = ev.get("account")
        phone = self._owner_phone(account)
        text = self.render(ev)
        self.send(phone, text, call=ev["type"] in CALL_TYPES, event=ev)

    def render(self, ev: dict) -> str:
        t = ev["type"]
        a = ev.get("account")
        if t == "budget.low":
            return f"Account {a}: budget low. ${ev['remaining_cents']/100:.2f} of ${ev['max_loss_cents']/100:.2f} left today. Reply freeze to stop new programs."
        if t == "agent.offline":
            return f"Account {a}: your agent has been silent for {ev['silent_ms']//1000}s with a live program. The program keeps running inside the venue."
        if t == "rejections.burst":
            return f"Account {a}: {ev['count']} rejected programs from one author key in a minute. This looks like a turned agent. Reply freeze."
        if t == "program.halted":
            return (f"Account {a}: program {ev.get('program_id')} ended ({ev.get('status')}). Realized loss "
                    f"${ev['realized_loss_cents']/100:.2f} of the ${ (ev.get('bound_max_loss_cents') or 0)/100:.2f} certified bound, "
                    f"fees ${ev['fees_cents']/100:.2f}, {ev['trades']} trades.")
        if t == "invariant.violated":
            return f"Account {a}: LEDGER INVARIANT VIOLATED. The account is frozen. This is a bug in the gateway, not your agent."
        return json.dumps(ev)

    def send(self, to: str | None, text: str, call: bool = False, event: dict | None = None) -> dict:
        """Deliver one message. `via` is "photon" only when the sidecar says Spectrum accepted it;
        otherwise "log", and `delivered` stays False. The record is kept in `outbox` either way."""
        msg = {"to": to, "text": text, "call": call, "t": now_ms(), "event_type": (event or {}).get("type"),
               "delivered": False, "via": "log"}
        if self.beta and to:
            try:
                sent = self.beta.send_text(to, text)
                msg["delivered"] = True            # accepted for asynchronous dispatch by Photon
                msg["via"] = "photon-beta"
                msg["message_id"] = sent.get("id")
                if call:
                    msg["voice"] = "unsupported: Photon Beta has no outbound call API yet; sent as text"
            except (PhotonError, httpx.HTTPError) as e:
                msg["error"] = str(e)[:200]
                log.warning("photon beta send failed: %s", e)
        elif self.live and to:
            try:
                r = self.http.post(f"{self.sidecar_url}/send", json={"to": to, "text": text, "call": call})
                body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
                if r.status_code < 300 and body.get("delivered"):
                    msg["delivered"] = True
                    msg["via"] = "photon"
                    msg["message_id"] = body.get("message_id")
                else:
                    msg["error"] = body.get("error") or f"sidecar {r.status_code}"
                    log.warning("photon send not delivered (%s): %s", r.status_code, (body.get("error") or r.text)[:200])
                if call:
                    msg["voice"] = body.get("voice") or "unsupported"
            except (httpx.HTTPError, ValueError) as e:
                msg["error"] = f"sidecar unreachable: {e}"
                log.warning("photon sidecar unreachable at %s: %s", self.sidecar_url, e)
        if msg["via"] == "log":
            log.info("PHONE %s -> %s: %s", "CALL" if call else "TEXT", to or "<no number>", text)
        self.outbox.append(msg)
        return msg

    # ---- inbound: one-word replies ---------------------------------------------------
    def handle_text(self, from_number: str, body: str) -> dict:
        account = self._account_for(from_number)
        if account is None:
            return {"reply": "Unknown number.", "action": None}
        word = (body or "").strip().lower()
        if word == "freeze":
            res = self.gw.freeze(account)
            reply = f"Frozen. {len(res['paused_programs'])} live program(s) paused down to their exits. New submissions are refused."
            sent = self.send(from_number, reply)
            return {"reply": reply, "action": "freeze", "result": res, "delivered": sent["delivered"], "via": sent["via"]}
        if word == "status":
            v = self.gw.account_view(account)
            b = v.get("budget") or {}
            reply = (f"Account {account}: {'FROZEN. ' if v['frozen'] else ''}{len(v['live_programs'])} live, "
                     f"${(b.get('remaining_loss_cents') or 0)/100:.2f} loss budget left, balance "
                     f"${(v.get('balance_cents') or 0)/100:.2f}.")
            sent = self.send(from_number, reply)
            return {"reply": reply, "action": "status", "view": v, "delivered": sent["delivered"], "via": sent["via"]}
        reply = "Only 'freeze' and 'status' are accepted by text."
        sent = self.send(from_number, reply)
        return {"reply": reply, "action": None, "delivered": sent["delivered"], "via": sent["via"]}

    # ---- inbound: envelope by voice ------------------------------------------------------
    def handle_voice(self, from_number: str, transcript: str, session_id: str) -> dict:
        account = self._account_for(from_number)
        if account is None:
            return {"say": "I don't recognise this number.", "state": "rejected"}
        words = (transcript or "").strip().lower()
        with self.gw.db.connect() as c:
            row = c.execute("SELECT pending_envelope_json FROM phone_sessions WHERE session_id=?", (session_id,)).fetchone()
        pending = json.loads(row["pending_envelope_json"]) if row and row["pending_envelope_json"] else None
        if pending and re.fullmatch(r"(yes[, ]*)?(confirm|confirmed|yes)[.!]?", words):
            stored = self.gw.set_envelope(account, pending)
            self._set_pending(session_id, account, None)
            return {"say": "Confirmed. " + self.read_back(stored) + " is now the envelope.", "state": "applied", "envelope": stored}
        if pending and re.fullmatch(r"(no|cancel|stop|never mind)[.!]?", words):
            self._set_pending(session_id, account, None)
            return {"say": "Cancelled. Nothing changed.", "state": "cancelled"}
        current = self.gw.account_view(account).get("envelope") or {}
        env = self.parse_envelope(transcript, current)
        self._set_pending(session_id, account, env)
        return {"say": self.read_back(env) + " — confirm?", "state": "awaiting_confirm", "envelope": env}

    def parse_envelope(self, transcript: str, current: dict) -> dict:
        ctx = {"transcript": transcript, "current": current}
        user = "TASK: envelope from speech.\n<context>\n" + json.dumps(ctx) + "\n</context>\nReturn only the JSON."
        out = ai_gateway.get_llm().complete(ENVELOPE_SYSTEM, user, max_tokens=800)
        try:
            env = json.loads(ai_gateway.extract_json(out.text))
        except ValueError:
            raise ApiError(422, "could_not_parse_envelope", text=out.text[:300])
        env.setdefault("env", current.get("env") or {"max_gap_ticks": 25, "fill": "full_at_print",
                                                       "fee_bps": {"maker": 1.0, "taker": 3.5}, "trigger_ref": "last"})
        for k in ("max_loss_cents", "max_fees_cents", "max_trades", "max_exposure_lots", "instruments"):
            env.setdefault(k, current.get(k))
            if env[k] is None:
                raise ApiError(422, "could_not_parse_envelope", missing=k)
        env.setdefault("require_live_stop", True)
        return env

    @staticmethod
    def read_back(env: dict) -> str:
        return (f"{_dollars(env['max_loss_cents'])} a day, fees {_dollars(env['max_fees_cents'])}, "
                f"{env['max_trades']} trades, {env['max_exposure_lots']} lot{'s' if env['max_exposure_lots'] != 1 else ''} exposure, "
                f"gap {env['env']['max_gap_ticks']} ticks, {' and '.join(env['instruments'])} only")

    # ---- helpers ------------------------------------------------------------------------
    def _owner_phone(self, account: int | None) -> str | None:
        if account is None:
            return config.PHOTON_OWNER_NUMBER or None
        with self.gw.db.connect() as c:
            row = c.execute("SELECT owner_phone FROM accounts WHERE id=?", (account,)).fetchone()
        # Only accounts opened with a phone get texts. The default number is not a fallback for every
        # account: the lab's tests open hundreds of throwaway accounts whose halts would all page the owner.
        return row["owner_phone"] if row and row["owner_phone"] else None

    def _account_for(self, number: str) -> int | None:
        with self.gw.db.connect() as c:
            row = c.execute("SELECT id FROM accounts WHERE owner_phone=? ORDER BY id DESC LIMIT 1", (number,)).fetchone()
        return int(row["id"]) if row else None

    def _set_pending(self, session_id: str, account: int, env: dict | None) -> None:
        with self.gw.db.tx() as c:
            c.execute("INSERT OR REPLACE INTO phone_sessions(session_id, account, pending_envelope_json, updated_t) VALUES (?,?,?,?)",
                      (session_id, account, json.dumps(env) if env else None, now_ms()))


def _dollars(cents) -> str:
    return f"${int(cents)/100:.2f}".replace(".00", "")
