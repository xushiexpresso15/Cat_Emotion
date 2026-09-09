#!/usr/bin/env python3
"""
貓咪情緒監控 — Python Web Server（免 ESP32 版）
直接透過 USB 串口連接 Grove Vision AI V2，
在電腦端提供 MJPEG 影像串流 + WebSocket 推論資料。

使用方式:
  python webserver/local_server.py --port /dev/ttyACM0
  然後開啟瀏覽器: http://localhost:8080
"""

import argparse
import base64
import json
import re
import sys
import threading
import time
from io import BytesIO
from pathlib import Path

import serial
from flask import Flask, Response, render_template_string, jsonify

# ─── 全域狀態 ───
latest_frame = None        # 最新 JPEG 影像 (bytes)
latest_result = None       # 最新推論結果 (dict)
frame_lock = threading.Lock()
result_lock = threading.Lock()
connected = False
fps_counter = 0
last_fps_time = time.time()
current_fps = 0

# ─── SSCMA AT 命令通訊 ───

class SSCMASerial:
    """透過 AT 命令與 Grove Vision AI V2 通訊"""

    def __init__(self, port: str, baudrate: int = 921600, timeout: float = 2.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None

    def connect(self):
        """建立串口連線"""
        self.ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=self.timeout,
            write_timeout=self.timeout,
        )
        time.sleep(0.5)
        # 清空緩衝區
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        print(f"  [OK] 已連接: {self.port} @ {self.baudrate}")

    def send_command(self, cmd: str) -> str:
        """發送 AT 命令並讀取回應"""
        if not self.ser or not self.ser.is_open:
            return ""
        self.ser.write((cmd + "\r\n").encode())
        time.sleep(0.05)
        response = ""
        deadline = time.time() + self.timeout
        while time.time() < deadline:
            if self.ser.in_waiting > 0:
                chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                response += chunk
                if "\n" in chunk:
                    # 檢查是否收到完整回應
                    if "OK" in response or "ERR" in response:
                        break
            else:
                time.sleep(0.01)
        return response

    def get_info(self) -> str:
        """取得裝置資訊"""
        return self.send_command("AT+ID?")

    def start_invoke(self):
        """開始連續推論（含影像）
        AT+INVOKE=-1,0,1
          -1 = 持續推論
           0 = 不需要事件
           1 = 回傳影像
        """
        if self.ser and self.ser.is_open:
            self.ser.write(b"AT+INVOKE=-1,0,1\r\n")

    def read_stream(self):
        """
        持續讀取串口資料流，解析 JPEG 與推論結果。
        SSCMA 回傳格式:
          {"type":0,"name":"INVOKE","code":0,"data":{
            "count":N,
            "image":"<base64 JPEG>",
            "boxes":[{"x":..,"y":..,"w":..,"h":..,"score":..,"target":..}]
          }}
        """
        global latest_frame, latest_result, connected, fps_counter, last_fps_time, current_fps

        buffer = ""
        json_depth = 0
        json_start = -1

        while True:
            try:
                if not self.ser or not self.ser.is_open:
                    time.sleep(1)
                    continue

                if self.ser.in_waiting > 0:
                    chunk = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
                    buffer += chunk

                    # 尋找完整的 JSON 物件
                    i = 0
                    while i < len(buffer):
                        ch = buffer[i]
                        if ch == '{':
                            if json_depth == 0:
                                json_start = i
                            json_depth += 1
                        elif ch == '}':
                            json_depth -= 1
                            if json_depth == 0 and json_start >= 0:
                                json_str = buffer[json_start:i + 1]
                                buffer = buffer[i + 1:]
                                self._process_json(json_str)
                                i = -1  # 重置索引
                                json_start = -1
                        i += 1

                    # 防止 buffer 無限增長
                    if len(buffer) > 100000:
                        buffer = buffer[-10000:]
                        json_depth = 0
                        json_start = -1

                else:
                    time.sleep(0.005)

            except serial.SerialException as e:
                print(f"  [FAIL] 串口錯誤: {e}")
                connected = False
                time.sleep(2)
                try:
                    self.connect()
                    self.start_invoke()
                    connected = True
                except Exception:
                    pass
            except Exception as e:
                print(f"  [WARN] 讀取錯誤: {e}")
                time.sleep(0.1)

    def _process_json(self, json_str: str):
        """處理解析到的 JSON 資料

        Grove Vision AI V2 支援兩種輸出格式：

        格式 1 — FOMO / 偵測模型（boxes）:
            {"type":0,"name":"INVOKE","code":0,"data":{
              "count":N,
              "image":"<base64>",
              "boxes":[{"x":x_center,"y":y_center,"w":w,"h":h,"score":score,"target":cls_id}]
            }}
            - score: 0-100（FOMO 推論期做過 softmax → 百分比）
            - target: COCO category_id（1-based），對應 angry=1, focus=2, relax=3, scared=4

        格式 2 — 純分類模型（classes）:
            {"type":0,"name":"INVOKE","code":0,"data":{
              "count":N,
              "image":"<base64>",
              "classes":[{"score":score,"target":cls_id}]
            }}
            - score: INT8 unsigned（0-255），需除以 255 轉機率；若含 softmax 則直接是 0-100
            - target: 0-based class index
        """
        global latest_frame, latest_result, connected, fps_counter, last_fps_time, current_fps

        try:
            data = json.loads(json_str)
        except json.JSONDecodeError:
            return

        if data.get("type") != 0 or data.get("name") != "INVOKE":
            return

        connected = True
        inner = data.get("data", {})

        # 解析影像
        img_b64 = inner.get("image", "")
        if img_b64:
            try:
                img_bytes = base64.b64decode(img_b64)
                with frame_lock:
                    global latest_frame
                    latest_frame = img_bytes
            except Exception:
                pass

        # --- 情緒名稱對照表 ---
        # COCO annotation 中 category_id 是 1-based: 1=angry, 2=focus, 3=relax, 4=scared
        emotion_names = ['angry', 'focus', 'relax', 'scared']

        result = None

        # ====================================================
        # 格式 1：FOMO / 偵測模型 → 解析 boxes
        # ====================================================
        boxes = inner.get("boxes", [])
        if boxes:
            # 過濾無效的 box（score=0 或 target=0）
            valid_boxes = [b for b in boxes if b.get("score", 0) > 0 and b.get("target", 0) > 0]
            if not valid_boxes:
                valid_boxes = boxes  # fallback：全部 box 都採用

            # 挑選信心度最高的 box
            best = max(valid_boxes, key=lambda b: b.get("score", 0))

            # target 是 COCO 1-based category id → emotion index (0-based)
            target_raw = best.get("target", 1)
            emotion_idx = (target_raw - 1) if target_raw >= 1 else 0
            emotion_idx = max(0, min(emotion_idx, len(emotion_names) - 1))
            emotion = emotion_names[emotion_idx]

            # score: FOMO 已做 softmax，為 0-100 整數
            confidence = best.get("score", 0) / 100.0
            confidence = max(0.0, min(1.0, confidence))

            result = {
                "emotion": emotion,
                "confidence": confidence,
                "mode": "fomo",
                "bbox": {
                    "x": best.get("x", 0),
                    "y": best.get("y", 0),
                    "w": best.get("w", 0),
                    "h": best.get("h", 0),
                },
                "timestamp": int(time.time() * 1000),
                "all_boxes": [
                    {
                        "emotion": emotion_names[max(0, min((b.get("target", 1) - 1), len(emotion_names) - 1))],
                        "confidence": max(0.0, min(1.0, b.get("score", 0) / 100.0)),
                        "bbox": {"x": b.get("x", 0), "y": b.get("y", 0),
                                 "w": b.get("w", 0), "h": b.get("h", 0)},
                    }
                    for b in valid_boxes
                ],
            }

        # ====================================================
        # 格式 2：純分類模型 → 解析 classes
        # ====================================================
        elif "classes" in inner and inner["classes"]:
            classes = inner["classes"]

            # softmax 後的分數最大值判斷
            raw_scores = [c.get("score", 0) for c in classes]
            max_raw = max(raw_scores) if raw_scores else 255

            # 如果最大分數 <= 100，代表已做過 softmax（百分比格式）
            # 如果最大分數 > 100，代表是原始 logit offset（需歸一化至 0-1）
            if max_raw <= 100:
                # 已含 softmax（用 --embed-softmax 匯出的模型）
                scores = [s / 100.0 for s in raw_scores]
            else:
                # 原始 INT8 unsigned (0-255) → 做 softmax 歸一化
                import math
                exp_scores = [math.exp(s / 32.0) for s in raw_scores]  # 縮放避免溢位
                sum_exp = sum(exp_scores)
                scores = [e / sum_exp for e in exp_scores]

            # 找最高分類
            best_idx = scores.index(max(scores))
            target_raw = classes[best_idx].get("target", 0)
            # 分類模型 target 是 0-based
            emotion_idx = max(0, min(target_raw, len(emotion_names) - 1))
            emotion = emotion_names[emotion_idx]
            confidence = scores[best_idx]

            result = {
                "emotion": emotion,
                "confidence": confidence,
                "mode": "classify",
                "bbox": None,  # 分類模型無 bbox
                "timestamp": int(time.time() * 1000),
                "all_scores": [
                    {"emotion": emotion_names[max(0, min(c.get("target", 0), 3))],
                     "confidence": scores[i]}
                    for i, c in enumerate(classes)
                ],
            }

        if result is not None:
            with result_lock:
                global latest_result
                latest_result = result

        # FPS 計算
        fps_counter += 1
        now = time.time()
        if now - last_fps_time >= 1.0:
            current_fps = fps_counter
            fps_counter = 0
            last_fps_time = now




