#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=3000

echo "========================================="
echo "  啟動 Cat Emotion 雲端中繼伺服器"
echo "========================================="

# 啟動 Node.js 中繼後端
node "$DIR/server.js" &
NODE_PID=$!

sleep 2

# 啟動 Cloudflare 穿透
echo "正在建立全球公網 HTTPS 穿透..."
cloudflared tunnel --url http://localhost:$PORT

trap "kill $NODE_PID 2>/dev/null || true" EXIT
