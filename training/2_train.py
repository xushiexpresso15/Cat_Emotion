#!/usr/bin/env python3
"""
Step 2: 訓練 YOLOv8n 貓咪情緒偵測模型（兩階段，參考專案 GitHub MANIFEST 的訓練流程）。

第一階段（stage1）：從 COCO 預訓練 yolov8n.pt 開始，250 epochs
    - SGD、Cosine 學習率 0.01 -> 0.001、最後 35 epochs 關 mosaic
    - 加上「困難負樣本懲罰」(Active Hard-Negative Suppression)：
      不是貓的位置（背景、人臉）只要任何類別分數 > 0.20 就加一個平方懲罰，
      專門壓低「把人/背景認成貓」的誤判
第二階段（stage2）：從 stage1 的 best.pt 用低學習率微調
    - 關 mosaic、減弱資料增強，讓模型在接近實際畫面的分佈上收斂

--recipe github（預設）再加上同學 GitHub（v8.2.0 MANIFEST + 他 best.pt 裡的 train_args）的做法：
    - TaskAlignedAssigner alpha=0.6、beta=4.5
    - 互斥 loss：同一個位置四類分數加總 > 1 就加平方懲罰（一隻貓只該有一種情緒）
    - 第二階段參數照他的 best.pt：SGD lr0=0.0003 lrf=0.01、25 epochs、box 7.5 / cls 1.5 / dfl 1.5、
      mosaic 0、degrees 3、translate 0.03、scale 0.1
--recipe v2：第二版（assigner 預設 0.5/6.0、無互斥 loss、第二階段 AdamW 30 epochs）

架構維持標準 YOLOv8n（SiLU）。SiLU 在 Vela 會變成 LOGISTIC + MUL，可以 100% 跑在 NPU 上
（專案現有的 best.pt 其實也是 SiLU）。

用法：
    python 2_train.py --data yolo_dataset/data.yaml --device 0
    python 2_train.py --stage 2 --weights runs/cat_emotion_stage1/weights/best.pt   # 只跑第二階段

訓練完成後，最終權重在：
    runs/cat_emotion_stage2/weights/best.pt
"""
import argparse
import os
from pathlib import Path

import torch
import torch.nn.functional as F
from ultralytics import YOLO
from ultralytics.nn.tasks import DetectionModel
from ultralytics.utils.loss import v8DetectionLoss

HARD_NEG_THRESH = 0.20
HARD_NEG_WEIGHT = 1.0
RECIPE = {"github": dict(alpha=0.6, beta=4.5, mutex=1.0), "v2": dict(alpha=0.5, beta=6.0, mutex=0.0)}
CFG = dict(RECIPE["github"])


class HardNegativeDetectionLoss(v8DetectionLoss):
    """標準 YOLOv8 loss + 困難負樣本懲罰 + 互斥 loss（都加在 cls_loss 上，所以訓練 log 的欄位不變）。"""

    def __init__(self, model):
        super().__init__(model)
        self.assigner.alpha, self.assigner.beta = CFG["alpha"], CFG["beta"]

    def get_assigned_targets_and_loss(self, preds, batch):
        assigned, loss, _ = super().get_assigned_targets_and_loss(preds, batch)
        fg_mask = assigned[0]  # (b, anchors) 被分配到貓的位置
        scores = preds["scores"].permute(0, 2, 1).sigmoid()  # (b, anchors, nc)
        excess = F.relu(scores.max(-1).values - HARD_NEG_THRESH)[~fg_mask]
        violators = (excess > 0).sum().clamp(min=1)
        # 只在「超過門檻的負樣本位置」上取平均，不會被大量正常的背景位置稀釋
        loss[1] = loss[1] + HARD_NEG_WEIGHT * (excess**2).sum() / violators
        if CFG["mutex"] > 0:
            # 互斥：四類分數加總超過 1 的位置加平方懲罰，同樣只在違反的位置上取平均
            over = F.relu(scores.sum(-1) - 1.0)
            loss[1] = loss[1] + CFG["mutex"] * (over**2).sum() / (over > 0).sum().clamp(min=1)
        return assigned, loss, dict(zip(self.loss_names, loss.detach()))


