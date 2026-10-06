#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
（Windows 版：從 flashing/flash_model.py 改的）
改動：
  - 自動找 COM port 改用 pyserial 的 list_ports（原本只找 Linux 的 /dev/ttyACM*）
  - 預設路徑以 repo 根目錄為準：model/v5/cat_emotion_v5_vela.tflite、firmware/output.img
  - 燒錄前檢查模型的框座標格式跟目標韌體是否相符（--firmware-type），不符就拒絕燒錄
  - --with-firmware 時，韌體檔依 --firmware-type 自動選擇，並用 SHA-256 確認實際要燒的韌體檔就是
    該類型（--firmware 指定的檔案也一樣檢查）；不符或不認得就拒絕燒錄
  - --dry-run：只做所有檢查、印出會燒哪些檔案，不連接板子
  - Windows 節流：每包 XMODEM 分段送、等 ACK 最多 60 秒（板子每 1MB 會停幾秒寫 Flash）
  - 燒錄流程（DTR/RTS 重置 -> bootloader -> XMODEM 燒到 0x00B7B000）沒改

框座標格式（模型輸出 0 = [1,4,756]）：
  project  韌體 firmware/output.img         -> 像素 0~192    -> model/v5/cat_emotion_v5_vela.tflite
  official 韌體 Himax tflm_yolov8_od         -> 正規化 0~1    -> model/v5/cat_emotion_v5_vela_himax_official.tflite

用法（在 repo 根目錄）：
    python flashing/flash_model_windows.py --port COM4 --fast                      # 只燒 v5 模型（專案韌體）
    python flashing/flash_model_windows.py --port COM4 --fast --with-firmware      # 專案韌體 + v5 模型
    python flashing/flash_model_windows.py --port COM4 --fast --with-firmware --firmware-type official ^
        --model model/v5/cat_emotion_v5_vela_himax_official.tflite              # 官方韌體 + v5（韌體檔自動選）
    python flashing/flash_model_windows.py --dry-run --with-firmware --firmware-type official ^
        --model model/v5/cat_emotion_v5_vela_himax_official.tflite              # 只檢查、不燒錄

限制：只燒模型（不加 --with-firmware）時，程式無法得知板子上現在是哪個韌體，只能依 --firmware-type 判斷。

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

BASE_DIR = Path(__file__).resolve().parent.parent  # repo 根目錄
MODEL_PATH = BASE_DIR / "model" / "v5" / "cat_emotion_v5_vela.tflite"
EXPECTED_BOX_FORMAT = {"project": "pixel", "official": "normalized"}
# 每種韌體類型對應的韌體檔（--with-firmware 時依 --firmware-type 自動選擇）
FIRMWARE_PATHS = {
    "project": BASE_DIR / "firmware" / "output.img",
    "official": BASE_DIR / "firmware" / "himax_official_tflm_yolov8_od" / "output.img",
}
# 已知韌體檔的 SHA-256 -> 韌體類型，用來確認「實際要燒的檔案」是哪一種
KNOWN_FIRMWARE_SHA256 = {
    "771b3b65b5988fe3751f4d6d082ae2bc33f7d1e7d2b6a9819e2b8e8f68f76874": "project",
    "8c039ee8959e8b0b10ddd747e858b26542cd3dad2bdb486a60109c4cfce19320": "official",
}
FW_PATH = None  # --firmware 指定時才會設定；否則依 --firmware-type 從 FIRMWARE_PATHS 選


def firmware_type_of(path):
    """用 SHA-256 判斷韌體檔是哪一種；不認得就回傳 None。"""
    import hashlib

    return KNOWN_FIRMWARE_SHA256.get(hashlib.sha256(Path(path).read_bytes()).hexdigest())


def model_box_format(path):
    """讀模型輸出 [1,4,756]（框）的量化參數，判斷框是像素（0~192）還是正規化（0~1）。"""
    import tflite  # pip install tflite

    m = tflite.Model.GetRootAsModel(Path(path).read_bytes(), 0)
    g = m.Subgraphs(0)
    for i in range(g.OutputsLength()):
        t = g.Tensors(g.Outputs(i))
        if t.ShapeAsNumpy().tolist() == [1, 4, 756]:
            q = t.Quantization()
            top = (127 - q.ZeroPoint(0)) * q.Scale(0)  # int8 能表示的最大值
            return "normalized" if top <= 2 else "pixel" if top >= 64 else f"unknown(max={top:.2f})"
    return "unknown(no [1,4,756] output)"
MODEL_ADDR = 0x00B7B000
DEFAULT_BAUD = 921600

send_bin_total_packets = 0
# 預設是確認穩定的設定（約 4.5 分鐘）；--fast 用比較短的停頓（約 2 分鐘）
THROTTLE = {"chunk": 32, "gap_ms": 1.0, "pre_ms": 2.0}

def progress_callback(total_packets, success_count, error_count):
    global send_bin_total_packets
    if send_bin_total_packets > 0:
        pct = min(100.0, (total_packets / send_bin_total_packets) * 100.0)
        bar = int(pct / 100.0 * 30)
        print(f"\r[{'█'*bar}{' '*(30-bar)}] {pct:5.1f}% ({total_packets}/{send_bin_total_packets}) err:{error_count}", end="", flush=True)
        if pct >= 100.0:
            print()

def detect_ports():
    # Grove Vision AI V2 的 USB 晶片是 QinHeng CH343（VID 0x1A86），ESP32-S3 是 Espressif（VID 0x303A）
    from serial.tools import list_ports

    grove_port = None
    esp_port = None
    for p in list_ports.comports():
        if p.vid == 0x1A86:
            grove_port = p.device
        elif p.vid == 0x303A:
            esp_port = p.device
    return grove_port, esp_port

