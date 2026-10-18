"""B6: the event bus. One consumer of `venued subscribe`; events go to SQLite, then to SSE
listeners and to the owner's phone. Derived events (budget.low, agent.offline,
rejections.burst) are computed in the service and emitted through the same path."""
from __future__ import annotations

import json
import logging
import threading
import time
from typing import Iterator

from .db import Database
from .service import Gateway

log = logging.getLogger("gateway.events")


class EventBus:
    def __init__(self, gateway: Gateway, offline_check_s: float = 5.0):
        self.gw = gateway
        self.offline_check_s = offline_check_s
        self._stop = threading.Event()
        self._consumer: threading.Thread | None = None
        self._checker: threading.Thread | None = None

    def start(self) -> None:
        # On (re)start: reconcile first, so halts missed while down go through the ledger path.
        try:
            res = self.gw.reconcile()
            if res.get("applied"):
                log.info("reconcile applied missed halts for programs %s", res["applied"])
        except Exception:
            log.exception("reconcile on start failed")
        self._stop.clear()
        self._consumer = threading.Thread(target=self._consume, name="venue-events", daemon=True)
        self._consumer.start()
        self._checker = threading.Thread(target=self._check_loop, name="derived-events", daemon=True)
        self._checker.start()

    def stop(self) -> None:
        self._stop.set()

    def _consume(self) -> None:
        while not self._stop.is_set():
            try:
                for ev in self.gw.venue.subscribe():
                    if self._stop.is_set():
                        return
                    try:
                        self.gw.on_venue_event(ev)
                    except Exception:
                        log.exception("applying venue event failed: %s", ev)
                return
            except Exception:
                log.exception("venue subscription failed; retrying")
                time.sleep(1.0)

    def _check_loop(self) -> None:
        while not self._stop.wait(self.offline_check_s):
            try:
                self.gw.check_agent_offline()
            except Exception:
                log.exception("agent.offline check failed")


def sse_stream(db: Database, account: int, last_id: int = 0, poll_s: float = 0.3,
               stop: threading.Event | None = None, keepalive_s: float = 15.0, once: bool = False) -> Iterator[str]:
    """Server-sent events for one account, replayed from `last_id` then live.
    `once` returns after the replay (a polling client passes `since=<last id>`)."""
    idle = 0.0
    while stop is None or not stop.is_set():
        with db.connect() as c:
            rows = c.execute("SELECT id, type, payload_json FROM events WHERE account=? AND id>? ORDER BY id LIMIT 200",
                             (account, last_id)).fetchall()
        if rows:
            idle = 0.0
            for r in rows:
                last_id = int(r["id"])
                yield f"id: {last_id}\nevent: {r['type']}\ndata: {r['payload_json']}\n\n"
        else:
            if once:
                return
            time.sleep(poll_s)
            idle += poll_s
            if idle >= keepalive_s:
                idle = 0.0
                yield ": keepalive\n\n"
