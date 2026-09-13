# Cat Emotion ESP32-S3 Gateway Firmware

Production-grade firmware for the **Seeed Studio XIAO ESP32-S3** microcontroller. It bridges the Himax WiseEye2 vision AI module with local browser clients and remote cloud relays via high-throughput dual-band streaming.

---

## Prerequisites & Runtime Environment

### Supported Operating Systems
- **Linux**: Ubuntu 20.04 / 22.04 / 24.04 LTS, Debian 11 / 12, Fedora, Arch Linux, or compatible Linux distributions.
- **macOS**: macOS 12 Monterey or newer (Intel / Apple Silicon).
- **Windows**: Windows 10 / 11.

### Development Toolchains & Compilers
- **Arduino CLI**: v0.35.0 or higher (or Arduino IDE 2.x).
- **Board Support Package**: `esp32:esp32` by Espressif Systems (version 3.0.0+ recommended).
  - Target Board FQBN: `esp32:esp32:XIAO_ESP32S3:PSRAM=opi`
  - Flash Size: 8 MB with Octal-SPI (OPI) PSRAM enabled.

### Required Arduino Libraries
- `Seeed_Arduino_SSCMA` (v2.0.0+) - Communication and packet parser for Seeed Grove Vision AI V2.
- `WebSockets` (by Markus Sattler / Links2004, v2.4.0+) - WebSocket client and server implementations.
- Built-in ESP32 core libraries: `WiFi`, `DNSServer`, `ESPmDNS`, `esp_random`.

### Linux Serial Port Permissions
To upload firmware and monitor serial output via `/dev/ttyACM*`, add your user to the `dialout` group:
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in for group changes to take effect
```

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

// Discord Webhook integration (boot PIN & feline stress anomaly alerts)
const bool     discord_enabled     = true;
const char*    discord_webhook_url = "https://discord.com/api/webhooks/YOUR_ID/YOUR_TOKEN";
```
`cat_emotion_camera_server/secrets.h` is excluded in `.gitignore` and will never be uploaded to GitHub. If `secrets.h` is not present, the firmware falls back safely to `secrets.example.h`.

---

## Feline Stress Detection Algorithm & Discord Integration

### Clinical & Ethological Literature Foundation
The firmware implements an advanced **Temporal Feline Stress Index (TFSI)** grounded in peer-reviewed animal behavior, veterinary welfare, and affective computing literature:

