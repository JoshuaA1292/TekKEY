"""Start one backtest window as soon as the running one finishes (no gateway restart needed).
    python -m race.queue_next 2025-10-10 2025-10-11 "leverage flash crash ..." [speed] [round_minutes]
"""
import os, sys, time, httpx
from race.backtest import fetch_window, parse_when

start, end, label = sys.argv[1], sys.argv[2], sys.argv[3]
speed = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
mins = float(sys.argv[5]) if len(sys.argv) > 5 else 10.0
token = os.environ["RACE_ADMIN_TOKEN"]
base = os.environ.get("RACE_BASE", "http://127.0.0.1:8080")
n = len(fetch_window(parse_when(start), parse_when(end)))
print(time.strftime("%H:%M:%S"), f"cached {n} candles for {start}..{end}; waiting for the running backtest to finish", flush=True)
while True:
    try:
        full = httpx.get(f"{base}/backtest/state?minutes=1", timeout=10).json()
        if full.get("finished") or not full.get("backtest"):
            r = httpx.post(f"{base}/backtest/start", json={"token": token, "start": start, "end": end, "speed": speed, "round_minutes": mins, "label": label}, timeout=120)
            print(time.strftime("%H:%M:%S"), "started:", r.status_code, r.text[:200], flush=True)
            break
        print(time.strftime("%H:%M:%S"), f"running: epoch {full['epoch']['id']} round {full['epoch']['rounds']}, progress {round(full['feed'].get('progress', 0) * 100, 1)}%", flush=True)
    except Exception as e:
        print(time.strftime("%H:%M:%S"), "poll failed:", e, flush=True)
    time.sleep(30)