# ─── Flask Web Server ───

app = Flask(__name__)

HTML_TEMPLATE = """
<!DOCTYPE html>
<html lang="zh-TW">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>貓咪情緒監控系統 — 本機模式</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-color: #0f0f1a;
            --panel-bg: rgba(255, 255, 255, 0.05);
            --border-color: rgba(255, 255, 255, 0.1);
            --text-main: #f0f0f0;
            --text-muted: #a0a0b0;
            --angry: #ef4444;
            --focus: #3b82f6;
            --relax: #22c55e;
            --scared: #f59e0b;
            --accent: #8b5cf6;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Inter', sans-serif;
            background: radial-gradient(ellipse at 20% 50%, #1a1a2e 0%, var(--bg-color) 60%);
            color: var(--text-main);
            min-height: 100vh;
            padding: 20px;
        }
        h1 {
            text-align: center;
            font-weight: 600;
            margin-bottom: 8px;
            background: linear-gradient(135deg, #a78bfa, #60a5fa);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            font-size: 1.8rem;
            letter-spacing: 2px;
        }
        .subtitle {
            text-align: center;
            color: var(--text-muted);
            font-size: 0.85rem;
            margin-bottom: 24px;
        }
        .subtitle span {
            background: rgba(139,92,246,0.2);
            padding: 2px 10px;
            border-radius: 20px;
            font-size: 0.75rem;
            color: var(--accent);
        }
        .container {
            display: flex;
            flex-wrap: wrap;
            gap: 20px;
            max-width: 1400px;
            margin: 0 auto;
        }
        .glass {
            background: var(--panel-bg);
            backdrop-filter: blur(16px);
            -webkit-backdrop-filter: blur(16px);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            padding: 20px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.4);
            transition: box-shadow 0.3s ease;
        }
        .video-section {
            flex: 2;
            min-width: 320px;
            display: flex;
            flex-direction: column;
            align-items: center;
        }
        .video-wrapper {
            position: relative;
            width: 100%;
            max-width: 640px;
            aspect-ratio: 4/3;
            border-radius: 14px;
            overflow: hidden;
            background: #000;
            box-shadow: 0 0 30px rgba(139,92,246,0.15);
        }
        #videoStream {
            width: 100%;
            height: 100%;
            object-fit: contain;
        }
        #bboxCanvas {
            position: absolute;
            top: 0; left: 0;
            width: 100%; height: 100%;
            pointer-events: none;
        }
        .stats-bar {
            width: 100%;
            max-width: 640px;
            display: flex;
            justify-content: space-between;
            margin-top: 12px;
            font-size: 0.85em;
            color: var(--text-muted);
        }
        .dot {
            display: inline-block;
            width: 8px; height: 8px;
            border-radius: 50%;
            background: var(--angry);
            margin-right: 6px;
            transition: background 0.3s;
        }
        .dot.on { background: var(--relax); box-shadow: 0 0 8px var(--relax); }
        .info-section {
            flex: 1;
            min-width: 300px;
            display: flex;
            flex-direction: column;
            gap: 16px;
        }
        .emotion-card {
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 28px 20px;
        }
        .emoji {
            font-size: 4.5rem;
            margin-bottom: 8px;
            filter: drop-shadow(0 0 12px currentColor);
            transition: all 0.4s ease;
            animation: pulse 2s ease-in-out infinite;
        }
        @keyframes pulse {
            0%, 100% { transform: scale(1); }
            50% { transform: scale(1.05); }
        }
        .emotion-name {
            font-size: 1.8rem;
            font-weight: 600;
            margin-bottom: 12px;
            text-transform: uppercase;
            letter-spacing: 3px;
        }
        .conf-track {
            width: 100%;
            background: rgba(0,0,0,0.3);
            border-radius: 10px;
            height: 10px;
            overflow: hidden;
        }
        .conf-fill {
            height: 100%;
            width: 0%;
            border-radius: 10px;
            transition: width 0.3s, background 0.3s;
        }
        .conf-text {
            margin-top: 8px;
            font-size: 0.85em;
            color: var(--text-muted);
        }
        #pieChart { width: 180px; height: 180px; margin: 0 auto; }
        .history {
            max-height: 180px;
            overflow-y: auto;
            font-family: 'JetBrains Mono', monospace, monospace;
            font-size: 0.8em;
            padding: 10px;
            background: rgba(0,0,0,0.25);
            border-radius: 10px;
        }
        .history div {
            padding: 4px 0;
            border-bottom: 1px solid rgba(255,255,255,0.04);
        }
        .section-title {
            margin: 0 0 8px 0;
            font-weight: 400;
            font-size: 1rem;
            color: var(--text-muted);
        }
        ::-webkit-scrollbar { width: 6px; }
        ::-webkit-scrollbar-track { background: transparent; }
        ::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.15); border-radius: 3px; }
        @media (max-width: 768px) {
            .container { flex-direction: column; }
            h1 { font-size: 1.4rem; }
        }
    </style>
</head>
<body>
    <h1> 貓咪情緒監控系統</h1>
    <p class="subtitle">Grove Vision AI V2 — 本機串口模式 <span>USB 直連</span></p>

    <div class="container">
        <div class="glass video-section">
            <div class="video-wrapper">
                <img id="videoStream" src="/stream" alt="載入中...">
                <canvas id="bboxCanvas"></canvas>
            </div>
            <div class="stats-bar">
                <div><span class="dot" id="statusDot"></span><span id="statusText">連線中...</span></div>
                <div>FPS: <span id="fpsVal">0</span> | 延遲: <span id="latVal">0</span>ms</div>
            </div>
        </div>

        <div class="info-section">
            <div class="glass emotion-card" id="emotionCard">
                <div class="emoji" id="emojiEl">?</div>
                <div class="emotion-name" id="emotionEl">等待中</div>
                <div class="conf-track"><div class="conf-fill" id="confBar"></div></div>
                <div class="conf-text">信心水準: <span id="confVal">0</span>%</div>
            </div>

            <div class="glass">
                <div class="section-title">情緒分佈</div>
                <canvas id="pieChart" width="180" height="180"></canvas>
            </div>

            <div class="glass" style="flex:1; display:flex; flex-direction:column;">
                <div class="section-title">歷史紀錄</div>
                <div class="history" id="historyLog"></div>
            </div>
        </div>
    </div>

    <script>
        const EM = {
            angry:  { name:'生氣', emoji:'', color:'var(--angry)' },
            focus:  { name:'專注', emoji:'', color:'var(--focus)' },
            relax:  { name:'放鬆', emoji:'', color:'var(--relax)' },
            scared: { name:'害怕', emoji:'', color:'var(--scared)' },
        };
        let counts = { angry:0, focus:0, relax:0, scared:0 };
        const canvas = document.getElementById('bboxCanvas');
        const ctx = canvas.getContext('2d');
        const wrapper = document.querySelector('.video-wrapper');

        function resize() {
            canvas.width = wrapper.clientWidth;
            canvas.height = wrapper.clientHeight;
        }
        window.addEventListener('resize', resize);
        resize();

        // 拉取推論結果
        let pollErrors = 0;
        async function poll() {
            try {
                const r = await fetch('/api/result');
                const d = await r.json();

                if (d.connected) {
                    document.getElementById('statusDot').classList.add('on');
                    document.getElementById('statusText').textContent = '已連線';
                    pollErrors = 0;
                }
                document.getElementById('fpsVal').textContent = d.fps || 0;

                if (d.result) {
                    const data = d.result;
                    const em = EM[data.emotion] || { name:'未知', emoji:'?', color:'#888' };
                    document.getElementById('emojiEl').textContent = em.emoji;
                    document.getElementById('emotionEl').textContent = em.name;
                    document.getElementById('emotionEl').style.color = em.color;
                    document.getElementById('emojiEl').style.color = em.color;

                    const pct = Math.round(data.confidence * 100);
                    document.getElementById('confBar').style.width = pct + '%';
                    document.getElementById('confBar').style.background = em.color;
                    document.getElementById('confVal').textContent = pct;
                    document.getElementById('emotionCard').style.boxShadow =
                        '0 8px 32px ' + em.color + '33';

                    document.getElementById('latVal').textContent =
                        Math.abs(Date.now() - data.timestamp) % 1000;

                    drawBoxes(data);
                    addHistory(data);
                    updatePie(data.emotion);
                }
            } catch(e) {
                pollErrors++;
                if (pollErrors > 5) {
                    document.getElementById('statusDot').classList.remove('on');
                    document.getElementById('statusText').textContent = '連線中斷';
                }
            }
        }
        setInterval(poll, 200);

        function drawBoxes(data) {
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            const boxes = data.all_boxes || [data];
            const img = document.getElementById('videoStream');
            const natW = img.naturalWidth || 240;
            const natH = img.naturalHeight || 240;
            const sx = canvas.width / natW;
            const sy = canvas.height / natH;

            for (const b of boxes) {
                if (!b.bbox) continue;
                const x = b.bbox.x * sx, y = b.bbox.y * sy;
                const w = b.bbox.w * sx, h = b.bbox.h * sy;
                const col = EM[b.emotion]?.color || '#fff';
                const resolved = getComputedStyle(document.documentElement)
                    .getPropertyValue(col.match(/var\\((.*?)\\)/)?.[1] || '').trim() || col;

                ctx.strokeStyle = resolved;
                ctx.lineWidth = 2.5;
                ctx.strokeRect(x - w/2, y - h/2, w, h);
                ctx.fillStyle = resolved;
                ctx.font = 'bold 14px Inter';
                const label = (EM[b.emotion]?.name || b.emotion) +
                    ' ' + Math.round(b.confidence * 100) + '%';
                const ty = (y - h/2) > 18 ? y - h/2 - 4 : y - h/2 + 16;
                ctx.fillText(label, x - w/2, ty);
            }
        }

        function addHistory(data) {
            const log = document.getElementById('historyLog');
            const em = EM[data.emotion] || { name: data.emotion };
            const t = new Date().toLocaleTimeString();
            const div = document.createElement('div');
            div.textContent = '[' + t + '] ' + em.name +
                ' (信心: ' + (data.confidence * 100).toFixed(1) + '%)';
            log.prepend(div);
            if (log.children.length > 50) log.removeChild(log.lastChild);
        }

        const pieC = document.getElementById('pieChart');
        const pieCtx = pieC.getContext('2d');
        function updatePie(emotion) {
            if (counts[emotion] !== undefined) counts[emotion]++;
            const total = Object.values(counts).reduce((a,b) => a+b, 0);
            if (!total) return;
            pieCtx.clearRect(0, 0, pieC.width, pieC.height);
            let angle = -Math.PI / 2;
            const cx = pieC.width/2, cy = pieC.height/2, r = Math.min(cx,cy) - 8;
            const colors = {};
            for (const k of Object.keys(counts)) {
                colors[k] = getComputedStyle(document.documentElement)
                    .getPropertyValue('--' + k).trim();
            }
            for (const [k, v] of Object.entries(counts)) {
                if (!v) continue;
                const slice = (v / total) * 2 * Math.PI;
                pieCtx.beginPath();
                pieCtx.moveTo(cx, cy);
                pieCtx.arc(cx, cy, r, angle, angle + slice);
                pieCtx.closePath();
                pieCtx.fillStyle = colors[k];
                pieCtx.fill();
                angle += slice;
            }
            pieCtx.beginPath();
            pieCtx.arc(cx, cy, r * 0.58, 0, 2 * Math.PI);
            pieCtx.fillStyle = '#0f0f1a';
            pieCtx.fill();

            // 中心文字
            pieCtx.fillStyle = '#fff';
            pieCtx.font = 'bold 18px Inter';
            pieCtx.textAlign = 'center';
            pieCtx.fillText(total, cx, cy + 6);
            pieCtx.font = '10px Inter';
            pieCtx.fillStyle = '#a0a0b0';
            pieCtx.fillText('偵測次數', cx, cy + 20);
        }
    </script>
</body>
</html>
"""


