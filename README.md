# Cat Emotion Cloud Relay Service

## Overview
This standalone Node.js service acts as an intermediate ingestion and redistribution broker for real-time video frames and inference telemetry from the ESP32-S3 edge device. It decouples high-bandwidth multi-client web consumption from the resource-constrained edge hardware.

## Features
- Ingestion Interface: Dedicated WebSocket route (`/esp32`) receiving binary JPEG frames and JSON metadata.
- Broadcast Distribution: High-throughput WebSocket route (`/ws`) forwarding data to connected client browsers.
- MJPEG Stream Fallback: HTTP multipart endpoint (`/stream`) compatible with standard video players and legacy browsers.
- Dynamic Web Dashboard: Responsive client application matching the local telemetry interface with real-time bounding box projection and emotion classification.
- Keepalive Mechanism: Periodic WebSocket heartbeat ping/pong to maintain persistent connections across reverse proxies.

## API Endpoints
- `GET /`: Serves the primary web monitoring dashboard.
- `GET /stream`: Multipart MJPEG continuous video stream.
- `GET /api/status`: JSON endpoint exposing camera online status, viewer count, active FPS, and uptime.
- `WS /esp32`: Authenticated upstream ingestion path for ESP32.
- `WS /ws`: Downstream client subscription path.

## Deployment on Render.com
1. Create a new Web Service on Render and point to this repository.
2. Select the `cloud-relay` branch.
3. Configure runtime settings:
   - Environment: Node
   - Build Command: `npm install`
   - Start Command: `node server.js`
   - Instance Type: Free ($0/month)
   - Region: Singapore (recommended for lowest APAC latency)
4. Deploy the service to obtain your permanent HTTPS/WSS URL.
