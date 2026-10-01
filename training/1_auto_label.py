#!/usr/bin/env python3
"""
Step 1: 自動標註 dataset/ 底下的貓咪情緒照片，產生 YOLO 格式的偵測資料集。

原理：dataset/ 裡的圖片只有「資料夾名稱 = 情緒類別」，沒有 bounding box。
這支腳本用一個 COCO 預訓練的 YOLOv8 模型去偵測圖片裡「貓」的位置（COCO class 15），
把偵測到的框，重新標成資料夾名稱代表的情緒類別，寫成 YOLO 格式的 .txt 標籤。

負樣本（不是貓，標籤檔是空的 -> 模型學到「這裡沒有貓」）：
    background/   -> 背景照，裡面沒有貓
    asian face/   -> 人臉照，用來壓低「把人認成貓」的誤判
負樣本不會新增類別，模型輸出仍然是 4 類（angry/focus/relax/scared），跟板子韌體相容。

用法：
    python 1_auto_label.py --input ../../dataset --output yolo_dataset   # 預設 80/10/10 切 train/val/test

輸出結構：
    yolo_dataset/
        images/train/*.jpg
        images/val/*.jpg
        labels/train/*.txt
        labels/val/*.txt
        data.yaml
        missed/<class>/*.jpg   <- 沒偵測到貓的圖片，丟在這裡讓你人工檢查
"""
import argparse
import hashlib
import random
import shutil
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np
from ultralytics import YOLO

CAT_COCO_ID = 15
VALID_EXTS = {".jpg", ".jpeg", ".png"}

# 情緒類別 -> class index（跟 MANIFEST.json 對齊）
EMOTION_CLASSES = ["angry", "focus", "relax", "scared"]
# 負樣本資料夾 -> 在訓練集裡重複幾次（人臉張數少，重複 2 次加強「人不是貓」）
NEGATIVE_DIRS = {"background": 1, "asian face": 2}
# 人工檢查後要排除的圖片（資料夾/檔名）
EXCLUDE = {
    "angry/截圖 2026-07-17 下午2.45.07.png",  # 純色小截圖，沒有貓
    "background/564.png",                   # 背景照裡其實有一隻小貓
    "angry/835528_s.jpg",                   # 大部分是哈士奇，貓很小抓不到，整張框會把狗標成貓
    "angry/835529_s.jpg",                   # 同上
    # 以下是偵測器抓不到貓、而且貓只佔畫面一部分（全身照）的圖：用整張圖當框會太鬆，教壞框的大小
    # （抓不到但貓臉佔滿畫面的特寫照則保留，整張圖當框是準的）
    "angry/image 54.PNG", "angry/image 55.PNG", "angry/截圖 2026-07-17 下午2.52.33.png",
    "relax/00000522_008.jpg",
    "scared/image 77.PNG", "scared/image-13.png", "scared/image-67.png", "scared/scared17.png",
    "scared/截圖 2026-07-17 凌晨1.38.40.png", "scared/截圖 2026-07-17 凌晨1.54.58.png",
    "scared/截圖 2026-07-17 凌晨1.55.18.png", "scared/截圖 2026-07-17 凌晨1.58.43.png",
    "scared/截圖 2026-07-17 凌晨12.55.15.png", "scared/截圖 2026-07-17 凌晨2.02.42.png",
    "scared/截圖 2026-07-17 凌晨2.06.14.png", "scared/截圖 2026-07-17 凌晨2.07.43.png",
    "scared/image-32.png",
}


def list_images(d: Path):
    return sorted(p for p in d.iterdir()
                  if p.is_file() and p.suffix.lower() in VALID_EXTS and f"{d.name}/{p.name}" not in EXCLUDE)


def imread(p: Path):
    # 用 imdecode 讀，避免 Windows 上中文檔名讀不到
    return cv2.imdecode(np.fromfile(str(p), np.uint8), cv2.IMREAD_COLOR)


def md5(p: Path):
    return hashlib.md5(p.read_bytes()).hexdigest()


def safe_stem(folder: str, p: Path):
    # 加上資料夾前綴，避免不同資料夾的同名檔案互相覆蓋
    return f"{folder.replace(' ', '_')}__{p.stem}"