def init_criterion(self):
    return HardNegativeDetectionLoss(self)


DetectionModel.init_criterion = init_criterion

# --camera-aug：在 ultralytics 的 Albumentations 位置插入「模擬板子鏡頭畫質」（camera_sim.py）。
# 用環境變數傳遞，Windows 的 DataLoader worker 是重新執行這支程式的頂層程式碼，才會一起套用。
def enable_camera_aug():
    import camera_sim
    from ultralytics.data import augment as _aug

    if getattr(_aug.Albumentations, "_camera_sim", False):
        return
    orig = _aug.Albumentations.__call__

    def _albu_call(self, labels):
        return camera_sim.augment_labels(orig(self, labels))

    _aug.Albumentations.__call__ = _albu_call
    _aug.Albumentations._camera_sim = True


if os.environ.get("CAMERA_AUG") == "1":
    enable_camera_aug()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="yolo_dataset/data.yaml")
    ap.add_argument("--stage", type=int, choices=[0, 1, 2], default=0, help="0 = 兩階段都跑")
    ap.add_argument("--weights", default="yolov8n.pt", help="stage1 的起始權重；只跑 stage2 時填 stage1 的 best.pt")
    ap.add_argument("--epochs1", type=int, default=250)
    ap.add_argument("--epochs2", type=int, default=None, help="預設：github 25、v2 30")
    ap.add_argument("--recipe", choices=list(RECIPE), default="github")
    ap.add_argument("--camera-aug", action="store_true",
                    help="加入模擬板子鏡頭畫質的增強（4:3 壓扁、模糊、泛白低對比、JPEG），並加大縮放／色彩增強範圍")
    ap.add_argument("--imgsz", type=int, default=192, help="跟板子上的模型輸入尺寸一致")
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--device", default="0")
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()
    CFG.update(RECIPE[args.recipe])
    if args.camera_aug:
        os.environ["CAMERA_AUG"] = "1"
        enable_camera_aug()
        print("已開啟鏡頭模擬增強（camera_sim.py）")
    # 鏡頭模擬時第二階段也用比較大的縮放／色彩範圍（近拍、遠拍、亮暗都要能應付）
    if args.camera_aug:
        s2_aug = dict(scale=0.3, translate=0.1, hsv_s=0.5, hsv_v=0.4)
    else:
        s2_aug = dict(scale=0.1, translate=0.03, hsv_s=0.3, hsv_v=0.3)
    epochs2 = args.epochs2 or (25 if args.recipe == "github" else 30)
    print(f"訓練設定 recipe={args.recipe}: {CFG}, stage2 epochs={epochs2}")

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
            mosaic=1.0, mixup=0.1, degrees=5.0, translate=0.1, scale=0.5 if args.camera_aug else 0.4, fliplr=0.5,
            hsv_h=0.015, hsv_s=0.5, hsv_v=0.4, erasing=0.2,
            **common,
        )
        weights = str(Path("runs/cat_emotion_stage1/weights/best.pt").resolve())

    if args.stage in (0, 2):
        model = YOLO(weights)
        if args.recipe == "github":
            opt = dict(optimizer="SGD", momentum=0.937, weight_decay=0.0005)
        else:
            opt = dict(optimizer="AdamW")
        model.train(
            name="cat_emotion_stage2", epochs=epochs2, patience=30, **opt,
            lr0=0.0003, lrf=0.01, warmup_epochs=1,
            close_mosaic=epochs2, box=7.5, cls=1.5, dfl=1.5,
            mosaic=0.0, mixup=0.0, degrees=3.0, fliplr=0.5, hsv_h=0.015, erasing=0.0, **s2_aug,
            **common,
        )

    final = Path("runs/cat_emotion_stage2/weights/best.pt")
    if final.exists():
        metrics = YOLO(str(final)).val(data=args.data, imgsz=args.imgsz, device=args.device, iou=0.45)
        print("驗證結果:", metrics.results_dict)
        print("最終權重:", final.resolve())


if __name__ == "__main__":
    main()
