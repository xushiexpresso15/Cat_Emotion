# Real-Time Cat Emotion Recognition and Edge Telemetry System

## Abstract
This project implements an embedded edge computing system for real-time feline facial detection and emotional state classification. The architecture couples on-device deep learning acceleration with flexible wireless telemetry. It supports autonomous field operation powered by a portable power bank without external network dependencies, while concurrently providing cloud relay broadcasting for remote multi-user observation.

## System Architecture

```text
+-----------------------+      High-Speed UART (921,600)      +-------------------------+
|  OV5647 Camera Sensor | ----------------------------------> |  Seeed Studio XIAO      |
|  + Himax WiseEye2 NPU | <---------------------------------- |  ESP32-S3 Module        |
|  (YOLOv8n QAT INT8)   |        Hardware Pulse Reset         |  (Dual Wi-Fi AP + STA)  |
+-----------------------+                                     +-------------------------+
                                                                           |
                                             +-----------------------------+-----------------------------+
                                             |                                                           |
                                             v                                                           v
                                  Local Hotspot (SoftAP)                                      Cloud WebSocket Client
                                  SSID: Cat_Emotion_AP                                        WSS Push to Cloud Relay
                                  Direct URL: http://192.168.4.1                              Target: Render.com Web Service
                                             |                                                           |
                                             v                                                           v
                                  Field Operator Device                                       Public Audience / Evaluators
```

## Branch Structure
To maintain modularity and allow independent deployment of each subsystem, this repository is organized into the following specialized branches:

- `main`: Comprehensive system architecture, specifications, and integration documentation.
- `cloud-relay`: Production-ready Node.js WebSocket relay service designed for one-click deployment on Render.com.
- `esp32-firmware`: XIAO ESP32-S3 Arduino firmware featuring dual-mode Wi-Fi (AP + STA), UART proxy, and WebSocket client.
- `edge-model-himax`: Optimized YOLOv8n QAT model weights (Vela INT8) and Himax WiseEye2 firmware binaries.

## Hardware Specifications
- Visual Processing Unit: Seeed Studio Grove Vision AI V2 (Himax WiseEye2 HX6538, Arm Cortex-M55 @ 400 MHz and Arm Ethos-U55 microNPU).
- Optical Sensor: OV5647 5MP Camera Module (192x192 RGB input stream).
- Telemetry Gateway: Seeed Studio XIAO ESP32-S3 (Xtensa 32-bit LX7 dual-core @ 240 MHz, 8 MB OPI PSRAM).
- Power Supply: Standard 5V USB power bank.

## License
MIT License
