# Cat Emotion Model – Training Pipeline (v5)

Reproducible pipeline used to train **model v5** (`model/v5/`): auto-labeling → two-stage training → INT8 export + Vela 3.9.0 compile → board-like evaluation.
Tested on Windows 11 with an RTX 3060 (about 15 minutes of training).

## 0. Environment

Two Python virtual environments are used, because Vela 3.9.0 needs `numpy<2` and conflicts with the training stack.

| Env | Packages |
|---|---|
| `train_env` (Python 3.12) | `torch` (CUDA build), `ultralytics`, `opencv-python`, `tensorflow<=2.19`, `tf_keras`, `onnx`, `onnxslim`, `onnxruntime`, `onnx2tf<1.29` (`--no-deps`), `sng4onnx`, `onnx_graphsurgeon`, `ai-edge-litert`, `flatbuffers`, `pyserial`, `xmodem==0.4.7` |
| `vela_env` | `ethos-u-vela==3.9.0`, `numpy<2` |

On Windows, `ethos-u-vela==3.9.0` only ships as source. It can be built with MinGW gcc by adding `[build_ext] compiler=mingw32` to its `setup.cfg` and then running `pip install --no-build-isolation .`.

Expected dataset layout, one folder per class. The folder names are used as labels:

```
dataset/
  angry/  focus/  relax/  scared/     # cat photos
  background/                          # negatives: no cat
  asian face/                          # negatives: human faces (not a cat)
```

All commands below are run inside `training/`.

## 1. Auto-label and split (80 / 10 / 10)

```bash
python 1_auto_label.py --input <path-to-dataset> --output yolo_dataset
```

- A COCO-pretrained **YOLOv8x** detects the cat. The box gets the emotion of its folder. Close-ups with no detection use the whole image as the box.
- `background/` and `asian face/` become **negatives** with empty label files, so they mean "no cat here". The model still has 4 classes. Faces are repeated twice in the training set.
- Images are **stretched to a 384×384 square**, the same way the board squashes its camera frame to 192×192.
- Split is **train / val / test = 80 / 10 / 10**. `test` is only used once, at the very end.
- Bad images found by hand inspection are listed in `EXCLUDE`. These include images that appear in two emotion folders, background photos that actually contain a cat, and full-body shots the detector cannot box.

## 2. Train (two stages)

```bash
python 2_train.py --data yolo_dataset/data.yaml --device 0 --recipe github --camera-aug
```

- **Stage 1**
  - 250 epochs from COCO `yolov8n.pt`.
  - SGD, cosine LR 0.01 → 0.001, mosaic closed for the last 35 epochs, imgsz 192, batch 64.
- **Stage 2**
  - 25 epochs of low-LR fine-tuning: SGD lr0 0.0003, no mosaic.
- **Loss additions**
  - Hard-negative suppression: non-cat anchors scoring above 0.20 get a squared penalty.
  - Emotion mutual-exclusion: class scores summing above 1 get a squared penalty.
- `--recipe` picks the training settings:
  - `github` (default): TaskAlignedAssigner alpha 0.6 / beta 4.5, the mutual-exclusion loss, and a stage-2 SGD for 25 epochs. These follow the project's v8.2.0 MANIFEST and `best.pt` training arguments.
  - `v2`: Ultralytics default assigner, no mutual-exclusion loss, and a stage-2 AdamW for 30 epochs.
- `--camera-aug` turns on **board camera simulation** (`camera_sim.py`):
  - Random horizontal squash 0.70–1.00, because the 4:3 camera is squashed to a square.
  - Gaussian or motion blur, downscaling, washed-out low contrast, desaturation, noise, and JPEG quality 30–85.
  - Boxes are updated together with the image.
- Output weights: `runs/cat_emotion_stage2/weights/best.pt`

## 3. Export + INT8 + Vela

```bash
python 3_export_and_compile.py --weights runs/cat_emotion_stage2/weights/best.pt --data-dir yolo_dataset --box-format normalized --vela <path-to-vela_env>/Scripts/vela.exe
```

The output is `cat_emotion_vela.tflite`, which is flashed to `0xB7B000`.

| | Shape | Content |
|---|---|---|
| Input | [1,192,192,3] int8 | RGB, scale 1/255, zero point −128 |
| Output 0 | [1,4,756] int8 | Boxes cx, cy, w, h |
| Output 1 | [1,756,4] int8 | 4 emotion scores (0–1) |

- `--box-format` must match the firmware that reads the boxes:
  - `normalized` (0–1) for the **Himax official `tflm_yolov8_od`** firmware, which multiplies the boxes by 192 itself.
  - `pixel` (0–192) for firmware that uses the values directly as pixels.
- The boxes tensor is always placed **first**. The official firmware reads `output(0)` as boxes and crashes with a BusFault if the order is reversed.
- The YOLOv8 head is rewritten in NHWC: DFL uses a matmul, and the box coordinates are concatenated on axis 1. As a result, the TFLite graph has **no Transpose op** and Vela 3.9.0 maps **100% of it to the NPU**.
- INT8 calibration uses 300 training images: cats, background and faces.
- The script checks automatically that the rewritten head matches the original Ultralytics output, and compares float vs INT8 results.
- Vela settings: version **3.9.0**, `himax_vela.ini`, `ethos-u55-64`, `My_Sys_Cfg`, `My_Mem_Mode_Parent`.

## 4. Board-like evaluation

```bash
SCORE_TH=0.25 python 4_evaluate.py --model export_out/cat_emotion_int8.tflite --data-dir yolo_dataset --split test
```

- Uses the INT8 model with the same post-processing as the firmware: score threshold from `SCORE_TH`, class-agnostic NMS at IoU 0.45.
- Reports background and face false positives, the cat detection rate, emotion accuracy and a confusion matrix.

## Results (model v5, held-out test set)

| | mAP50 | mAP50-95 | Emotion accuracy (INT8, score ≥ 0.25) | Background FP | Face FP |
|---|---|---|---|---|---|
| Val | 85.3% | 66.3% | – | – | – |
| **Test** | **89.8%** | **71.0%** | **84.1%** | 0/40 | 0/15 |
| Test, simulated board camera | 89.9% | 72.6% | 86.6% | 1/40 | 0/15 |

Per-class test mAP50: angry 96.9%, focus 82.7%, relax 89.3%, scared 90.3%.
The test set has about 40 images per class, so expect roughly ±4% noise.

## Flashing (Windows)

```bash
python ../flashing/flash_model_windows.py --port COM4 --fast --model cat_emotion_vela.tflite
```

- The Windows CH343 driver sends data too fast for the bootloader. The flasher therefore throttles each XMODEM packet and waits up to 60 seconds for an ACK, because the board pauses for several seconds every 1 MB to write flash.
- `--fast` takes about 2m20s for a 2.8 MB model. Leave out `--fast` for the slower, most conservative timing.
