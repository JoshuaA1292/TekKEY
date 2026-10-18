#!/bin/sh
# Bring the whole demo back after a laptop sleep: a fresh public tunnel first (its URL changes every start), then the
# stack (serve.sh reads LAB_PUBLIC_URL at startup to register the Photon webhook), then republish the Vercel proxy.
#   scripts/morning.sh        then open https://bounded-keys.vercel.app
set -e
cd "$(dirname "$0")/.."
pkill -f "cloudflared tunnel" 2>/dev/null || true
nohup cloudflared tunnel --url http://127.0.0.1:8080 >gateway/data/tunnel.log 2>&1 &
URL=""; for i in $(seq 1 60); do sleep 1; URL=$(grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' gateway/data/tunnel.log | head -1); [ -n "$URL" ] && break; done
[ -n "$URL" ] || { echo "no tunnel URL after 60 s; see gateway/data/tunnel.log"; exit 1; }
sed -i '' "s#^LAB_PUBLIC_URL=.*#LAB_PUBLIC_URL=$URL#" .env; grep -q '^LAB_PUBLIC_URL=' .env || echo "LAB_PUBLIC_URL=$URL" >> .env
echo "tunnel: $URL"
nohup sh scripts/serve.sh >>gateway/data/serve.log 2>&1 &
for i in $(seq 1 90); do sleep 1; curl -s -m 2 http://127.0.0.1:8080/healthz >/dev/null && break; done
curl -s -m 5 http://127.0.0.1:8080/healthz; echo
sh scripts/deploy-vercel.sh | tail -2
echo "live: https://bounded-keys.vercel.app   (keep this laptop awake: caffeinate -dims &)"
