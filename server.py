import os
import sys
import time
import json
import base64
import binascii
import argparse
import threading
import webbrowser
from collections import deque
from flask import Flask, request, jsonify, Response
import serial
import serial.tools.list_ports

app = Flask(__name__)

# --- Global State ---
state_lock = threading.RLock()
server_started_at = time.time()
browser_sessions = {}
browser_seen = False
browser_empty_since = None
shutdown_started = threading.Event()
current_mode = None
serial_conn = None
serial_thread = None
serial_stop_event = None

latest_frame = b''
latest_result = {"boxes": []}
fps = 0.0
frame_count = 0
last_fps_time = time.time()
connected = False
invoke_event_count = 0
last_frame_at = None
last_stream_error = None

# Logs ring buffer (max 200)
logs = deque(maxlen=200)

EMOTION_MAP = {
    0: "angry",
    1: "focus",
    2: "relax",
    3: "scared"
}


class SSCMAJsonStream:
    """Incrementally extract JSON objects from the SSCMA serial stream.

    Camera events contain a large Base64 JPEG and routinely span many serial
    reads.  Keeping the scan state between reads is essential: rescanning the
    accumulated buffer repeats the opening brace and makes its nesting depth
    grow forever.
    """

    MAX_MESSAGE_BYTES = 2 * 1024 * 1024

    def __init__(self):
        self._chars = []
        self._depth = 0
        self._in_string = False
        self._escaped = False

    def reset(self):
        self._chars.clear()
        self._depth = 0
        self._in_string = False
        self._escaped = False

    def feed(self, chunk):
        messages = []
        error = None
        for char in chunk:
            if self._depth == 0:
                if char != '{':
                    continue
                self._chars = [char]
                self._depth = 1
                continue

            self._chars.append(char)
            if self._in_string:
                if self._escaped:
                    self._escaped = False
                elif char == '\\':
                    self._escaped = True
                elif char == '"':
                    self._in_string = False
            elif char == '"':
                self._in_string = True
            elif char == '{':
                self._depth += 1
            elif char == '}':
                self._depth -= 1
                if self._depth == 0:
                    messages.append(''.join(self._chars))
                    self.reset()

            if len(self._chars) > self.MAX_MESSAGE_BYTES:
                error = f"Discarded oversized SSCMA message (>{self.MAX_MESSAGE_BYTES // 1024} KB)"
                self.reset()

        return messages, error


# --- Background Threads ---
def add_log(log_type, msg):
    with state_lock:
        logs.append({
            "ts": int(time.time() * 1000),
            "type": log_type,
            "msg": msg
        })

def heartbeat_checker():
    """Stop the local server after the last browser page has gone away."""
    global browser_empty_since
    while not shutdown_started.wait(2):
        now = time.time()
        with state_lock:
            stale = [client_id for client_id, seen_at in browser_sessions.items()
                     if now - seen_at > 10]
            for client_id in stale:
                browser_sessions.pop(client_id, None)

            # Give the auto-opened browser time to load.  Once a page has
            # registered, only its explicit/timeout removal can stop us.
            if browser_sessions:
                browser_empty_since = None
            elif browser_seen and browser_empty_since is None:
                browser_empty_since = now

            should_stop = ((browser_seen and browser_empty_since is not None and
                            now - browser_empty_since > 30) or
                           (not browser_seen and now - server_started_at > 300))

        if should_stop:
            print("No active browser session. Shutting down.")
            shutdown_started.set()
            cleanup_usb_connection()
            os._exit(0)

def serial_reader_thread(conn, stop_event):
    """Read SSCMA response/event JSON, including multi-read JPEG messages."""
    global latest_frame, latest_result, frame_count, fps, last_fps_time, connected

    parser = SSCMAJsonStream()

    while not stop_event.is_set():
        try:
            if conn.is_open and conn.in_waiting:
                chunk = conn.read(conn.in_waiting).decode('utf-8', errors='ignore')
                messages, parser_error = parser.feed(chunk)
                for json_str in messages:
                    _process_invoke(json_str)
                if parser_error:
                    add_log("error", parser_error)
            else:
                time.sleep(0.005)
        except serial.SerialException as e:
            if stop_event.is_set():
                break
            add_log("error", f"Serial error: {e}")
            with state_lock:
                connected = False
            break
        except Exception as e:
            add_log("error", f"Read error: {e}")
            time.sleep(0.1)


