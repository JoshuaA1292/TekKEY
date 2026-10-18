"""Dump the gateway's OpenAPI spec to agent/image/openapi.json so the sandbox image carries the
API surface without carrying any gateway code. Run from the repo root:

    .venv-gateway/bin/python agent/image/export_openapi.py

The app is built lazily with the fake venue and a throwaway database; nothing is started.
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent / "openapi.json"


def main() -> int:
    sys.path.insert(0, str(ROOT))
    os.environ.setdefault("VENUE", "fake")
    os.environ["GATEWAY_LAZY"] = "1"
    from gateway import app as app_mod
    from gateway.db import Database
    from gateway.fake_venue import FakeVenue
    from gateway.nessie import LocalBank

    with tempfile.TemporaryDirectory() as tmp:
        db = Database(str(Path(tmp) / "g.db"))
        venue = FakeVenue(fixtures_dir=ROOT / "gateway" / "fixtures", state_path=str(Path(tmp) / "fake.json"), autoplay=False)
        app = app_mod.build(db=db, venue=venue, bank=LocalBank(db), start_bus=False)
        spec = app.openapi()
        venue.close()
    # the delegate's view: drop the phone webhook, which is not part of the public API
    spec["paths"] = {p: v for p, v in spec["paths"].items() if not p.startswith("/photon")}
    OUT.write_text(json.dumps(spec, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({len(spec['paths'])} paths)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
