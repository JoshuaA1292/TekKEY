"""Photon 2.0 Beta client: exact requests, no network (httpx.MockTransport)."""
import json

import httpx

from gateway.photon_beta import PhotonBeta

PID = "pho_prj_test"


def _client(handler):
    return httpx.Client(base_url="https://api.photon.codes", transport=httpx.MockTransport(handler),
                        headers={"Authorization": "Bearer pho_sk_test"})


def test_send_text_creates_user_space_and_posts_message():
    """An iMessage space needs two participants: the owner, and the agent whose handle is the
    shared-line number assigned to that owner. The message is sent by the agent user."""
    calls = []

    def handler(req: httpx.Request):
        body = json.loads(req.content or b"{}")
        calls.append((req.method, req.url.path, body, dict(req.headers)))
        if req.url.path.endswith("/users") and req.method == "POST":
            return httpx.Response(200, json={"id": "usr_owner" if body["handle"] == "+15550001111" else "usr_agent", "handle": body["handle"]})
        if req.url.path.endswith("/platforms/imessage/assignments") and req.method == "GET":
            return httpx.Response(200, json={"assignments": [{"handle": "+15550001111", "assignedPhoneNumber": "+15559990000"}]})
        if req.url.path.endswith("/spaces"):
            return httpx.Response(200, json={"id": "spc_1", "participantIds": body["participantIds"]})
        if req.url.path.endswith("/messages"):
            return httpx.Response(202, json={"id": "msg_out"})
        return httpx.Response(404)

    b = PhotonBeta(PID, "pho_sk_test", http=_client(handler))
    out = b.send_text("+15550001111", "Account 1: budget low.")
    assert out["id"] == "msg_out"
    users = [c[2]["handle"] for c in calls if c[1].endswith("/users") and c[0] == "POST"]
    assert users == ["+15550001111", "+15559990000"]                       # the owner, then the agent (the assigned line)
    space = next(c for c in calls if c[1].endswith("/spaces"))
    assert space[2] == {"participantIds": ["usr_owner", "usr_agent"], "platform": "imessage"}
    m, path, body, headers = calls[-1]
    assert path == f"/v1/projects/{PID}/messages" and headers["authorization"] == "Bearer pho_sk_test"
    assert body == {"spaceId": "spc_1", "senderId": "usr_agent",
                    "content": {"type": "text", "format": "plain", "text": "Account 1: budget low."}}
    assert headers.get("idempotency-key")
    n = len(calls)
    b.send_text("+15550001111", "again")                                   # the space is reused: one more call only
    assert len(calls) == n + 1


def test_parse_inbound_event_and_reply_in_place():
    event = {"type": "message.received", "apiVersion": "2026-07-01", "data": {"projectId": PID, "object": {
        "id": "msg_in", "platform": "imessage", "space": {"id": "spc_9", "participantIds": ["u_owner", "u_agent"]},
        "sentBy": {"id": "u_owner", "handle": "+15550001111", "kind": "human"},
        "receivedBy": {"id": "u_agent", "handle": "+15559990000", "kind": "agent"},
        "content": {"type": "text", "format": "plain", "text": "freeze"}}}}
    parsed = PhotonBeta.parse_event(event)
    assert parsed["handle"] == "+15550001111" and parsed["text"] == "freeze" and parsed["space_id"] == "spc_9"
    assert PhotonBeta.parse_event({"type": "message.delivered"}) is None

    sent = []

    def handler(req):
        sent.append(json.loads(req.content))
        return httpx.Response(202, json={"id": "msg_reply"})

    b = PhotonBeta(PID, "pho_sk_test", http=_client(handler))
    b.remember_inbound(parsed["message"])
    b.send_text("+15550001111", "Frozen.")
    assert sent[0]["spaceId"] == "spc_9" and sent[0]["senderId"] == "u_agent"


def test_ensure_line_reuses_existing_assignment():
    def handler(req):
        if req.method == "GET":
            return httpx.Response(200, json={"assignments": [{"handle": "+15550001111", "assignedPhoneNumber": "+15559990000"}]})
        raise AssertionError("no POST expected")

    b = PhotonBeta(PID, "pho_sk_test", http=_client(handler))
    assert b.ensure_line("+15550001111") == "+15559990000"
