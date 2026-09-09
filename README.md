# Cat Emotion Cloud Relay Service

A lightweight, high-concurrency Node.js relay broker that bridges resource-constrained edge hardware (ESP32-S3) with multi-client web browsers and home automation systems.

---

## Why a Cloud Relay?

The ESP32-S3 microcontroller handles video capture, dual-UART synchronization with the Himax WiseEye2 NPU, and JPEG compression. While the ESP32-S3 has integrated Wi-Fi, its internal network buffer and memory cannot sustain multiple concurrent HTTP/WebSocket streams without severe frame drops or Wi-Fi stack panic.

This relay solves the problem by decoupling ingestion from distribution:
- **Upstream (1 Connection)**: The ESP32 maintains a single persistent WebSocket connection to the relay, streaming binary JPEG frames and JSON bounding box/classification metadata.
- **Downstream (N Connections)**: The relay caches the latest state and broadcasts incoming frames and telemetry to arbitrary numbers of connected web browsers, MJPEG players, and external webhooks.
- **Zero Transcoding Overhead**: Frames are forwarded directly as raw binary chunks without decoding or re-encoding on the server, keeping CPU usage minimal.

---

## Features

- **Dual-Path Streaming**:
  - `WS /esp32`: High-speed binary JPEG and UTF-8 JSON ingestion from the edge camera.
  - `WS /ws`: Low-latency broadcast to browser clients with canvas bounding box rendering.
  - `GET /stream`: Standard multipart MJPEG stream compatible with VLC, Home Assistant, and OpenCV.
- **Embedded Web Dashboard**: Modern responsive UI with real-time bounding box visualization, emotion classification badges, FPS counter, and connection health monitors.
- **Reverse Proxy Resilience**: Integrated 20-second WebSocket heartbeat (`ping`/`pong`) to prevent connection termination by cloud load balancers.
- **Zero External Database Requirement**: Completely stateless in-memory cache for ultra-low latency.

---

## System Architecture

```
+------------------+         WebSocket (/esp32)         +----------------------+
|  ESP32-S3 Edge   | ---------------------------------> |   Cloud Relay Node   |
|  Camera Device   |   (Binary JPEG + JSON Metadata)    |     (Express + WS)   |
+------------------+                                    +----------------------+
                                                               |          |
                              +--------------------------------+          |
                              | WebSocket (/ws)                           | HTTP (/stream)
                              v                                           v
                     +--------------------+                             +--------------------+
                     |  Browser Clients   |                             |   Home Assistant   |
                     |  (HTML5 Dashboard) |                             |     / VLC Player   |
                     +--------------------+                             +--------------------+
```

---

## Quick Start

### Prerequisites

- Node.js 18.x or later
- npm 9.x or later

### Installation & Local Run

1. Clone this branch:
   ```bash
   git clone -b cloud-relay https://github.com/xushiexpresso15/Cat_Emotion.git cat-emotion-relay
   cd cat-emotion-relay
   ```

2. Install dependencies:
   ```bash
   npm install
   ```

3. Start the server:
   ```bash
   npm start
   ```

   The dashboard will be available at `http://localhost:3000`.

---

## Docker Deployment

### Using Docker CLI

Build and launch the containerized relay:

```bash
docker build -t cat-emotion-relay .
docker run -d --name cat-relay -p 3000:3000 --restart unless-stopped cat-emotion-relay
```

### Using Docker Compose

Create a `docker-compose.yml` file:

```yaml
version: '3.8'
services:
  cat-relay:
    build: .
    ports:
      - "3000:3000"
    environment:
      - PORT=3000
    restart: unless-stopped
```

Run with:
```bash
docker compose up -d
```

---

## Production Cloud Deployment

### Deploying to Render (Free Tier)

1. Fork or push this repository to your GitHub account.
2. In the Render Dashboard, create a new **Web Service**.
3. Connect your repository and select the `cloud-relay` branch.
4. Configure the build parameters:
   - **Environment**: `Node`
   - **Build Command**: `npm install`
   - **Start Command**: `node server.js`
   - **Plan**: `Free`
5. (Optional but Recommended) Under **Environment Variables**, add:
   - `STREAM_SECRET`: Set to a strong secret key. Your ESP32 must provide this token via `?token=<SECRET>` to stream.
   - `MAX_PAYLOAD`: `1048576` (Default 1MB, limits frame payload size against DoS).
   - `MAX_VIEWERS`: `50` (Protects server memory from excess connections).
6. Click **Create Web Service**. Once deployed, Render provides a permanent public URL (for example, `https://your-service.onrender.com`).
7. Point your ESP32 firmware to this URL:
   - Host: `your-service.onrender.com`
   - Port: `443`
   - Path: `/esp32`
   - SSL: Enabled
   - Token: Matching `STREAM_SECRET`

