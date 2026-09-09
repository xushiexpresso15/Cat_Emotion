# Edge AI Model and Himax WiseEye2 Firmware

## Overview
This branch contains the neural network assets and firmware enhancements for the Himax WiseEye2 (WE2) edge AI processor on the Seeed Studio Grove Vision AI V2 module. The architecture leverages an Arm Cortex-M55 processor paired with an Arm Ethos-U55 microNPU.

## Model Specifications
- Model Architecture: Custom YOLOv8n object detection model with decoupled dual-tensor output heads (bounding box coordinates and emotional class distributions).
- Activation Function: ReLU activation replacing default SiLU, ensuring 100% operator mapping to the Ethos-U55 microNPU with zero CPU fallback overhead.
- Quantization Strategy: INT8 Quantization-Aware Training (QAT) with target score calibration.
- Input Resolution: 192x192 RGB image tensor.
- Target Emotion Classes: 4 categories (Angry, Focus, Relax, Scared).
- Model Storage Footprint: 2.7 MB (Vela-compiled INT8 TFLite).
- Inference Latency: ~75 ms per frame, enabling up to 13 FPS native inference throughput.

## Flash Memory Map
- Firmware Base Address: `0x00000000` (Executable application binary: `firmware/output.img`)
- Model Weights Base Address: `0x00B7B000` (Direct memory mapped at `0x3AB7B000`)

## Firmware Enhancements
- High-Speed UART DMA: Expanded to dual 16 KB ping-pong DMA buffers to prevent UART buffer overruns during high frame activity.
- Retrigger Synchronization: Sensor capture triggers are synchronized directly to inference completion to eliminate pipeline stalls.
- CIS Hardware Fault Recovery: Automatic state recovery on camera interface CDM error events without halting the processing loop.

## Flashing Instructions
To flash the calibrated QAT model to Flash memory address `0x00B7B000`:

```bash
# Python flashing utility
python flashing/flash_v4.py /dev/ttyACM1 921600

# Alternative Shell script
bash flashing/flash_v4.sh /dev/ttyACM1
```
