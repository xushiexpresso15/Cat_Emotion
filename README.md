# Cat Emotion ESP32-S3 Gateway Firmware

Production-grade firmware for the **Seeed Studio XIAO ESP32-S3** microcontroller. It bridges the Himax WiseEye2 vision AI module with local browser clients and remote cloud relays via high-throughput dual-band streaming.

---

## Architecture Overview

The ESP32-S3 serves as the network gateway and telemetry distributor for the entire system:

```
+--------------------------------+                  +-----------------------------------+
|     Grove Vision AI V2         |                  |        Seeed Studio XIAO          |
|  (Himax HX6538 + Ethos-U55)   |                  |             ESP32-S3              |
|                                |   High-Speed     |                                   |
| - 240x240 Camera ISP           |   UART @ 921600  | - Core 1: SSCMA Frame Parser      |
| - Dual-Head YOLO NPU Inference | ---------------> | - Core 0: Dual-Mode Wi-Fi Stack   |
| - D3 Hardware Reset Line       | <--------------- | - 8MB OPI PSRAM Frame Buffers     |
+--------------------------------+                  +-----------------------------------+
                                                                   |             |
                                            Local Wi-Fi / SoftAP   |             | TLS WebSocket
                                            (192.168.4.1)          |             | (wss://)
                                                                   v             v
                                                            +------------+ +------------+
                                                            | Local Web  | | Cloud      |
                                                            | Dashboard  | | Relay Node |
                                                            +------------+ +------------+
```

### Key Technical Capabilities

1. **Dual-Mode Wi-Fi (Concurrent AP + STA)**:
   - **SoftAP Mode (`192.168.4.1`)**: Creates an independent wireless hotspot (`Cat_Emotion_AP`). Allows mobile phones or laptops to connect and monitor live telemetry directly in the field without requiring an external Wi-Fi router.
   - **Station Mode (STA)**: Concurrently associates with external routers or mobile hotspots. Enables automatic outbound tunneling to the cloud relay.
   - **Non-Blocking Reconnection**: Asynchronous background health check automatically reconnects dropped Station links without freezing frame capture or dropping SoftAP clients.

2. **High-Speed Dual-Ring Buffer (PSRAM)**:
   - Communicates with the Himax processor at 921,600 baud.
   - Leverages 8 MB Octal-SPI (OPI) PSRAM for zero-copy frame double-buffering, preventing packet corruption during high-framerate bursts.

3. **Multi-Channel Distribution**:
   - **Embedded Web Server (Port 80)**: Serves a modern HTML5 dashboard stored in compressed flash memory.
   - **Local WebSocket Server (Port 81)**: Broadcasts binary JPEG frames and JSON bounding boxes directly to local clients at ultra-low latency.
   - **Outbound Cloud Relay Client**: Simultaneously pushes frames and inference telemetry to a public cloud relay service (such as Render or Railway) over secure TLS.
   - **Standard MJPEG Endpoint (`/stream`)**: Provides a native HTTP multipart stream for Home Assistant, VLC, or OpenCV.

4. **Hardware State Synchronization**:
   - GPIO pin D3 drives the Himax module reset line upon startup, ensuring deterministic SSCMA protocol synchronization on every boot.

---

## Hardware Pinout & Wiring

| XIAO ESP32-S3 Pin | Function | Grove Vision AI V2 Pin | Description |
|---|---|---|---|
| `D6` | UART TX | `RX` | Serial command transmission |
| `D7` | UART RX | `TX` | High-speed telemetry ingestion (921,600 baud) |
| `D3` | GPIO Out | `RST` | Active-low hardware reset pulse |
| `3V3` | Power Out | `3V3` | Regulated 3.3V power rail |
| `GND` | Ground | `GND` | Common ground reference |

---

## Configuration

Credentials and environment parameters are separated into a dedicated configuration file to keep sensitive data out of Git tracking.

