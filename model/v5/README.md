# Cat Emotion Model v5 (YOLOv8n, Grove Vision AI V2)

Detects a cat and classifies its emotion as **angry / focus / relax / scared**. Runs on the Seeed Grove Vision AI V2 (Himax WiseEye2 HX6538, Ethos-U55 NPU).

What is new in this version:
1. **Robustness to the board camera.** Training simulates what the board actually sees: a 4:3 frame squashed to a square, blur, a washed-out low-contrast image, and JPEG compression.
2. **🆕 Human-face negatives (new in v5).** Earlier versions sometimes detected people as cats, and their hard negatives only covered room backgrounds and objects such as airplane engines and clocks.
   - v5 adds **150 human-face photos** labeled as "no cat" (train 120, repeated 2×; val 15; test 15), together with 400 background photos.
   - Result: **0 / 15 faces detected as a cat** on the held-out test set.

---

## Results

The data is split **train 80% / val 10% / test 10%**. The test set was never used for training or model selection, and was evaluated only once.

### mAP (PyTorch float model)

| Set | Precision | Recall | mAP50 | mAP50-95 |
|---|---|---|---|---|
| Val | 82.5% | 82.0% | 85.3% | 66.3% |
| **Test** | **84.0%** | **83.3%** | **89.8%** | **71.0%** |
| Test, simulated board camera | 87.2% | 83.6% | 89.9% | 72.6% |

Per-class test mAP50 / mAP50-95:

| angry | focus | relax | scared |
|---|---|---|---|
| 96.9% / 79.5% | 82.7% / 65.7% | 89.3% / 68.9% | 90.3% / 69.9% |

### Board-like evaluation (INT8 model, score threshold 0.25, NMS IoU 0.45)

| | Test | Test, simulated board camera |
|---|---|---|
| Background false positives | 0 / 40 (0%) | 1 / 40 (2.5%) |
| Human-face false positives | 0 / 15 (0%) | 0 / 15 (0%) |
| Cat detection rate | 162 / 164 (98.8%) | 161 / 164 (98.2%) |
| **Emotion accuracy** | **138 / 164 (84.1%)** | **142 / 164 (86.6%)** |
| angry / focus / relax / scared | 92.5 / 93.2 / 73.2 / 76.9% | 97.5 / 90.9 / 73.2 / 84.6% |

> **Notes**
> - The test set has about 40 images per class, so the numbers carry roughly ±4% noise.
> - The "simulated board camera" set is generated in software. It is not real board footage.
> - The weakest class is **relax**, which is most often confused with focus.

---

## Model and hardware specs

| Item | Value |
|---|---|
| Architecture | YOLOv8n (SiLU), 3.0M parameters |
| Input | `[1, 192, 192, 3]` int8, RGB, scale 1/255, zero point −128 |
| Output 0 | `[1, 4, 756]` int8: boxes cx, cy, w, h, **normalized 0–1** |
| Output 1 | `[1, 756, 4]` int8: emotion scores (0–1) |
| Quantization | Full INT8, calibrated on 300 training images |
| NPU compiler | Vela **3.9.0**, `ethos-u55-64`, `himax_vela.ini` (My_Sys_Cfg / My_Mem_Mode_Parent) |
| NPU mapping | **100%** (0 CPU ops, no Transpose in the graph) |
| SRAM used | 948 KiB (firmware tensor arena: 1053 KiB) |
| Measured frame time | About 99 ms on the board, including image capture |
| Flash address | `0xB7B000` |

### Firmware compatibility

- ✅ **Himax official `tflm_yolov8_od`** ([Seeed_Grove_Vision_AI_Module_V2](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2), `APP_TYPE = tflm_yolov8_od`): tested on the board, and both boxes and emotions work.
  - This firmware reads **`output(0)` as boxes** without checking the order. If the order were reversed, the board would crash with a BusFault. This model already puts the boxes first.
  - Detection results and camera frames are both sent over USB, so you can watch them on a PC with `tools/board_view_cv.py`.
