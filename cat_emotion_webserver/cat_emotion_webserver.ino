#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Seeed_Arduino_SSCMA.h>

// 網路設定
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* ap_ssid = "Cat_Emotion_AP";
const char* ap_password = "password123";

WebServer server(80);
WebSocketsServer webSocket(81);
SSCMA AI;

// MJPEG 分界字串
#define PART_BOUNDARY "123456789000000000000987654321"

// 網頁前端 HTML（從 File 2 壓縮或直接嵌入）
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-TW">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>貓咪情緒監控系統</title>
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
        }

        body {
            margin: 0;
            padding: 20px;
            font-family: 'Inter', sans-serif;
            background: radial-gradient(circle at center, #1a1a2e 0%, var(--bg-color) 100%);
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            overflow-x: hidden;
        }

        h1 {
            text-align: center;
            font-weight: 600;
            margin-bottom: 30px;
            text-shadow: 0 0 20px rgba(255,255,255,0.2);
            letter-spacing: 2px;
        }

        .container {
            display: flex;
            flex-wrap: wrap;
            gap: 20px;
            max-width: 1400px;
            margin: 0 auto;
            width: 100%;
        }

        .glass-panel {
            background: var(--panel-bg);
            backdrop-filter: blur(12px);
            -webkit-backdrop-filter: blur(12px);
            border: 1px solid var(--border-color);
            border-radius: 20px;
            padding: 20px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
        }

        .video-container {
            flex: 2;
            min-width: 300px;
            display: flex;
            flex-direction: column;
            align-items: center;
            position: relative;
        }

        .video-wrapper {
            position: relative;
            width: 100%;
            max-width: 640px;
            aspect-ratio: 4/3;
            border-radius: 12px;
            overflow: hidden;
            background: #000;
        }

        #videoStream {
            width: 100%;
            height: 100%;
            object-fit: cover;
        }

        #bboxCanvas {
            position: absolute;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            pointer-events: none;
        }

        .stats-bar {
            width: 100%;
            max-width: 640px;
            display: flex;
            justify-content: space-between;
            margin-top: 15px;
            font-size: 0.9em;
            color: var(--text-muted);
        }

        .info-panel {
            flex: 1;
            min-width: 300px;
            display: flex;
            flex-direction: column;
            gap: 20px;
        }

        .emotion-display {
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 30px 20px;
        }

        .emoji {
            font-size: 5rem;
            margin-bottom: 10px;
            filter: drop-shadow(0 0 10px currentColor);
            transition: all 0.3s ease;
        }

        .emotion-name {
            font-size: 2rem;
            font-weight: 600;
            margin-bottom: 15px;
            text-transform: uppercase;
        }

        .confidence-container {
            width: 100%;
            background: rgba(0,0,0,0.3);
            border-radius: 10px;
            height: 12px;
            overflow: hidden;
        }

        .confidence-bar {
            height: 100%;
            width: 0%;
            border-radius: 10px;
            transition: width 0.3s ease, background-color 0.3s ease;
        }

        .chart-container {
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        #pieChart {
            width: 200px;
            height: 200px;
        }

        .history-log {
            flex: 1;
            max-height: 200px;
            overflow-y: auto;
            font-family: monospace;
            font-size: 0.85em;
            padding: 10px;
            background: rgba(0,0,0,0.2);
            border-radius: 10px;
        }
        
        .history-log div {
            margin-bottom: 5px;
            border-bottom: 1px solid rgba(255,255,255,0.05);
            padding-bottom: 5px;
        }

        ::-webkit-scrollbar { width: 8px; }
        ::-webkit-scrollbar-track { background: rgba(0,0,0,0.1); }
        ::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.2); border-radius: 4px; }

        .status-indicator {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: var(--angry);
            margin-right: 8px;
            box-shadow: 0 0 8px currentColor;
        }

        .connected {
            background: var(--relax);
        }

        @media (max-width: 768px) {
            .container { flex-direction: column; }
        }
    </style>
