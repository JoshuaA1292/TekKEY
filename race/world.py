"""The race's own venue: a second exchange + venued pair (so the lab's runs never move this
book), the bounded lane's gateway built from the same `gateway.app.build` as the public one and
driven through its HTTP routes in-process, and three exchange sessions: the environment's (the
tape prints through it), the raw lane's (an ordinary key: orders), and a control session for
queries. Everything is real; the only difference from production is the port numbers."""
from __future__ import annotations

import logging
import os
from pathlib import Path

from fastapi.testclient import TestClient

from gateway import config
from gateway.db import Database, now_ms
from gateway.nessie import make_bank
from gateway.venue import RealVenue

from .exchange_client import Session

log = logging.getLogger("race.world")

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXCHANGE_URL = os.environ.get("RACE_EXCHANGE_URL", "tcp://127.0.0.1:7020")
DEFAULT_VENUED_PORT = int(os.environ.get("RACE_VENUED_PORT", "7030"))
DEFAULT_DB = os.environ.get("RACE_DB", str(Path(config.DB_PATH).parent / "race.db"))


class World:
    def __init__(self, exchange_url: str = DEFAULT_EXCHANGE_URL, venued_port: int = DEFAULT_VENUED_PORT, db_path: str = DEFAULT_DB):
        from gateway.app import build          # here, not at import: gateway.app imports race.app
        self.exchange_url = exchange_url
        self.venued_port = venued_port
        self.db = Database(db_path)
        self.now_ms = now_ms                   # market time in a backtest; the conductor swaps it
        self.venue = RealVenue(host="127.0.0.1", port=venued_port)
        self.bank = make_bank(self.db)
        self.app = build(db=self.db, venue=self.venue, bank=self.bank, start_bus=False, lab=False, race=False)
        self.gw = self.app.state.gw
        self.env = Session(exchange_url, name="env", timeout=5.0)
        self.ctl = Session(exchange_url, name="ctl", timeout=5.0)
        self.raw = Session(exchange_url, name="raw", timeout=5.0)
        self.venue.mark("ETH")                 # proves venued answers before anything else starts
        self.app.state.bus.start()             # reconcile, then consume venued's events into the ledger
        self.http = TestClient(self.app, raise_server_exceptions=False)
        self.instruments = self.venue.instruments()
        self.tick_cents = int(self.instruments["ETH"]["tick_cents"])
        self.book = list(self.instruments).index("ETH")
        log.info("race world up: exchange %s, venued :%d, db %s, bank %s", exchange_url, venued_port, db_path, self.bank.name)

    def describe(self) -> dict:
        return {"exchange": self.exchange_url, "venued_port": self.venued_port, "db": self.db.path, "bank": self.bank.name,
                "venue": self.venue.name, "tick_cents": self.tick_cents, "book": self.book}

    def close(self) -> None:
        try:
            self.app.state.bus.stop()
        except Exception:
            pass
        for s in (self.env, self.ctl, self.raw):
            try:
                s.close()
            except Exception:
                pass
        try:
            self.http.close()
        except Exception:
            pass
