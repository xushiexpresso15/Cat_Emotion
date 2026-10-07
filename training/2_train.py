#!/usr/bin/env python3
"""
Step 2: 訓練 YOLOv8n 貓咪情緒偵測模型（model v5 的訓練設定）。

第一階段（stage1）：從 COCO 預訓練 yolov8n.pt 開始，250 epochs
    - SGD、Cosine 學習率 0.01 -> 0.001、最後 35 epochs 關 mosaic
第二階段（stage2）：從 stage1 的 best.pt 用低學習率微調 25 epochs
    - SGD lr0=0.0003 lrf=0.01、box 7.5 / cls 1.5 / dfl 1.5、關 mosaic

Loss（參考專案 v8.2.0 MANIFEST）：
    - 困難負樣本懲罰：不是貓的位置（背景、人臉）任何類別分數 > 0.20 就加平方懲罰
    - 情緒互斥：同一個位置四類分數加總 > 1 就加平方懲罰（一隻貓只該有一種情緒）
    - TaskAlignedAssigner alpha=0.6、beta=4.5

鏡頭模擬（camera_sim.py）：訓練時隨機模擬板子鏡頭畫面（4:3 壓扁、模糊、泛白低對比、JPEG）。

架構維持標準 YOLOv8n（SiLU）。SiLU 在 Vela 會變成 LOGISTIC + MUL，可以 100% 跑在 NPU 上。

用法：
    python 2_train.py --data yolo_dataset/data.yaml --device 0
    python 2_train.py --stage 2 --weights runs/cat_emotion_stage1/weights/best.pt   # 只跑第二階段

訓練完成後，最終權重在：
    runs/cat_emotion_stage2/weights/best.pt
"""
import argparse
from pathlib import Path

import torch.nn.functional as F
from ultralytics import YOLO
from ultralytics.nn.tasks import DetectionModel
from ultralytics.utils.loss import v8DetectionLoss

HARD_NEG_THRESH = 0.20
HARD_NEG_WEIGHT = 1.0
MUTEX_WEIGHT = 1.0
ASSIGNER_ALPHA, ASSIGNER_BETA = 0.6, 4.5


class HardNegativeDetectionLoss(v8DetectionLoss):
    """標準 YOLOv8 loss + 困難負樣本懲罰 + 互斥 loss（都加在 cls_loss 上，所以訓練 log 的欄位不變）。"""

    def __init__(self, model):
        super().__init__(model)
        self.assigner.alpha, self.assigner.beta = ASSIGNER_ALPHA, ASSIGNER_BETA

    def get_assigned_targets_and_loss(self, preds, batch):
        assigned, loss, _ = super().get_assigned_targets_and_loss(preds, batch)
        fg_mask = assigned[0]  # (b, anchors) 被分配到貓的位置
        scores = preds["scores"].permute(0, 2, 1).sigmoid()  # (b, anchors, nc)
        excess = F.relu(scores.max(-1).values - HARD_NEG_THRESH)[~fg_mask]
        violators = (excess > 0).sum().clamp(min=1)
        # 只在「超過門檻的負樣本位置」上取平均，不會被大量正常的背景位置稀釋
        loss[1] = loss[1] + HARD_NEG_WEIGHT * (excess**2).sum() / violators
        # 互斥：四類分數加總超過 1 的位置加平方懲罰，同樣只在違反的位置上取平均
        over = F.relu(scores.sum(-1) - 1.0)
        loss[1] = loss[1] + MUTEX_WEIGHT * (over**2).sum() / (over > 0).sum().clamp(min=1)
        return assigned, loss, dict(zip(self.loss_names, loss.detach()))


def init_criterion(self):
    return HardNegativeDetectionLoss(self)


DetectionModel.init_criterion = init_criterion


def enable_camera_aug():
    """在 ultralytics 的 Albumentations 位置插入「模擬板子鏡頭畫質」（camera_sim.py）。"""
    import camera_sim
    from ultralytics.data import augment as _aug

    if getattr(_aug.Albumentations, "_camera_sim", False):
        return
    orig = _aug.Albumentations.__call__

    def _albu_call(self, labels):
        return camera_sim.augment_labels(orig(self, labels))

    _aug.Albumentations.__call__ = _albu_call
    _aug.Albumentations._camera_sim = True


# 放在頂層：Windows 的 DataLoader worker 會重新執行這支程式的頂層程式碼，這樣 worker 也會套用
enable_camera_aug()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="yolo_dataset/data.yaml")
    ap.add_argument("--stage", type=int, choices=[0, 1, 2], default=0, help="0 = 兩階段都跑")
    ap.add_argument("--weights", default="yolov8n.pt", help="stage1 的起始權重；只跑 stage2 時填 stage1 的 best.pt")
    ap.add_argument("--epochs1", type=int, default=250)
    ap.add_argument("--epochs2", type=int, default=25)
    ap.add_argument("--imgsz", type=int, default=192, help="跟板子上的模型輸入尺寸一致")
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--device", default="0")
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    common = dict(data=args.data, imgsz=args.imgsz, batch=args.batch, device=args.device, workers=args.workers,
                  project=str(Path("runs").resolve()), exist_ok=True, seed=0, deterministic=True,
                  cos_lr=True, iou=0.45, agnostic_nms=True, cache="ram")

    weights = args.weights
    if args.stage in (0, 1):
        model = YOLO(weights)
        model.train(
            name="cat_emotion_stage1", epochs=args.epochs1, patience=80,
            optimizer="SGD", lr0=0.01, lrf=0.1, momentum=0.937, weight_decay=0.0005, warmup_epochs=3,
            close_mosaic=35, box=7.5, cls=1.0, dfl=1.5,
            mosaic=1.0, mixup=0.1, degrees=5.0, translate=0.1, scale=0.5, fliplr=0.5,
            hsv_h=0.015, hsv_s=0.5, hsv_v=0.4, erasing=0.2,
            **common,
        )
        weights = str(Path("runs/cat_emotion_stage1/weights/best.pt").resolve())

    if args.stage in (0, 2):
        model = YOLO(weights)
        model.train(
            name="cat_emotion_stage2", epochs=args.epochs2, patience=30,
            optimizer="SGD", momentum=0.937, weight_decay=0.0005, lr0=0.0003, lrf=0.01, warmup_epochs=1,
            close_mosaic=args.epochs2, box=7.5, cls=1.5, dfl=1.5,
            mosaic=0.0, mixup=0.0, degrees=3.0, translate=0.1, scale=0.3, fliplr=0.5,
            hsv_h=0.015, hsv_s=0.5, hsv_v=0.4, erasing=0.0,
            **common,
        )

    final = Path("runs/cat_emotion_stage2/weights/best.pt")
    if final.exists():
        metrics = YOLO(str(final)).val(data=args.data, imgsz=args.imgsz, device=args.device, iou=0.45)
        print("驗證結果:", metrics.results_dict)
        print("最終權重:", final.resolve())


if __name__ == "__main__":
    main()
