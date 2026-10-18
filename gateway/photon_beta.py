"""Photon 2.0 Beta REST client (projects whose id starts with `pho_prj_`).

Verified against https://api.photon.codes/openapi.json on 2026-10-17:
  auth                 Authorization: Bearer <project secret pho_sk_...>
  line for an owner    POST /v1/projects/{id}/platforms/imessage/assignments {handle}  -> assignedPhoneNumber
  user / space         POST /v1/projects/{id}/users {handle, platform}; POST /v1/projects/{id}/spaces {participantIds, platform}
  send                 POST /v1/projects/{id}/messages (header idempotency-key) {spaceId, senderId, content:{type:text, format:plain, text}}
  inbound              webhook destination (apiVersion 2026-07-01), event type message.received:
                       {type, data:{object: message}} with message.space.id, message.sentBy.{id,handle}, message.receivedBy.id, message.content.text

What is real: every call below hits api.photon.codes. What is not verified: delivery to a handset
(it needs the owner's own number assigned to the project's shared line), and the webhook
signature scheme (the destination returns a signingSecret; we store it and verify an HMAC-SHA256
over the raw body when a signature header is present, which is best effort until Photon documents it).
"""
from __future__ import annotations

import hashlib
import hmac
import logging
import os
import uuid

import httpx

log = logging.getLogger("gateway.photon_beta")

BASE = os.environ.get("PHOTON_API_BASE", "https://api.photon.codes")
API_VERSION = "2026-07-01"


def is_beta_project(project_id: str | None) -> bool:
    return bool(project_id) and project_id.startswith("pho_prj_")