---

## Environment Variables Reference

| Variable | Default | Description |
|---|---|---|
| `PORT` | `3000` | Local HTTP and WebSocket port |
| `STREAM_SECRET` | `null` (Open dev mode) | Pre-shared key required at `WS /esp32?token=<SECRET>` |
| `MAX_PAYLOAD` | `1048576` (1 MB) | Maximum incoming WebSocket payload in bytes |
| `MAX_VIEWERS` | `50` | Maximum simultaneous browser connections before rate-limiting |

### Exposing Local Server via Cloudflare Tunnel

If hosting on a local machine or Raspberry Pi without public IP / port forwarding:

```bash
# Using the helper script
chmod +x start_relay.sh
./start_relay.sh
```

Or manually:
```bash
cloudflared tunnel --url http://localhost:3000
```

Cloudflare provides a free HTTPS hostname routing directly to your local instance.

---

## API & Protocol Specification

### 1. HTTP Endpoints

| Method | Path | Description | Response Content-Type |
|---|---|---|---|
| `GET` | `/` | Web monitoring dashboard | `text/html` |
| `GET` | `/stream` | Multipart continuous MJPEG video stream | `multipart/x-mixed-replace; boundary=frame` |
| `GET` | `/api/status` | Current system health and stream metrics | `application/json` |

#### Sample `/api/status` Response:
```json
{
  "esp32_online": true,
  "viewers_count": 3,
  "fps": 8.5,
  "total_frames": 1420,
  "uptime_seconds": 320
}
```

### 2. WebSocket Ingestion (`WS /esp32`)

Reserved for edge camera upload. The server automatically classifies messages by data type:

- **Binary Buffer**: Interpreted as a complete JPEG image frame. Immediately cached in memory and broadcast to active viewer clients and MJPEG consumers.
- **UTF-8 Text**: Interpreted as inference telemetry in JSON format. Immediately broadcast to browser clients.

#### Example Telemetry Payload:
```json
{
  "box": [48, 62, 185, 204],
  "emotion": "relaxed",
  "score": 0.88,
  "fps": 8.3
}
```

### 3. WebSocket Downstream (`WS /ws`)

Subscribed by browser clients. Upon connection, the server:
1. Emits a status handshake: `{"type": "status", "esp32_online": true}`
2. Sends the latest cached telemetry string and JPEG binary frame.
3. Streams all subsequent frames and metadata in real time.

---

## Extending This Service

This relay is deliberately designed to be minimal and modular. Here are common extensions you can build:

### 1. Alert Webhooks (Discord, Telegram, LINE)
Intercept the JSON payload in `server.js` within the `ws.on('message')` handler:
```javascript
if (!isBinary) {
  const meta = JSON.parse(data.toString('utf8'));
  if ((meta.emotion === 'angry' || meta.emotion === 'scared') && meta.score > 0.85) {
    sendNotificationToDiscord(meta);
  }
}
```

### 2. Time-Series Telemetry Storage
Pipe incoming predictions into InfluxDB, SQLite, or PostgreSQL to generate historical feline behavior graphs:
- Track emotion distribution across different hours of the day.
- Correlate ambient factors (feeding time, noise) with stress levels.

### 3. Home Assistant Integration
Add the MJPEG endpoint to your `configuration.yaml`:
```yaml
camera:
  - platform: mjpeg
    name: Cat Emotion Cam
    mjpeg_url: https://your-service.onrender.com/stream
```

### 4. Token-Based Authentication
To secure the upstream `/esp32` endpoint against unauthorized ingestion, inspect query parameters or headers during the HTTP upgrade step in `server.js`:
```javascript
server.on('upgrade', (request, socket, head) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname === '/esp32') {
    const token = url.searchParams.get('token');
    if (token !== process.env.DEVICE_SECRET) {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
      socket.destroy();
      return;
    }
  }
  // proceed with upgrade...
});
```

---

## Project Structure

```
.
├── Dockerfile          # Multi-stage production container configuration
├── LICENSE             # MIT License
├── package.json        # Dependencies and runtime scripts
├── public/
│   └── index.html      # Responsive HTML5/Canvas dashboard
├── README.md           # This documentation
├── server.js           # Core Express + WebSocket relay server
└── start_relay.sh      # Portable launcher with Cloudflare Tunnel detection
```

---

## Contributing

Contributions, bug reports, and feature requests are welcome.
1. Fork this repository.
2. Create a feature branch (`git checkout -b feature/alert-webhook`).
3. Commit your changes with clear, descriptive commit messages.
4. Push to the branch (`git push origin feature/alert-webhook`).
5. Open a Pull Request.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
