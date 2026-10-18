"""B2: the three key classes. 32 random bytes, base32, prefixed by class; only sha256(key) is stored.

    ok_  owner    bound to one account
    ak_  author   bound to one account
    bk_  program  bound to one program; states issued -> deployed -> (paused) -> spent
"""
from __future__ import annotations

import base64
import hashlib
import secrets
from dataclasses import dataclass

from .db import Database, now_ms
from .errors import ApiError

PREFIX = {"ok": "ok_", "ak": "ak_", "bk": "bk_"}
BK_STATES = ("issued", "deployed", "paused", "spent")


def mint() -> str:
    return base64.b32encode(secrets.token_bytes(32)).decode().rstrip("=").lower()


def hash_key(key: str) -> str:
    return hashlib.sha256(key.encode()).hexdigest()


@dataclass
class KeyInfo:
    id: int
    cls: str
    account: int
    program_id: int | None
    state: str


def issue(conn, cls: str, account: int, program_id: int | None = None) -> tuple[str, int]:
    """Create a key row; returns (plaintext, key id). The plaintext is never stored."""
    plaintext = PREFIX[cls] + mint()
    state = "issued" if cls == "bk" else "active"
    cur = conn.execute(
        "INSERT INTO keys(class, hash, account, program_id, state, created_t) VALUES (?,?,?,?,?,?)",
        (cls, hash_key(plaintext), account, program_id, state, now_ms()),
    )
    return plaintext, int(cur.lastrowid)


def authenticate(db: Database, authorization: str | None) -> KeyInfo:
    if not authorization or not authorization.lower().startswith("bearer "):
        raise ApiError(401, "missing_key")
    token = authorization[7:].strip()
    with db.connect() as conn:
        row = conn.execute("SELECT * FROM keys WHERE hash=?", (hash_key(token),)).fetchone()
        if row is None:
            raise ApiError(401, "bad_key")
        conn.execute("UPDATE keys SET last_used_t=? WHERE id=?", (now_ms(), row["id"]))
    return KeyInfo(int(row["id"]), row["class"], int(row["account"]), row["program_id"], row["state"])


def require_class(key: KeyInfo, *classes: str) -> None:
    if key.cls not in classes:
        raise ApiError(403, "key_class", required=list(classes), got=key.cls)


def require_owner(key: KeyInfo, account: int) -> None:
    require_class(key, "ok")
    if key.account != account:
        raise ApiError(403, "key_bound_to_account", account=key.account)


def require_author(key: KeyInfo, account: int) -> None:
    require_class(key, "ak")
    if key.account != account:
        raise ApiError(403, "key_bound_to_account", account=key.account)


def require_program_key(key: KeyInfo, program_id: int, allowed_states: tuple[str, ...]) -> None:
    """§10.4: wrong class -> key_class; other program -> key_bound_to_program; wrong state -> key_spent."""
    require_class(key, "bk")
    if key.program_id != program_id:
        raise ApiError(403, "key_bound_to_program", program_id=key.program_id)
    if key.state not in allowed_states:
        # Every disallowed transition is reported as key_spent: the key has already done what it
        # was allowed to do (deploy once), or it has ended.
        raise ApiError(403, "key_spent", state=key.state)


def set_state(conn, key_id: int, state: str) -> None:
    assert state in BK_STATES
    conn.execute("UPDATE keys SET state=? WHERE id=?", (state, key_id))
