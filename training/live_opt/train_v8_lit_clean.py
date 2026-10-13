#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Clean retrain after quarantining auto-ingested junk from dataset_v2_multicat.

dataset_v2 is untouched except missing empty labels for 200 scene_*.png.
VAL/TEST restored to the official v2 split. TRAIN keeps v2 + real multi-cat
copies + IMX castcat + empty face/hand/livecast hardnegatives.

Fine-tunes dest with MultiCatDecoupledLoss / WIoU v3 / DomainJitter,
freeze=10 then freeze=8. USER TARGETS on VAL. Live photos print-only.
mixup=0 copy_paste=0 imgsz=192. GPU only. Does not ingest new images.
"""
from __future__ import annotations

import shutil
from copy import deepcopy
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from ultralytics import YOLO
from ultralytics.models.yolo.detect.train import DetectionTrainer
from ultralytics.nn.tasks import DetectionModel
from ultralytics.utils.loss import BboxLoss, bbox2dist, v8DetectionLoss
from ultralytics.utils.tal import TaskAlignedAssigner, make_anchors

BASE = Path("/home/freud/Documents/research2")
DATA = BASE / "dataset_v2_multicat" / "data.yaml"
ARCH = BASE / "models_archive" / "v8_generalized_acc" / "model"
DEST = ARCH / "best.pt"
WIOU = ARCH / "best_wiou.pt"
RIVAL = ARCH / "best_rival.pt"
REFINED = BASE / "models_archive" / "v8_refined_multicat" / "model" / "best.pt"
RUNS = BASE / "training_pipeline" / "runs"
CLASS_NAMES = ["angry", "focus", "relax", "scared"]
LIVE = [
    Path("/home/freud/.grok/sessions/%2Fhome%2Ffreud%2FDocuments%2Fresearch2/01a0c7d4-5b6d-7963-884b-060ff37c4360/assets/image-c1d5f871-fe73-4366-9657-3b0b3c8ab008.webp"),
    Path("/home/freud/.grok/sessions/%2Fhome%2Ffreud%2FDocuments%2Fresearch2/01a0c7d4-5b6d-7963-884b-060ff37c4360/assets/image-72054080-ed95-440f-8eda-d0b8879ff22b.webp"),
]
ORIG_INIT = DetectionModel.init_criterion
CV3W = ("cv3.0.2.weight", "cv3.1.2.weight", "cv3.2.2.weight")
CV3B = ("cv3.0.2.bias", "cv3.1.2.bias", "cv3.2.2.bias")
TMP_MIX = Path("/tmp/cat_clean_interp.pt")
TMP_BIAS = Path("/tmp/cat_clean_bias.pt")
CM_PROJ = Path("/tmp/cat_clean_cm")
TRAIN_COUNTS = torch.tensor([514.0, 631.0, 652.0, 822.0])
BG_PENALTY = 10.0
if not torch.cuda.is_available():
    raise SystemExit("GPU required")


def inner_boxes_xyxy(box: torch.Tensor, ratio: float) -> torch.Tensor:
    x1, y1, x2, y2 = box.unbind(-1)
    w = (x2 - x1).clamp(min=1e-6)
    h = (y2 - y1).clamp(min=1e-6)
    cx = (x1 + x2) * 0.5
    cy = (y1 + y2) * 0.5
    nw = w * ratio * 0.5
    nh = h * ratio * 0.5
    return torch.stack((cx - nw, cy - nh, cx + nw, cy + nh), dim=-1)


class WIoUv3BboxLoss(BboxLoss):
    def __init__(self, reg_max: int = 16, alpha: float = 1.9, delta: float = 3.0, inner_ratio: float = 1.0):
        super().__init__(reg_max)
        self.alpha = alpha
        self.delta = delta
        self.inner_ratio = inner_ratio
        self.register_buffer("iou_mean", torch.tensor(1.0))

    def forward(self, pred_dist, pred_bboxes, anchor_points, target_bboxes,
                target_scores, target_scores_sum, fg_mask, imgsz, stride):
        weight = target_scores[fg_mask].sum(-1, keepdim=True)
        pb = pred_bboxes[fg_mask]
        tb = target_bboxes[fg_mask]
        if self.inner_ratio != 1.0:
            pb = inner_boxes_xyxy(pb, self.inner_ratio)
            tb = inner_boxes_xyxy(tb, self.inner_ratio)
        x1, y1, x2, y2 = pb.unbind(-1)
        x1g, y1g, x2g, y2g = tb.unbind(-1)
        inter_w = (torch.min(x2, x2g) - torch.max(x1, x1g)).clamp(min=0)
        inter_h = (torch.min(y2, y2g) - torch.max(y1, y1g)).clamp(min=0)
        inter = inter_w * inter_h
        area_p = (x2 - x1).clamp(min=0) * (y2 - y1).clamp(min=0)
        area_g = (x2g - x1g).clamp(min=0) * (y2g - y1g).clamp(min=0)
        union = (area_p + area_g - inter).clamp(min=1e-7)
        iou = inter / union
        loss_iou = 1.0 - iou
        ww = (torch.max(x2, x2g) - torch.min(x1, x1g)).clamp(min=1e-7)
        hh = (torch.max(y2, y2g) - torch.min(y1, y1g)).clamp(min=1e-7)
        cx, cy = (x1 + x2) * 0.5, (y1 + y2) * 0.5
        cxg, cyg = (x1g + x2g) * 0.5, (y1g + y2g) * 0.5
        rho2 = (cx - cxg).pow(2) + (cy - cyg).pow(2)
        r_wiou = torch.exp(rho2 / (ww.pow(2) + hh.pow(2)).detach())
        with torch.no_grad():
            self.iou_mean.mul_(0.9).add_(loss_iou.mean(), alpha=0.1)
        beta = loss_iou.detach() / self.iou_mean.clamp(min=1e-6)
        focus = beta / (self.delta * (self.alpha ** (beta - self.delta)))
        wiou = (focus * r_wiou * loss_iou).unsqueeze(-1)
        loss_box = (wiou * weight).sum() / target_scores_sum
        if self.dfl_loss:
            target_ltrb = bbox2dist(anchor_points, target_bboxes, self.dfl_loss.reg_max - 1)
            loss_dfl = (
                self.dfl_loss(pred_dist[fg_mask].view(-1, self.dfl_loss.reg_max), target_ltrb[fg_mask])
                * weight
            )
            loss_dfl = loss_dfl.sum() / target_scores_sum
        else:
            loss_dfl = pred_dist.sum() * 0.0
        return loss_box, loss_dfl


class DomainJitterTrainer(DetectionTrainer):
    def preprocess_batch(self, batch: dict) -> dict:
        batch = super().preprocess_batch(batch)
        if not self.model.training:
            return batch
        img = batch["img"]
        bsz, _, h, w = img.shape
        device = img.device
        mask = (torch.rand(bsz, device=device) < 0.25).view(bsz, 1, 1, 1).to(img.dtype)
        r = torch.empty(bsz, 1, 1, 1, device=device, dtype=img.dtype).uniform_(0.72, 1.20)
        g = torch.empty(bsz, 1, 1, 1, device=device, dtype=img.dtype).uniform_(0.92, 1.10)
        bch = torch.empty(bsz, 1, 1, 1, device=device, dtype=img.dtype).uniform_(0.80, 1.28)
        img = (img * (1.0 - mask + mask * torch.cat([r, g, bch], dim=1))).clamp(0.0, 1.0)
        out = img.clone()
        for i in range(bsz):
            if torch.rand(1, device=device) < 0.16:
                bar = int(h * float(torch.empty(1).uniform_(0.08, 0.18)))
                out[i, :, :bar, :] *= float(torch.empty(1).uniform_(0.0, 0.25))
            if torch.rand(1, device=device) < 0.12:
                bw = int(w * float(torch.empty(1).uniform_(0.10, 0.32)))
                if torch.rand(1).item() < 0.5:
                    out[i, :, :, :bw] *= 0.05
                else:
                    out[i, :, :, w - bw :] *= 0.05
        batch["img"] = out.clamp(0.0, 1.0)
        return batch


class MultiCatDecoupledLoss(v8DetectionLoss):
    def __init__(self, model, tal_topk=10, bg_penalty_weight=10.0, inner_ratio=1.0):
        super().__init__(model, tal_topk=tal_topk)
        self.bg_penalty_weight = bg_penalty_weight
        self.assigner = TaskAlignedAssigner(
            topk=tal_topk, num_classes=self.nc, alpha=1.0, beta=3.0,
            stride=self.stride.tolist(),
        )
        self.bbox_loss = WIoUv3BboxLoss(
            reg_max=self.reg_max, alpha=1.9, delta=3.0, inner_ratio=inner_ratio
        ).to(self.device)
        inv = TRAIN_COUNTS.sum() / (TRAIN_COUNTS * len(TRAIN_COUNTS))
        w = inv.pow(0.25)
        w = (w / w.mean()).to(self.device)
        self.freq_weights = w.view(1, 1, -1)

    def get_assigned_targets_and_loss(self, preds, batch):
        loss = torch.zeros(3, device=self.device)
        pred_distri, pred_scores = (
            preds["boxes"].permute(0, 2, 1).contiguous(),
            preds["scores"].permute(0, 2, 1).contiguous(),
        )
        anchor_points, stride_tensor = make_anchors(preds["feats"], self.stride, 0.5)
        dtype = pred_scores.dtype
        batch_size = pred_scores.shape[0]
        imgsz = (
            torch.tensor(preds["feats"][0].shape[2:], device=self.device, dtype=dtype)
            * self.stride[0]
        )
        targets = torch.cat(
            (batch["batch_idx"].view(-1, 1), batch["cls"].view(-1, 1), batch["bboxes"]), 1
        )
        targets = self.preprocess(
            targets.to(self.device), batch_size, scale_tensor=imgsz[[1, 0, 1, 0]]
        )
        gt_labels, gt_bboxes = targets.split((1, 4), 2)
        mask_gt = gt_bboxes.sum(2, keepdim=True).gt_(0.0)
        pred_bboxes = self.bbox_decode(anchor_points, pred_distri)
        _, target_bboxes, target_scores, fg_mask, target_gt_idx = self.assigner(
            pred_scores.detach().sigmoid(),
            (pred_bboxes.detach() * stride_tensor).type(gt_bboxes.dtype),
            anchor_points * stride_tensor,
            gt_labels, gt_bboxes, mask_gt,
        )
        target_scores_sum = max(target_scores.sum(), 1.0)
        t = target_scores.to(dtype)
        bce_loss = F.binary_cross_entropy_with_logits(pred_scores, t, reduction="none")
        bce_loss = bce_loss * self.freq_weights.to(dtype)
        loss[1] = bce_loss.sum() / target_scores_sum
        is_bg_image = mask_gt.squeeze(-1).sum(dim=-1) == 0
        if is_bg_image.any():
            bg_p = torch.sigmoid(pred_scores[is_bg_image])
            bg_excess = F.relu(bg_p - 0.05)
            num_bg_anchors = max(
                is_bg_image.sum().item() * pred_scores.shape[1] * self.nc, 1
            )
            loss[1] += (bg_excess.pow(2)).sum() / num_bg_anchors * self.bg_penalty_weight
        if fg_mask.sum():
            loss[0], loss[2] = self.bbox_loss(
                pred_distri, pred_bboxes, anchor_points, target_bboxes / stride_tensor,
                target_scores, target_scores_sum, fg_mask, imgsz, stride_tensor,
            )
        else:
            loss[0] += pred_distri[..., :0].sum()
        loss[0] *= self.hyp.box
        loss[1] *= self.hyp.cls
        loss[2] *= self.hyp.dfl
        return (
            (fg_mask, target_gt_idx, target_bboxes, anchor_points, stride_tensor),
            loss,
            dict(zip(self.loss_names, loss.detach())),
        )


def custom_init_criterion(self):
    return MultiCatDecoupledLoss(self, tal_topk=10, bg_penalty_weight=BG_PENALTY, inner_ratio=1.0)


def summarize(tag, split, metrics):
    per = {n: dict(zip(["P", "R", "mAP50", "mAP"], map(float, metrics.box.class_result(i))))
           for i, n in enumerate(CLASS_NAMES)}
    out = {"P": float(metrics.box.mp), "R": float(metrics.box.mr),
           "mAP50": float(metrics.box.map50), "mAP": float(metrics.box.map), "per": per}
    print(f"  {tag} {split}: P={out['P']:.3f} R={out['R']:.3f} mAP50={out['mAP50']:.3f} "
          f"mAP={out['mAP']:.3f} sci={sci_score(out):.3f}")
    for n in CLASS_NAMES:
        d = per[n]
        print(f"    {n:8s} P={d['P']:.3f} R={d['R']:.3f} mAP50={d['mAP50']:.3f} mAP={d['mAP']:.3f}")
    return out


def sci_score(s):
    return (s["R"] + s["mAP50"] + s["mAP"]
            + min(d["R"] for d in s["per"].values())
            + min(d["mAP50"] for d in s["per"].values()))


def val_targets_hit(val001, diag):
    rec = all(d["R"] >= 0.90 for d in val001["per"].values())
    m50 = all(d["mAP50"] >= 0.90 for d in val001["per"].values())
    mmap = val001["mAP"] >= 0.75
    cm = all(x >= 0.90 for x in diag)
    print(f"  VAL_TARGETS rec={rec} mAP50={m50} mAP={mmap} CMdiag={cm}")
    return rec and m50 and mmap and cm


def evaluate(path, split, conf, plots=False):
    DetectionModel.init_criterion = ORIG_INIT
    kw = dict(data=str(DATA), split=split, imgsz=192, conf=conf, iou=0.45,
              batch=16, device=0, plots=plots, verbose=False)
    if plots:
        kw.update(project=str(CM_PROJ), name=f"{split}_{conf}", exist_ok=True)
    m = YOLO(str(path)).val(**kw)
    return summarize(f"conf={conf}", split, m), m


def cm_diag_from_metrics(metrics):
    mat = np.array(metrics.confusion_matrix.matrix, dtype=float)
    diags = []
    for i in range(4):
        col = float(mat[:, i].sum())
        diags.append(float(mat[i, i] / col) if col > 0 else 0.0)
    return diags, mat


def empty_and_face_fp(path, split="val"):
    labels = BASE / "dataset_v2_multicat" / "labels" / split
    images = BASE / "dataset_v2_multicat" / "images" / split
    paths, stems = [], []
    for p in sorted(labels.glob("*.txt")):
        if p.read_text().strip():
            continue
        img = next((images / f"{p.stem}{ext}" for ext in (".jpg", ".png", ".jpeg")
                    if (images / f"{p.stem}{ext}").exists()), None)
        if img is None:
            continue
        paths.append(str(img))
        stems.append(p.stem)
    if not paths:
        return 0, 0
    DetectionModel.init_criterion = ORIG_INIT
    model = YOLO(str(path))
    fp20 = face50 = 0
    for i in range(0, len(paths), 16):
        chunk = paths[i:i + 16]
        res = model.predict(chunk, imgsz=192, conf=0.20, device=0, verbose=False)
        for stem, r in zip(stems[i:i + 16], res):
            if r.boxes is None or len(r.boxes) == 0:
                continue
            fp20 += len(r.boxes)
            n50 = int((r.boxes.conf >= 0.50).sum().item()) if r.boxes.conf is not None else 0
            if stem.startswith("face_"):
                face50 += n50
    return fp20, face50


def live_counts(path):
    DetectionModel.init_criterion = ORIG_INIT
    model = YOLO(str(path))
    out = []
    for p in LIVE:
        if not p.exists():
            print("  live FP MISSING", p)
            out.append(-1)
            continue
        n = len(model.predict(str(p), imgsz=192, conf=0.25, verbose=False)[0].boxes or [])
        out.append(n)
        print("  live FP", p.name, n)
    return out


def op_quality(diag, op, empty20, face50):
    md, mn = float(np.mean(diag)), float(np.min(diag))
    rmin = min(d["R"] for d in op["per"].values())
    return md + mn + float(op["R"]) + rmin - 0.015 * empty20 - 0.05 * face50


def quality_eval(path, tag):
    sci_m, _ = evaluate(path, "val", 0.001, plots=False)
    op_m, raw = evaluate(path, "val", 0.25, plots=True)
    diag, _ = cm_diag_from_metrics(raw)
    empty20, face50 = empty_and_face_fp(path, "val")
    q = op_quality(diag, op_m, empty20, face50)
    rec = {
        "tag": tag, "path": str(path), "q": q, "sci": sci_score(sci_m),
        "diag": diag, "minD": min(diag), "meanD": float(np.mean(diag)),
        "R25": op_m["R"], "empty20": empty20, "face50": face50,
        "sci_m": sci_m, "op_m": op_m,
    }
    dtxt = "/".join(f"{x:.2f}" for x in diag)
    print(f"  Q {tag} q={q:.4f} sci={rec['sci']:.4f} diag={dtxt} "
          f"minD={rec['minD']:.3f} R25={op_m['R']:.3f} empty20={empty20} "
          f"face50={face50}")
    return rec


def better(cand, best):
    if best is None:
        return True
    if cand["minD"] < best["minD"] - 0.01:
        print(f"  veto minD {cand['tag']} {cand['minD']:.3f} < {best['minD']:.3f}")
        return False
    ds = cand["sci"] - best["sci"]
    if cand["minD"] > best["minD"] + 0.005 and ds >= -0.08:
        return True
    dq = cand["q"] - best["q"]
    if dq > 1e-4 and ds >= -0.08 and cand["minD"] >= best["minD"] - 0.005:
        return True
    if abs(dq) < 1e-4 and ds > 1e-6 and cand["minD"] >= best["minD"] - 1e-6:
        return True
    return False


def load_sd(path):
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    model = ckpt.get("ema") or ckpt["model"]
    return ckpt, {k: v.float().cpu() for k, v in model.state_dict().items()}


def write_mix(sd_a, sd_b, ckpt_a, alpha, dest):
    avg = {k: (1.0 - alpha) * sd_a[k] + alpha * sd_b[k]
           for k in sd_a if k in sd_b and sd_a[k].shape == sd_b[k].shape}
    out = deepcopy(ckpt_a)
    for key in ("ema", "model"):
        target = out.get(key)
        if target is None:
            continue
        target.load_state_dict(avg, strict=False)
    torch.save(out, dest)


def apply_cv3(src, dest, bias=None, scale=None):
    ckpt = torch.load(src, map_location="cpu", weights_only=False)
    b = None if bias is None else torch.tensor(bias, dtype=torch.float32)
    sc = None if scale is None else torch.tensor(scale, dtype=torch.float32)
    for key in ("ema", "model"):
        target = ckpt.get(key)
        if target is None:
            continue
        sd = target.state_dict()
        for k, v in list(sd.items()):
            if sc is not None and any(k.endswith(s) for s in CV3W):
                mixed = v.float().clone()
                for c in range(min(4, mixed.shape[0])):
                    mixed[c] = mixed[c] * sc[c]
                sd[k] = mixed.to(dtype=v.dtype)
            if b is not None and any(k.endswith(s) for s in CV3B):
                sd[k] = (v.float() + b.to(dtype=torch.float32)).to(dtype=v.dtype)
            if sc is not None and any(k.endswith(s) for s in CV3B):
                adj = torch.log(sc.clamp(min=1e-4))
                sd[k] = (sd[k].float() + adj.to(dtype=torch.float32)).to(dtype=v.dtype)
        target.load_state_dict(sd, strict=True)
    torch.save(ckpt, dest)


def promote(path, tag):
    shutil.copy2(path, DEST)
    shutil.copy2(path, ARCH / "best_soup.pt")
    keep = ARCH / f"best_interp_{tag}.pt"
    if Path(path).resolve() != keep.resolve():
        shutil.copy2(path, keep)
    print("Promoted", tag, Path(path).name)


def maybe_take(cand, best, dest_changed):
    if better(cand, best):
        print(f"  NEW BEST {cand['tag']} q={cand['q']:.4f} sci={cand['sci']:.4f}")
        promote(cand["path"], cand["tag"].replace(" ", "_")[:28])
        return cand, True
    return best, dest_changed


def mix_grid(parent_a, parent_b, tag, alphas, best, dest_changed):
    ckpt_a, sd_a = load_sd(parent_a)
    try:
        _, sd_b = load_sd(parent_b)
    except Exception as e:
        print("skip load", tag, type(e).__name__)
        return best, dest_changed
    shared = [k for k in sd_a if k in sd_b and sd_a[k].shape == sd_b[k].shape]
    if len(shared) < 0.9 * len(sd_a):
        print("skip mismatch", tag, len(shared), len(sd_a))
        return best, dest_changed
    for a in alphas:
        write_mix(sd_a, sd_b, ckpt_a, a, TMP_MIX)
        name = f"{tag}@{a:.2f}"
        cand = quality_eval(TMP_MIX, name)
        snap = ARCH / f"best_interp_{tag}_{int(a * 100)}.pt"
        shutil.copy2(TMP_MIX, snap)
        cand["path"] = str(snap)
        best, dest_changed = maybe_take(cand, best, dest_changed)
    return best, dest_changed


def report_targets(val, test_ap, test_op, diag, tag):
    rec = all(d["R"] >= 0.90 for d in val["per"].values())
    m50 = all(d["mAP50"] >= 0.90 for d in val["per"].values())
    mmap = val["mAP"] >= 0.75
    cm = all(x >= 0.90 for x in diag)
    print(f"  TARGETS {tag} rec={rec} mAP50={m50} mAP={mmap} CMdiag={cm}")
    print("USER TARGETS", "PASS" if rec and m50 and mmap and cm else "MISS")
    print("VAL", val["mAP50"], val["mAP"], val["R"])
    print("TEST@0.001", test_ap["mAP50"], test_ap["mAP"], test_ap["R"])
    print("VAL_CM", "/".join(f"{x:.3f}" for x in diag))
    return rec and m50 and mmap and cm


def checkpoint(dest_changed, tag):
    val, _ = evaluate(DEST, "val", 0.001, plots=False)
    op, raw = evaluate(DEST, "val", 0.25, plots=True)
    diag, _ = cm_diag_from_metrics(raw)
    test_ap, _ = evaluate(DEST, "test", 0.001, plots=False)
    test_op, _ = evaluate(DEST, "test", 0.25, plots=False)
    print("holdout live photos (not used to select):")
    live_counts(DEST)
    report_targets(val, test_ap, test_op, diag, tag)
    if dest_changed:
        try:
            from archive_training_metrics import archive_metrics
            archive_metrics(model_path=DEST, metrics_dir=ARCH.parent / "metrics",
                            data_yaml=DATA, title=f"V8 Generalized Acc ({tag})")
        except Exception as e:
            print("archive skip", e)
    return val_targets_hit(val, diag)


def train_custom(start, name, freeze, lr0, epochs, seed, mosaic=0.55):
    DetectionModel.init_criterion = custom_init_criterion
    YOLO(str(start)).train(
        data=str(DATA), project=str(RUNS), name=name, exist_ok=True,
        imgsz=192, batch=16, device=0, workers=4, epochs=epochs, patience=8,
        close_mosaic=max(3, epochs // 4), freeze=freeze, lr0=lr0, lrf=0.01,
        optimizer="SGD", mosaic=mosaic, mixup=0.0, copy_paste=0.0, scale=0.50,
        degrees=8.0, translate=0.10, hsv_v=0.35, hsv_s=0.40, hsv_h=0.015,
        cls=1.2, box=7.5, dfl=1.5, fliplr=0.5, iou=0.45, agnostic_nms=True,
        val=True, plots=True, verbose=True, seed=seed, trainer=DomainJitterTrainer,
        cls_pw=0.25, label_smoothing=0.05, warmup_epochs=0.5, cos_lr=True,
    )
    DetectionModel.init_criterion = ORIG_INIT
    return RUNS / name / "weights" / "best.pt", RUNS / name / "weights" / "last.pt"


def take_run(best_pt, last_pt, tag, best, dest_changed, mix_alphas):
    shutil.copy2(best_pt, ARCH / f"best_{tag}.pt")
    spec = quality_eval(best_pt, tag)
    spec["path"] = str(ARCH / f"best_{tag}.pt")
    best, dest_changed = maybe_take(spec, best, dest_changed)
    best, dest_changed = mix_grid(
        DEST, ARCH / f"best_{tag}.pt", f"cleanx{tag}", mix_alphas, best, dest_changed,
    )
    if last_pt.exists():
        shutil.copy2(last_pt, ARCH / f"best_{tag}_last.pt")
        best, dest_changed = mix_grid(
            DEST, last_pt, f"cleanx{tag}L", (0.08, 0.16, 0.28), best, dest_changed,
        )
    return best, dest_changed


def main():
    print("CLEAN_RETRAIN_START")
    print("strategy: VAL min(CM_diag) hard floor; q is tie-break; USER TARGETS on VAL")
    print("integrity: no ingest; live photos print-only; WIoU+bg FT from dest")
    dest_changed = False
    pre = ARCH / "best_pre_clean.pt"
    if not pre.exists():
        shutil.copy2(DEST, pre)
        print("backed dest ->", pre)

    print("=== dest baseline on cleaned VAL ===")
    best = quality_eval(DEST, "dest")

    print("=== freeze=10 WIoU+bg FT from dest on cleaned train ===")
    bpt, lpt = train_custom(DEST, "yolov8n_silu_lit_clean_f10", freeze=10, lr0=2e-5,
                            epochs=16, seed=701, mosaic=0.55)
    best, dest_changed = take_run(bpt, lpt, "clean_f10", best, dest_changed,
                                  (0.08, 0.16, 0.28, 0.40))

    print("=== freeze=8 WIoU+bg FT from dest on cleaned train ===")
    bpt, lpt = train_custom(DEST, "yolov8n_silu_lit_clean_f8", freeze=8, lr0=1.2e-5,
                            epochs=12, seed=702, mosaic=0.60)
    best, dest_changed = take_run(bpt, lpt, "clean_f8", best, dest_changed,
                                  (0.08, 0.16, 0.28, 0.40))

    hit = checkpoint(dest_changed, "clean")
    print("CLEAN_RETRAIN_HIT" if hit else "CLEAN_RETRAIN_MISS")
    print("CHAIN_DONE")


if __name__ == "__main__":
    main()