def split_list(items, val_ratio, test_ratio, rng):
    """切成 train / val / test。test 只在最後評估一次，訓練和挑模型都不能用。"""
    items = list(items)
    rng.shuffle(items)
    n_val = max(1, round(len(items) * val_ratio))
    n_test = round(len(items) * test_ratio)
    return items[n_val + n_test:], items[:n_val], items[n_val:n_val + n_test]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="../../dataset", help="原始資料夾（含 angry/focus/relax/scared/background/asian face）")
    ap.add_argument("--output", default="yolo_dataset", help="輸出的 YOLO 格式資料集資料夾")
    ap.add_argument("--val-ratio", type=float, default=0.10, help="驗證集比例（訓練中挑最佳權重用）")
    ap.add_argument("--test-ratio", type=float, default=0.10, help="測試集比例（只在最後評估一次）")
    ap.add_argument("--detector", default="yolov8x.pt", help="用來抓貓咪位置的預訓練偵測模型（越大越準）")
    ap.add_argument("--conf", type=float, default=0.25, help="偵測貓咪的信心門檻")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--square", type=int, default=384, help="把圖片拉伸成 NxN 正方形（跟板子一樣不保持比例）；0 = 保留原圖")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    input_dir = Path(args.input)
    out_dir = Path(args.output)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    for sub in ["images/train", "images/val", "images/test", "labels/train", "labels/val", "labels/test"]:
        (out_dir / sub).mkdir(parents=True, exist_ok=True)
    missed_dir = out_dir / "missed"

    # 找出同一張圖出現在不同類別資料夾的情況（標籤互相矛盾），直接排除
    hash_folders = defaultdict(set)
    for folder in EMOTION_CLASSES + list(NEGATIVE_DIRS):
        d = input_dir / folder
        if d.exists():
            for p in list_images(d):
                hash_folders[md5(p)].add(folder)
    conflicting = {h for h, f in hash_folders.items() if len(f) > 1}
    if conflicting:
        print(f"[注意] {len(conflicting)} 張圖同時出現在多個類別資料夾，排除不用")

    print("載入偵測模型:", args.detector)
    detector = YOLO(args.detector)

    stats = defaultdict(int)

    def write(img_path, split, stem, label_text):
        if args.square:
            # 板子韌體是把鏡頭畫面「直接拉伸」成 192x192（不保持比例），這裡先把訓練圖也拉伸成正方形，
            # 訓練時看到的形狀才會跟板子一致。框是 0~1 的相對座標，拉伸後仍然正確。
            img = imread(img_path)
            img = cv2.resize(img, (args.square, args.square), interpolation=cv2.INTER_AREA)
            cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 95])[1].tofile(
                str(out_dir / "images" / split / (stem + ".jpg")))
        else:
            shutil.copy(img_path, out_dir / "images" / split / (stem + img_path.suffix.lower()))
        (out_dir / "labels" / split / (stem + ".txt")).write_text(label_text)

    for class_idx, emotion in enumerate(EMOTION_CLASSES):
        class_dir = input_dir / emotion
        if not class_dir.exists():
            print(f"[警告] 找不到資料夾: {class_dir}，跳過")
            continue
        images = [p for p in list_images(class_dir) if md5(p) not in conflicting]
        print(f"\n=== 處理 {emotion}（class {class_idx}）: {len(images)} 張 ===")
        train, val, test = split_list(images, args.val_ratio, args.test_ratio, rng)

        for split, subset in (("train", train), ("val", val), ("test", test)):
            for img_path in subset:
                img = imread(img_path)
                if img is None:
                    print(f"[警告] 讀不到圖片: {img_path}")
                    stats["unreadable"] += 1
                    continue
                h, w = img.shape[:2]

                results = detector.predict(img, conf=args.conf, classes=[CAT_COCO_ID], verbose=False)
                boxes = results[0].boxes

                if boxes is None or len(boxes) == 0:
                    # 沒偵測到貓：整張圖當作這個情緒類別的框（通常是貓臉特寫，
                    # 整張圖幾乎都是貓）；也存一份到 missed/ 方便人工檢查
                    (missed_dir / emotion).mkdir(parents=True, exist_ok=True)
                    shutil.copy(img_path, missed_dir / emotion / img_path.name)
                    cx, cy, bw, bh = 0.5, 0.5, 1.0, 1.0
                    stats["missed"] += 1
                else:
                    # 取信心最高的框
                    best = boxes[boxes.conf.argmax()]
                    x1, y1, x2, y2 = best.xyxy[0].tolist()
                    cx = (x1 + x2) / 2 / w
                    cy = (y1 + y2) / 2 / h
                    bw = (x2 - x1) / w
                    bh = (y2 - y1) / h
                    stats["labeled"] += 1

                write(img_path, split, safe_stem(emotion, img_path),
                      f"{class_idx} {cx:.6f} {cy:.6f} {bw:.6f} {bh:.6f}\n")
                stats[f"{emotion}_{split}"] += 1

    # 負樣本：不寫任何標籤（空 label 檔 = 這張圖裡沒有貓）
    for folder, repeat in NEGATIVE_DIRS.items():
        neg_dir = input_dir / folder
        if not neg_dir.exists():
            print(f"[警告] 找不到負樣本資料夾: {neg_dir}，跳過")
            continue
        images = [p for p in list_images(neg_dir) if md5(p) not in conflicting]
        print(f"\n=== 處理 {folder}: {len(images)} 張（負樣本，不標框，訓練集重複 {repeat} 次）===")
        train, val, test = split_list(images, args.val_ratio, args.test_ratio, rng)
        for img_path in train:
            for r in range(repeat):
                stem = safe_stem(folder, img_path) + (f"__dup{r}" if r else "")
                write(img_path, "train", stem, "")
            stats[f"{folder}_train"] += 1
        for split, subset in (("val", val), ("test", test)):
            for img_path in subset:
                write(img_path, split, safe_stem(folder, img_path), "")
                stats[f"{folder}_{split}"] += 1

    data_yaml = out_dir / "data.yaml"
    data_yaml.write_text(
        "path: " + str(out_dir.resolve()).replace("\\", "/") + "\n"
        "train: images/train\n"
        "val: images/val\n"
        "test: images/test\n"
        f"nc: {len(EMOTION_CLASSES)}\n"
        f"names: {EMOTION_CLASSES}\n"
    )

    print("\n完成！統計：", dict(stats))
    print(f"沒偵測到貓的圖片：{stats['missed']} 張，存在 {missed_dir}，建議抽查一下標註品質。")
    print(f"資料集設定檔：{data_yaml}")


if __name__ == "__main__":
    main()
