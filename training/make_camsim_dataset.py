#!/usr/bin/env python3
r"""
產生「模擬板子鏡頭畫質」版本的資料集（val / test），用來重現 README 裡 simulated board camera 的 mAP。

每張圖套用 camera_sim.degrade_fixed()（固定強度：4:3 壓扁 0.75、模糊、低解析度、泛白、JPEG），
種子 = 該圖在 split 內的排序位置；框會跟著壓扁一起調整。存檔用 OpenCV 預設 JPEG 品質 95，
所以跟 4_evaluate.py --camsim 即時產生的影像逐像素相同。

用法：
    python make_camsim_dataset.py --src yolo_dataset --dst yolo_dataset_camsim
    yolo val model=runs/cat_emotion_stage2/weights/best.pt data=yolo_dataset_camsim/data.yaml split=test imgsz=192 iou=0.45 workers=0
"""
import argparse
import shutil
from pathlib import Path

import cv2
import numpy as np

import camera_sim


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="yolo_dataset")
    ap.add_argument("--dst", default="yolo_dataset_camsim")
    ap.add_argument("--splits", nargs="+", default=["val", "test"])
    args = ap.parse_args()

    src, dst = Path(args.src), Path(args.dst)
    if dst.exists():
        shutil.rmtree(dst)
    for split in args.splits:
        (dst / "images" / split).mkdir(parents=True)
        (dst / "labels" / split).mkdir(parents=True)
        for i, p in enumerate(sorted((src / "images" / split).iterdir())):
            img = cv2.imdecode(np.fromfile(str(p), np.uint8), cv2.IMREAD_COLOR)
            t = (src / "labels" / split / (p.stem + ".txt")).read_text().split()
            boxes = [tuple(map(float, t[j + 1:j + 5])) for j in range(0, len(t), 5)]
            cls = [int(t[j]) for j in range(0, len(t), 5)]
            out, nb = camera_sim.degrade_fixed(img, boxes, seed=i)
            cv2.imencode(".jpg", out)[1].tofile(str(dst / "images" / split / (p.stem + ".jpg")))
            (dst / "labels" / split / (p.stem + ".txt")).write_text(
                "".join(f"{c} {b[0]:.6f} {b[1]:.6f} {b[2]:.6f} {b[3]:.6f}\n" for c, b in zip(cls, nb)))
        print(split, len(list((dst / "images" / split).iterdir())), "張")
    (dst / "data.yaml").write_text(
        f"path: {dst.resolve().as_posix()}\ntrain: images/{args.splits[0]}\nval: images/val\ntest: images/test\n"
        "nc: 4\nnames: ['angry', 'focus', 'relax', 'scared']\n")


if __name__ == "__main__":
    main()
