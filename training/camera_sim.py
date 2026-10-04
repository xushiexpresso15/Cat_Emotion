"""
模擬 Grove Vision AI V2 鏡頭拍到的畫面（訓練資料增強 + 評估用）。

板子實際畫面跟訓練照片的差別：
    - 鏡頭是 4:3（640x480），韌體直接壓成 192x192 -> 內容被橫向壓扁約 25%
    - 定焦鏡頭、近拍會模糊；畫面偏白、對比低、顏色淡；JPEG 壓縮
這裡用 OpenCV 隨機模擬這些效果，框會跟著一起調整。
"""
import random

import cv2
import numpy as np


def _aspect(img, k, rng):
    """垂直拉長 k 倍再裁回原高度 = 內容相對被橫向壓扁 1/k（不會產生灰邊）。回傳 (圖, 位移 offset)。"""
    h, w = img.shape[:2]
    nh = int(round(h * k))
    big = cv2.resize(img, (w, nh), interpolation=cv2.INTER_LINEAR)
    off = rng.randint(0, nh - h) if nh > h else 0
    return big[off:off + h], off


def _photometric(img, rng, strength=1.0):
    """模糊、降解析度、泛白低對比、褪色、雜訊、JPEG。strength=1 是訓練用的隨機強度。"""
    if rng.random() < 0.5 * strength:  # 失焦 / 晃動模糊
        if rng.random() < 0.7:
            s = rng.uniform(0.5, 1.6)
            img = cv2.GaussianBlur(img, (0, 0), s)
        else:
            k = rng.choice([3, 5])
            kern = np.zeros((k, k), np.float32)
            kern[k // 2, :] = 1.0 / k
            M = cv2.getRotationMatrix2D((k / 2 - 0.5, k / 2 - 0.5), rng.uniform(0, 180), 1)
            img = cv2.filter2D(img, -1, cv2.warpAffine(kern, M, (k, k)))
    if rng.random() < 0.4 * strength:  # 低解析度（240x240 JPEG 再縮放）
        h, w = img.shape[:2]
        f = rng.uniform(0.4, 0.85)
        img = cv2.resize(cv2.resize(img, (max(8, int(w * f)), max(8, int(h * f))), interpolation=cv2.INTER_AREA),
                         (w, h), interpolation=cv2.INTER_LINEAR)
    x = img.astype(np.float32)
    if rng.random() < 0.6 * strength:  # 泛白、低對比（印出來的紙 + 曝光）
        a = rng.uniform(0.55, 1.0)
        b = rng.uniform(0, 70)
        x = x * a + b + (1 - a) * x.mean()
    if rng.random() < 0.4 * strength:  # 褪色
        gray = x.mean(axis=2, keepdims=True)
        t = rng.uniform(0.1, 0.55)
        x = x * (1 - t) + gray * t
    if rng.random() < 0.3 * strength:  # 感光雜訊
        x = x + np.random.normal(0, rng.uniform(2, 9), x.shape)
    img = np.clip(x, 0, 255).astype(np.uint8)
    if rng.random() < 0.5 * strength:  # JPEG 壓縮
        q = int(rng.uniform(30, 85))
        img = cv2.imdecode(cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, q])[1], cv2.IMREAD_COLOR)
    return img


def augment_labels(labels, p=0.7, p_aspect=0.6, rng=random):
    """給 ultralytics 訓練流程用：labels["img"] 是 BGR，labels["instances"] 是框。"""
    if rng.random() >= p:
        return labels
    img = labels["img"]
    if img.ndim != 3 or img.shape[2] != 3:
        return labels
    h, w = img.shape[:2]
    if rng.random() < p_aspect:
        k = 1.0 / rng.uniform(0.7, 1.0)
        img, off = _aspect(img, k, rng)
        inst = labels["instances"]
        if len(inst):
            inst.convert_bbox("xyxy")
            if inst.normalized:
                inst.denormalize(w, h)
            before = (inst.bboxes[:, 2] - inst.bboxes[:, 0]) * (inst.bboxes[:, 3] - inst.bboxes[:, 1])
            inst.scale(1.0, k, bbox_only=True)
            inst.add_padding(0, -off)
            inst.clip(w, h)
            after = (inst.bboxes[:, 2] - inst.bboxes[:, 0]) * (inst.bboxes[:, 3] - inst.bboxes[:, 1])
            keep = after > 0.35 * before * k  # 被裁掉太多的貓就不要了
            if not keep.all():
                labels["instances"] = inst[keep]
                labels["cls"] = labels["cls"][keep]
    labels["img"] = _photometric(img, rng)
    return labels


def degrade_fixed(img, boxes_xywhn, seed):
    """評估用：固定強度的「板子畫質」（壓扁 0.75 + 模糊 + 低對比 + JPEG），框是 0~1 的 cx,cy,w,h。"""
    rng = random.Random(seed)
    h, w = img.shape[:2]
    k = 1 / 0.75
    nh = int(round(h * k))
    off = (nh - h) // 2  # 置中裁切
    img = cv2.resize(img, (w, nh), interpolation=cv2.INTER_LINEAR)[off:off + h]
    out = []
    for cx, cy, bw, bh in boxes_xywhn:
        y1, y2 = ((cy - bh / 2) * h * k - off) / h, ((cy + bh / 2) * h * k - off) / h
        y1, y2 = max(0.0, y1), min(1.0, y2)
        if y2 - y1 > 0.05:
            out.append((cx, (y1 + y2) / 2, bw, y2 - y1))
    img = cv2.GaussianBlur(img, (0, 0), rng.uniform(0.8, 1.3))
    small = cv2.resize(img, (max(8, int(w * 0.6)), max(8, int(h * 0.6))), interpolation=cv2.INTER_AREA)
    img = cv2.resize(small, (w, h), interpolation=cv2.INTER_LINEAR)
    x = img.astype(np.float32)
    a = rng.uniform(0.65, 0.8)
    x = x * a + rng.uniform(25, 50) + (1 - a) * x.mean()
    gray = x.mean(axis=2, keepdims=True)
    x = x * 0.75 + gray * 0.25
    img = np.clip(x, 0, 255).astype(np.uint8)
    img = cv2.imdecode(cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 55])[1], cv2.IMREAD_COLOR)
    return img, out


def camsim_eval_image(img, seed):
    """評估用的板子畫質：degrade_fixed() 之後再存一次 JPEG（OpenCV 預設品質 95）。
    跟 make_camsim_dataset.py 存到硬碟、再讀回來的圖逐像素相同，所以 4_evaluate.py --camsim
    和用資料集跑 mAP 看到的是同一批影像。seed = 該圖在 split 內的排序位置。"""
    out, _ = degrade_fixed(img, [], seed)
    return cv2.imdecode(cv2.imencode(".jpg", out)[1], cv2.IMREAD_COLOR)
