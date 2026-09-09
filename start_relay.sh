#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${PORT:-3000}"

echo "========================================="
echo "  Cat Emotion Cloud Relay Server"
echo "========================================="

# Start Node.js relay server
node "$DIR/server.js" &
NODE_PID=$!

trap "kill $NODE_PID 2>/dev/null || true" EXIT

sleep 1

# Check for cloudflared
CLOUDFLARED_BIN=""
if command -v cloudflared &>/dev/null; then
    CLOUDFLARED_BIN="$(command -v cloudflared)"
elif [ -x "$HOME/.local/bin/cloudflared" ]; then
    CLOUDFLARED_BIN="$HOME/.local/bin/cloudflared"
fi

if [ -n "$CLOUDFLARED_BIN" ]; then
    echo "Starting Cloudflare Tunnel on port $PORT..."
    "$CLOUDFLARED_BIN" tunnel --url "http://localhost:$PORT"
else
    echo "cloudflared not found in PATH or ~/.local/bin."
    echo "Relay server is running locally on http://localhost:$PORT"
    echo "To expose to the public internet, install cloudflared or deploy with Docker/Render."
    wait $NODE_PID
fi