- ⚠️ **Project ESP32 firmware** (`firmware/output.img` on this branch): **not yet tested on the board**.
  - Its source code treats boxes as pixel coordinates (0–192), while this model outputs normalized 0–1 boxes.
  - Boxes may therefore look too small on that firmware. If so, the export needs to output pixel coordinates (multiply the boxes by 192) and the model must be re-exported.

---

## Files

| File | Description |
|---|---|
| `cat_emotion_v5_vela.tflite` | Vela-compiled model, ready to flash to `0xB7B000` |
| `cat_emotion_v5_int8.tflite` | INT8 TFLite model before Vela compilation |
| `best.pt` | PyTorch weights |
| `vela_summary.csv` | Vela compiler report |
| `MANIFEST.json` | Model metadata |

---

## Training

The full pipeline and commands are in [`training/README.md`](../../training/README.md).

### Dataset

| Class | train | val | test |
|---|---|---|---|
| angry | 322 | 40 | 40 |
| focus | 350 | 44 | 44 |
| relax | 331 | 41 | 41 |
| scared | 314 | 39 | 39 |
| Background (negative) | 319 | 40 | 40 |
| Human face (negative, repeated 2× in train) | 120 | 15 | 15 |

- **Boxes are auto-labeled.** A COCO-pretrained YOLOv8x finds the cat, and the box gets the emotion of its folder. Close-ups with no detection use the whole image as the box.
- **Negatives** (background and faces) have empty label files, meaning "no cat here". The model still has 4 classes.
- All images are **stretched to a square**, matching how the board squashes its frame to 192×192.
- Images removed after hand inspection:
  - duplicates that appear in two emotion folders,
  - background photos that actually contain a cat,
  - full-body shots the detector could not box.

### Two-stage training (about 15 minutes on an RTX 3060)

- **Stage 1:** 250 epochs from COCO `yolov8n.pt`. SGD, cosine LR 0.01 → 0.001, mosaic closed for the last 35 epochs, imgsz 192, batch 64.
- **Stage 2:** 25 epochs of fine-tuning. SGD, lr0 0.0003, no mosaic, box 7.5 / cls 1.5 / dfl 1.5.
- **Loss changes** (based on the project's v8.2.0 MANIFEST):
  - **Hard-negative suppression:** non-cat anchors scoring above 0.2 get a squared penalty, weight 1.0.
  - **Emotion mutual exclusion:** a squared penalty when the 4 class scores sum above 1.
  - TaskAlignedAssigner: alpha 0.6, beta 4.5.
- **Camera simulation (new in v5):**
  - random horizontal squash 0.70–1.00 (the 4:3 camera is squashed to a square),
  - Gaussian or motion blur, and downscale-then-upscale,
  - washed-out low contrast, desaturation, noise, and JPEG quality 30–85.

### Export

- The YOLOv8 detection head is rewritten for export in NHWC. DFL uses a matrix multiply, and the four box coordinates are computed separately and concatenated. As a result, the TFLite graph has **no Transpose op**, and Vela 3.9.0 maps it **100% to the NPU**.
- The export script checks automatically that the rewritten head matches the original YOLOv8 output, and that the INT8 results match the float model.

---

## Comparison with earlier versions

All rows use the INT8 model with a score threshold of 0.25, on the simulated board-camera validation set.

| Version | Main change | Emotion accuracy | focus | scared |
|---|---|---|---|---|
| v3 | Added the project's loss changes and stage-2 settings | 66.8% | 70.8% | 55.2% |
| v4 / v5 | Added camera simulation on top of v3 (human-face negatives used in all of v1–v5) | 79.5% | 87.7% | 63.8% |

v4 and v5 use exactly the same training settings. v5 was retrained on the 80/10/10 split so that it has a held-out test set.