def _process_invoke(json_str):
    """Process SSCMA INVOKE events and retain only valid JPEG frames.

    SSCMA uses type 0 for the command acknowledgement and type 1 for the
    asynchronous inference event.  The latter is where ``data.image`` lives.
    """
    global latest_frame, latest_result, frame_count, fps, last_fps_time, connected
    global invoke_event_count, last_frame_at, last_stream_error
    try:
        obj = json.loads(json_str)
    except json.JSONDecodeError:
        return

    if obj.get("name") != "INVOKE":
        return

    response_type = obj.get("type")
    code = obj.get("code", 0)
    if response_type == 0:
        # This is only the command acknowledgement, never a video frame.
        if code != 0:
            add_log("error", f"INVOKE command rejected (code {code})")
        return
    if response_type != 1:
        return
    if code != 0:
        add_log("error", f"INVOKE event failed (code {code})")
        return

    data_obj = obj.get("data", {})
    if not isinstance(data_obj, dict):
        return

    frame = None
    decode_error = None
    image_data = data_obj.get("image")
    if image_data:
        try:
            if not isinstance(image_data, str):
                raise ValueError("image payload is not text")
            if image_data.startswith("data:image/"):
                image_data = image_data.split(",", 1)[-1]
            frame = base64.b64decode("".join(image_data.split()))
            if len(frame) < 4 or not frame.startswith(b'\xff\xd8'):
                raise ValueError("decoded image does not start with JPEG SOI")
            idx = frame.find(b'\xff\xd9')
            if idx != -1:
                frame = frame[:idx + 2]
            else:
                raise ValueError("decoded image has no JPEG EOI marker")
        except (ValueError, TypeError, binascii.Error) as e:
            decode_error = f"Image decode failed: {e}"

    # --- Parse detection boxes (Swift YOLO / FOMO detection models) ---
    boxes = data_obj.get("boxes")
    if isinstance(boxes, list):
        # SSCMA-Micro documents boxes as [x, y, w, h, score, target], while
        # some firmware versions emit object dictionaries. Support both.
        normalized_boxes = []
        for box in boxes:
            if isinstance(box, dict):
                normalized_boxes.append(box)
            elif isinstance(box, list) and len(box) >= 6:
                normalized_boxes.append({
                    "x": box[0], "y": box[1], "w": box[2], "h": box[3],
                    "score": box[4], "target": box[5],
                })
    else:
        # --- Fallback: classification model output (data.classes) ---
        # Classification models output e.g. [{"class": 0, "score": 87}, ...]
        # Convert to a synthetic full-frame box so the UI can display results.
        CLASS_NAME_TO_IDX = {"angry": 0, "focus": 1, "relax": 2, "scared": 3}
        classes = data_obj.get("classes")
        if isinstance(classes, list) and classes:
            normalized_boxes = []
            for c in classes:
                if isinstance(c, dict):
                    # class may be an int index or a string name
                    cls_raw = c.get("class", c.get("target", -1))
                    if isinstance(cls_raw, str):
                        target = CLASS_NAME_TO_IDX.get(cls_raw.lower(), -1)
                    else:
                        target = int(cls_raw) if cls_raw is not None else -1
                    score = c.get("score", 0)
                    if target >= 0 and score > 0:
                        # Full-frame synthetic box (centre at 96,96; 192×192)
                        normalized_boxes.append({
                            "x": 96, "y": 96, "w": 192, "h": 192,
                            "score": score, "target": target,
                        })
            # Only keep the highest-confidence class
            if normalized_boxes:
                normalized_boxes = [max(normalized_boxes, key=lambda b: b["score"])]
        else:
            normalized_boxes = None

    log_messages = []
    with state_lock:
        invoke_event_count += 1
        connected = True
        if frame:
            latest_frame = frame
            last_frame_at = time.time()
            last_stream_error = None
        elif decode_error:
            if last_stream_error != decode_error:
                log_messages.append(("error", decode_error))
            last_stream_error = decode_error
        elif not latest_frame:
            missing_image = "INVOKE event contains no image; verify show parameter and camera input"
            if last_stream_error != missing_image:
                log_messages.append(("warn", missing_image))
            last_stream_error = missing_image

        if normalized_boxes is not None:
            latest_result["boxes"] = normalized_boxes
            if normalized_boxes:
                best_box = max(normalized_boxes, key=lambda x: x.get("score", 0))
                emo = EMOTION_MAP.get(best_box.get("target", -1), "unknown")
                score = best_box.get("score", 0)
                log_messages.append(("detect", f"{emo} ({score}%)"))
                if score < 40:
                    log_messages.append(("warn", f"Low confidence: {score}%"))

        # FPS must measure decodable camera frames, not inference events.  An
        # event-only stream means the camera image was not requested.
        if frame:
            frame_count += 1
            now = time.time()
            if now - last_fps_time >= 1.0:
                fps = frame_count / (now - last_fps_time)
                frame_count = 0
                last_fps_time = now
    # add_log also uses state_lock, so logging must happen after releasing it.
    for log_type, message in log_messages:
        add_log(log_type, message)


def cleanup_usb_connection():
    """Detach and stop the active USB reader without holding state_lock."""
    global serial_conn, serial_thread, serial_stop_event, connected
    with state_lock:
        conn = serial_conn
        thread = serial_thread
        stop_event = serial_stop_event
        serial_conn = None
        serial_thread = None
        serial_stop_event = None
        connected = False

    if stop_event:
        stop_event.set()
    if conn:
        try:
            conn.close()  # Wake a pending serial read before joining.
        except serial.SerialException:
            pass
    if thread and thread is not threading.current_thread():
        thread.join(timeout=2.0)

# --- Routes ---
@app.route('/')
def index():
    return HTML_CONTENT

