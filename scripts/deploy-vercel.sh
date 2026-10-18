#!/bin/sh
# Publish the page to Vercel (https://bounded-keys.vercel.app): the static site/ files plus rewrites that proxy
# /lab, /race, /backtest, /photon, /account, /program and /healthz to the backend named by LAB_PUBLIC_URL in .env
# (the machine running scripts/serve.sh behind scripts/tunnel.sh). Re-run after editing site/ or when the tunnel URL changes.
set -e
cd "$(dirname "$0")/.."
[ -f .env ] && { set -a; . ./.env; set +a; }
: "${LAB_PUBLIC_URL:?set LAB_PUBLIC_URL (the backend public URL) in .env}"
mkdir -p deploy/vercel
rsync -a --delete --exclude mock --exclude vercel.json --exclude .vercel --exclude .vercelignore --exclude .env.local --exclude .gitignore site/ deploy/vercel/
sed -E "s#https://[a-z0-9.-]+\.trycloudflare\.com#$LAB_PUBLIC_URL#g" deploy/vercel/vercel.json > deploy/vercel/vercel.json.tmp && mv deploy/vercel/vercel.json.tmp deploy/vercel/vercel.json
cd deploy/vercel && vercel deploy --prod --yes