### 1. Create Your Local `secrets.h`
Copy the provided template:
```bash
cp cat_emotion_camera_server/secrets.example.h cat_emotion_camera_server/secrets.h
```
Edit `cat_emotion_camera_server/secrets.h` with your Wi-Fi credentials and cloud relay token:
```cpp
// Station networks (tried in order)
const KnownNetwork known_networks[] = {
    {"YOUR_HOTSPOT_SSID", "YOUR_HOTSPOT_PASSWORD"},  // Primary (e.g. mobile hotspot)
    {"YOUR_ROUTER_SSID",  "YOUR_ROUTER_PASSWORD"}   // Secondary (e.g. home router)
};

// SoftAP settings (direct connection)
const char* ap_ssid     = "Cat_Emotion_AP";
const char* ap_password = "password123";

// Cloud Relay settings
const bool     cloud_relay_enabled = true;
const char*    cloud_relay_host    = "cat-emo-live.onrender.com";
const uint16_t cloud_relay_port    = 443;
const char*    cloud_relay_path    = "/esp32";
const bool     cloud_relay_ssl     = true;

// Pre-shared authentication token (matches STREAM_SECRET on the relay)
const char*    cloud_relay_token   = "your_stream_secret_here";
```
`cat_emotion_camera_server/secrets.h` is excluded in `.gitignore` and will never be uploaded to GitHub. If `secrets.h` is not present, the firmware falls back safely to `secrets.example.h`.

---

## Dynamic Session Security PIN

Every time the ESP32 boots up, a cryptographically secure 6-digit hardware-random PIN is generated:
- The ESP32 registers the PIN with the cloud relay when establishing the outbound WebSocket tunnel.
- Visitors to the public stream must enter the PIN to unlock video streaming and telemetry.
- The PIN is printed over the USB Serial Monitor (115200 baud).
- Authorized local users connected to the ESP32's SoftAP (`192.168.4.1`) can view the current PIN at `http://192.168.4.1/status` or query `http://192.168.4.1/pin`.

---

## Build & Flash Instructions

### Prerequisites

1. Install [Arduino CLI](https://arduino.github.io/arduino-cli/) or Arduino IDE (v2.x).
2. Install the ESP32 Board Support Package (version 3.0+ recommended):
   ```bash
   arduino-cli core update-index
   arduino-cli core install esp32:esp32
   ```
3. Install required libraries:
   - `Seeed_Arduino_SSCMA`
   - `WebSockets` (by Markus Sattler)

### Compilation via Arduino CLI (Recommended)

> **Important**: The XIAO ESP32-S3 requires Octal PSRAM (`PSRAM=opi`). In BSP v3+, the default partition scheme is 8MB (3MB APP / 1.5MB SPIFFS).

```bash
# Clone the firmware branch
git clone -b esp32-firmware https://github.com/xushiexpresso15/Cat_Emotion.git esp32-gateway
cd esp32-gateway

# Compile sketch
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi cat_emotion_camera_server

# Upload to the device (replace /dev/ttyACM0 with your board port)
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi cat_emotion_camera_server
```

### Arduino IDE Configuration

If using the GUI:
- **Board**: `XIAO_ESP32S3`
- **Flash Size**: `8MB (64Mb)`
- **Partition Scheme**: `Huge APP (3MB No OTA/1MB SPIFFS)`
- **PSRAM**: `OPI PSRAM`
- **Upload Speed**: `921600`
- **USB CDC On Boot**: `Enabled`

---

## Local Web Dashboard

The firmware embeds a compressed web application (`web_index.h`). When you connect to the device IP or navigate to `http://cat.local` (or `http://192.168.4.1` on SoftAP), the page renders:
- Real-time video frame canvas.
- Dynamically rendered bounding boxes with confidence scores.
- Classified cat emotional state (e.g., Relaxed, Scared, Angry, Focused).
- Network RSSI and FPS performance graphs.

### Regenerating `web_index.h`
If you modify the frontend HTML/CSS/JS in a local `index.html` file, update the embedded C header with:
```bash
gzip -9 -c index.html | xxd -i > web_index.h
```

---

## Extending the Firmware

- **Home Assistant MQTT Auto-Discovery**: Include `PubSubClient` to broadcast emotion detection events to an MQTT broker whenever classification confidence exceeds a threshold.
- **On-Device Logging**: Save snapshot frames to an attached micro-SD card module when negative emotional states (Fear/Aggression) are detected.
- **RGB Status Indicator**: Drive the on-board WS2812 RGB LED to visually reflect the cat's detected state in real time without opening a browser.

---

## License

This firmware is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