1. **Cat Stress Score (CSS)**:
   - *Citation*: Kessler, M. R., & Turner, D. C. (1997). Stress and adaptation of cats (*Felis silvestris catus*) housed singly, in pairs and in groups in boarding catteries. *Animal Welfare*, 6(3), 243-254.
   - *Clinical Resource*: [ASPCApro Cat Stress Score (CSS) Guide](https://www.aspcapro.org/resource/cat-stress-score-css)
   - *Model Application*: Maps ordinal behavioral states (CSS 1-7) to numerical weights and estimates continuous clinical welfare levels.

2. **Feline Grimace Scale (FGS)**:
   - *Citation*: Evangelista, M. C., Watanabe, R., Leung, V. S. Y., Monteiro, B. P., O'Toole, E., Pang, D. S. J., & Steagall, P. V. (2019). Facial expressions of pain in cats: the development and validation of Feline Grimace Scale. *Nature Scientific Reports*, 9, 19128.
   - *DOI*: [10.1038/s41598-019-55693-8](https://www.nature.com/articles/s41598-019-55693-8)
   - *Model Application*: Validates orbital tightening (squinting), ear flattening / rotating outwards ("airplane ears"), and facial muzzle tension as objective distress indicators.

3. **Cat Facial Action Coding System (CatFACS)**:
   - *Citation*: Caeiro, C. C., Burrows, A. M., & Waller, B. M. (2017). Development and application of CatFACS: Are human cat adopters influenced by cat facial expressions?. *Applied Animal Behaviour Science*, 189, 66-78.
   - *DOI*: [10.1016/j.applanim.2017.01.005](https://www.sciencedirect.com/science/article/pii/S016815911730105X)
   - *Model Application*: Identifies anatomical muscle action units: AU105 (ear rotator down - fear) and AU43 (eyes closed / squint).

4. **Automated Deep Learning Affect Recognition**:
   - *Citation*: Feighelstein, M., et al. (2023). Explainable automated recognition of feline pain and facial expressions using deep learning. *Nature Scientific Reports*, 13, 14227.
   - *DOI*: [10.1038/s41598-023-41076-7](https://www.nature.com/articles/s41598-023-41076-7)
   - *Model Application*: Supports edge temporal aggregation of spatial bounding boxes over isolated single-frame classifications.

5. **Environmental Stressors & Autonomic Dynamics**:
   - *Citation*: Stella, J., Croney, C., & Buffington, T. (2013). Effects of stressors on the behavior and physiology of domestic cats. *Journal of Feline Medicine and Surgery*, 15(7), 578-586.
   - *DOI*: [10.1177/1098612X13489215](https://journals.sagepub.com/doi/10.1177/1098612X13489215)
   - *Model Application*: Governs acute surge dynamics where high-certainty distress escalates sympathetic arousal faster than gradual baseline shifts.

6. **Acoustic & Agonistic Posture Escalation**:
   - *Citation*: Yeon, S. C., et al. (2002). Differences in vocalization between feral and domestic cats. *Applied Animal Behaviour Science*, 77(3), 209-221.
   - *DOI*: [10.1016/S0168-1591(02)00030-X](https://www.sciencedirect.com/science/article/pii/S016815910200030X)
   - *Model Application*: Distinguishes fleeting orientation responses (< 2-3s) from sustained fight-or-flight sympathetic escalation (> 6-8s).

### Algorithmic Parameters & Mathematical Dynamics
- **State Weighting ($W_k$)**:
  - `relax`: $W = 0.00$ (CSS Level 1-2, calm baseline)
  - `focus`: $W = 0.15$ (CSS Level 3-4, environmental curiosity / mild vigilance)
  - `scared`: $W = 0.85$ (CSS Level 5-6, acute fear / flattened ears)
  - `angry`: $W = 1.00$ (CSS Level 6-7, defensive aggression / fight-or-flight)
- **Exponentially Weighted Moving Average (EWMA)**:
  Continuous temporal smoothing with time constant $\tau = 3.5\text{ seconds}$:
  $$\alpha = \frac{\Delta t}{\tau + \Delta t}, \quad S_t = (1 - \alpha) S_{t-1} + \alpha (W_k \times C_k)$$
  where $C_k$ is the classification confidence score.
- **Acute Distress Surge Acceleration**:
  When distress confidence exceeds $0.80$, an acute surge multiplier $M = 1.0 + (C_k - 0.80) \times 2.0$ accelerates the duration accumulator up to $1.4\times$, rapidly capturing acute panic events (Stella et al., 2013).
- **Dual-Threshold Schmitt Trigger (Hysteresis)**:
  - *Trigger Threshold*: $S_{\text{smoothed}} \ge 0.65$ sustained for $\ge 7.0\text{ seconds}$.
  - *Reset Threshold*: $S_{\text{smoothed}} \le 0.35$. Prevents alert bouncing when emotional state oscillates near the boundary.
- **Notification Cooldown**: 300-second (5-minute) timer prevents notification spamming.
- **24/7 Autonomous Operation**: Emotion inference and stress scoring run autonomously regardless of whether a web browser or WebSocket client is connected.

### On-Device Snapshot Annotation & Multipart Discord Upload
When an acute feline stress anomaly triggers, the ESP32-S3 automatically captures, annotates, and uploads a high-resolution camera screenshot:

```
[Core 1: Vision / Stream Engine]
  -> Cat distress detected (S >= 0.65, t >= 7.0s)
  -> Zero-copy snapshot clone to PSRAM (heap_caps_malloc)
  -> Enqueue (snapshot_buf, len, bbox) to FreeRTOS s_discord_queue
  -> Resume 60 FPS video streaming with ZERO latency spike

[Core 0: discordWorkerTask (Low Priority Background)]
  -> Receive message from s_discord_queue
  -> esp_jpeg_decode: Decompress JPEG to RGB888 in 8MB Octal PSRAM
  -> Coordinate verification & boundary clamping
  -> Draw 3-pixel thick bounding box outline in alert color (Crimson / Amber)
  -> Render high-contrast label badge (e.g., "SCARED 92%") using 5x7 ASCII font
  -> fmt2jpg: Re-encode annotated RGB888 buffer back to JPEG
  -> Free intermediate RGB888 buffer
  -> Construct multipart/form-data payload with payload_json and files[0]
  -> HTTPS POST to Discord Webhook: rich embed card with embedded snapshot
  -> Free re-encoded JPEG and snapshot buffers
```

- **Zero Stream Freezing**: All image decoding, drawing, re-encoding, and HTTPS networking occur exclusively on Core 0 in a background FreeRTOS task. The live video stream on Core 1 maintains constant 60 FPS without dropping frames.
- **Rich Embed Presentation**: Discord messages include the detected emotion, calculated CSS level, sustained duration, model confidence percentage, academic literature links, caregiver recommendations, and the annotated camera snapshot displayed directly inside the alert card.

---

## Dynamic Session Security PIN

Every time the ESP32 boots up, a cryptographically secure 6-digit hardware-random PIN is generated:
- The ESP32 registers the PIN with the cloud relay when establishing the outbound WebSocket tunnel.
- Visitors to the public stream must enter the PIN to unlock video streaming and telemetry.
- The PIN is printed over the USB Serial Monitor (115200 baud).
- Authorized local users connected to the ESP32's SoftAP (`192.168.4.1`) can view the current PIN at `http://192.168.4.1/status` or query `http://192.168.4.1/pin`.
- When Discord alerts are enabled, the PIN is also posted directly to your Discord channel on every boot.

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