@app.route('/api/ports')
def get_ports():
    """只回傳有實際裝置連接的串口（過濾虛擬/空接口）"""
    ports = serial.tools.list_ports.comports()
    port_list = []
    for p in ports:
        # 過濾掉沒有實際裝置的接口（description 為 n/a 或與 device 相同）
        desc = p.description or ""
        if desc.lower() == "n/a" or desc == "" or desc == p.device:
            continue
        # 過濾掉純虛擬串口 /dev/ttyS*（沒有 USB VID/PID 的）
        if p.vid is None and p.device.startswith('/dev/ttyS'):
            continue
        port_list.append({
            "device": p.device,
            "description": desc,
            "manufacturer": p.manufacturer or "",
            "hwid": p.hwid or ""
        })
    return jsonify(port_list)

@app.route('/api/connect', methods=['POST'])
def connect():
    global current_mode, serial_conn, serial_thread, serial_stop_event, connected, latest_frame, latest_result
    global invoke_event_count, last_frame_at, last_stream_error

    data = request.get_json(silent=True) or {}
    mode = data.get("mode")

    # Do not let an old reader survive and consume bytes from the new device.
    cleanup_usb_connection()
    with state_lock:
        current_mode = None
        latest_frame = b''
        latest_result = {"boxes": []}
        invoke_event_count = 0
        last_frame_at = None
        last_stream_error = None

    if mode == "usb":
        port = data.get("port")
        conn = None
        try:
            # 在鎖外開啟串口（可能耗時）
            conn = serial.Serial(port, 921600, timeout=1)
            time.sleep(1.5)  # 等待 Himax WE2 開機與串口穩定
            conn.reset_input_buffer()
            conn.reset_output_buffer()
            # A prior infinite INVOKE can survive a reconnect.  Stop it before
            # installing our reader, then discard the BREAK acknowledgement.
            conn.write(b'AT+BREAK\r\n')
            time.sleep(0.05)
            conn.reset_input_buffer()

            with state_lock:
                serial_conn = conn
                current_mode = "usb"
                connected = False  # Only true after a real INVOKE response.
                serial_stop_event = threading.Event()
                serial_thread = threading.Thread(
                    target=serial_reader_thread,
                    args=(conn, serial_stop_event),
                    daemon=True,
                )
                serial_thread.start()

            # Protocol parameter 3 is RESULT_ONLY: 0 includes the Base64 JPEG
            # in each type:1 event; 1 returns inference data only.
            conn.write(b'AT+INVOKE=-1,0,0\r\n')
            add_log("info", f"Connected to USB: {port}; requested JPEG + inference stream")
            return jsonify({"status": "ok"})
        except Exception as e:
            if conn:
                try:
                    conn.close()
                except serial.SerialException:
                    pass
            add_log("error", f"USB connect failed: {e}")
            return jsonify({"status": "error", "message": str(e)}), 400

    elif mode == "esp32":
        ip = data.get("ip")
        port = data.get("port", 80)
        with state_lock:
            current_mode = "esp32"
            connected = True
        add_log("info", f"Connected to ESP32: {ip}:{port}")
        return jsonify({"status": "ok", "ip": ip, "port": port})

    return jsonify({"status": "error", "message": "Invalid mode"}), 400

@app.route('/api/disconnect', methods=['POST'])
def disconnect():
    global current_mode, connected
    cleanup_usb_connection()
    with state_lock:
        current_mode = None
        connected = False
        add_log("info", "Disconnected")
    return jsonify({"status": "ok"})

@app.route('/stream')
def stream():
    def generate():
        """MJPEG 串流 — 持續產生影像幀"""
        blank_sent = False
        while current_mode == "usb":
            with state_lock:
                frame = latest_frame
            if frame:
                yield (b'--frame\r\n'
                       b'Content-Type: image/jpeg\r\n'
                       b'Content-Length: ' + str(len(frame)).encode() + b'\r\n\r\n' +
                       frame + b'\r\n')
                blank_sent = False
            else:
                # 尚未收到影像時送出一個最小的黑色 JPEG 保持連線
                if not blank_sent:
                    # 1x1 black JPEG
                    black_jpg = bytes([0xFF,0xD8,0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,0x07,0x07,0x07,0x09,0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,0x24,0x2E,0x27,0x20,0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,0x39,0x3D,0x38,0x32,0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x1F,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0xFF,0xC4,0x00,0xB5,0x10,0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7D,0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00,0x7B,0x94,0x11,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xD9])
                    yield (b'--frame\r\n'
                           b'Content-Type: image/jpeg\r\n'
                           b'Content-Length: ' + str(len(black_jpg)).encode() + b'\r\n\r\n' +
                           black_jpg + b'\r\n')
                    blank_sent = True
            time.sleep(0.05)

    if current_mode == "usb":
        return Response(generate(), mimetype='multipart/x-mixed-replace; boundary=frame')
    return "Not in USB mode", 400

