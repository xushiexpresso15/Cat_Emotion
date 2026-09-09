# Cat Emotion ESP32-S3 Wireless Telemetry Gateway Firmware

## Overview
This firmware runs on the Seeed Studio XIAO ESP32-S3 microcontroller. It serves as the primary communications gateway between the Grove Vision AI V2 module and downstream consumers.

## Architecture and Capabilities
- Dual-Mode Wi-Fi: Maintains an independent Access Point (SoftAP SSID: `Cat_Emotion_AP`, static IP: `192.168.4.1`) for local peer-to-peer access, while simultaneously operating as a Station (STA) client connecting to external Wi-Fi or mobile hotspots.
- High-Speed UART Communication: Communicates with the Himax WiseEye2 processor at 921,600 baud using large circular buffers allocated in 8 MB OPI PSRAM.
- Simultaneous Dual-WebSocket Streaming: Broadcasts decoded JPEG frames and inference JSON locally on port 81, and simultaneously establishes an outbound TLS WebSocket client connection (`wss://`) to push telemetry to the designated cloud relay.
- Non-Blocking Background Reconnection: Background health checks ensure automatic reconnection to mobile hotspots without interrupting the real-time video stream.
- Hardware Reset Line: Pin D3 triggers an active-low reset pulse to the Himax module upon boot to ensure synchronized state machine execution.

## Hardware Pinout
- GPIO D6: UART TX -> Connected to Grove Vision AI RX
- GPIO D7: UART RX -> Connected to Grove Vision AI TX
- GPIO D3: Reset Out -> Connected to Grove Vision AI RST
- VCC: 3.3V power supply rail
- GND: Common ground reference

## Compilation and Flashing
Prerequisites: Arduino CLI configured with ESP32 board support package (v3.x or later).

```bash
# Compile with 8MB OPI PSRAM enabled
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi .

# Upload to connected board
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi .
```
