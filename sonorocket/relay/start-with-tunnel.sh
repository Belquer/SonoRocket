#!/usr/bin/env bash
# Zero-server mode: run the relay on the machine that runs the DAW and expose
# only the listener page through a free Cloudflare quick tunnel (HTTPS, no account).
# The plugin connects to 127.0.0.1:9100 on the same machine.
#
#   brew install cloudflared node      # macOS, once
#   ./start-with-tunnel.sh mix-luciano
set -euo pipefail
cd "$(dirname "$0")"
ROOM="${1:-mix-$(openssl rand -hex 4)}"
[ -d node_modules ] || npm install --silent

node server.js &
RELAY=$!
trap 'kill $RELAY 2>/dev/null' EXIT

LOG=$(mktemp)
cloudflared tunnel --no-autoupdate --url http://localhost:8080 >"$LOG" 2>&1 &
TUN=$!
trap 'kill $RELAY $TUN 2>/dev/null; rm -f "$LOG"' EXIT

for _ in $(seq 1 60); do
  URL=$(grep -oE 'https://[a-z0-9-]+\.trycloudflare\.com' "$LOG" | head -1 || true)
  [ -n "${URL:-}" ] && break
  sleep 0.5
done
[ -n "${URL:-}" ] || { echo "tunnel did not start; see $LOG"; exit 1; }

cat <<EOF

  SonoRocket is up.

  In the plugin:   Relay 127.0.0.1   Port 9100   Room $ROOM
                   Listen URL $URL
  Share this:      $URL/?room=$ROOM

  (Ctrl-C to stop)
EOF
wait $RELAY
