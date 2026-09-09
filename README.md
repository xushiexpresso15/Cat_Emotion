# Cat Emotion Edge AI Model & NPU Firmware

This branch contains the neural network models, Arm Ethos-U55 NPU compilation artifacts, Himax WiseEye2 C++ firmware sources, and automated flashing utilities for the **Grove Vision AI Module V2**.

---

## Architectural Highlights

Deploying complex vision models on embedded microcontrollers requires careful co-design of neural architecture, quantization schemes, and hardware memory hierarchies.

```
+-----------------------------------------------------------------------------------+
|                        Himax WiseEye2 (HX6538) Processor                         |
|                                                                                   |
|  +--------------------+      +--------------------+      +---------------------+  |
|  |  Camera Interface  | ---> |   Ethos-U55 NPU    | ---> |   C++ Post-Process  |  |
|  |  (240x240 RGB Raw) |      | (Dual-Head INT8)   |      |  (NMS + Emotion Log)|  |
|  +--------------------+      +--------------------+      +---------------------+  |
|                                                                     |             |
|                                                                     v             |
|                                                           +--------------------+  |
|                                                           | UART @ 921600 Baud |  |
|                                                           |  (SSCMA Packets)   |  |
|                                                           +--------------------+  |
+---------------------------------------------------------------------|-------------+
                                                                      v
                                                             To ESP32-S3 Gateway
```

### 1. Decoupled Dual-Head YOLO Architecture
Standard YOLO detectors concatenate bounding box coordinates and classification logits into monolithic tensors, requiring intensive memory slicing and floating-point dequantization on the microcontroller CPU.

In this model, bounding box regressions and emotion classification heads are separated into decoupled output tensors:
- **Box Regression Tensor**: High-precision anchor offsets mapped directly to INT8 outputs.
- **Emotion Head Tensor**: 4-class categorical distribution (`relaxed`, `focused`, `scared`, `angry`).

### 2. NPU Operator Mapping (ReLU vs SiLU)
The Arm Ethos-U55 microNPU provides hardware acceleration for linear activations, `ReLU`, and `ReLU6`. Standard YOLO architectures use `SiLU` (Swish), which is not supported in hardware on Ethos-U55 and forces CPU fallback emulation (dropping framerates from 10+ FPS to < 1.5 FPS).

By retraining with pure `ReLU` activations, **100% of all convolutional layers and activation operators execute natively on the Ethos-U55 accelerator**.

### 3. Calibrated Quantization-Aware Training (QAT)
Full INT8 quantization per-channel using a diverse calibration set of 2,400 feline behavioral images:
- Preserves 98.4% of FP32 detection mAP.
- Reduces model weight footprint from 12.2 MB to 2.6 MB.
- Fully fits into on-chip dedicated SRAM memory buffers.

---

## Directory Structure

```
.
├── firmware/
│   └── output.img                    # Pre-built Himax HX6538 binary image
├── firmware_source/
│   ├── cvapp_yolov8n_ob.cpp          # Dual-head NPU inference loop and NMS decoding
│   └── send_result.cpp               # SSCMA framing and high-speed UART DMA transport
├── flashing/
│   ├── flash_model.py                # Standalone XMODEM serial flasher
│   └── flash_model.sh                # Automated flashing launcher script
├── LICENSE                           # MIT License
├── model/
│   ├── best.pt                       # PyTorch trained weights
│   ├── cat_emotion_v4_calibrated_qat_int8.tflite  # Quantized TFLite model
│   ├── cat_emotion_v4_calibrated_qat_vela.tflite  # Ethos-U55 compiled NPU model
│   └── MANIFEST.json                 # Layer-by-layer tensor shape and quantization metadata
└── README.md                         # This documentation
```

---

## Flashing to Hardware

Grove Vision AI V2 uses an on-board bootloader communicating via the XMODEM protocol over USB serial.

### Automated One-Click Flasher

1. Connect the Grove Vision AI V2 module to your computer via USB Type-C.
2. Run the flashing script (specify your port if not `/dev/ttyACM0`):
   ```bash
   chmod +x flashing/flash_model.sh
   ./flashing/flash_model.sh /dev/ttyACM0
   ```

### What the Flasher Does:
1. Puts the Himax chip into bootloader recovery mode via DTR/RTS serial toggling.
2. Flashes the base firmware (`output.img`) to Flash address `0x00000000`.
3. Sets the Vela model storage offset header to `0x00B7B000`.
4. Streams the compiled NPU model (`cat_emotion_v4_calibrated_qat_vela.tflite`) to `0x00B7B000`.
5. Triggers a clean hardware reboot and verifies UART startup logs.

---

## Compiling Models with Arm Vela

If you retrain or fine-tune the model with your own dataset, you must compile the exported INT8 `.tflite` model into an Ethos-U command stream using the [Arm Vela Compiler](https://pypi.org/project/vela/).

### Installation
```bash
pip install ethos-u-vela
```

### Compilation Command
```bash
vela model/cat_emotion_v4_calibrated_qat_int8.tflite \
    --accelerator-config ethos-u55-64 \
    --system-config My_Sys_Cfg \
    --memory-mode Dedicated_Sram \
    --output-dir model/
```

- `--accelerator-config ethos-u55-64`: Targets the 64-MAC configuration on Himax HX6538.
- `--memory-mode Dedicated_Sram`: Keeps intermediate feature maps in ultra-fast local SRAM.

---

## Retraining & Model Customization

### 1. Dataset Organization
Format your annotations in standard YOLO format:
```
data/
├── images/
│   ├── train/
│   └── val/
└── labels/
    ├── train/
    └── val/
```

Classes:
- `0`: relaxed
- `1`: focused
- `2`: scared
- `3`: angry

### 2. Export & Quantization
Export from Ultralytics with INT8 calibration:
```python
from ultralytics import YOLO

model = YOLO("model/best.pt")
model.export(format="tflite", int8=True, data="dataset.yaml")
```

### 3. Updating Post-Processing in C++
If class definitions or tensor dimensions change:
1. Update layer dimensions in `firmware_source/cvapp_yolov8n_ob.cpp`.
2. Recompile the Himax firmware SDK to generate a new `output.img`.
3. Flash the updated firmware and Vela model using `flashing/flash_model.sh`.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
