#!/bin/bash
set -e
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
PORT="${1:-/dev/ttyACM0}"
BAUD="${2:-921600}"

echo "=================================================================="
echo " 🚀 Burning YOLOv8n V4 Calibrated QAT to Grove Vision AI V2"
echo " Directory: $DIR"
echo " Port:      $PORT @ $BAUD"
echo "=================================================================="

python3 "$DIR/flash_v4.py" "$PORT" "$BAUD"
