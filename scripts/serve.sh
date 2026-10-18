#!/bin/sh
# Start the whole stack: exchange, venued, the race's own exchange + venued, the gateway with the lab, the race and the page.
# Works on the Mac (from the repo root, using .venv-gateway) and inside the Docker image.
set -e
cd "$(dirname "$0")/.."
[ -f .env ] && { set -a; . ./.env; set +a; }   # local credentials, never committed
PY=${PY:-$( [ -x .venv-gateway/bin/python ] && echo .venv-gateway/bin/python || echo python )}
export VENUE=${VENUE:-real} PORT=${PORT:-8080} HOST=${HOST:-127.0.0.1}
export EXCHANGE_URL=${EXCHANGE_URL:-tcp://127.0.0.1:7000} VENUED_PORT=${VENUED_PORT:-7010}
export GATEWAY_DB=${GATEWAY_DB:-gateway/data/gateway.db}
mkdir -p "$(dirname "$GATEWAY_DB")"
cleanup() { kill $EX $VD $REX $RVD $BEX $BVD $PH 2>/dev/null || true; }
trap cleanup EXIT INT TERM
if [ "$VENUE" = "real" ]; then
  ./minsky/build/exchange --tcp 7000 --accounts 255 >"$(dirname "$GATEWAY_DB")/exchange.log" 2>&1 & EX=$!
  sleep 0.5
  ./minsky/build/venued --listen "$VENUED_PORT" --exchange "$EXCHANGE_URL" \
      --state "$(dirname "$GATEWAY_DB")/venued.state.json" --instruments fixtures/instruments.json \
      >"$(dirname "$GATEWAY_DB")/venued.log" 2>&1 & VD=$!
  sleep 0.8
  if [ "${RACE:-1}" != "0" ]; then
    # The race (race/README.md) gets its own exchange + venued pair so lab runs never move its book.
    export RACE_EXCHANGE_PORT=${RACE_EXCHANGE_PORT:-7020} RACE_VENUED_PORT=${RACE_VENUED_PORT:-7030}
    export RACE_EXCHANGE_URL=${RACE_EXCHANGE_URL:-tcp://127.0.0.1:$RACE_EXCHANGE_PORT}
    ./minsky/build/exchange --tcp "$RACE_EXCHANGE_PORT" --feed "$((RACE_EXCHANGE_PORT + 2))" --accounts 255 \
        >"$(dirname "$GATEWAY_DB")/race.exchange.log" 2>&1 & REX=$!
    sleep 0.5
    ./minsky/build/venued --listen "$RACE_VENUED_PORT" --exchange "$RACE_EXCHANGE_URL" \
        --state "$(dirname "$GATEWAY_DB")/race.venued.state.json" --instruments fixtures/instruments.json \
        >"$(dirname "$GATEWAY_DB")/race.venued.log" 2>&1 & RVD=$!
    sleep 0.8
    if [ "${BACKTEST:-1}" != "0" ]; then
      # A third pair for the backtest race (race/backtest.py), so a replayed day never touches the live race's book.
      export BACKTEST_EXCHANGE_PORT=${BACKTEST_EXCHANGE_PORT:-7040} BACKTEST_VENUED_PORT=${BACKTEST_VENUED_PORT:-7050}
      export BACKTEST_EXCHANGE_URL=${BACKTEST_EXCHANGE_URL:-tcp://127.0.0.1:$BACKTEST_EXCHANGE_PORT}
      ./minsky/build/exchange --tcp "$BACKTEST_EXCHANGE_PORT" --feed "$((BACKTEST_EXCHANGE_PORT + 2))" --accounts 255 \
          >"$(dirname "$GATEWAY_DB")/backtest.exchange.log" 2>&1 & BEX=$!
      sleep 0.5
      ./minsky/build/venued --listen "$BACKTEST_VENUED_PORT" --exchange "$BACKTEST_EXCHANGE_URL" \
          --state "$(dirname "$GATEWAY_DB")/backtest.venued.state.json" --instruments fixtures/instruments.json \
          >"$(dirname "$GATEWAY_DB")/backtest.venued.log" 2>&1 & BVD=$!
      sleep 0.8
    fi
  fi
fi
if [ -n "$PHOTON_PROJECT_ID" ] && [ -f phone/package.json ]; then
  (cd phone && npm start >"../$(dirname "$GATEWAY_DB")/phone.log" 2>&1) & PH=$!
fi
exec $PY -m gateway
