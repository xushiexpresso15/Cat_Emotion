#!/usr/bin/env python3
"""
Grove Vision AI V2 貓咪情緒即時畫面（OpenCV 視窗版，不經過網頁）

搭配 Himax 官方 tflm_yolov8_od 韌體：板子開機後會自動連續推論，
每一幀從 USB 送一行 JSON（框 [x左上, y左上, w, h, score, target] + base64 JPEG）。
畫面速度上限約 7 FPS（受限於 921600 bps 序列埠頻寬）。

用法（在 webserver 資料夾）：
    ..\\train_env\\Scripts\\python.exe board_view_cv.py --port COM4
按 q 或 Esc 關閉視窗，按 s 存目前畫面。
"""
import argparse
import base64
import json
import threading
import time

import cv2
import numpy as np
import serial

EMOTIONS = ["angry", "focus", "relax", "scared"]
COLORS = {"angry": (38, 38, 220), "focus": (235, 99, 37), "relax": (74, 163, 22), "scared": (234, 51, 147)}  # BGR

latest = {"jpg": None, "boxes": [], "ms": 0, "seq": 0}
lock = threading.Lock()


def reader(port, baud, stop):
    while not stop.is_set():
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, baud, 0.05
            ser.dtr = ser.rts = False  # 不要觸發板子重開
            ser.open()
            ser.reset_input_buffer()
            buf = b""
            while not stop.is_set():
                buf += ser.read(ser.in_waiting or 1)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line.startswith(b"{") or b'"INVOKE"' not in line:
                        continue
                    try:
                        data = json.loads(line).get("data") or {}
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(data, dict) or "image" not in data:
                        continue
                    tick = data.get("algo_tick") or [[0]]
                    with lock:
                        latest["jpg"] = base64.b64decode(data["image"])
                        latest["boxes"] = data.get("boxes", [])
                        latest["ms"] = round(tick[0][0] / 400000)  # CPU 週期數（400 MHz）-> 毫秒
                        latest["seq"] += 1
        except Exception as e:
            print("序列埠錯誤，2 秒後重試：", e)
            time.sleep(2)


def draw(img, boxes, scale):
    for b in boxes:
        x, y, w, h, score, target = (list(b) + [0] * 6)[:6]
        name = EMOTIONS[target] if 0 <= target < len(EMOTIONS) else f"#{target}"
        c = COLORS.get(name, (0, 200, 255))
        p1 = (int(x * scale), int(y * scale))
        p2 = (int((x + w) * scale), int((y + h) * scale))
        cv2.rectangle(img, p1, p2, c, 3)
        label = f"{name} {score}%"
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.8, 2)
        ty = max(p1[1], th + 8)
        cv2.rectangle(img, (p1[0], ty - th - 8), (p1[0] + tw + 8, ty), c, -1)
        cv2.putText(img, label, (p1[0] + 4, ty - 5), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--size", type=int, default=640, help="視窗畫面邊長（板子送的是 240x240，放大顯示）")
    args = ap.parse_args()

    stop = threading.Event()
    threading.Thread(target=reader, args=(args.port, args.baud, stop), daemon=True).start()

    win = "Cat Emotion - Grove Vision AI V2 (q: quit, s: save)"
    cv2.namedWindow(win, cv2.WINDOW_AUTOSIZE)
    shown, times = -1, []
    blank = np.zeros((args.size, args.size, 3), np.uint8)
    cv2.putText(blank, "waiting for board...", (20, args.size // 2), cv2.FONT_HERSHEY_SIMPLEX, 1, (200, 200, 200), 2)
    frame = blank
    while True:
        with lock:
            seq, jpg, boxes, ms = latest["seq"], latest["jpg"], list(latest["boxes"]), latest["ms"]
        if seq != shown and jpg:
            shown = seq
            img = cv2.imdecode(np.frombuffer(jpg, np.uint8), cv2.IMREAD_COLOR)
            if img is not None:
                scale = args.size / img.shape[1]
                frame = cv2.resize(img, (args.size, int(img.shape[0] * scale)), interpolation=cv2.INTER_LINEAR)
                draw(frame, boxes, scale)
                now = time.time()
                times = [t for t in times if now - t < 2] + [now]
                fps = (len(times) - 1) / max(now - times[0], 1e-6) if len(times) > 1 else 0
                info = f"{fps:.1f} FPS | board {ms} ms | " + (", ".join(
                    f"{EMOTIONS[b[5]] if 0 <= b[5] < 4 else b[5]} {b[4]}%" for b in boxes) or "no cat")
                cv2.rectangle(frame, (0, 0), (frame.shape[1], 34), (0, 0, 0), -1)
                cv2.putText(frame, info, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
        cv2.imshow(win, frame)
        k = cv2.waitKey(15) & 0xFF
        if k in (ord("q"), 27) or cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
            break
        if k == ord("s"):
            fn = time.strftime("capture_%Y%m%d_%H%M%S.jpg")
            cv2.imwrite(fn, frame)
            print("已存檔：", fn)
    stop.set()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
