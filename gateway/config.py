"""Environment-driven settings. One env var, VENUE=fake|real, switches the venue."""
from __future__ import annotations

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

VENUE = os.environ.get("VENUE", "fake")            # fake | real
DB_PATH = os.environ.get("GATEWAY_DB", str(ROOT / "gateway" / "data" / "gateway.db"))
MINSKY_BUILD = Path(os.environ.get("MINSKY_BUILD", str(ROOT / "minsky" / "build")))
VENUED_HOST = os.environ.get("VENUED_HOST", "127.0.0.1")
VENUED_PORT = int(os.environ.get("VENUED_PORT", "7010"))
EXCHANGE_URL = os.environ.get("EXCHANGE_URL", "tcp://127.0.0.1:7000")
DEFAULT_MARK = int(os.environ.get("DEFAULT_MARK", "3010"))   # used until the venue has printed
VERIFY_TIMEOUT_MS = int(os.environ.get("VERIFY_TIMEOUT_MS", "10000"))


def fixtures_dir() -> Path:
    """Build A's `fixtures/` wins when it exists; until then the hand-written copy in gateway/."""
    env = os.environ.get("FIXTURES_DIR")
    if env:
        return Path(env)
    shared = ROOT / "fixtures"
    if (shared / "ex_v2.result.json").exists():
        return shared
    return ROOT / "gateway" / "fixtures"


def instruments_path() -> Path:
    p = fixtures_dir() / "instruments.json"
    return p if p.exists() else ROOT / "gateway" / "fixtures" / "instruments.json"


WINDOW_MS = 24 * 3600 * 1000                        # §5.1 "24h_rolling"
AGENT_OFFLINE_T_MS = int(os.environ.get("AGENT_OFFLINE_T_MS", "120000"))
REJECTIONS_BURST_N = 3
REJECTIONS_BURST_WINDOW_MS = 60_000
BUDGET_LOW_FRACTION = 0.25

# Fake venue timing: milliseconds between played-back events; 0 = instant.
FAKE_STEP_MS = int(os.environ.get("FAKE_STEP_MS", "600"))
FAKE_AUTOPLAY = os.environ.get("FAKE_AUTOPLAY", "1") == "1"

# External services. Each is real when its key is set, otherwise a local simulator that says so.
NESSIE_API_KEY = os.environ.get("NESSIE_API_KEY", "")
NESSIE_BASE = os.environ.get("NESSIE_BASE", "https://v2-prod-api.nessieisreal.com")   # Nessie v2: Bearer auth, cents
PHOTON_API_URL = os.environ.get("PHOTON_API_URL", "")
PHOTON_API_KEY = os.environ.get("PHOTON_API_KEY", "")
PHOTON_OWNER_NUMBER = os.environ.get("PHOTON_OWNER_NUMBER", "")
CHALK_AI_GATEWAY_URL = os.environ.get("CHALK_AI_GATEWAY_URL", "")
CHALK_API_KEY = os.environ.get("CHALK_API_KEY", "")
DEFAULT_MODEL = os.environ.get("GATEWAY_MODEL", "claude-opus-5-5")
DROID_BIN = os.environ.get("DROID_BIN", "droid")
REPAIR_MAX_ROUNDS = 3
