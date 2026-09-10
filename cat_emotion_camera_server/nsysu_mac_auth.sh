#!/usr/bin/env bash
# =========================================================================
# NSYSU TANetRoaming MAC Authorization Helper for Cat Emotion ESP32
# Target ESP32 MAC: d8:3b:da:75:e9:f4
# =========================================================================
set -e

ESP32_MAC="d8:3b:da:75:e9:f4"
TMP_CON_NAME="TANetRoaming-ESP32"

echo "========================================================="
echo "  NSYSU TANetRoaming MAC Authorization Helper"
echo "  Target ESP32 MAC: ${ESP32_MAC}"
echo "========================================================="

# 1. Detect Wi-Fi device
WIFI_DEV=$(nmcli -t -f DEVICE,TYPE dev | grep ':wifi$' | head -n1 | cut -d: -f1)
if [ -z "$WIFI_DEV" ]; then
    echo "[ERROR] No Wi-Fi interface detected."
    exit 1
fi
echo "[INFO] Detected Wi-Fi interface: ${WIFI_DEV}"

# 2. Record current active Wi-Fi connection (to restore later)
PREV_CON=$(nmcli -t -f NAME,DEVICE con show --active | grep ":${WIFI_DEV}$" | head -n1 | cut -d: -f1)
echo "[INFO] Current active Wi-Fi connection: ${PREV_CON:-None}"

# Clean up any leftover temporary profile
nmcli con delete "$TMP_CON_NAME" 2>/dev/null || true

# 3. Create temporary profile with ESP32 cloned MAC
echo "[INFO] Creating temporary profile with cloned MAC ${ESP32_MAC}..."
nmcli connection add type wifi con-name "$TMP_CON_NAME" ifname "$WIFI_DEV" ssid "TANetRoaming" wifi.cloned-mac-address "$ESP32_MAC" >/dev/null

# 4. Activate connection
echo "[INFO] Connecting to TANetRoaming as ${ESP32_MAC}..."
nmcli connection up "$TMP_CON_NAME"

echo ""
echo "========================================================="
echo "  Connected to TANetRoaming using ESP32 MAC!"
echo "  Opening browser to trigger NSYSU login..."
echo "========================================================="
echo ""
echo "Please enter your student ID and password on the NSYSU web page."
echo ""

# Attempt to open browser
if command -v xdg-open &>/dev/null; then
    xdg-open "http://neverssl.com" 2>/dev/null || true
fi

read -p "Once you have successfully logged in on the web page, press [ENTER] to restore your laptop Wi-Fi: " dummy

echo ""
echo "[INFO] Restoring previous connection..."
nmcli con delete "$TMP_CON_NAME" >/dev/null 2>&1 || true

if [ -n "$PREV_CON" ]; then
    echo "[INFO] Reconnecting to ${PREV_CON}..."
    nmcli connection up "$PREV_CON" || true
fi

echo ""
echo "========================================================="
echo "  DONE! Your laptop is back to normal."
echo "  The ESP32 MAC (${ESP32_MAC}) is now authorized on NSYSU!"
echo "  Your ESP32 can now connect to TANetRoaming without password."
echo "========================================================="
