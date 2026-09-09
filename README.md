# Cat Emotion Recognition: Edge AI and Wireless Telemetry System

## Introduction
Cat Emotion is an open-source embedded edge AI project designed for real-time feline facial expression detection and emotional state classification. 

Traditional animal behavior monitoring systems often rely on streaming raw video to centralized servers or high-power GPUs, introducing latency, network dependency, and privacy concerns. This project shifts the entire deep learning inference workload directly onto an ultra-low-power edge processor with an on-chip microNPU, enabling fully autonomous on-device inference while simultaneously providing robust wireless telemetry.

The system is split into two coordinated hardware layers:
1. Edge Inference Node: Seeed Studio Grove Vision AI V2 (powered by the Himax WiseEye2 processor with an Arm Cortex-M55 core and Arm Ethos-U55 microNPU) running a custom INT8 quantized YOLOv8n model at up to 13 FPS native inference throughput.
2. Wireless Telemetry Gateway: Seeed Studio XIAO ESP32-S3 acting as a dual-band network bridge, providing an offline local web server (SoftAP) and publishing telemetry to an upstream cloud relay.

## Demonstration and Deployment Modes
The system is built to handle diverse deployment scenarios:
- Standalone Field Mode: Powered solely by a portable 5V USB power bank. The ESP32 emits an independent Wi-Fi hotspot (`Cat_Emotion_AP`), allowing any nearby smartphone or laptop to view live bounding boxes and classifications at `http://192.168.4.1` with zero external network dependencies.
- Cloud Broadcast Mode: The ESP32 connects to an available Wi-Fi network or smartphone hotspot and pushes telemetry via TLS WebSocket to a cloud relay server (hosted on Render), enabling distributed audiences to view the live dashboard concurrently from any browser.
- Local Network Mode: Connects to local LAN routers, providing direct dashboard access via mDNS at `http://cat.local`.

Live cloud demo: https://cat-emo-live.onrender.com

## Repository Branch Structure
This repository uses functional branches to keep build artifacts and deployment configurations modular and clean:
- `main`: Core project documentation, integration guide, and architectural specifications.
- `edge-model-himax`: Neural network model weights (INT8 TFLite, Vela compiled), Himax WiseEye2 firmware binaries (`output.img`), C++ firmware source modifications, and flashing scripts.
- `esp32-firmware`: Complete Arduino firmware for the XIAO ESP32-S3, including dual AP/STA Wi-Fi handling, UART proxy, and WebSocket client.
- `cloud-relay`: Production-ready Node.js WebSocket relay service with responsive web UI, optimized for one-click deployment on cloud platforms like Render.com.

## Hardware Requirements
- Visual Processing Unit: Seeed Studio Grove Vision AI V2 (Himax WiseEye2 HX6538, Arm Cortex-M55 @ 400 MHz + Arm Ethos-U55 microNPU).
- Wireless Gateway: Seeed Studio XIAO ESP32-S3 (Xtensa dual-core LX7 @ 240 MHz, 8 MB OPI PSRAM).
- Optical Sensor: OV5647 5MP Camera Module (192x192 RGB input stream).
- Power Supply: Standard 5V USB power bank or USB-C adapter.

### Pinout and Interconnects
Connect the Grove Vision AI V2 and XIAO ESP32-S3 as follows:
- ESP32 Pin D6 (TX) -> Himax Module Pin RX
- ESP32 Pin D7 (RX) -> Himax Module Pin TX
- ESP32 Pin D3 (GPIO) -> Himax Module Pin RST
- 3.3V -> 3.3V
- GND -> GND

## Getting Started

### 1. Flashing the Edge Model (Himax WiseEye2)
Switch to the `edge-model-himax` branch:
```bash
git checkout edge-model-himax
```
Connect the Grove Vision AI V2 module to your computer via USB (typically `/dev/ttyACM1` on Linux or `COMx` on Windows) and run the flashing utility:
```bash
./flashing/flash_model.sh /dev/ttyACM1 921600
```
This writes the Vela-compiled INT8 model to flash memory address `0x00B7B000` while preserving system boot configurations.

