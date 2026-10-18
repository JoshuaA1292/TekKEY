#!/bin/sh
# Start the exchange, compile Snake, start the router, and open the visualiser so a person can
# play. Arrow keys / WASD steer; + and - turn the latency knob; m cycles the engine mutations;
# q quits the visualiser (the exchange and router keep running until this script exits).
#
#   ./demo.sh                      Snake over TCP loopback, paced to 12 frames per second
#   ./demo.sh --fps 0              unpaced: the frame rate is the latency law
#   ./demo.sh --transport shm      shared memory instead of TCP
#   ./demo.sh --rule110            the Rule 110 wall instead of Snake
#   ./demo.sh --pad 450            burn 450 extra instructions per frame (~1000 per frame)
set -e
cd "$(dirname "$0")"
FPS=12; TRANSPORT=tcp; PROG=snake; PAD=0; EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --fps) FPS=$2; shift ;;
    --transport) TRANSPORT=$2; shift ;;
    --rule110) PROG=rule110 ;;
    --pad) PAD=$2; shift ;;
    --wrap) EXTRA="$EXTRA --wrap" ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done
make -s build/exchange build/router build/compile build/viz
mkdir -p programs
case "$TRANSPORT" in
  tcp) URL=tcp://127.0.0.1:7000 ;;
  udp) URL=udp://127.0.0.1:7001 ;;
  shm) URL=shm:///tmp/minsky.shm ;;
  *) echo "transport must be tcp, udp or shm"; exit 2 ;;
esac
if [ "$PROG" = snake ]; then
  ./build/compile snake --out programs/demo.tbl --layout programs/demo.layout --pad "$PAD" $EXTRA
  VIZARGS=""
else
  ./build/compile rule110 --cells 96 --out programs/demo.tbl --layout programs/demo.layout
  VIZARGS="--wall"
  FPS=0
fi
./build/exchange --tcp 7000 --udp 7001 --shm /tmp/minsky.shm --feed 7002 --quiet &
EXPID=$!
trap 'kill $ROUTERPID $EXPID 2>/dev/null; wait 2>/dev/null; rm -f /tmp/minsky.shm' EXIT INT TERM
sleep 0.3
./build/router --table programs/demo.tbl --connect "$URL" --reset --max-fps "$FPS" --quiet &
ROUTERPID=$!
sleep 0.3
./build/viz --feed tcp://127.0.0.1:7002 --orders tcp://127.0.0.1:7000 --layout programs/demo.layout $VIZARGS
