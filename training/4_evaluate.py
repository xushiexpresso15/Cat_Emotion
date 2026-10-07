#!/usr/bin/env python3
r"""
Step 4: 模擬板子上的實際行為評估 int8 TFLite 模型（Vela 編譯前的那個檔，運算結果跟 NPU 一樣）。

跟韌體 cvapp_yolov8n_ob.cpp 相同的後處理：分數門檻（--score）、NMS IoU 0.45（不分類別）。
    - 專案 ESP32 韌體 firmware/output.img：門檻 0.50
    - Himax 官方 tflm_yolov8_od 韌體：門檻 0.25
計算：
    - 背景照誤判率：有框出任何東西就算誤判
    - 人臉照誤判率：同上
    - 貓咪照：有沒有偵測到、最高分那個框的情緒對不對、混淆矩陣

--camsim：每張圖先套用 camera_sim.camsim_eval_image()（固定強度的板子鏡頭畫質：4:3 壓扁、模糊、
低解析度、泛白、JPEG），種子 = 該圖在 split 內的排序位置，所以每次結果都一樣，
也跟 make_camsim_dataset.py 產生的資料集逐張相同。

用法：
    python 4_evaluate.py --model export_out/cat_emotion_int8.tflite --data-dir yolo_dataset --split test --score 0.25
    python 4_evaluate.py --model export_out/cat_emotion_int8.tflite --data-dir yolo_dataset --split test --score 0.25 --camsim
"""
import argparse
import os
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

CLASSES = ["angry", "focus", "relax", "scared"]
NMS_TH, IMGSZ = 0.45, 192
SCORE_TH = float(os.environ.get("SCORE_TH", 0.25))  # 給 import 這支程式的腳本用；命令列請用 --score


def imread_bgr(path):
    return cv2.imdecode(np.fromfile(str(path), np.uint8), cv2.IMREAD_COLOR)


def to_input(bgr):
    """跟板子一樣直接拉伸成 192x192（不保持比例），轉 RGB。"""
    return cv2.cvtColor(cv2.resize(bgr, (IMGSZ, IMGSZ), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)


def imread_rgb(path):
    return to_input(imread_bgr(path))


def nms(boxes, scores):
    order, keep = scores.argsort()[::-1], []
    x1, y1 = boxes[:, 0] - boxes[:, 2] / 2, boxes[:, 1] - boxes[:, 3] / 2
    x2, y2 = x1 + boxes[:, 2], y1 + boxes[:, 3]
    area = boxes[:, 2] * boxes[:, 3]
    while len(order):
        i = order[0]
        keep.append(i)
        iw = np.clip(np.minimum(x2[i], x2[order[1:]]) - np.maximum(x1[i], x1[order[1:]]), 0, None)
        ih = np.clip(np.minimum(y2[i], y2[order[1:]]) - np.maximum(y1[i], y1[order[1:]]), 0, None)
        inter = iw * ih
        iou = inter / (area[i] + area[order[1:]] - inter + 1e-9)
        order = order[1:][iou <= NMS_TH]
    return keep


def make_interpreter(path):
    try:  # LiteRT（輕量，CI 用）
        from ai_edge_litert.interpreter import Interpreter
    except ImportError:
        from tensorflow.lite import Interpreter
    return Interpreter(model_path=str(path))


class BoardModel:
    def __init__(self, path, score_th=None):
        self.score_th = SCORE_TH if score_th is None else score_th
        self.it = make_interpreter(path)
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]
        outs = {tuple(o["shape"]): o for o in self.it.get_output_details()}
        self.ob, self.os = outs[(1, 4, 756)], outs[(1, 756, 4)]

    def deq(self, o):
        return (self.it.get_tensor(o["index"]).astype(np.float32) - o["quantization"][1]) * o["quantization"][0]

    def __call__(self, rgb):
        s, zp = self.inp["quantization"]
        self.it.set_tensor(self.inp["index"], np.clip(np.round(rgb / 255.0 / s + zp), -128, 127).astype(np.int8)[None])
        self.it.invoke()
        boxes, scores = self.deq(self.ob)[0].T, self.deq(self.os)[0]  # (756,4) cxcywh（像素或 0~1 都可以，NMS 只看比例）, (756,4)
        conf, cls = scores.max(1), scores.argmax(1)
        m = conf >= self.score_th
        boxes, conf, cls = boxes[m], conf[m], cls[m]
        keep = nms(boxes, conf) if len(conf) else []
        return [(CLASSES[cls[i]], float(conf[i])) for i in keep]


def iter_inputs(data_dir, split, camsim=False):
    """依排序產生 (圖片路徑, 模型輸入 RGB)。camsim=True 時套用固定的板子鏡頭畫質。"""
    imgs = sorted((Path(data_dir) / "images" / split).iterdir())
    for i, p in enumerate(imgs):
        bgr = imread_bgr(p)
        if camsim:
            import camera_sim

            bgr = camera_sim.camsim_eval_image(bgr, seed=i)
        yield p, to_input(bgr)


def evaluate(model, data_dir, split, camsim=False):
    st = defaultdict(lambda: [0, 0])  # key -> [count, total]
    confusion = defaultdict(int)
    for p, rgb in iter_inputs(data_dir, split, camsim):
        folder = p.stem.split("__")[0]
        dets = model(rgb)
        if folder in ("background", "asian_face"):
            st[folder][0] += bool(dets)
            st[folder][1] += 1
        else:
            st["cat_detected"][0] += bool(dets)
            st["cat_detected"][1] += 1
            pred = max(dets, key=lambda d: d[1])[0] if dets else "missed"
            confusion[(folder, pred)] += 1
            st[f"acc_{folder}"][0] += pred == folder
            st[f"acc_{folder}"][1] += 1
            st["acc_all"][0] += pred == folder
            st["acc_all"][1] += 1
            st["multi_box"][0] += len(dets) > 1
            st["multi_box"][1] += 1
    return st, confusion


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", action="append", required=True)
    ap.add_argument("--data-dir", default="yolo_dataset")
    ap.add_argument("--split", default="test")
    ap.add_argument("--score", type=float, default=SCORE_TH,
                    help="分數門檻：專案 ESP32 韌體 0.50；Himax 官方 tflm_yolov8_od 韌體 0.25")
    ap.add_argument("--camsim", action="store_true", help="套用固定的板子鏡頭畫質模擬（camera_sim.camsim_eval_image）")
    args = ap.parse_args()

    for mpath in args.model:
        st, confusion = evaluate(BoardModel(mpath, args.score), args.data_dir, args.split, args.camsim)
        pct = lambda k: f"{st[k][0]}/{st[k][1]} ({100 * st[k][0] / max(st[k][1], 1):.1f}%)"
        print(f"\n===== {mpath} | split={args.split} | score>={args.score} | camsim={args.camsim} =====")
        print(f"背景誤判     : {pct('background')}   （越低越好）")
        print(f"人臉誤判     : {pct('asian_face')}   （越低越好）")
        print(f"貓咪有偵測到 : {pct('cat_detected')}")
        print(f"情緒正確率   : {pct('acc_all')}")
        for c in CLASSES:
            print(f"   {c:<7}: {pct('acc_' + c)}")
        print(f"同一隻貓出現多個框: {pct('multi_box')}")
        print("混淆矩陣（列=正確答案，欄=模型預測）:")
        cols = CLASSES + ["missed"]
        print("          " + "".join(f"{c[:6]:>8}" for c in cols))
        for r in CLASSES:
            print(f"  {r:<8}" + "".join(f"{confusion[(r, c)]:>8}" for c in cols))


if __name__ == "__main__":
    main()
