const http = require('http');
const express = require('express');
const { WebSocketServer, WebSocket } = require('ws');
const path = require('path');
const cors = require('cors');

const app = express();
app.use(cors());
app.use(express.static(path.join(__dirname, 'public')));

const server = http.createServer(app);
const PORT = process.env.PORT || 3000;
const STREAM_SECRET = process.env.STREAM_SECRET || null;
const MAX_PAYLOAD = parseInt(process.env.MAX_PAYLOAD || '1048576', 10); // 1 MB limit against OOM DoS
const MAX_VIEWERS = parseInt(process.env.MAX_VIEWERS || '50', 10); // Max concurrent viewers

// State management
let esp32Ws = null;
let activeSessionPin = null;
let esp32LocalIp = null;
const viewerClients = new Set();
const mjpegClients = new Set();

let latestJpeg = null;
let latestMeta = null;
let totalFrames = 0;
let frameCountWindow = 0;
let currentFps = 0.0;
let lastFpsCheck = Date.now();
const startTime = Date.now();

// Calculate FPS every second
setInterval(() => {
    const now = Date.now();
    const elapsed = (now - lastFpsCheck) / 1000;
    if (elapsed >= 1.0) {
        currentFps = parseFloat((frameCountWindow / elapsed).toFixed(1));
        frameCountWindow = 0;
        lastFpsCheck = now;
    }
}, 1000);

// Status API
app.get('/api/status', (req, res) => {
    res.json({
        esp32_online: esp32Ws !== null && esp32Ws.readyState === WebSocket.OPEN,
        pin_protected: activeSessionPin !== null,
        esp32_local_ip: esp32LocalIp,
        viewers_count: viewerClients.size + mjpegClients.size,
        fps: currentFps,
        total_frames: totalFrames,
        uptime_seconds: Math.floor((Date.now() - startTime) / 1000)
    });
});

// MJPEG Stream endpoint
app.get('/stream', (req, res) => {
    // If stream is PIN protected, require ?pin=XXXXXX
    if (activeSessionPin && req.query.pin !== activeSessionPin) {
        return res.status(403).json({
            error: 'Forbidden: Valid session PIN required. Access via /stream?pin=XXXXXX'
        });
    }

    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Connection': 'close',
        'Pragma': 'no-cache'
    });

    mjpegClients.add(res);

    // If we have a cached frame, send it immediately
    if (latestJpeg) {
        try {
            res.write(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${latestJpeg.length}\r\n\r\n`);
            res.write(latestJpeg);
            res.write('\r\n');
        } catch (e) {}
    }

    req.on('close', () => {
        mjpegClients.delete(res);
    });
});

// WebSocket Server attached to HTTP server (enforcing 1MB max payload to prevent OOM DoS)
const wss = new WebSocketServer({
    noServer: true,
    maxPayload: MAX_PAYLOAD
});

server.on('upgrade', (request, socket, head) => {
    let parsedUrl;
    try {
        parsedUrl = new URL(request.url, `http://${request.headers.host || 'localhost'}`);
    } catch (e) {
        socket.destroy();
        return;
    }
    const pathname = parsedUrl.pathname;

    if (pathname === '/esp32') {
        if (STREAM_SECRET) {
            const token = parsedUrl.searchParams.get('token') || request.headers['x-stream-token'];
            if (token !== STREAM_SECRET) {
                console.warn(`[Security] Unauthorized /esp32 ingestion attempt from ${socket.remoteAddress}`);
                socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
                socket.destroy();
                return;
            }
        }
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request);
        });
    } else if (pathname === '/ws') {
        if (viewerClients.size >= MAX_VIEWERS) {
            console.warn(`[Security] Viewer connection rejected: maximum viewer limit (${MAX_VIEWERS}) reached.`);
            socket.write('HTTP/1.1 429 Too Many Requests\r\n\r\n');
            socket.destroy();
            return;
        }
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request);
        });
    } else {
        socket.destroy();
    }
});

