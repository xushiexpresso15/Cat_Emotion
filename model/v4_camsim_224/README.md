# Cat Emotion Model v4_camsim_224 (YOLOv8n, 224×224 input, QAT)

Detects a cat and classifies its emotion as **angry / focus / relax / scared** (same class order as v5 and the live model). Built for the Grove Vision AI V2 with the IMX519 camera.

- Trained on the team dataset `dataset_v2` (train split only; val for model selection; test evaluated only).
- Board-camera augmentation: `camera_sim.augment_labels(p=0.7, p_aspect=0.0)`. The 4:3 squash is off because the IMX519 firmware letterboxes instead of stretching.
- Float teacher: Ultralytics `yolov8n.pt`, `imgsz=224`, AdamW lr0 0.001 cosine, up to 300 epochs (patience 50), mosaic 0.5 with close_mosaic 30.
- Then quantization-aware training with knowledge distillation from the float teacher (16 epochs, lr 2e-5, camera_sim also applied), exported as a two-output INT8 model and compiled with Vela.

## Files

| file | bytes | md5 |
|---|---:|---|
| cat_emotion_v4_camsim_224_int8.tflite | 3238224 | a107bf9d76cee277b21c31bfc9800857 |
| cat_emotion_v4_camsim_224_vela.tflite | 3064912 | 3e68bc21796139d213ebedbd033b0634 |
| vela_summary.csv | 1621 | Vela report (Ethos_U55_64, Shared_Sram) |
| metadata.yaml | 434 | class names and input size for Ultralytics |

Flash the Vela model to `0xB7B000`.

## Tensors

| tensor | shape | type | quantization (scale, zero point) | content |
|---|---|---|---|---|
| input | [1, 224, 224, 3] | int8 | 0.003922, -128 | RGB / 255 |
| box | [1, 4, 1029] | int8 | 0.004417, -113 | cx, cy, w, h, **normalized 0-1** (max (127+113)·0.004417 = 1.06) |
| scores | [1, 1029, 4] | int8 | 0.003906, -128 | per-class score 0-1 |

1029 anchors = 28² + 14² + 7² (strides 8 / 16 / 32 at 224).

Vela (Ethos_U55_64, 500 MHz model): 886 KiB SRAM, about 36 ms per inference (the 192 model: about 28 ms).

## Firmware requirements

1. **The input size must be 224.** `firmware_source/live/cvapp_yolov8n_ob.cpp` fixes the size at compile time (`#if 1` → 192). Switch it to the `#else` branch (224) or read the size from the input tensor at init, otherwise the image is written into the tensor at the wrong size.
2. **Box units**: normalized. The live cvapp multiplies by the input width when the box scale is below 0.1, so it handles this model once the width is 224.
3. **Preprocessing**: the model was trained and evaluated with **letterbox** (aspect kept, grey 114 padding). With stretch the accuracy is lower (table below).
4. Tensor arena: the model needs about 886 KiB of SRAM per Vela; check the arena size of the firmware build.

## Results (INT8 model, PC)

Same evaluation for all rows: mAP at conf 0.001, NMS IoU 0.7; board-like emotion accuracy = top box per image at score ≥ 0.50, NMS IoU 0.45.

| dataset_v2 | preprocessing | mAP50 | mAP50-95 | emotion accuracy | background false positives |
|---|---|---|---|---|---|
| val | letterbox | 86.3% | 74.4% | 135/161 (83.9%) | 0/28 |
| **test** | **letterbox** | **83.6%** | **73.7%** | **135/160 (84.4%)** | 1/25 |
| test, centre-cropped to 4:3 | letterbox | 84.4% | 72.9% | 133/159 (83.6%) | 1/26 |
| test, 4:3 | stretch | 76.2% | 62.6% | 125/159 (78.6%) | 0/26 |
| test, 4:3 then 256×192 centre crop (current board frame) | letterbox | 83.9% | 74.9% | 130/159 (81.8%) | 0/26 |
| test, 4:3 then 256×192 centre crop | stretch | 78.3% | 68.7% | 130/159 (81.8%) | 0/26 |

Test per-class AP50 (letterbox): angry 0.89 / focus 0.86 / relax 0.77 / scared 0.83. Main confusion: relax and scared predicted as focus.

## Board results (screen replay)

The board (IMX519, focus held after the boot lock, letterbox) filmed the dataset_v2 test images shown on a laptop screen about 30 cm away: 160 cats + 25 backgrounds, one verdict per image.

| model | board frame | emotion accuracy | cats missed | background false positives |
|---|---|---|---|---|
| **v4_camsim_224** | 320×240 | **124/160 (77.5%)** | 17 | 0/25 |
| v4_camsim (192 input), same session | 320×240 | 120/160 (75.0%) | 16 | 0/25 |

The paired difference is not significant (McNemar p = 0.48). The 224 model mostly gains on angry (30 vs 27 correct). The 224 model has not yet been tested on the board with the current 256×192 frame (the 192 model scored 70.0% there).

> **Notes**
> - About 40 images per class, so the numbers carry roughly ±4% noise.
> - The screen replay is not real-cat footage; it compares models on images with known labels.