### 2. Building and Flashing the ESP32 Firmware
Switch to the `esp32-firmware` branch:
```bash
git checkout esp32-firmware
```
Copy `secrets.example.h` to create your local `secrets.h` (which is excluded from Git tracking):
```bash
cp cat_emotion_camera_server/secrets.example.h cat_emotion_camera_server/secrets.h
```
Configure your Wi-Fi credentials and cloud relay token in `cat_emotion_camera_server/secrets.h`:
```cpp
// Target networks for Station mode (prioritized connection)
const KnownNetwork known_networks[] = {
    {"YOUR_HOTSPOT_SSID", "YOUR_HOTSPOT_PASSWORD"},
    {"YOUR_WIFI_SSID",    "YOUR_WIFI_PASSWORD"}
};

// Cloud relay configuration
const bool     cloud_relay_enabled = true;
const char*    cloud_relay_host    = "your-relay-service.onrender.com";
const uint16_t cloud_relay_port    = 443;
const char*    cloud_relay_path    = "/esp32";
const bool     cloud_relay_ssl     = true;
const char*    cloud_relay_token   = "your_stream_secret_here";
```
Compile and flash using Arduino CLI:
```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi cat_emotion_camera_server
arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi cat_emotion_camera_server
```

#### Dynamic Boot Session PIN
Upon every boot, the ESP32 generates a hardware-random 6-digit session PIN (e.g., `839201`):
- Printed in the USB Serial Monitor console on boot.
- When powered by a portable battery pack, connect your smartphone to the device's SoftAP (`Cat_Emotion_AP`) and open `http://192.168.4.1/status` to view the active PIN.
- Viewers accessing the public cloud relay must enter this PIN on the web dashboard to unlock video frames and telemetry.

### 3. Deploying the Cloud Relay Service
Switch to the `cloud-relay` branch:
```bash
git checkout cloud-relay
```
To run the relay service locally:
```bash
npm install
node server.js
```
To deploy on Render:
1. Log in to dashboard.render.com and create a new Web Service.
2. Select this repository and set the branch to `cloud-relay`.
3. Set Language to `Node`, Build Command to `npm install`, and Start Command to `node server.js`.
4. (Optional) Set the `STREAM_SECRET` environment variable in the Render dashboard to require token authentication from the ESP32.
5. Choose the free plan and deploy. Render will assign you a permanent `https://<service-name>.onrender.com` URL.

## Model Details and Quantization
- Emotional Classes:
  - Angry: Ears pinned backwards, tense facial muscles, narrowed eyes.
  - Focus: Pupils dilated/focused, ears erect and facing forward, intense gaze.
  - Relax: Neutral ear posture, soft eyes, calm facial state.
  - Scared: Flattened ears (airplane ears), wide open eyes, retracted posture.
- Network Architecture: YOLOv8n object detection backbone with decoupled dual-tensor output heads (bounding box coordinates and emotional class logits).
- NPU Acceleration: Standard SiLU activations were replaced with ReLU across all convolutional layers, ensuring 100% operator mapping to the Arm Ethos-U55 microNPU with zero CPU fallback.
- Quantization Scheme: Full INT8 Quantization-Aware Training (QAT) with target score calibration.
- SRAM Footprint: Model tensor arena fits entirely within the Himax WE2 internal SRAM (~830 KiB), eliminating external memory access overhead.

## How to Extend and Customize

### 1. Training on Custom Animal Datasets
The training and conversion pipeline can be extended to other animal emotion or behavior datasets (e.g., dogs, livestock):
1. Annotate bounding boxes and target emotion categories in YOLO format.
2. Train a YOLOv8n model using ReLU activations instead of SiLU.
3. Export to TFLite INT8 with representative calibration data.
4. Compile using the Arm Vela toolchain:
```bash
vela --accelerator-config ethos-u55-64 --system-config My_Sys_Cfg --memory-mode Dedicated_Sram model_int8.tflite
```

### 2. Smart Home and Automation Integration
The ESP32 firmware broadcasts standard JSON packets containing bounding boxes and emotion states over WebSocket. You can easily integrate this into Home Assistant, Node-RED, or MQTT brokers:
- Trigger pet-calming pheromone diffusers when continuous `Angry` or `Scared` states are detected.
- Record pet daily mood analytics and activity heatmaps without storing invasive video streams.

