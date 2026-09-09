#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
One-Click Dedicated Flasher for YOLOv8n Cat Emotion V4 (Calibrated QAT).
Hermetic: All paths are self-contained relative to this script.
Target:
  - Firmware: firmware/output.img -> 0x00000000
  - Model:    model/cat_emotion_v4_calibrated_qat_vela.tflite -> 0x00B7B000
"""

import io
import os
import sys
import time
import math
import argparse
import serial
from xmodem import XMODEM
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
FW_PATH = BASE_DIR / "firmware" / "output.img"
MODEL_PATH = BASE_DIR / "model" / "cat_emotion_v4_calibrated_qat_vela.tflite"
MODEL_ADDR = 0x00B7B000
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 921600

send_bin_total_packets = 0

def progress_callback(total_packets, success_count, error_count):
    global send_bin_total_packets
    if send_bin_total_packets > 0:
        pct = min(100.0, (total_packets / send_bin_total_packets) * 100.0)
        bar = int(pct / 100.0 * 30)
        print(f"\r[{'█'*bar}{' '*(30-bar)}] {pct:5.1f}% ({total_packets}/{send_bin_total_packets}) err:{error_count}", end="", flush=True)
        if pct >= 100.0:
            print()

def wait_for_prompt(ser, prompt_substr, timeout=10.0):
    start = time.time()
    buf = ""
    ser.timeout = 0.5
    while time.time() - start < timeout:
        line = ser.readline().decode('utf-8', errors='ignore')
        if line:
            buf += line
            print("  [BL]:", line.strip())
            if prompt_substr in buf:
                return True
    return False

def flash(port=DEFAULT_PORT, baudrate=DEFAULT_BAUD):
    global send_bin_total_packets
    if not FW_PATH.exists():
        print(f"❌ Error: Firmware file not found: {FW_PATH}")
        return False
    if not MODEL_PATH.exists():
        print(f"❌ Error: Model file not found: {MODEL_PATH}")
        return False

    fw_size = FW_PATH.stat().st_size
    model_size = MODEL_PATH.stat().st_size

    print("==================================================================")
    print(" 🚀 Grove Vision AI V2 - Flashing YOLOv8n V4 Calibrated QAT Model")
    print(f" Port:        {port} @ {baudrate}")
    print(f" Firmware:    {FW_PATH.name} ({fw_size/1024:.1f} KB)")
    print(f" Model:       {MODEL_PATH.name} ({model_size/1024/1024:.2f} MB)")
    print(f" Model Addr:  0x{MODEL_ADDR:06X}")
    print("==================================================================")

    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)
    except Exception as e:
        print(f"❌ Error opening port {port}: {e}")
        return False

    print("\n[1/5] Resetting board into bootloader mode...")
    ser.dtr = False
    ser.rts = True
    time.sleep(0.05)
    ser.dtr = True
    ser.rts = False

    t0 = time.time()
    bootloader_caught = False
    while time.time() - t0 < 3.0:
        ser.write(b'1')
        chunk = ser.read(256)
        if b'xmodem' in chunk.lower() or b'waiting input key' in chunk.lower() or b'set x-modem flag' in chunk.lower():
            bootloader_caught = True
            break
        time.sleep(0.01)

    print("  Bootloader caught:", bootloader_caught)
    time.sleep(0.5)

    start = time.time()
    while time.time() - start < 5.0:
        ser.write(b'1\r\n')
        line = ser.readline().decode('utf-8', errors='ignore')
        if line:
            if "Send data using the xmodem protocol" in line:
                break
        time.sleep(0.05)

    time.sleep(0.5)
    ser.flushInput()

    def getc(size, timeout=60):
        ser.timeout = timeout
        return ser.read(size)

    def putc(data, timeout=60):
        ser.timeout = timeout
        return ser.write(data)

    modem = XMODEM(getc=getc, putc=putc, mode='xmodem')

    print(f"\n[2/5] Flashing Firmware {FW_PATH.name} ({fw_size} bytes)...")
    send_bin_total_packets = math.ceil(fw_size / 128)
    with open(FW_PATH, "rb") as f:
        ret = modem.send(f, callback=progress_callback)

    if not ret:
        print("\n❌ Failed to flash firmware!")
        ser.close()
        return False
    print("\n✅ Firmware flashed successfully!")

    wait_for_prompt(ser, "Do you want to end file transmission and reboot system", timeout=10.0)
    time.sleep(0.5)
    ser.flushInput()
    ser.write(b'n\r\n')
    time.sleep(0.5)

    print(f"\n[3/5] Setting Model Flash Address to 0x{MODEL_ADDR:06X}...")
    header = bytearray([0xC0, 0x5A] + list(MODEL_ADDR.to_bytes(4, 'little')) + list((0).to_bytes(4, 'little')) + [0x5A, 0xC0] + [0xFF] * (128 - 12))
    send_bin_total_packets = 1
    ser.flushInput()
    ret = modem.send(io.BytesIO(header), callback=progress_callback)
    if not ret:
        print("\n❌ Failed to send model preamble!")
        ser.close()
        return False
    print("\n✅ Flash offset configured!")

    wait_for_prompt(ser, "Do you want to end file transmission and reboot system", timeout=10.0)
    time.sleep(0.5)
    ser.flushInput()
    ser.write(b'n\r\n')
    time.sleep(0.5)

    print(f"\n[4/5] Flashing Vela Model {MODEL_PATH.name} ({model_size} bytes)...")
    send_bin_total_packets = math.ceil(model_size / 128)
    ser.flushInput()
    with open(MODEL_PATH, "rb") as f:
        ret = modem.send(f, callback=progress_callback)

    if not ret:
        print("\n❌ Failed to flash model!")
        ser.close()
        return False
    print("\n✅ Model flashed successfully!")

    print("\n[5/5] Instructing board to reboot...")
    wait_for_prompt(ser, "Do you want to end file transmission and reboot system", timeout=10.0)
    time.sleep(0.2)
    ser.write(b'y\r\n')
    print("✅ Reboot command sent!")

    print("\n--- Board Startup Output (4s) ---")
    ser.timeout = 0.5
    start = time.time()
    while time.time() - start < 4.0:
        line = ser.readline().decode('utf-8', errors='ignore')
        if line.strip():
            print("  ", line.strip())

    ser.close()
    print("\n🎉 V4 Model successfully flashed and running on Grove Vision AI V2!")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="One-click flasher for YOLOv8n V4 Calibrated QAT on Grove Vision AI V2")
    parser.add_argument("port", nargs="?", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    parser.add_argument("baud", nargs="?", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    args = parser.parse_args()
    success = flash(args.port, args.baud)
    sys.exit(0 if success else 1)