@app.route('/')
def index():
    return render_template_string(HTML_TEMPLATE)


@app.route('/stream')
def stream():
    """MJPEG 影像串流"""
    def generate():
        boundary = b"--frame\r\n"
        while True:
            with frame_lock:
                frame = latest_frame
            if frame:
                yield (boundary +
                       b"Content-Type: image/jpeg\r\n"
                       b"Content-Length: " + str(len(frame)).encode() + b"\r\n\r\n" +
                       frame + b"\r\n")
            time.sleep(0.03)  # ~30 FPS max

    return Response(
        generate(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )


@app.route('/api/result')
def api_result():
    """JSON API: 最新推論結果"""
    with result_lock:
        result = latest_result
    return jsonify({
        "connected": connected,
        "fps": current_fps,
        "result": result,
    })


def main():
    parser = argparse.ArgumentParser(description='貓咪情緒監控 — 本機 Web Server')
    parser.add_argument('--port', default='/dev/ttyACM0', help='串口路徑')
    parser.add_argument('--baud', type=int, default=921600, help='鮑率')
    parser.add_argument('--host', default='0.0.0.0', help='Web Server 綁定位址')
    parser.add_argument('--web-port', type=int, default=8080, help='Web Server 端口')
    args = parser.parse_args()

    print("=" * 50)
    print(" 貓咪情緒監控 — 本機 Web Server")
    print("=" * 50)
    print(f"  串口: {args.port} @ {args.baud}")
    print(f"  Web:  http://localhost:{args.web_port}")
    print()

    # 連接 Grove Vision AI V2
    sscma = SSCMASerial(args.port, args.baud)
    try:
        sscma.connect()
    except Exception as e:
        print(f"[FAIL] 無法連接串口: {e}")
        print(f"   請確認裝置已連接，並檢查權限: sudo chmod 666 {args.port}")
        sys.exit(1)

    # 啟動連續推論
    print("   啟動連續推論...")
    sscma.start_invoke()

    # 背景讀取串口資料
    reader_thread = threading.Thread(target=sscma.read_stream, daemon=True)
    reader_thread.start()

    # 啟動 Web Server
    print(f"\n   Web Server 啟動中...")
    print(f"  -> 開啟瀏覽器: http://localhost:{args.web_port}")
    print(f"  按 Ctrl+C 停止\n")

    app.run(host=args.host, port=args.web_port, debug=False, threaded=True)


if __name__ == '__main__':
    main()
