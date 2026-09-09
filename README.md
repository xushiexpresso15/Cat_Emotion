# Cat Emotion Local Python Web Server & Hardware Bridge

A standalone Python Flask web server that connects directly to the **Seeed Studio Grove Vision AI Module V2** (or XIAO ESP32-S3) over USB Serial. It ingests SSCMA JSON frames, parses real-time camera imagery and bounding box detections, and broadcasts an interactive web dashboard with MJPEG streaming.

---

## Prerequisites & Runtime Environment

### Supported Operating Systems
- **Linux (Recommended)**: Ubuntu 20.04 / 22.04 / 24.04 LTS, Debian 11 / 12, Fedora, Arch Linux, or compatible distributions.
- **macOS**: macOS 12 Monterey or newer (Intel / Apple Silicon).
- **Windows**: Windows 10 / 11 (native Python or WSL2).

### Python Runtime & Dependencies
- **Python Version**: Python 3.10, 3.11, or 3.12 (Python 3.10+ required).
- **Required Packages** (install via `requirements.txt`):
  - `Flask>=2.0.0` - Web framework for dashboard UI, REST API, and multipart MJPEG stream.
  - `pyserial>=3.5` - High-speed serial communication interface.

### Hardware & Port Requirements
- **Microcontroller**: Seeed Studio Grove Vision AI Module V2 (flashed with Cat Emotion YOLOv8n model) or XIAO ESP32-S3.
- **Connection**: USB-C data cable connecting the board to your host PC.
- **Serial Port Identification**:
  - Linux: `/dev/ttyACM0` or `/dev/ttyUSB0`
  - macOS: `/dev/cu.usbmodem*`
  - Windows: `COM3`, `COM4`, etc.
- **Linux Serial Port Permissions**:
  Ensure your user belongs to the `dialout` (or `uucp`) group:
  ```bash
  sudo usermod -a -G dialout $USER
  # Log out and log back in, or grant temporary access:
  sudo chmod 666 /dev/ttyACM*
  ```

---

## Installation & Setup

1. **Clone this branch**:
   ```bash
   git clone -b webserver https://github.com/xushiexpresso15/Cat_Emotion.git cat-emotion-webserver
   cd cat-emotion-webserver
   ```

2. **Set up Python Virtual Environment**:
   ```bash
   python3 -m venv .venv
   source .venv/bin/activate
   pip install --upgrade pip
   pip install -r requirements.txt
   ```

3. **Launch the Server**:
   You can start the server using the helper script:
   ```bash
   chmod +x launch.sh
   ./launch.sh
   ```
   Or directly with Python:
   ```bash
   python3 server.py --port 8080
   ```

4. **Access the Dashboard**:
   Open your browser and navigate to:
   ```text
   http://localhost:8080/
   ```

---

## Architecture & Communication Flow

```
+------------------------------------+
|     Grove Vision AI Module V2      |
|    (Himax HX6538 + Ethos-U55)      |
+------------------------------------+
                  |
                  | USB Serial @ 921600 Baud
                  | SSCMA JSON Packets (Base64 JPEG + BBox)
                  v
+------------------------------------+
|         Python Web Server          |
|            (server.py)             |
|                                    |
| - SSCMA Frame Decoder              |
| - Sliding-window FPS Counter       |
| - Emotion Classification Mapper    |
| - Ring Buffer Log Storage          |
+------------------------------------+
         |                  |
         | HTTP / MJPEG     | REST API (/status, /logs)
         v                  v
+------------------------------------+
|          Web Browser UI            |
|       (frontend/index.html)        |
+------------------------------------+
```

---

## API Endpoints

- `GET /`: Interactive web dashboard with real-time video canvas and emotion telemetry.
- `GET /video_feed`: Multipart HTTP MJPEG video stream (`multipart/x-mixed-replace; boundary=frame`). Compatible with external tools such as Home Assistant, OpenCV, and VLC.
- `GET /status`: JSON snapshot of connection health, device status, current emotion, inference confidence, and FPS.
- `GET /logs`: JSON array of recent system events and classification detections.
- `GET /history`: Historical emotion classification counts.

---

## Repository Structure

- `server.py`: Primary production server with auto-port detection and SSCMA stream parsing.
- `local_server.py`: Alternate development server for local testing.
- `test_board.py`: Minimal script to test USB serial connectivity and verify board response.
- `launch.sh`: Shell script to set permissions and run the server.
- `requirements.txt`: Python package dependencies.
- `frontend/`: HTML5 dashboard assets.
  - `index.html`: Main glassmorphic dark-mode dashboard.
  - `index_clean.html`: Minimal clean dashboard layout.
- `cat_emotion_webserver/`: Standalone legacy ESP32 Arduino sketch for direct web streaming.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
