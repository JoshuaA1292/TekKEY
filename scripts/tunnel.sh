#!/bin/sh
# A public URL for the running gateway in one command, no account needed (Cloudflare quick tunnel).
#   scripts/serve.sh &   then   scripts/tunnel.sh
# The URL is printed as https://<random>.trycloudflare.com and lasts while this process runs.
PORT=${PORT:-8080}
exec cloudflared tunnel --url "http://127.0.0.1:$PORT" --no-autoupdate