@app.route('/api/result')
def get_result():
    with state_lock:
        if current_mode == "esp32":
            return jsonify({"connected": True, "mode": "esp32"})
            
        return jsonify({
            "mode": "usb",
            "connected": connected,
            "fps": fps,
            "result": latest_result,
            "stream": {
                "invoke_events": invoke_event_count,
                "last_frame_at": last_frame_at,
                "last_error": last_stream_error,
                "frame_bytes": len(latest_frame),
            },
        })

@app.route('/api/client/heartbeat', methods=['POST'])
def browser_heartbeat():
    """Register or refresh one browser page's ownership of this server."""
    global browser_seen
    data = request.get_json(silent=True) or {}
    client_id = data.get("client_id")
    if not isinstance(client_id, str) or not client_id:
        return jsonify({"status": "error", "message": "client_id is required"}), 400
    with state_lock:
        browser_sessions[client_id] = time.time()
        browser_seen = True
    return jsonify({"ok": True})

@app.route('/api/client/disconnect', methods=['POST'])
def browser_disconnect():
    data = request.get_json(silent=True) or {}
    client_id = data.get("client_id")
    if isinstance(client_id, str):
        with state_lock:
            browser_sessions.pop(client_id, None)
    return jsonify({"ok": True})

@app.route('/api/logs')
def get_logs():
    with state_lock:
        return jsonify(list(logs))