</head>
<body>

    <h1>🐱 貓咪情緒監控系統</h1>

    <div class="container">
        <div class="glass-panel video-container">
            <div class="video-wrapper">
                <img id="videoStream" src="" alt="影像串流加載中..." crossorigin="anonymous">
                <canvas id="bboxCanvas"></canvas>
            </div>
            <div class="stats-bar">
                <div><span class="status-indicator" id="wsStatus"></span> <span id="wsStatusText">連線中...</span></div>
                <div>FPS: <span id="fpsCounter">0</span> | 延遲: <span id="latencyDisplay">0</span> ms</div>
            </div>
        </div>

        <div class="info-panel">
            <div class="glass-panel emotion-display" id="emotionBox">
                <div class="emoji" id="currentEmoji">--</div>
                <div class="emotion-name" id="currentEmotion">未知</div>
                <div class="confidence-container">
                    <div class="confidence-bar" id="confBar"></div>
                </div>
                <div style="margin-top: 10px; font-size: 0.9em; color: var(--text-muted)">
                    信心水準: <span id="confText">0</span>%
                </div>
            </div>

            <div class="glass-panel chart-container">
                <h3 style="margin-top:0; font-weight:400; font-size:1.1rem; color:var(--text-muted)">情緒分佈</h3>
                <canvas id="pieChart" width="200" height="200"></canvas>
            </div>

            <div class="glass-panel" style="display:flex; flex-direction:column;">
                <h3 style="margin-top:0; font-weight:400; font-size:1.1rem; color:var(--text-muted)">歷史紀錄</h3>
                <div class="history-log" id="historyLog"></div>
            </div>
        </div>
    </div>

    <script>
        const emotionMap = {
            'angry': { name: '生氣', emoji: '😾', color: 'var(--angry)' },
            'focus': { name: '專注', emoji: '🔍', color: 'var(--focus)' },
            'relax': { name: '放鬆', emoji: '😺', color: 'var(--relax)' },
            'scared': { name: '害怕', emoji: '🙀', color: 'var(--scared)' }
        };

        let emotionCounts = { 'angry': 0, 'focus': 0, 'relax': 0, 'scared': 0 };
        
        const host = window.location.hostname || 'localhost';
        document.getElementById('videoStream').src = `http://${host}/stream`;

        const canvas = document.getElementById('bboxCanvas');
        const ctx = canvas.getContext('2d');
        const videoWrapper = document.querySelector('.video-wrapper');

        function resizeCanvas() {
            canvas.width = videoWrapper.clientWidth;
            canvas.height = videoWrapper.clientHeight;
        }
        window.addEventListener('resize', resizeCanvas);
        resizeCanvas();

        let ws;
        let reconnectDelay = 1000;
        let framesCount = 0;
        let lastFpsTime = Date.now();

        function connectWebSocket() {
            const wsUrl = `ws://${host}:81/`;
            ws = new WebSocket(wsUrl);

            ws.onopen = () => {
                document.getElementById('wsStatus').classList.add('connected');
                document.getElementById('wsStatusText').innerText = '已連線 WebSocket';
                reconnectDelay = 1000;
            };

            ws.onclose = () => {
                document.getElementById('wsStatus').classList.remove('connected');
                document.getElementById('wsStatusText').innerText = '連線中斷，重新連線中...';
                setTimeout(connectWebSocket, reconnectDelay);
                reconnectDelay = Math.min(reconnectDelay * 2, 10000);
            };

            ws.onmessage = (event) => {
                const now = Date.now();
                framesCount++;
                if (now - lastFpsTime >= 1000) {
                    document.getElementById('fpsCounter').innerText = framesCount;
                    framesCount = 0;
                    lastFpsTime = now;
                }

                try {
                    const data = JSON.parse(event.data);
                    
                    if (data.timestamp) {
                        document.getElementById('latencyDisplay').innerText = Math.abs(now - data.timestamp) % 1000;
                    }

                    updateEmotionUI(data);
                    drawBBox(data.bbox, data.emotion);
                    updateHistory(data);
                    updateChart(data.emotion);
                    
                } catch(e) {
                    console.error("Parse error:", e);
                }
            };
        }

        function updateEmotionUI(data) {
            const em = emotionMap[data.emotion] || { name: '未知', emoji: '❓', color: '#888' };
            document.getElementById('currentEmoji').innerText = em.emoji;
            document.getElementById('currentEmotion').innerText = em.name;
            document.getElementById('currentEmotion').style.color = em.color;
            document.getElementById('currentEmoji').style.color = em.color;
            
            const confPercent = Math.round(data.confidence * 100);
            const confBar = document.getElementById('confBar');
            confBar.style.width = `${confPercent}%`;
            confBar.style.backgroundColor = em.color;
            document.getElementById('confText').innerText = confPercent;
            
            document.getElementById('emotionBox').style.boxShadow = `0 8px 32px 0 ${em.color}33`;
        }

        function drawBBox(bbox, emotion) {
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            if(!bbox) return;

            const scaleX = canvas.width / 640;
            const scaleY = canvas.height / 480;

            const x = bbox.x * scaleX;
            const y = bbox.y * scaleY;
            const w = bbox.w * scaleX;
            const h = bbox.h * scaleY;

            const color = (emotionMap[emotion] ? getComputedStyle(document.documentElement).getPropertyValue(emotionMap[emotion].color.match(/var\((.*?)\)/)[1]).trim() : '#fff');

            ctx.strokeStyle = color;
            ctx.lineWidth = 3;
            ctx.strokeRect(x, y, w, h);
            
            ctx.fillStyle = color;
            ctx.font = '16px Inter';
            ctx.fillText(emotionMap[emotion]?.name || emotion, x, y > 20 ? y - 5 : y + 20);
        }

        function updateHistory(data) {
            const log = document.getElementById('historyLog');
            const em = emotionMap[data.emotion] || { name: data.emotion };
            const time = new Date().toLocaleTimeString();
            const entry = document.createElement('div');
            entry.innerText = `[${time}] 偵測到 ${em.name} (信心: ${(data.confidence*100).toFixed(1)}%)`;
            log.prepend(entry);
            if(log.children.length > 50) {
                log.removeChild(log.lastChild);
            }
        }

        const pieCanvas = document.getElementById('pieChart');
        const pieCtx = pieCanvas.getContext('2d');

        function updateChart(emotion) {
            if(emotionCounts[emotion] !== undefined) {
                emotionCounts[emotion]++;
            }
            
            const total = Object.values(emotionCounts).reduce((a,b)=>a+b, 0);
            if(total === 0) return;

            pieCtx.clearRect(0,0,pieCanvas.width,pieCanvas.height);
            let startAngle = 0;
            const cx = pieCanvas.width/2;
            const cy = pieCanvas.height/2;
            const radius = Math.min(cx, cy) - 10;

            const colors = {
                'angry': getComputedStyle(document.documentElement).getPropertyValue('--angry').trim(),
                'focus': getComputedStyle(document.documentElement).getPropertyValue('--focus').trim(),
                'relax': getComputedStyle(document.documentElement).getPropertyValue('--relax').trim(),
                'scared': getComputedStyle(document.documentElement).getPropertyValue('--scared').trim()
            };

            for(const [em, count] of Object.entries(emotionCounts)) {
                if(count === 0) continue;
                const sliceAngle = (count / total) * 2 * Math.PI;
                pieCtx.beginPath();
                pieCtx.moveTo(cx, cy);
                pieCtx.arc(cx, cy, radius, startAngle, startAngle + sliceAngle);
                pieCtx.closePath();
                pieCtx.fillStyle = colors[em];
                pieCtx.fill();
                startAngle += sliceAngle;
            }
            
            pieCtx.beginPath();
            pieCtx.arc(cx, cy, radius * 0.6, 0, 2 * Math.PI);
            pieCtx.fillStyle = '#0f0f1a';
            pieCtx.fill();
        }

        connectWebSocket();
    </script>
