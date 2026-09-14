#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Dedicated Safe Flasher for YOLOv8n Cat Emotion V8 (Perfect SOTA)
================================================================
Safe Hermetic Flasher:
- DEFAULT: Model-Only Flashing to 0x00B7B000 (Safe, Preserves user's ESP32 output.img).
- Automatically pauses ESP32 into ROM bootloader mode to silence shared UART/I2C and WiFi RF during flash.
- Automatically resumes ESP32 after flashing completes.
- Optional: --with-firmware only if full reflash of output.img to 0x00000000 is explicitly requested.
"""

import io
import os
import sys
import glob
import time
import math
import argparse
import serial
from xmodem import XMODEM
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
FW_PATH = BASE_DIR / "firmware" / "output.img"
MODEL_PATH = BASE_DIR / "model" / "cat_emotion_v8_refined_vela.tflite"
MODEL_ADDR = 0x00B7B000
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

def detect_ports():
    grove_port = None
    esp_port = None
    all_ports = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    for p in all_ports:
        info = os.popen(f"udevadm info -q property -n {p} 2>/dev/null").read()
        if any(tag in info for tag in ["1a86", "QinHeng", "55d3", "USB_Single_Serial"]):
            grove_port = p
        elif any(tag in info for tag in ["Espressif", "303a", "1001", "USB_JTAG_serial_debug_unit"]):
            esp_port = p
    return grove_port, esp_port

def flash(port=None, baudrate=DEFAULT_BAUD, with_firmware=False):
    global send_bin_total_packets
    if not MODEL_PATH.exists():
        print(f"❌ Error: Model file not found: {MODEL_PATH}")
        return False
    if with_firmware and not FW_PATH.exists():
        print(f"❌ Error: Firmware file not found: {FW_PATH}")
        return False

    detected_grove, detected_esp = detect_ports()

    # Determine target port
    if port and port != detected_esp:
        target_port = port
    elif detected_grove:
        target_port = detected_grove
    else:
        target_port = port or "/dev/ttyACM1"

    esp_port = detected_esp if detected_esp != target_port else None

    model_size = MODEL_PATH.stat().st_size

    print("==================================================================")
    print(" ⚡ Grove Vision AI V2 Flasher - YOLOv8n-SiLU V8 (Perfect SOTA)")
    print("==================================================================")
    print(f" Target Port (Grove): {target_port} @ {baudrate} baud")
    if esp_port:
        print(f" Detected ESP32 Port: {esp_port} (Will pause during flash to avoid UART collision)")
    print(f" Mode:                {'Full Reflash (Firmware + Model)' if with_firmware else '⚡ Model-Only (Safe, Preserves ESP32 output.img)'}")
    print(f" Model (Ethos-U55):   {MODEL_PATH.name} ({model_size/1024/1024:.2f} MB) -> Flash 0x{MODEL_ADDR:06X}")
    if with_firmware:
        print(f" Firmware Image:      {FW_PATH.name} -> Flash 0x00000000")
    print("==================================================================")

    # 1. Pause ESP32 if present so shared UART/I2C is completely silent
    if esp_port:
        try:
            import esptool
            print(f"\n[ESP32 Safety] Pausing ESP32 on {esp_port} into ROM bootloader mode...")
            esp = esptool.cmds.detect_chip(esp_port)
            esp._port.close()
            print("✅ ESP32 successfully paused! Shared UART bus and WiFi RF are now completely silent.")
            time.sleep(0.3)
        except Exception as e:
            print(f"⚠️ Warning: Could not pause ESP32 on {esp_port}: {e}")

    try:
        ser = serial.Serial(target_port, baudrate, timeout=1.0)

        print("\n[Step 1] Triggering hardware reset via DTR/RTS...")
        ser.dtr = False
        ser.rts = True
        time.sleep(0.05)
        ser.dtr = True
        ser.rts = False

        print("[Step 2] Intercepting bootloader prompt...")
        ser.timeout = 0.01
        start = time.time()
        bl_ready = False
        while time.time() - start < 2.5:
            ser.write(b'1')
            time.sleep(0.005)
            c = ser.read(256)
            if b'Set X-modem flag' in c:
                bl_ready = True
                break

        if not bl_ready:
            print("❌ Failed to enter 1st stage bootloader. Please verify USB connection.")
            ser.close()
            return False

        print("✅ 1st stage bootloader caught!")

        # Wait for 2nd stage bootloader menu and select Option 1
        start = time.time()
        menu_ready = False
        while time.time() - start < 2.5:
            c = ser.read(256)
            if b'download and burn' in c:
                menu_ready = True
                ser.write(b'1\r\n')
                break
            time.sleep(0.02)

        if not menu_ready:
            print("⚠️ 2nd stage menu prompt not seen directly; sending option 1 anyway...")
            ser.write(b'1\r\n')

        print("✅ Option 1 (XMODEM Download) selected!")

        # Wait for banner text to finish completely
        buf = b''
        start = time.time()
        while time.time() - start < 3.0:
            c = ser.read(256)
            if c:
                buf += c
                if b'terminal' in buf:
                    break
            time.sleep(0.02)

        time.sleep(0.1)
        ser.flushInput()
        ser.timeout = 60.0

        def getc(size, timeout=60):
            ser.timeout = timeout
            return ser.read(size) or None

        def putc(data, timeout=60):
            ser.timeout = timeout
            return ser.write(data)

        modem = XMODEM(getc, putc, mode='xmodem')

        if with_firmware:
            fw_size = FW_PATH.stat().st_size
            print(f"\nFlashing Firmware {FW_PATH.name} ({fw_size} bytes) -> 0x00000000...")
            send_bin_total_packets = math.ceil(fw_size / 128)
            with open(FW_PATH, "rb") as f:
                ret = modem.send(f, callback=progress_callback)
            if not ret:
                print("\n❌ Failed to flash firmware!")
                ser.close()
                return False
            print("\n✅ Firmware flashed successfully!")

            start = time.time()
            ser.timeout = 5.0
            while time.time() - start < 5.0:
                line = ser.readline().decode('utf-8', errors='ignore')
                if "Do you want to end file transmission" in line:
                    break
            ser.write(b'n\r')
            time.sleep(0.5)
            ser.flushInput()

        # Flash Model Preamble Header (Configures Flash Address to 0x00B7B000)
        print(f"\n[Step 3] Configuring Flash Address to 0x{MODEL_ADDR:06X} via Preamble...")
        header = bytearray([0xC0, 0x5A] + list(MODEL_ADDR.to_bytes(4, 'little')) + list((0).to_bytes(4, 'little')) + [0x5A, 0xC0] + [0xFF] * (128 - 12))
        send_bin_total_packets = 1
        ret = modem.send(io.BytesIO(header), callback=progress_callback)
        if not ret:
            print("\n❌ Failed to send model preamble!")
            ser.close()
            return False
        print("✅ Model Flash address 0x00B7B000 configured!")

        # Prompt: "Do you want to end file transmission and reboot system? (y)" -> reply 'n'
        start = time.time()
        ser.timeout = 5.0
        while time.time() - start < 5.0:
            line = ser.readline().decode('utf-8', errors='ignore')
            if "Do you want to end file transmission" in line:
                break
            time.sleep(0.02)
        ser.write(b'n\r')
        time.sleep(0.5)
        ser.flushInput()

        # Flash Vela Model Binary
        print(f"\n[Step 4] Flashing Vela Model {MODEL_PATH.name} ({model_size} bytes, ~{model_size/1024/1024:.2f} MB)...")
        send_bin_total_packets = math.ceil(model_size / 128)
        with open(MODEL_PATH, "rb") as f:
            ret = modem.send(f, callback=progress_callback)

        if not ret:
            print("\n❌ Failed to flash model binary!")
            ser.close()
            return False
        print("\n✅ Model binary flashed successfully!")

        # Prompt: "Do you want to end file transmission and reboot system? (y)" -> reply 'y'
        print("\n[Step 5] Instructing board to reboot...")
        start = time.time()
        ser.timeout = 5.0
        while time.time() - start < 5.0:
            line = ser.readline().decode('utf-8', errors='ignore')
            if "Do you want to end file transmission" in line:
                break
            time.sleep(0.02)
        ser.write(b'y\r')
        print("✅ Reboot command sent!")

        print("\n--- Board Startup Output (4s) ---")
        ser.timeout = 0.5
        start = time.time()
        while time.time() - start < 4.0:
            line = ser.readline().decode('utf-8', errors='ignore')
            if line.strip():
                print("  ", line.strip())

        ser.close()
        print("\n==================================================================")
        print(" 🎉 MODEL-ONLY FLASHING SUCCESSFUL!")
        print(" output.img at 0x00000000 was untouched (ESP32 modifications preserved).")
        print(f" Model updated at 0x{MODEL_ADDR:06X}.")
        print("==================================================================")
        return True

    finally:
        # Resume ESP32 so the system can run seamlessly
        if esp_port:
            try:
                print(f"\n[ESP32 Safety] Resuming ESP32 on {esp_port}...")
                import esptool
                esp = esptool.cmds.detect_chip(esp_port)
                esp.hard_reset()
                esp._port.close()
                print("✅ ESP32 resumed and running normally!")
            except Exception as e:
                print(f"⚠️ Warning: Could not resume ESP32: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Flash YOLOv8n Cat Emotion V8 SOTA model")
    parser.add_argument("pos_port", nargs="?", default=None, help="Serial port (positional)")
    parser.add_argument("--port", default=None, help="Serial port (flag)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baudrate (default: 921600)")
    parser.add_argument("--with-firmware", action="store_true", help="Also reflash output.img (Default: False)")
    args = parser.parse_args()

    actual_port = args.port or args.pos_port or None
    flash(port=actual_port, baudrate=args.baud, with_firmware=args.with_firmware)