class PhotonBeta:
    def __init__(self, project_id: str, secret: str, platform: str = "imessage", http: httpx.Client | None = None):
        self.project_id = project_id
        self.platform = platform
        self.http = http or httpx.Client(base_url=BASE, timeout=20, headers={"Authorization": f"Bearer {secret}"})
        self._users: dict[str, str] = {}
        self._spaces: dict[str, tuple[str, str]] = {}      # handle -> (space_id, sender_id)
        self.agent_user_id: str | None = None

    # ---- helpers --------------------------------------------------------------------
    def _p(self, path: str) -> str:
        return f"/v1/projects/{self.project_id}{path}"

    def _post(self, path: str, body: dict, idempotency: str | None = None) -> dict:
        headers = {"idempotency-key": idempotency or uuid.uuid4().hex} if idempotency is not False else {}
        r = self.http.post(self._p(path), json=body, headers=headers)
        if r.status_code >= 300:
            raise PhotonError(r.status_code, r.text[:300])
        return r.json()

    def _get(self, path: str, **params) -> dict:
        r = self.http.get(self._p(path), params=params)
        if r.status_code >= 300:
            raise PhotonError(r.status_code, r.text[:300])
        return r.json()

    # ---- the line --------------------------------------------------------------------
    def project(self) -> dict:
        return self._get("")

    def platform_state(self) -> dict:
        return self._get(f"/platforms/{self.platform}")

    def assignments(self) -> list[dict]:
        return self._get(f"/platforms/{self.platform}/assignments").get("assignments", [])

    def ensure_line(self, owner_handle: str) -> str:
        """The number the owner texts. A shared-pool number is assigned per owner handle."""
        for a in self.assignments():
            if a.get("handle") == owner_handle and not a.get("releasedAt"):
                return a["assignedPhoneNumber"]
        a = self._post(f"/platforms/{self.platform}/assignments", {"handle": owner_handle},
                       idempotency=f"line:{self.project_id}:{owner_handle}")
        log.info("photon: assigned %s to owner %s", a.get("assignedPhoneNumber"), owner_handle)
        return a["assignedPhoneNumber"]

    # ---- users and spaces ---------------------------------------------------------------
    def ensure_user(self, handle: str) -> str:
        if handle not in self._users:
            u = self._post("/users", {"handle": handle, "platform": self.platform}, idempotency=False)
            self._users[handle] = u["id"]
        return self._users[handle]

    def ensure_space(self, owner_handle: str) -> tuple[str, str]:
        """(space_id, sender_id) for talking to the owner. An iMessage space needs two participants: the
        owner and the agent, whose user handle is the shared-line number assigned to the owner. On a
        shared line Photon refuses outbound until the owner has texted the line once
        (SHARED_LINE_INBOUND_REQUIRED); the gateway surfaces that error in the outbox."""
        if owner_handle in self._spaces:
            return self._spaces[owner_handle]
        owner_id = self.ensure_user(owner_handle)
        line = self.ensure_line(owner_handle)
        agent_id = self.ensure_user(line)
        self.agent_user_id = agent_id
        sp = self._post("/spaces", {"participantIds": [owner_id, agent_id], "platform": self.platform},
                        idempotency=f"space:{self.project_id}:{owner_handle}:{line}")
        self._spaces[owner_handle] = (sp["id"], agent_id)
        return self._spaces[owner_handle]

    # ---- messages ----------------------------------------------------------------------
    def send_text(self, owner_handle: str, text: str, *, space_id: str | None = None, sender_id: str | None = None,
                  parent_message_id: str | None = None) -> dict:
        if not (space_id and sender_id):
            space_id, sender_id = self.ensure_space(owner_handle)
        body = {"spaceId": space_id, "senderId": sender_id,
                "content": {"type": "text", "format": "plain", "text": text}}
        if parent_message_id:
            body["parentMessageId"] = parent_message_id
        return self._post("/messages", body)

    def remember_inbound(self, msg: dict) -> None:
        """An inbound message tells us the exact space and the agent's sender id for that owner."""
        handle = ((msg.get("sentBy") or {}).get("handle"))
        space = (msg.get("space") or {}).get("id")
        sender = (msg.get("receivedBy") or {}).get("id")
        if handle and space and sender:
            self._spaces[handle] = (space, sender)
            self.agent_user_id = sender

    # ---- webhooks ---------------------------------------------------------------------
    def ensure_webhook(self, url: str, name: str = "bounded-key-gateway") -> dict:
        """One destination for this URL; returns {destinationId, url, signingSecret|None}."""
        listing = self._get("/webhooks/destinations")
        for d in listing.get("destinations") or listing.get("items") or []:
            if d.get("url") == url and not d.get("deletedAt"):
                return {"destinationId": d["destinationId"], "url": url, "signingSecret": None, "existing": True}
        created = self._post("/webhooks/destinations",
                             {"apiVersion": API_VERSION, "name": name, "url": url, "status": "enabled",
                              "description": "bounded-key owner's phone: alarms out, freeze/status in",
                              "enabledEvents": ["message.received"]},          # WebhookEventTypeList
                             idempotency=f"webhook:{self.project_id}:{url}")
        dest = created.get("destination", {})
        return {"destinationId": dest.get("destinationId"), "url": url, "signingSecret": created.get("signingSecret"), "existing": False}

    def rotate_webhook_secret(self, destination_id: str) -> str | None:
        out = self._post(f"/webhooks/destinations/{destination_id}/secret-rotations", {}, idempotency=uuid.uuid4().hex)
        return out.get("signingSecret") or (out.get("destination") or {}).get("signingSecret")

    @staticmethod
    def verify_signature(secret: str | None, raw_body: bytes, headers: dict) -> bool | None:
        """None when no signature header is present (undocumented scheme), True/False otherwise."""
        if not secret:
            return None
        candidates = [v for k, v in headers.items() if "signature" in k.lower()]
        if not candidates:
            return None
        mac = hmac.new(secret.encode(), raw_body, hashlib.sha256).hexdigest()
        return any(mac in c for c in candidates)

    @staticmethod
    def parse_event(event: dict) -> dict | None:
        """message.received -> {message_id, handle, text, space_id, sender_id, platform} or None."""
        if event.get("type") != "message.received":
            return None
        msg = (event.get("data") or {}).get("object") or event.get("object") or {}
        content = msg.get("content") or {}
        if content.get("type") != "text":
            return None
        return {"message_id": msg.get("id"), "handle": (msg.get("sentBy") or {}).get("handle"),
                "text": content.get("text", ""), "space_id": (msg.get("space") or {}).get("id"),
                "sender_id": (msg.get("receivedBy") or {}).get("id"), "platform": msg.get("platform"), "message": msg}


class PhotonError(Exception):
    def __init__(self, status: int, detail: str):
        super().__init__(f"photon {status}: {detail}")
        self.status, self.detail = status, detail