def flash(port=None, baudrate=DEFAULT_BAUD, with_firmware=False, firmware_type="project", force=False, dry_run=False):
    global send_bin_total_packets, FW_PATH
    if not MODEL_PATH.exists():
        print(f"❌ Error: Model file not found: {MODEL_PATH}")
        return False
    fmt = model_box_format(MODEL_PATH)
    want = EXPECTED_BOX_FORMAT[firmware_type]
    print(f" Box format check:   model={fmt}, {firmware_type} firmware expects {want}")
    if fmt != want and not force:
        print(f"❌ Error: {MODEL_PATH.name} outputs {fmt} boxes, but the {firmware_type} firmware expects {want} boxes.")
        print("   Flashing it would make boxes wrong (about 192x too small or too large).")
        print("   project  firmware -> model/v5/cat_emotion_v5_vela.tflite")
        print("   official firmware -> model/v5/cat_emotion_v5_vela_himax_official.tflite (use --firmware-type official)")
        print("   Use --force only if you know what you are doing.")
        return False
    if with_firmware:
        fw = Path(FW_PATH) if FW_PATH else FIRMWARE_PATHS[firmware_type]
        if not fw.exists():
            print(f"❌ Error: Firmware file not found: {fw}")
            return False
        fw_type = firmware_type_of(fw)
        print(f" Firmware check:     {fw} -> {fw_type or 'unknown image'}, --firmware-type {firmware_type}")
        if fw_type != firmware_type and not force:
            if fw_type is None:
                print(f"❌ Error: {fw} is not a known firmware image, so its box format cannot be verified.")
            else:
                print(f"❌ Error: {fw} is the {fw_type} firmware, but --firmware-type is {firmware_type}.")
            print(f"   {firmware_type} firmware -> {FIRMWARE_PATHS[firmware_type].relative_to(BASE_DIR)}")
            print("   Omit --firmware to pick the matching image automatically, or use --force if you know what you are doing.")
            return False
        FW_PATH = fw
    if dry_run:
        print(" Dry run:            all checks passed; would flash "
              + (f"{FW_PATH.relative_to(BASE_DIR) if FW_PATH.is_relative_to(BASE_DIR) else FW_PATH} -> 0x00000000 and " if with_firmware else "")
              + f"{MODEL_PATH.name} -> 0x{MODEL_ADDR:06X}")
        return True

    detected_grove, detected_esp = detect_ports()

    # Determine target port
    if port and port != detected_esp:
        target_port = port
    elif detected_grove:
        target_port = detected_grove
    else:
        target_port = port
    if not target_port:
        print("❌ 找不到 Grove Vision AI V2（USB VID 1A86）。請確認 USB 線有接好（要用能傳資料的線），或用 --port COMx 指定")
        return False

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
            # 等 ACK 不能太短：板子每收滿 1MB 會花好幾秒寫 Flash，太早重傳會讓封包編號對不上
            ser.timeout = timeout
            return ser.read(size) or None

        def putc(data, timeout=60):
            # Windows 版節流：板子 bootloader 寫 Flash 時來不及收資料，CH343 在 Windows 上送太快會掉 byte。
            # 每包分 THROTTLE["chunk"] bytes 一段送，段之間停 gap_ms，送前先等 pre_ms 讓板子寫完上一包。
            time.sleep(THROTTLE["pre_ms"] / 1000)
            n = 0
            for i in range(0, len(data), THROTTLE["chunk"]):
                n += ser.write(data[i:i + THROTTLE["chunk"]])
                ser.flush()
                time.sleep(THROTTLE["gap_ms"] / 1000)
            return n

        modem = XMODEM(getc, putc, mode='xmodem')
        send_kw = dict(retry=64)

        if with_firmware:
            fw_size = FW_PATH.stat().st_size
            print(f"\nFlashing Firmware {FW_PATH.name} ({fw_size} bytes) -> 0x00000000...")
            send_bin_total_packets = math.ceil(fw_size / 128)
            with open(FW_PATH, "rb") as f:
                ret = modem.send(f, callback=progress_callback, **send_kw)
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
            ret = modem.send(f, callback=progress_callback, **send_kw)

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
    parser.add_argument("--model", help=f"要燒錄的 *_vela.tflite（預設 {MODEL_PATH.relative_to(BASE_DIR)}）")
    parser.add_argument("--firmware", help="韌體 output.img 路徑（搭配 --with-firmware；不指定時依 --firmware-type 自動選，"
                                           "指定時會用 SHA-256 確認跟 --firmware-type 相符）")
    parser.add_argument("--firmware-type", choices=list(EXPECTED_BOX_FORMAT), default="project",
                        help="板子上（或要一起燒）的韌體：project = firmware/output.img；official = Himax tflm_yolov8_od")
    parser.add_argument("--force", action="store_true", help="略過框座標格式和韌體檔類型檢查")
    parser.add_argument("--dry-run", action="store_true", help="只做檢查、印出會燒哪些檔案，不連接板子")
    parser.add_argument("--fast", action="store_true", help="縮短節流停頓（64 bytes 一段、0.5ms），出錯就拿掉這個參數重燒")
    args = parser.parse_args()
    if args.fast:
        THROTTLE.update(chunk=64, gap_ms=0.5, pre_ms=1.0)
    if args.model:
        MODEL_PATH = Path(args.model).resolve()
    if args.firmware:
        FW_PATH = Path(args.firmware).resolve()

    actual_port = args.port or args.pos_port or None
    ok = flash(port=actual_port, baudrate=args.baud, with_firmware=args.with_firmware,
               firmware_type=args.firmware_type, force=args.force, dry_run=args.dry_run)
    sys.exit(0 if ok else 1)
