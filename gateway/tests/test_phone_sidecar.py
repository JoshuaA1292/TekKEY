"""gateway/photon.py against a mocked phone/ sidecar (httpx.MockTransport): the exact request,
the `via` field, and the fallback to the log outbox when the sidecar is absent or refuses."""
import json
import sys
from pathlib import Path

import httpx
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from gateway.photon import Photon  # noqa: E402


class _GW:
    def __init__(self):
        self.listeners = []


def _sidecar(reply: dict, status: int = 200):
    seen = []

    def handler(request: httpx.Request) -> httpx.Response:
        seen.append(request)
        return httpx.Response(status, json=reply)
    return seen, httpx.Client(transport=httpx.MockTransport(handler))


def test_send_posts_to_the_sidecar_and_records_via_photon_on_success():
    seen, http = _sidecar({"delivered": True, "via": "photon", "message_id": "m-42"})
    ph = Photon(_GW(), sidecar_url="http://127.0.0.1:8787/", project_id="proj_1", http=http)
    assert ph.live
    msg = ph.send("+15550001111", "Account 1: budget low.", event={"type": "budget.low"})
    assert msg["delivered"] is True and msg["via"] == "photon" and msg["message_id"] == "m-42"
    assert str(seen[0].url) == "http://127.0.0.1:8787/send"
    assert json.loads(seen[0].content) == {"to": "+15550001111", "text": "Account 1: budget low.", "call": False}
    assert list(ph.outbox)[-1] is msg and msg["event_type"] == "budget.low"


def test_call_sends_the_text_and_reports_voice_unsupported():
    seen, http = _sidecar({"delivered": True, "via": "photon", "voice": "unsupported", "message_id": "m-1"})
    ph = Photon(_GW(), sidecar_url="http://127.0.0.1:8787", project_id="proj_1", http=http)
    msg = ph.send("+15550001111", "LEDGER INVARIANT VIOLATED", call=True)
    assert json.loads(seen[0].content)["call"] is True
    assert msg["via"] == "photon" and msg["voice"] == "unsupported"


def test_sidecar_in_log_mode_or_down_leaves_via_log_and_delivered_false():
    _, http = _sidecar({"delivered": False, "via": "log"})
    ph = Photon(_GW(), sidecar_url="http://127.0.0.1:8787", project_id="proj_1", http=http)
    msg = ph.send("+15550001111", "hello")
    assert msg["delivered"] is False and msg["via"] == "log" and "error" in msg

    def down(request):
        raise httpx.ConnectError("refused")
    ph = Photon(_GW(), sidecar_url="http://127.0.0.1:1", project_id="proj_1", http=httpx.Client(transport=httpx.MockTransport(down)))
    msg = ph.send("+15550001111", "hello")
    assert msg["delivered"] is False and msg["via"] == "log" and "unreachable" in msg["error"]


def test_without_project_id_nothing_is_posted():
    seen, http = _sidecar({"delivered": True, "via": "photon"})
    ph = Photon(_GW(), sidecar_url="http://127.0.0.1:8787", project_id="", http=http)
    assert not ph.live
    msg = ph.send("+15550001111", "hello")
    assert seen == [] and msg["via"] == "log" and msg["delivered"] is False
