#!/usr/bin/env python3
"""
Step 4: 模擬板子上的實際行為評估 int8 TFLite 模型（Vela 編譯前的那個檔，運算結果跟 NPU 一樣）。

跟韌體 cvapp_yolov8n_ob.cpp 相同的後處理：分數門檻 0.50、NMS IoU 0.45（不分類別）。
在驗證集上計算：
    - 背景照誤判率：有框出任何東西就算誤判
    - 人臉照誤判率：同上
    - 貓咪照：有沒有偵測到、最高分那個框的情緒對不對

用法：
    python 4_evaluate.py --model export_out/cat_emotion_int8.tflite [--model 另一個模型.tflite ...]
"""
import argparse
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np

CLASSES = ["angry", "focus", "relax", "scared"]
SCORE_TH, NMS_TH, IMGSZ = 0.50, 0.45, 192
import os
SCORE_TH = float(os.environ.get("SCORE_TH", SCORE_TH))


def imread_rgb(path):
    im = cv2.imdecode(np.fromfile(str(path), np.uint8), cv2.IMREAD_COLOR)
    return cv2.cvtColor(cv2.resize(im, (IMGSZ, IMGSZ), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)


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


class BoardModel:
    def __init__(self, path):
        import tensorflow as tf

        self.it = tf.lite.Interpreter(model_path=str(path))
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
        boxes, scores = self.deq(self.ob)[0].T, self.deq(self.os)[0]  # (756,4) 像素 cxcywh, (756,4)
        conf, cls = scores.max(1), scores.argmax(1)
        m = conf >= SCORE_TH
        boxes, conf, cls = boxes[m], conf[m], cls[m]
        keep = nms(boxes, conf) if len(conf) else []
        return [(CLASSES[cls[i]], float(conf[i])) for i in keep]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", action="append", required=True)
    ap.add_argument("--data-dir", default="yolo_dataset")
    ap.add_argument("--split", default="val")
    args = ap.parse_args()

    imgs = sorted((Path(args.data_dir) / "images" / args.split).iterdir())
    for mpath in args.model:
        model = BoardModel(mpath)
        st = defaultdict(lambda: [0, 0])  # key -> [count, total]
        confusion = defaultdict(int)
        for p in imgs:
            folder = p.stem.split("__")[0]
            dets = model(imread_rgb(p))
            if folder in ("background", "asian_face"):
                st[folder][0] += bool(dets)
                st[folder][1] += 1
            else:
                st["cat_detected"][0] += bool(dets)
                st["cat_detected"][1] += 1
                pred = max(dets, key=lambda d: d[1])[0] if dets else "(沒偵測到)"
                confusion[(folder, pred)] += 1
                st[f"acc_{folder}"][0] += pred == folder
                st[f"acc_{folder}"][1] += 1
                st["acc_all"][0] += pred == folder
                st["acc_all"][1] += 1
                st["multi_box"][0] += len(dets) > 1
                st["multi_box"][1] += 1

        pct = lambda k: f"{st[k][0]}/{st[k][1]} ({100 * st[k][0] / max(st[k][1], 1):.1f}%)"
        print(f"\n===== {mpath} =====")
        print(f"背景誤判     : {pct('background')}   （越低越好）")
        print(f"人臉誤判     : {pct('asian_face')}   （越低越好）")
        print(f"貓咪有偵測到 : {pct('cat_detected')}")
        print(f"情緒正確率   : {pct('acc_all')}")
        for c in CLASSES:
            print(f"   {c:<7}: {pct('acc_' + c)}")
        print(f"同一隻貓出現多個框: {pct('multi_box')}")
        print("混淆矩陣（列=正確答案，欄=模型預測）:")
        cols = CLASSES + ["(沒偵測到)"]
        print("          " + "".join(f"{c[:6]:>8}" for c in cols))
        for r in CLASSES:
            print(f"  {r:<8}" + "".join(f"{confusion[(r, c)]:>8}" for c in cols))


if __name__ == "__main__":
    main()
