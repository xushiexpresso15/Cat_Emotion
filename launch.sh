#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$HOME/cat_emotion_env"
SERVER_SCRIPT="$SCRIPT_DIR/server.py"
PORT=8080

GREEN='\033[0;32m'
CYAN='\033[0;36m'
DIM='\033[2m'
NC='\033[0m'

echo ""
echo -e "${CYAN} System start${NC}"
echo ""

if [ -d "$VENV_DIR" ]; then
    source "$VENV_DIR/bin/activate"
elif [ -d "$SCRIPT_DIR/.venv" ]; then
    source "$SCRIPT_DIR/.venv/bin/activate"
elif [ -d "$SCRIPT_DIR/venv" ]; then
    source "$SCRIPT_DIR/venv/bin/activate"
fi

for port in /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$port" ] && chmod 666 "$port" 2>/dev/null || true
done

if lsof -i :$PORT >/dev/null 2>&1; then
    echo "Port $PORT is occupied..."
    fuser -k $PORT/tcp 2>/dev/null || true
    sleep 1
fi

echo -e "  ${GREEN}${NC} Launching web server on port $PORT..."
echo ""

cd "$SCRIPT_DIR"
python3 "$SERVER_SCRIPT" --port $PORT

echo ""
echo -e "  ${DIM}Server stopped.${NC}"
echo ""
