"""Regenerate Build B's hand-written copy of the CONTRACT.md §7 fixtures from the Python model.

Build A's binaries are authoritative: when `fixtures/` at the repo root exists, the gateway
uses it instead of this directory (see config.fixtures_dir). Run: python -m gateway.fixtures.make
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))

from gateway import fake_verify, mpl  # noqa: E402
from gateway.fake_venue import FakeVenue  # noqa: E402

ins = json.loads((HERE / "instruments.json").read_text())
envelope = json.loads((HERE / "envelope_40.json").read_text())
MARK = 3010


def dump(path: Path, obj) -> None:
    path.write_text(json.dumps(obj, indent=2, sort_keys=False) + "\n")


for name in ("ex_v1", "ex_v2", "ex_noallow"):
    src = (HERE / f"{name}.mpl").read_text()
    try:
        compiled = mpl.compile_program(src, ins, envelope["env"])
    except mpl.StaticError as e:
        dump(HERE / f"{name}.result.json", e.body())
        print(name, "static_error line", e.line)
        continue
    dump(HERE / f"{name}.compiled.json", compiled)
    code, result = fake_verify.verify(compiled, envelope, MARK, ins)
    dump(HERE / f"{name}.result.json", result)
    print(name, result["status"], result.get("bound") or (result["violates"], result["value_on_path"]))

# events_v2_worst.jsonl: v2 running its worst path through the fake venued
v = FakeVenue(fixtures_dir=HERE, state_path="", autoplay=False)
compiled = json.loads((HERE / "ex_v2.compiled.json").read_text())
result = json.loads((HERE / "ex_v2.result.json").read_text())
v.deploy(7, 3, compiled, 300000 + result["bound"]["max_loss_cents"], path=result["worst_path"])
v.drive(7)
v.close()
lines = []
while True:
    try:
        ev = v._events.get_nowait()
    except Exception:
        break
    if ev is None:
        break
    lines.append(json.dumps(ev))
(HERE / "events_v2_worst.jsonl").write_text("\n".join(lines) + "\n")
print("events_v2_worst.jsonl:", [json.loads(l)["type"] for l in lines])