### 3. Power Optimization
The XIAO ESP32-S3 supports low-power light and deep sleep modes. By utilizing the Himax hardware motion detection trigger (CDM) or external PIR sensors, the system can wake up on demand, extending battery endurance from hours to weeks on a standard Li-Po cell.

## Troubleshooting
- 0 FPS or No Video: Ensure the UART pins between ESP32 and Himax are crossed correctly (ESP32 D6 TX -> Himax RX; ESP32 D7 RX -> Himax TX) and both share a common ground reference.
- First-Time Cloud Latency (Render Cold Start): On the Render free tier, instances spin down after 15 minutes of inactivity. When first accessed after dormancy, initial wake-up takes approximately 30-45 seconds. Subsequent connections are immediate.
- Wi-Fi Reconnection: If moving between environments, the ESP32 automatically scans and attempts reconnection in the background every 15 seconds without blocking local frame processing.

## Collaborative Workflow & Contribution Rules

Whether you are an internal team member or an external open-source contributor, please follow this standardized Git workflow.

### Core Policy: Always Use Feature Branches

- **Direct pushes to primary branches are strictly prohibited**:
  Never push commits directly to `main`, `cloud-relay`, `esp32-firmware`, or `edge-model-himax`. Direct pushes disrupt concurrent work and introduce merge conflicts.
- **Every new feature or bug fix must live on its own branch**:
  All modifications must be developed in an isolated branch and integrated into the repository exclusively through a GitHub Pull Request (PR).

---

### Step-by-Step Team Workflow

#### 1. Synchronize the Base Branch
Before writing any code, switch to the subsystem branch you plan to modify and pull the latest upstream changes:
```bash
# Example: working on ESP32 firmware features
git checkout esp32-firmware
git pull origin esp32-firmware
```

#### 2. Create a Dedicated Feature Branch
Branch off the synchronized base branch using a standardized naming convention:
- `feature/<subsystem>-<feature-description>`: For new capabilities
- `fix/<subsystem>-<bug-description>`: For bug fixes
- `refactor/<subsystem>-<scope>`: For code refactoring
- `docs/<description>`: For documentation updates

Examples:
```bash
# Branching from esp32-firmware
git checkout -b feature/esp32-line-notify

# Branching from cloud-relay
git checkout -b feature/relay-sqlite-storage

# Branching from edge-model-himax
git checkout -b fix/himax-xmodem-timeout
```

#### 3. Commit with Clear Messages
Make small, logical commits with descriptive messages following Conventional Commits format:
```bash
git add cat_emotion_camera_server.ino
git commit -m "feat(esp32): implement line notify webhook integration"
```

#### 4. Push Feature Branch to GitHub
Push your local branch to the remote repository:
```bash
git push -u origin feature/esp32-line-notify
```

#### 5. Open a Pull Request (PR)
1. Go to the GitHub repository: https://github.com/xushiexpresso15/Cat_Emotion
2. Click **Compare & pull request** next to your newly pushed branch.
3. Select the correct **base branch** corresponding to your subsystem:
   - For cloud/UI features: Base `cloud-relay`
   - For ESP32 firmware: Base `esp32-firmware`
   - For NPU model & Himax code: Base `edge-model-himax`
   - For general documentation: Base `main`
4. Provide a clear summary:
   - What changed
   - Why the change was made
   - How the change was tested on physical hardware
5. Submit the PR for maintainer code review.

#### 6. Code Review & Merge
- The repository maintainer reviews the diff, tests if necessary, and approves the PR.
- Once merged into the base branch, delete the remote feature branch to keep the repository tidy.

---

### Resolving Merge Conflicts

If GitHub reports that your branch cannot be automatically merged:
```bash
# 1. Update your local base branch
git checkout <base-branch>
git pull origin <base-branch>

# 2. Switch to your feature branch and merge the updated base branch
git checkout <your-feature-branch>
git merge <base-branch>

# 3. Resolve conflicts in your editor, then commit and push
git add .
git commit -m "chore: resolve merge conflicts with <base-branch>"
git push origin <your-feature-branch>
```

---

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

