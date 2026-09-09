#!/usr/bin/env bash
set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
PORT="${1:-/dev/ttyACM0}"
BAUD="${2:-921600}"

echo "=================================================================="
echo " Burning Firmware and Vela NPU Model to Grove Vision AI V2"
echo " Port: $PORT @ $BAUD"
echo "=================================================================="

# Check Python environment
if ! python3 -c "import serial, xmodem" &>/dev/null; then
    echo "[INFO] Missing required Python packages (pyserial, xmodem)."
    echo "[INFO] Installing via pip..."
    python3 -m pip install pyserial xmodem
fi

python3 "$DIR/flash_model.py" "$PORT" "$BAUD"