</body>
</html>
)rawliteral";

// WebSocket 事件處理
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] 斷開連線!\n", num);
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] 已連線來自 %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
            }
            break;
        case WStype_TEXT:
            break;
    }
}

// 處理首頁請求
void handleRoot() {
    server.send(200, "text/html", index_html);
}

// 處理 MJPEG 串流請求
void handleStream() {
    WiFiClient client = server.client();
    String response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: multipart/x-mixed-replace; boundary=" PART_BOUNDARY "\r\n";
    response += "Connection: close\r\n\r\n";
    client.print(response);

    while (client.connected()) {
        if (!AI.invoke()) {
            if (AI.cam()->framesize() > 0) {
                String header = "--" PART_BOUNDARY "\r\n";
                header += "Content-Type: image/jpeg\r\n";
                header += "Content-Length: " + String(AI.cam()->framesize()) + "\r\n\r\n";
                
                client.print(header);
                client.write(AI.cam()->frame(), AI.cam()->framesize());
                client.print("\r\n");
            }
        }
        delay(30); // 控制幀率，避免佔用過多資源
    }
}

void setup() {
    Serial.begin(115200);
    
    // 初始化 AI 模組
    if (!AI.begin()) {
        Serial.println("初始化 Grove Vision AI V2 失敗！");
        while (1);
    }
    Serial.println("Grove Vision AI V2 初始化成功！");

    // 連線 WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("正在連線 WiFi");
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi 連線成功！");
        Serial.print("IP 位址: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi 連線失敗，切換至 AP 模式...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(ap_ssid, ap_password);
        Serial.print("AP IP 位址: ");
        Serial.println(WiFi.softAPIP());
    }

    // 啟動 WebServer 與 WebSocket
    server.on("/", handleRoot);
    server.on("/stream", handleStream);
    server.begin();
    Serial.println("Web Server 已啟動");

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("WebSocket 伺服器已啟動");
}

void loop() {
    server.handleClient();
    webSocket.loop();
    
    // 進行推論並推播結果
    if (!AI.invoke()) {
        if (AI.boxes().size() > 0) {
            // 假設模型輸出情緒的 target
            // 需根據實際訓練模型的 target id 對應
            String emotions[] = {"angry", "focus", "relax", "scared"};
            auto box = AI.boxes()[0];
            int targetIndex = box.target; 
            String currentEmotion = "relax"; // 預設值
            
            if (targetIndex >= 0 && targetIndex < 4) {
                currentEmotion = emotions[targetIndex];
            }

            // 組裝 JSON
            // {"emotion": "angry", "confidence": 0.92, "bbox": {"x": 100, "y": 50, "w": 200, "h": 180}, "timestamp": 12345}
            char jsonString[256];
            unsigned long timestamp = millis();
            snprintf(jsonString, sizeof(jsonString), 
                "{\"emotion\": \"%s\", \"confidence\": %.2f, \"bbox\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}, \"timestamp\": %lu}",
                currentEmotion.c_str(), 
                box.score / 100.0, 
                box.x, box.y, box.w, box.h,
                timestamp
            );
            
            webSocket.broadcastTXT(jsonString);
        }
    }
}