wss.on('connection', (ws, req) => {
    let parsedUrl;
    try {
        parsedUrl = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
    } catch (e) {
        parsedUrl = { pathname: '' };
    }
    const pathname = parsedUrl.pathname;

    if (pathname === '/esp32') {
        const incomingPin = parsedUrl.searchParams.get('pin');
        const incomingIp = parsedUrl.searchParams.get('ip') || parsedUrl.searchParams.get('local_ip');
        if (incomingIp) {
            esp32LocalIp = incomingIp.trim();
            console.log(`[ESP32] Registered Local IP: ${esp32LocalIp}`);
        }
        if (incomingPin) {
            activeSessionPin = incomingPin;
            console.log(`[Security] ESP32 registered dynamic Session PIN: ${activeSessionPin}`);
        } else {
            activeSessionPin = null;
            console.log(`[Security] ESP32 connected without PIN (open stream mode).`);
        }

        console.log(`[ESP32] Connected from ${req.socket.remoteAddress}`);
        if (esp32Ws && esp32Ws !== ws && esp32Ws.readyState === WebSocket.OPEN) {
            esp32Ws.close();
        }
        esp32Ws = ws;

        // If a new PIN was registered, require re-authentication for connected viewers
        if (activeSessionPin) {
            for (const client of viewerClients) {
                client.isAuthenticated = false;
                client.send(JSON.stringify({
                    type: 'auth_required',
                    locked: true,
                    esp32_local_ip: esp32LocalIp
                }));
            }
        }

        ws.on('message', (data, isBinary) => {
            if (isBinary) {
                // Reject invalid or non-JPEG binary payloads (JPEG starts with SOI 0xFF, 0xD8)
                if (!data || data.length < 4 || data[0] !== 0xFF || data[1] !== 0xD8) {
                    return;
                }
                latestJpeg = data;
                totalFrames++;
                frameCountWindow++;

                // Broadcast binary JPEG ONLY to authenticated viewer WebSockets
                for (const client of viewerClients) {
                    if (client.readyState === WebSocket.OPEN && client.isAuthenticated) {
                        client.send(data, { binary: true });
                    }
                }

                // Broadcast to MJPEG HTTP clients
                if (mjpegClients.size > 0) {
                    const header = Buffer.from(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${data.length}\r\n\r\n`);
                    const trailer = Buffer.from('\r\n');
                    for (const res of mjpegClients) {
                        try {
                            res.write(header);
                            res.write(data);
                            res.write(trailer);
                        } catch (err) {
                            mjpegClients.delete(res);
                        }
                    }
                }
            } else {
                // JSON Metadata / Emotion string
                const text = data.toString('utf8');
                try {
                    const parsed = JSON.parse(text);
                    if (parsed.ip || parsed.local_ip) {
                        esp32LocalIp = String(parsed.ip || parsed.local_ip).trim();
                    }
                } catch (e) {}
                latestMeta = text;

                // Broadcast metadata ONLY to authenticated viewer WebSockets
                for (const client of viewerClients) {
                    if (client.readyState === WebSocket.OPEN && client.isAuthenticated) {
                        client.send(text, { binary: false });
                    }
                }
            }
        });

        ws.on('close', () => {
            console.log('[ESP32] Disconnected');
            if (esp32Ws === ws) {
                esp32Ws = null;
            }
            // Notify viewers that camera is offline
            const offlineMsg = JSON.stringify({ type: 'status', esp32_online: false, esp32_local_ip: esp32LocalIp });
            for (const client of viewerClients) {
                if (client.readyState === WebSocket.OPEN && client.isAuthenticated) {
                    client.send(offlineMsg);
                }
            }
        });

        ws.on('error', (err) => {
            console.error('[ESP32 Error]', err.message);
        });

    } else if (pathname === '/ws') {
        viewerClients.add(ws);
        ws.isAuthenticated = (activeSessionPin === null);
        ws.failedAttempts = 0;
        console.log(`[Viewer] Connected from ${req.socket.remoteAddress}. Auth: ${ws.isAuthenticated ? 'OPEN' : 'PIN_REQUIRED'}. Total viewers: ${viewerClients.size}`);

        // Notify viewer about status and authentication requirement
        if (!ws.isAuthenticated) {
            ws.send(JSON.stringify({
                type: 'auth_required',
                locked: true,
                esp32_online: esp32Ws !== null && esp32Ws.readyState === WebSocket.OPEN,
                esp32_local_ip: esp32LocalIp
            }));
        } else {
            ws.send(JSON.stringify({
                type: 'status',
                locked: false,
                esp32_online: esp32Ws !== null && esp32Ws.readyState === WebSocket.OPEN,
                esp32_local_ip: esp32LocalIp
            }));
            if (latestMeta) ws.send(latestMeta);
            if (latestJpeg) ws.send(latestJpeg, { binary: true });
        }

        ws.on('message', (data) => {
            try {
                const msg = JSON.parse(data.toString('utf8'));
                if (msg.type === 'auth') {
                    if (activeSessionPin && msg.pin === activeSessionPin) {
                        ws.isAuthenticated = true;
                        ws.failedAttempts = 0;
                        console.log(`[Viewer] PIN Verified for ${req.socket.remoteAddress}`);
                        ws.send(JSON.stringify({
                            type: 'auth_result',
                            success: true,
                            locked: false,
                            esp32_online: esp32Ws !== null && esp32Ws.readyState === WebSocket.OPEN
                        }));
                        if (latestMeta) ws.send(latestMeta);
                        if (latestJpeg) ws.send(latestJpeg, { binary: true });
                    } else {
                        ws.failedAttempts = (ws.failedAttempts || 0) + 1;
                        console.warn(`[Viewer] Invalid PIN (${ws.failedAttempts}/5) from ${req.socket.remoteAddress}`);
                        if (ws.failedAttempts >= 5) {
                            ws.send(JSON.stringify({
                                type: 'auth_result',
                                success: false,
                                locked: true,
                                error: 'Too many failed attempts. Disconnected.'
                            }));
                            ws.close();
                        } else {
                            ws.send(JSON.stringify({
                                type: 'auth_result',
                                success: false,
                                locked: true,
                                error: 'Invalid PIN. Please check ESP32 Serial or AP status page.'
                            }));
                        }
                    }
                }
            } catch (err) {}
        });

        ws.on('close', () => {
            viewerClients.delete(ws);
            console.log(`[Viewer] Disconnected. Remaining viewers: ${viewerClients.size}`);
        });

        ws.on('error', (err) => {
            viewerClients.delete(ws);
        });
    }
});

// Periodic ping to keep cloud connections alive (Render, Cloudflare, etc.)
setInterval(() => {
    wss.clients.forEach((ws) => {
        if (ws.isAlive === false) return ws.terminate();
        ws.isAlive = false;
        ws.ping();
    });
}, 20000);

wss.on('connection', (ws) => {
    ws.isAlive = true;
    ws.on('pong', () => {
        ws.isAlive = true;
    });
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`=========================================`);
    console.log(`  Cat Emotion Cloud Relay Server Online  `);
    console.log(`  Local HTTP / Viewer: http://localhost:${PORT}`);
    console.log(`  ESP32 Push Endpoint: ws://localhost:${PORT}/esp32`);
    console.log(`  Viewer WS Endpoint:   ws://localhost:${PORT}/ws`);
    console.log(`  Authentication:      ${STREAM_SECRET ? 'ENABLED (STREAM_SECRET set)' : 'DISABLED (Open dev mode)'}`);
    console.log(`  Max Message Payload: ${Math.round(MAX_PAYLOAD / 1024)} KB`);
    console.log(`  Max Concurrent View: ${MAX_VIEWERS}`);
    console.log(`=========================================`);
});
