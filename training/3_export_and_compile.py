#!/usr/bin/env python3
"""
Step 3: 把訓練好的 best.pt 匯出成板子韌體吃的 int8 TFLite，再用 Vela 3.9.0 編譯成可燒錄的 *_vela.tflite。

板子韌體（cvapp_yolov8n_ob.cpp）要求的格式（跟目前板子上的模型一致）：
    輸入   : [1, 192, 192, 3] int8（RGB，scale 1/255、zero_point -128）
    輸出 0 : [1, 4, 756]  int8  框 = [cx, cy, w, h]，單位是 192x192 輸入上的「像素」
    輸出 1 : [1, 756, 4]  int8  四種情緒的分數（sigmoid 後 0~1）
    韌體只註冊了 Transpose + Ethos-U 兩種 op -> 其他所有 op 都必須跑在 NPU 上

流程：
    best.pt -> ONNX（雙輸出） -> onnx2tf int8 全整數量化 -> 補 Transpose 常數的量化參數
    （Vela 3.9.0 才會把 Transpose 放到 NPU） -> Vela 3.9.0（Himax 設定檔）

用法（train_env 環境）：
    python 3_export_and_compile.py --weights runs/cat_emotion_stage2/weights/best.pt

Vela 裝在另一個環境 vela_env（需要 numpy<2），腳本會自動呼叫 ../../vela_env/Scripts/vela.exe。

輸出：
    export_out/cat_emotion_int8.tflite   <- 量化但還沒 Vela 編譯
    cat_emotion_vela.tflite              <- 最終可燒錄的檔案（燒到 0x00B7B000）
"""
import argparse
import random
import shutil
import subprocess
import sys
import types
from pathlib import Path

import cv2
import numpy as np
import torch

IMGSZ = 192
HERE = Path(__file__).resolve().parent


