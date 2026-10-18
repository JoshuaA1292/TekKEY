#!/bin/sh
# The §11 story end to end: rejected, certified, the worst path, the turned delegate refused.
#   ./gateway/demo.sh          on the fake venue
#   ./gateway/demo.sh real     on Build A's exchange + venued (minsky/build must exist)
set -e
cd "$(dirname "$0")/.."
PY=.venv-gateway/bin/python
MODE=${1:-fake}
export PORT=${PORT:-8080} FAKE_STEP_MS=${FAKE_STEP_MS:-400} GATEWAY_DB=${GATEWAY_DB:-gateway/data/demo.db}
rm -f "$GATEWAY_DB" "$GATEWAY_DB-wal" "$GATEWAY_DB-shm" "$(dirname "$GATEWAY_DB")/fake_venue.json" gateway/data/venued.state.json
cleanup() { kill $GW $VD $EX 2>/dev/null || true; }
trap cleanup EXIT
wait_port() {
  port=$1
  i=0
  while [ "$i" -lt 50 ]; do
    if $PY -c "import socket; s=socket.create_connection(('127.0.0.1', $port), 0.2); s.close()" 2>/dev/null; then
      return 0
    fi
    i=$((i + 1))
    sleep 0.1
  done
  echo "port $port never opened" >&2
  return 1
}
if [ "$MODE" = "real" ]; then
  export VENUE=real EXCHANGE_URL=${EXCHANGE_URL:-tcp://127.0.0.1:7000} VENUED_PORT=${VENUED_PORT:-7010}
  EX_PORT=${EXCHANGE_URL##*:}
  mkdir -p gateway/data
  ./minsky/build/exchange --tcp "$EX_PORT" --accounts 255 >gateway/data/exchange.log 2>&1 & EX=$!
  wait_port "$EX_PORT"
  ./minsky/build/venued --listen "$VENUED_PORT" --exchange "$EXCHANGE_URL" --state gateway/data/venued.state.json \
      --instruments fixtures/instruments.json >gateway/data/venued.log 2>&1 & VD=$!
  wait_port "$VENUED_PORT"
else
  export VENUE=fake
fi
$PY -m gateway & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/healthz" >/dev/null && break; sleep 0.2; done
$PY -m gateway.wireup --gateway "http://127.0.0.1:$PORT" --exchange "${EXCHANGE_URL:-tcp://127.0.0.1:7000}"
