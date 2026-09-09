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

// State management
let esp32Ws = null;
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
        viewers_count: viewerClients.size + mjpegClients.size,
        fps: currentFps,
        total_frames: totalFrames,
        uptime_seconds: Math.floor((Date.now() - startTime) / 1000)
    });
});

// MJPEG Stream endpoint
app.get('/stream', (req, res) => {
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

// WebSocket Server attached to HTTP server
const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (request, socket, head) => {
    const pathname = request.url ? request.url.split('?')[0] : '';
    if (pathname === '/esp32' || pathname === '/ws') {
        wss.handleUpgrade(request, socket, head, (ws) => {
            wss.emit('connection', ws, request);
        });
    } else {
        socket.destroy();
    }
});

wss.on('connection', (ws, req) => {
    const pathname = req.url ? req.url.split('?')[0] : '';

    if (pathname === '/esp32') {
        console.log(`[ESP32] Connected from ${req.socket.remoteAddress}`);
        if (esp32Ws && esp32Ws !== ws && esp32Ws.readyState === WebSocket.OPEN) {
            esp32Ws.close();
        }
        esp32Ws = ws;

        ws.on('message', (data, isBinary) => {
            if (isBinary) {
                // JPEG Frame
                latestJpeg = data;
                totalFrames++;
                frameCountWindow++;

                // Broadcast binary JPEG to all viewer WebSockets
                for (const client of viewerClients) {
                    if (client.readyState === WebSocket.OPEN) {
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
                latestMeta = text;

                // Broadcast metadata to all viewer WebSockets
                for (const client of viewerClients) {
                    if (client.readyState === WebSocket.OPEN) {
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
            const offlineMsg = JSON.stringify({ type: 'status', esp32_online: false });
            for (const client of viewerClients) {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(offlineMsg);
                }
            }
        });

        ws.on('error', (err) => {
            console.error('[ESP32 Error]', err.message);
        });

    } else if (pathname === '/ws') {
        viewerClients.add(ws);
        console.log(`[Viewer] Connected. Total viewers: ${viewerClients.size}`);

        // Notify viewer about current status
        ws.send(JSON.stringify({
            type: 'status',
            esp32_online: esp32Ws !== null && esp32Ws.readyState === WebSocket.OPEN
        }));

        // Send latest metadata and JPEG immediately if available
        if (latestMeta) {
            ws.send(latestMeta);
        }
        if (latestJpeg) {
            ws.send(latestJpeg, { binary: true });
        }

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
    console.log(`=========================================`);
});