def imread_rgb(path, size=IMGSZ):
    # 用 imdecode 讀，避免 Windows 上中文檔名讀不到；直接拉伸成 192x192（跟板子一樣，不做 letterbox）
    im = cv2.imdecode(np.fromfile(str(path), np.uint8), cv2.IMREAD_COLOR)
    return cv2.cvtColor(cv2.resize(im, (size, size), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)


def build_export_model(weights, box_format="pixel"):
    """把 Detect 頭換成「全程 channel-last」的寫法，讓 onnx2tf 轉出來的圖完全沒有 Transpose。

    原本 YOLOv8 的頭是 NCHW：x.view(bs, C, -1) 攤平、DFL 在 channel 維做 softmax、最後再 permute，
    TFLite 是 NHWC，onnx2tf 只好插入一堆 Transpose，而 Vela 3.9.0 會把 Transpose 丟給 CPU。
    這裡改成每一層先 permute(0,2,3,1) 成 NHWC（onnx2tf 會把它消掉），之後所有運算都在最後一維做，
    框的四個座標各自算成 [1,1,756] 再在第 1 維串起來，直接得到韌體要的 [1,4,756]，完全不需要轉置。
    數學上跟原本的 Detect 輸出一模一樣（下面會用 assert 比對）。"""
    from ultralytics import YOLO
    from ultralytics.utils.tal import make_anchors

    model = YOLO(weights).model.float().eval().fuse()
    detect = model.model[-1]
    detect.export = True
    detect.format = "saved_model"
    detect.dynamic = False
    reg_max, nc = detect.reg_max, detect.nc

    feats = [torch.zeros(1, 1, IMGSZ // int(s), IMGSZ // int(s)) for s in detect.stride]
    anchors, strides = make_anchors(feats, detect.stride, 0.5)  # (756, 2), (756, 1)
    anchors = anchors.unsqueeze(0)  # (1, 756, 2)
    strides = strides.unsqueeze(0)  # (1, 756, 1)
    proj = torch.arange(reg_max, dtype=torch.float32).view(reg_max, 1)

    box_heads, cls_heads = detect.one2many["box_head"], detect.one2many["cls_head"]

    def forward(self, x):
        box = torch.cat([box_heads[i](x[i]).permute(0, 2, 3, 1).reshape(1, -1, 4 * reg_max) for i in range(self.nl)], 1)
        cls = torch.cat([cls_heads[i](x[i]).permute(0, 2, 3, 1).reshape(1, -1, nc) for i in range(self.nl)], 1)
        # DFL：每個邊 16 個 bin 做 softmax，再乘上 bin 編號加總 = 距離（單位：該層 stride）
        # 用矩陣乘法做加總（會變成 NPU 支援的 FULLY_CONNECTED；用 sum() 會變成 CPU 才能跑的 SUM）
        dist = (box.reshape(1, -1, 4, reg_max).softmax(-1) @ proj).reshape(1, -1, 4)  # (1, 756, 4) = l, t, r, b
        l, t, r, b = (dist[..., i : i + 1] for i in range(4))  # 各 (1, 756, 1)
        ax, ay, s = anchors[..., 0:1], anchors[..., 1:2], strides
        cx = (ax + (r - l) * 0.5) * s
        cy = (ay + (b - t) * 0.5) * s
        w = (l + r) * s
        h = (t + b) * s
        boxes = torch.cat([v.reshape(1, 1, -1) for v in (cx, cy, w, h)], 1)  # (1, 4, 756) 像素 cxcywh
        # 限制在 0~192：輸出的量化範圍變小，int8 每一格約 0.75 px（不限制的話會被離譜的大框撐到 2 px 以上）
        boxes = boxes.clamp(0, IMGSZ)
        if box_format == "normalized":
            # Himax 官方 tflm_yolov8_od 韌體會自己把框乘上 192，所以要輸出 0~1
            boxes = boxes * (1.0 / IMGSZ)
        return boxes, cls.sigmoid()  # (1, 756, 4)

    orig = types.MethodType(type(detect).forward, detect)
    detect.forward = types.MethodType(forward, detect)
    for p in model.parameters():
        p.requires_grad = False

    # 驗證：新寫法 == ultralytics 原本的 Detect 輸出
    with torch.no_grad():
        im = torch.rand(1, 3, IMGSZ, IMGSZ)
        new_boxes, new_scores = model(im)
        detect.forward = orig
        ref = model(im)
        ref = ref[0] if isinstance(ref, (tuple, list)) else ref  # (1, 4+nc, 756)
        detect.forward = types.MethodType(forward, detect)
    ref_boxes = ref[:, :4].clamp(0, IMGSZ) / (IMGSZ if box_format == "normalized" else 1)
    assert torch.allclose(new_boxes, ref_boxes, atol=1e-3), (new_boxes - ref_boxes).abs().max()
    assert torch.allclose(new_scores, ref[:, 4:].permute(0, 2, 1), atol=1e-5)
    return model


def export_onnx(model, onnx_path):
    import onnx
    import onnxslim

    im = torch.zeros(1, 3, IMGSZ, IMGSZ)
    with torch.no_grad():
        boxes, scores = model(im)
    assert tuple(boxes.shape) == (1, 4, 756) and tuple(scores.shape) == (1, 756, 4), (boxes.shape, scores.shape)
    torch.onnx.export(model, im, str(onnx_path), opset_version=17, input_names=["images"],
                      output_names=["boxes", "scores"], do_constant_folding=True, dynamo=False)
    onnx.save(onnxslim.slim(onnx.load(str(onnx_path))), str(onnx_path))


def calibration_images(data_dir, n):
    imgs = sorted((Path(data_dir) / "images" / "train").iterdir())
    random.Random(0).shuffle(imgs)
    # 校正資料包含貓、背景、人臉，讓量化範圍涵蓋實際會看到的畫面
    return np.stack([imread_rgb(p) for p in imgs[:n]]).astype(np.float32)  # BHWC, 0~255


def fix_transpose_quant(src, dst):
    """Vela 3.9.0 只在 Transpose 的所有 tensor（包含 perm 常數）都有量化參數時才放上 NPU。
    perm 只是索引常數，補上 scale=1、zero_point=0 不會改變任何計算結果。"""
    import flatbuffers
    from tensorflow.lite.python import schema_py_generated as schema

    buf = bytearray(Path(src).read_bytes())
    model = schema.ModelT.InitFromObj(schema.Model.GetRootAsModel(buf, 0))
    fixed = 0
    for g in model.subgraphs:
        for op in g.operators:
            code = model.operatorCodes[op.opcodeIndex]
            if max(code.builtinCode, code.deprecatedBuiltinCode) != schema.BuiltinOperator.TRANSPOSE:
                continue
            perm = g.tensors[op.inputs[1]]
            if perm.quantization is None or perm.quantization.scale is None or len(perm.quantization.scale) == 0:
                q = schema.QuantizationParametersT()
                q.scale, q.zeroPoint = [1.0], [0]
                perm.quantization = q
                fixed += 1
        # 輸出順序固定成「框 [1,4,756] 在前、分數 [1,756,4] 在後」：
        # Himax 官方 tflm_yolov8_od 韌體直接把 output(0) 當框讀，順序反了會寫爆陣列導致 BusFault 當機
        # （同學的韌體有自動交換，兩種順序都能用）
        outs = list(g.outputs)
        outs.sort(key=lambda i: 0 if list(g.tensors[i].shape)[1] == 4 else 1)
        g.outputs = outs
    b = flatbuffers.Builder(1024)
    b.Finish(model.Pack(b), file_identifier=b"TFL3")
    Path(dst).write_bytes(b.Output())
    print(f"補上 {fixed} 個 Transpose perm 常數的量化參數；輸出順序："
          + ", ".join(str(list(model.subgraphs[0].tensors[i].shape)) for i in model.subgraphs[0].outputs))


def compare(model, tflite_path, data_dir, n=40):
    """比較 PyTorch 浮點模型 vs int8 TFLite 的輸出，確認量化後沒有壞掉。"""
    import tensorflow as tf

    it = tf.lite.Interpreter(model_path=str(tflite_path))
    it.allocate_tensors()
    inp = it.get_input_details()[0]
    outs = {tuple(o["shape"]): o for o in it.get_output_details()}
    ob, osc = outs[(1, 4, 756)], outs[(1, 756, 4)]
    deq = lambda o: (it.get_tensor(o["index"]).astype(np.float32) - o["quantization"][1]) * o["quantization"][0]
    imgs = sorted((Path(data_dir) / "images" / "val").iterdir())
    random.Random(1).shuffle(imgs)
    agree = total = 0
    box_err, score_err = [], []
    for p in imgs[:n]:
        rgb = imread_rgb(p)
        with torch.no_grad():
            fb, fs = model(torch.from_numpy(rgb).permute(2, 0, 1)[None].float() / 255)
        fb, fs = fb[0].numpy(), fs[0].numpy()
        s, zp = inp["quantization"]
        it.set_tensor(inp["index"], np.clip(np.round(rgb / 255.0 / s + zp), -128, 127).astype(np.int8)[None])
        it.invoke()
        qb, qs = deq(ob)[0], deq(osc)[0]
        score_err.append(np.abs(qs - fs).max())
        a = fs.max(1).argmax()
        if fs[a].max() > 0.5:  # 有信心的偵測：比較類別跟框
            total += 1
            agree += int(qs[a].argmax() == fs[a].argmax())
            box_err.append(np.abs(qb[:, a] - fb[:, a]).max())
    print(f"量化檢查（{n} 張驗證圖）：分數最大誤差 {np.max(score_err):.3f}，"
          f"有信心偵測 {total} 個、類別一致 {agree}/{total}，框最大誤差 {np.max(box_err) if box_err else 0:.1f} px")
    print(f"框輸出量化 scale={ob['quantization'][0]:.4f}（像素）, 分數量化 scale={osc['quantization'][0]:.5f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", required=True, help="訓練完的 best.pt 路徑")
    ap.add_argument("--data-dir", default="yolo_dataset", help="1_auto_label.py 產生的資料集（做 int8 校正用）")
    ap.add_argument("--calib", type=int, default=300, help="校正圖片張數")
    ap.add_argument("--output", default="cat_emotion_vela.tflite")
    ap.add_argument("--box-format", choices=["pixel", "normalized"], default="pixel",
                    help="pixel = 同學的韌體（框是 192x192 像素座標）；normalized = Himax 官方 tflm_yolov8_od 韌體（框是 0~1）")
    ap.add_argument("--out-dir", default="export_out")
    ap.add_argument("--vela", default=str(HERE.parent.parent / "vela_env" / "Scripts" / "vela.exe"))
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir()

    print("=== 1. 匯出 ONNX（雙輸出：框 [1,4,756] 像素座標 + 分數 [1,756,4]）===")
    model = build_export_model(args.weights, args.box_format)
    onnx_path = out_dir / "cat_emotion.onnx"
    export_onnx(model, onnx_path)

    print("\n=== 2. onnx2tf int8 全整數量化 ===")
    from ultralytics.utils.export.tensorflow import onnx2saved_model

    onnx2saved_model(str(onnx_path), out_dir / "saved_model", quantize=8,
                     images=calibration_images(args.data_dir, args.calib))
    q = list((out_dir / "saved_model").glob("*_full_integer_quant.tflite"))
    if not q:
        sys.exit("找不到 *_full_integer_quant.tflite，請檢查上面 onnx2tf 的訊息")
    int8_path = out_dir / "cat_emotion_int8.tflite"
    fix_transpose_quant(q[0], int8_path)
    compare(model, int8_path, args.data_dir)

    print("\n=== 3. Vela 3.9.0 編譯（Himax 設定檔）===")
    vela_dir = out_dir / "vela"
    cmd = [args.vela, str(int8_path), "--accelerator-config", "ethos-u55-64",
           "--config", str(HERE / "himax_vela.ini"), "--system-config", "My_Sys_Cfg",
           "--memory-mode", "My_Mem_Mode_Parent", "--output-dir", str(vela_dir), "--show-cpu-operations"]
    ver = subprocess.run([args.vela, "--version"], capture_output=True, text=True).stdout.strip()
    if ver != "3.9.0":
        sys.exit(f"Vela 版本是 {ver}，板子韌體要 3.9.0")
    res = subprocess.run(cmd, capture_output=True, text=True)
    print("\n".join(l for l in res.stdout.splitlines()
                    if any(k in l for k in ("CPU", "NPU operators", "Total SRAM used", "Batch Inference time"))))
    if res.returncode != 0:
        print(res.stdout[-3000:], res.stderr[-3000:])
        sys.exit("Vela 編譯失敗")
    vela_file = next(vela_dir.glob("*_vela.tflite"))
    shutil.copy(vela_file, args.output)
    size = Path(args.output).stat().st_size
    print(f"\n完成！可燒錄的檔案：{Path(args.output).resolve()}（{size / 1024:.0f} KiB）")


if __name__ == "__main__":
    main()