HTML_CONTENT = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Cat Emotion Monitor</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #08080c;
            --surface: rgba(255,255,255,0.03);
            --border: rgba(255,255,255,0.06);
            --text-primary: rgba(255,255,255,0.85);
            --text-muted: rgba(255,255,255,0.35);
            --c-angry: #ff4d4d;
            --c-focus: #4d8bff;
            --c-relax: #4dff88;
            --c-scared: #ffb84d;
            --c-unknown: #ffffff;
        }

        body {
            margin: 0;
            padding: 0;
            background: var(--bg-color);
            color: var(--text-primary);
            font-family: 'Inter', sans-serif;
            overflow: hidden;
            display: flex;
            height: 100vh;
        }

        /* --- Setup Screen --- */
        #setup-screen {
            position: fixed;
            top: 0; left: 0; width: 100%; height: 100%;
            background: var(--bg-color);
            display: flex;
            justify-content: center;
            align-items: center;
            z-index: 100;
            gap: 30px;
            transition: opacity 0.6s ease;
        }

        .setup-card {
            width: 180px;
            height: 200px;
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 20px;
            display: flex;
            flex-direction: column;
            justify-content: center;
            align-items: center;
            cursor: pointer;
            transition: all 0.3s ease;
            overflow: hidden;
            position: relative;
        }

        .setup-card:hover {
            transform: translateY(-4px);
            box-shadow: 0 10px 30px rgba(255,255,255,0.05);
            border-color: rgba(255,255,255,0.1);
        }

        .setup-card svg {
            width: 48px;
            height: 48px;
            fill: var(--text-primary);
            margin-bottom: 20px;
            transition: all 0.3s ease;
        }
        
        .setup-card span {
            font-weight: 400;
            font-size: 16px;
            transition: all 0.3s ease;
        }

        .setup-expanded {
            width: 340px;
            height: 420px;
            padding: 24px;
            justify-content: flex-start;
            cursor: default;
        }

        .setup-expanded > svg, .setup-expanded > span {
            display: none;
        }

        .disconnect-btn {
            position: fixed;
            top: 20px;
            left: 20px;
            z-index: 60;
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 8px;
            cursor: pointer;
            display: flex;
            justify-content: center;
            align-items: center;
            transition: background 0.2s;
        }
        .disconnect-btn:hover { background: rgba(255,255,255,0.06); }
        .disconnect-btn svg { fill: var(--text-primary); width: 20px; height: 20px; }

        .btn:disabled {
            opacity: 0.4;
            cursor: not-allowed;
        }
        .btn.loading {
            pointer-events: none;
            opacity: 0.6;
        }

        .form-content {
            display: none;
            flex-direction: column;
            width: 100%;
            height: 100%;
            opacity: 0;
            transition: opacity 0.3s ease 0.2s;
        }
        
        .setup-expanded .form-content {
            display: flex;
            opacity: 1;
        }

        .form-title {
            font-size: 18px;
            margin-bottom: 20px;
            text-align: center;
        }

        .port-list {
            flex-grow: 1;
            overflow-y: auto;
            margin-bottom: 20px;
        }

        .port-item {
            padding: 10px;
            border-radius: 8px;
            background: rgba(255,255,255,0.02);
            margin-bottom: 8px;
            cursor: pointer;
            border: 1px solid transparent;
        }
        
        .port-item:hover { background: rgba(255,255,255,0.05); }
        .port-item.selected { border-color: var(--text-primary); }
        .port-name { font-size: 14px; }
        .port-desc { font-size: 11px; color: var(--text-muted); margin-top: 4px; }

        input {
            width: 100%;
            padding: 12px;
            background: rgba(0,0,0,0.2);
            border: 1px solid var(--border);
            border-radius: 8px;
            color: white;
            margin-bottom: 15px;
            box-sizing: border-box;
            outline: none;
        }
        input:focus { border-color: rgba(255,255,255,0.2); }

        .btn {
            padding: 12px;
            border-radius: 8px;
            background: var(--text-primary);
            color: var(--bg-color);
            border: none;
            cursor: pointer;
            font-weight: 500;
            width: 100%;
            margin-top: auto;
        }

        /* --- Main Screen --- */
        #main-screen {
            flex: 1 1 auto;
            display: flex;
            justify-content: center;
            align-items: center;
            position: relative;
            min-width: 0;
            padding: 28px;
        }

        .video-container {
            width: 100%;
            max-width: 920px;
            aspect-ratio: 4/3;
            max-height: 80vh;
            border-radius: 16px;
            border: 2px solid var(--border);
            position: relative;
            transition: border-color 0.6s ease, box-shadow 0.6s ease;
            overflow: hidden;
            background: #000;
        }

        #video-stream {
            width: 100%;
            height: 100%;
            /* Preserve the camera frame: cropping makes bbox alignment wrong. */
            object-fit: contain;
        }

        #bbox-canvas {
            position: absolute;
            top: 0; left: 0;
            width: 100%; height: 100%;
            pointer-events: none;
        }

        .status-indicator {
            position: absolute;
            top: 15px;
            right: 15px;
            display: flex;
            align-items: center;
            gap: 8px;
        }

        .status-dot {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: #ff4d4d;
            box-shadow: 0 0 8px #ff4d4d;
        }
        .status-dot.connected {
            background: #4dff88;
            box-shadow: 0 0 8px #4dff88;
        }

        #fps-display {
            font-size: 11px;
            color: var(--text-muted);
        }

        /* --- Log Panel --- */
        #log-panel {
            width: 320px;
            height: 100vh;
            flex: 0 0 320px;
            background: rgba(255,255,255,0.025);
            border-left: 1px solid var(--border);
            display: flex;
            flex-direction: column;
        }

        .log-header {
            padding: 15px;
            border-bottom: 1px solid var(--border);
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .log-content {
            flex-grow: 1;
            overflow-y: auto;
            padding: 12px;
            font-family: monospace;
            font-size: 11px;
            display: flex;
            flex-direction: column;
            gap: 7px;
        }

        .log-summary {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 8px;
            padding: 12px;
            border-bottom: 1px solid var(--border);
            color: var(--text-muted);
            font-size: 11px;
        }
        .log-summary strong { display:block; color:var(--text-primary); font: 500 12px Inter, sans-serif; margin-top:2px; }
        .log-entry { display: grid; grid-template-columns: 6px 1fr; gap: 8px; align-items: flex-start; padding: 8px; border: 1px solid var(--border); border-radius: 7px; background: rgba(255,255,255,0.018); }
        .log-meta { display:flex; justify-content:space-between; color: var(--text-muted); font-size:10px; margin-bottom:4px; }
        .log-type { text-transform:uppercase; letter-spacing:.06em; }
        .log-msg { color: var(--text-primary); word-break: break-all; }
        
        .log-dot {
            width: 6px; height: 6px; border-radius: 50%; margin-top: 4px; flex-shrink: 0;
        }
        .log-info { background: white; }
        .log-detect { background: var(--c-focus); }
        .log-warn { background: #ffb84d; }
        .log-error { background: #ff4d4d; }

        .icon-btn {
            background: none; border: none; padding: 5px; cursor: pointer;
            fill: var(--text-muted); transition: fill 0.2s;
        }
        .icon-btn:hover { fill: var(--text-primary); }

        @media (max-width: 900px) {
            body { display:block; overflow:auto; }
            #main-screen { min-height:72vh; padding:20px; }
            .video-container { width: 100%; }
            #log-panel {
                width:100%; height:40vh; min-height:280px; border-left:none; border-top:1px solid var(--border);
                flex:none;
            }
        }
    </style>
</head>
<body>

    <div id="setup-screen">
        <div class="setup-card" id="card-usb" onclick="expandCard('usb')">
            <svg viewBox="0 0 24 24"><path d="M15 7v4h1v2h-3V5h2l-3-4-3 4h2v8H8v-2h1V7H5v2h1v2H3v-2h1V7H2v4h1v2h2v8h4v-2h2v2h4v-10h2V7h-2z"/></svg>
            <span>USB</span>
            <div class="form-content" id="form-usb" onclick="event.stopPropagation()">
                <div class="form-title">Select Port</div>
                <div class="port-list" id="port-list"></div>
                <button class="btn" id="btn-usb-connect" onclick="connectUSB()">Connect</button>
            </div>
        </div>
        
        <div class="setup-card" id="card-wifi" onclick="expandCard('wifi')">
            <svg viewBox="0 0 24 24"><path d="M12 3C7.79 3 3.96 4.54 1 7.02l2.21 2.65C5.4 7.82 8.52 6.5 12 6.5s6.6 1.32 8.79 3.17L23 7.02C20.04 4.54 16.21 3 12 3zm0 5c-2.3 0-4.4.82-6.11 2.19l2.21 2.65C9.28 11.95 10.59 11.5 12 11.5s2.72.45 3.9 1.34l2.21-2.65C16.4 8.82 14.3 8 12 8zm0 5c-1.07 0-2.07.4-2.86 1.07l2.86 3.43 2.86-3.43C14.07 13.4 13.07 13 12 13z"/></svg>
            <span>WiFi</span>
            <div class="form-content" id="form-wifi" onclick="event.stopPropagation()">
                <div class="form-title">ESP32 Address</div>
                <input type="text" id="esp-ip" placeholder="IP Address (192.168.68.100 或 cat.local)" value="192.168.68.100">
                <input type="number" id="esp-port" placeholder="Port (default: 80)" value="80">
                <button class="btn" id="btn-wifi-connect" onclick="connectWiFi()">Connect</button>
            </div>
        </div>
    </div>

    <div id="main-screen">
        <button class="disconnect-btn" onclick="goDisconnect()" title="Disconnect">
            <svg viewBox="0 0 24 24"><path d="M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"/></svg>
        </button>

        <div class="video-container" id="video-wrap">
            <img id="video-stream" src="" alt="">
            <canvas id="bbox-canvas"></canvas>
            <div class="status-indicator">
                <span id="fps-display">0 fps</span>
                <div class="status-dot" id="conn-dot"></div>
            </div>
        </div>
    </div>

    <aside id="log-panel">
        <div class="log-header">
            <span style="font-weight: 500; font-size: 14px;">System Log</span>
            <button class="icon-btn" onclick="clearLogs()" title="Clear visible logs">
                <svg width="16" height="16" viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
            </button>
        </div>
        <div class="log-summary">
            <div>Connection<strong id="log-connection">Waiting</strong></div>
            <div>Frame rate<strong id="log-fps">0 fps</strong></div>
            <div>JPEG frame<strong id="log-frame">0 bytes</strong></div>
            <div>INVOKE events<strong id="log-events">0</strong></div>
        </div>
        <div class="log-content" id="log-content"></div>
    </aside>

    <script>
        const EMOTION_COLORS = {
            angry: 'var(--c-angry)',
            focus: 'var(--c-focus)',
            relax: 'var(--c-relax)',
            scared: 'var(--c-scared)',
            unknown: 'var(--c-unknown)'
        };
        
        let selectedPort = null;
        let currentMode = null;
        let pollInterval = null;
        let logInterval = null;
        let isConnected = false;
        const videoStream = document.getElementById('video-stream');
        const clientId = (window.crypto && crypto.randomUUID)
            ? crypto.randomUUID()
            : `${Date.now()}-${Math.random().toString(16).slice(2)}`;

        async function refreshBrowserSession() {
            try {
                await fetch('/api/client/heartbeat', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ client_id: clientId }),
                    keepalive: true,
                });
            } catch (e) {
                // The normal UI requests will show an unavailable server.
            }
        }

        function releaseBrowserSession() {
            const payload = JSON.stringify({ client_id: clientId });
            if (!navigator.sendBeacon('/api/client/disconnect',
                                      new Blob([payload], { type: 'application/json' }))) {
                fetch('/api/client/disconnect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: payload,
                    keepalive: true,
                }).catch(() => {});
            }
        }

        refreshBrowserSession();
        setInterval(refreshBrowserSession, 3000);
        window.addEventListener('pagehide', releaseBrowserSession);

        videoStream.addEventListener('load', () => {
            if (isConnected) document.getElementById('log-connection').textContent = 'Rendering JPEG';
        });
        videoStream.addEventListener('error', () => {
            if (isConnected) document.getElementById('log-connection').textContent = 'Stream request failed';
        });
        
        async function fetchPorts() {
            try {
                const res = await fetch('/api/ports');
                const ports = await res.json();
                const list = document.getElementById('port-list');
                list.innerHTML = '';
                ports.forEach(p => {
                    const el = document.createElement('div');
                    el.className = 'port-item';
                    el.innerHTML = `<div class="port-name">${p.device}</div><div class="port-desc">${p.description}</div>`;
                    el.onclick = () => {
                        document.querySelectorAll('.port-item').forEach(i => i.classList.remove('selected'));
                        el.classList.add('selected');
                        selectedPort = p.device;
                    };
                    list.appendChild(el);
                });
            } catch (e) {
                console.error(e);
            }
        }

        function expandCard(type) {
            document.querySelectorAll('.setup-card').forEach(c => {
                c.classList.remove('setup-expanded');
            });
            const card = document.getElementById('card-' + type);
            setTimeout(() => card.classList.add('setup-expanded'), 50);
            if (type === 'usb') fetchPorts();
        }

        function goBackSetup() {
            document.querySelectorAll('.setup-card').forEach(c => {
                c.classList.remove('setup-expanded');
                c.style.display = 'flex';
            });
            selectedPort = null;
        }

        async function connectUSB() {
            if (!selectedPort) return;
            const btn = document.getElementById('btn-usb-connect');
            btn.textContent = 'Connecting...';
            btn.classList.add('loading');
            try {
                const res = await fetch('/api/connect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ mode: 'usb', port: selectedPort })
                });
                const data = await res.json();
                if (res.ok && data.status === 'ok') {
                    enterMainScreen('usb');
                } else {
                    btn.textContent = data.message || 'Failed';
                    setTimeout(() => { btn.textContent = 'Connect'; btn.classList.remove('loading'); }, 2000);
                }
            } catch (e) {
                btn.textContent = 'Error';
                setTimeout(() => { btn.textContent = 'Connect'; btn.classList.remove('loading'); }, 2000);
            }
        }

        async function connectWiFi() {
            const ip = document.getElementById('esp-ip').value.trim();
            const port = document.getElementById('esp-port').value || '80';
            if (!ip) return;
            const btn = document.getElementById('btn-wifi-connect');
            btn.textContent = 'Connecting...';
            btn.classList.add('loading');
            try {
                const res = await fetch('/api/connect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ mode: 'esp32', ip, port: parseInt(port) })
                });
                const data = await res.json();
                if (res.ok && data.status === 'ok') {
                    enterMainScreen('esp32', ip, port);
                } else {
                    btn.textContent = data.message || 'Failed';
                    setTimeout(() => { btn.textContent = 'Connect'; btn.classList.remove('loading'); }, 2000);
                }
            } catch (e) {
                btn.textContent = 'Error';
                setTimeout(() => { btn.textContent = 'Connect'; btn.classList.remove('loading'); }, 2000);
            }
        }

        let espWs = null;
        let espFpsCount = 0;
        let espLastFpsTime = performance.now();
        let espFps = 0;

        function enterMainScreen(mode, ip, port) {
            const setup = document.getElementById('setup-screen');
            setup.style.opacity = '0';
            setTimeout(() => { setup.style.display = 'none'; }, 600);
            isConnected = true;
            currentMode = mode;
            if (mode === 'usb') {
                // A distinct URL guarantees that a reconnect opens a new
                // MJPEG request instead of reusing a terminated stream.
                videoStream.src = `/stream?session=${Date.now()}`;
            } else {
                videoStream.src = `http://${ip}:${port}/stream?session=${Date.now()}`;
                try {
                    espWs = new WebSocket(`ws://${ip}:81/`);
                    espWs.onmessage = (ev) => {
                        const now = performance.now();
                        espFpsCount++;
                        if (now - espLastFpsTime >= 1000) {
                            espFps = Math.round((espFpsCount * 1000) / (now - espLastFpsTime));
                            espFpsCount = 0;
                            espLastFpsTime = now;
                        }
                        document.getElementById('fps-display').innerText = espFps + ' fps';
                        document.getElementById('log-fps').textContent = espFps + ' fps';
                        try {
                            const d = JSON.parse(ev.data);
                            if (d.bbox && d.emotion && d.emotion !== 'none') {
                                drawBoxes([{
                                    x: d.bbox.x + d.bbox.w / 2,
                                    y: d.bbox.y + d.bbox.h / 2,
                                    w: d.bbox.w,
                                    h: d.bbox.h,
                                    score: Math.round(d.confidence * 100),
                                    target: { angry: 0, focus: 1, relax: 2, scared: 3 }[d.emotion] ?? -1
                                }]);
                                updateUI(d.emotion, Math.round(d.confidence * 100));
                            } else {
                                drawBoxes([]);
                                updateUI('unknown', 0);
                            }
                        } catch(e) {}
                    };
                } catch(e) {
                    console.warn("WebSocket init error:", e);
                }
            }
            pollInterval = setInterval(pollData, 200);
            logInterval = setInterval(fetchLogs, 1000);
        }

        async function goDisconnect() {
            // 停止輪詢
            if (pollInterval) { clearInterval(pollInterval); pollInterval = null; }
            if (logInterval) { clearInterval(logInterval); logInterval = null; }
            if (espWs) { espWs.close(); espWs = null; }
            isConnected = false;
            videoStream.src = '';
            // 通知後端斷開
            try { await fetch('/api/disconnect', { method: 'POST' }); } catch(e) {}
            // 回到設定畫面
            const setup = document.getElementById('setup-screen');
            setup.style.display = 'flex';
            setup.style.opacity = '1';
            goBackSetup();
            // 重置 UI
            document.getElementById('conn-dot').classList.remove('connected');
            document.getElementById('video-wrap').style.borderColor = 'var(--border)';
            document.getElementById('video-wrap').style.boxShadow = 'none';
            // 重置按鈕
            document.querySelectorAll('.btn').forEach(b => { b.textContent = 'Connect'; b.classList.remove('loading'); });
        }

        async function pollData() {
            try {
                const res = await fetch('/api/result');
                const data = await res.json();
                
                const dot = document.getElementById('conn-dot');
                if (data.connected) dot.classList.add('connected');
                else dot.classList.remove('connected');

                if (data.mode === 'usb') {
                    document.getElementById('fps-display').innerText = Math.round(data.fps) + ' fps';
                    document.getElementById('log-connection').textContent = data.connected ? 'Streaming' : 'Waiting for camera';
                    document.getElementById('log-fps').textContent = Math.round(data.fps) + ' fps';
                    document.getElementById('log-frame').textContent = `${data.stream?.frame_bytes || 0} bytes`;
                    document.getElementById('log-events').textContent = data.stream?.invoke_events || 0;
                    
                    const boxes = data.result.boxes || [];
                    drawBoxes(boxes);
                    
                    if (boxes.length > 0) {
                        const bestBox = boxes.reduce((prev, curr) => (prev.score > curr.score) ? prev : curr);
                        const targetMap = {0: 'angry', 1: 'focus', 2: 'relax', 3: 'scared'};
                        const emotion = targetMap[bestBox.target] || 'unknown';
                        const score = bestBox.score || 0;
                        
                        updateUI(emotion, score);
                    } else {
                        updateUI('unknown', 0);
                    }
                } else if (data.mode === 'esp32') {
                    document.getElementById('log-connection').textContent = data.connected ? 'ESP32 Streaming' : 'Waiting for ESP32';
                }
            } catch (e) {
                document.getElementById('conn-dot').classList.remove('connected');
                document.getElementById('log-connection').textContent = 'API request failed';
            }
        }
        
        function updateUI(emotion, score) {
            const color = EMOTION_COLORS[emotion];
            const wrap = document.getElementById('video-wrap');
            wrap.style.borderColor = color;
            wrap.style.boxShadow = `0 0 30px ${color}33`; // Add subtle glow
            
        }

        function drawBoxes(boxes) {
            const canvas = document.getElementById('bbox-canvas');
            const video = document.getElementById('video-stream');
            canvas.width = video.clientWidth;
            canvas.height = video.clientHeight;
            const ctx = canvas.getContext('2d');
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            if (!boxes.length || !video.naturalWidth || !video.naturalHeight) return;

            // This matches object-fit: contain. SSCMA detection coordinates
            // are the bbox centre; translate them to the canvas top-left.
            const scale = Math.min(canvas.width / video.naturalWidth,
                                   canvas.height / video.naturalHeight);
            const imageWidth = video.naturalWidth * scale;
            const imageHeight = video.naturalHeight * scale;
            const offsetX = (canvas.width - imageWidth) / 2;
            const offsetY = (canvas.height - imageHeight) / 2;
            const targetMap = {0: 'angry', 1: 'focus', 2: 'relax', 3: 'scared'};

            boxes.forEach(b => {
                const emo = targetMap[b.target] || 'unknown';
                ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue('--c-' + emo) || '#fff';
                ctx.lineWidth = 3;
                const w = b.w * scale;
                const h = b.h * scale;
                const x = offsetX + b.x * scale - w / 2;
                const y = offsetY + b.y * scale - h / 2;
                ctx.strokeRect(x, y, w, h);
                ctx.fillStyle = ctx.strokeStyle;
                ctx.font = 'bold 14px Inter, sans-serif';
                ctx.fillText(`${emo.toUpperCase()} ${Math.round(b.score || 0)}%`,
                             x, Math.max(15, y - 6));
            });
        }

        let lastLogTs = 0;
        function escapeHtml(value) {
            const element = document.createElement('div');
            element.textContent = value;
            return element.innerHTML;
        }
        async function fetchLogs() {
            try {
                const res = await fetch('/api/logs');
                const logs = await res.json();
                
                const content = document.getElementById('log-content');
                let added = false;
                
                logs.forEach(l => {
                    if (l.ts > lastLogTs) {
                        const div = document.createElement('div');
                        div.className = 'log-entry';
                        const d = new Date(l.ts);
                        const tsStr = d.getHours().toString().padStart(2,'0') + ':' + 
                                      d.getMinutes().toString().padStart(2,'0') + ':' + 
                                      d.getSeconds().toString().padStart(2,'0');
                        
                        div.innerHTML = `
                            <div class="log-dot log-${escapeHtml(l.type)}"></div>
                            <div>
                                <div class="log-meta"><span class="log-type">${escapeHtml(l.type)}</span><span>${tsStr}.${String(l.ts % 1000).padStart(3, '0')}</span></div>
                                <div class="log-msg">${escapeHtml(l.msg)}</div>
                            </div>`;
                        content.appendChild(div);
                        lastLogTs = l.ts;
                        added = true;
                    }
                });
                
                if (added) {
                    content.scrollTop = content.scrollHeight;
                }
            } catch (e) {}
        }

        function clearLogs() {
            document.getElementById('log-content').innerHTML = '';
        }

    </script>
</body>
</html>
"""

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Cat Emotion Monitor')
    parser.add_argument('--port', type=int, default=8080, help='Web server port')
    parser.add_argument('--no-browser', action='store_true', help='Do not auto-open browser')
    args = parser.parse_args()

    # 啟動心跳檢查（瀏覽器關閉後自動關閉 server）
    ht = threading.Thread(target=heartbeat_checker, daemon=True)
    ht.start()

    # 自動開啟瀏覽器
    if not args.no_browser:
        def open_browser():
            time.sleep(1.2)
            webbrowser.open(f"http://127.0.0.1:{args.port}")
        threading.Thread(target=open_browser, daemon=True).start()

    print(f"\nServer running")
    print(f"  http://localhost:{args.port}")
    app.run(host='0.0.0.0', port=args.port, threaded=True)
