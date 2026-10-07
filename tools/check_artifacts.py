#!/usr/bin/env python3
"""
Lightweight static checks for the v5 model release (run by CI and locally from the repo root):

1. Artifact paths: model / firmware files exist, MANIFEST.json is valid, and the Windows flasher's
   default paths resolve to existing files.
2. Tensor contract of every model/v5/*.tflite:
   - input  [1,192,192,3] int8, scale 1/255, zero point -128
   - output 0 = boxes [1,4,756] int8, output 1 = scores [1,756,4] int8 (the Himax official firmware
     reads output(0) as boxes, so the order matters)
   - box format matches the file name: *_himax_official* -> normalized 0-1, otherwise pixel 0-192
   - Vela files contain a single ethos-u custom op (100% NPU, no CPU fallback)
3. Flasher guard: the default model matches the project firmware, and a mismatched model is refused.
   --with-firmware regression (--dry-run, no serial port): the firmware image is picked from
   --firmware-type, and every firmware/model mismatch is refused, including an explicit --firmware
   that does not match --firmware-type and an unknown image.
4. Evaluation command: runs training/4_evaluate.py (with and without --camsim) on a tiny synthetic
   dataset to make sure the documented command executes end to end.

Usage:  python tools/check_artifacts.py
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np
import tflite

ROOT = Path(__file__).resolve().parent.parent
V5 = ROOT / "model" / "v5"
MODELS = {
    "cat_emotion_v5_vela.tflite": ("pixel", True),
    "cat_emotion_v5_int8.tflite": ("pixel", False),
    "cat_emotion_v5_vela_himax_official.tflite": ("normalized", True),
    "cat_emotion_v5_int8_himax_official.tflite": ("normalized", False),
}
FIRMWARE = [ROOT / "firmware" / "output.img", ROOT / "firmware" / "himax_official_tflm_yolov8_od" / "output.img"]
OPNAMES = {v: k for k, v in vars(tflite.BuiltinOperator).items() if not k.startswith("_")}

failures = []


def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def tensor_info(g, idx):
    t = g.Tensors(idx)
    q = t.Quantization()
    return t.ShapeAsNumpy().tolist(), t.Type(), q.Scale(0), q.ZeroPoint(0)


def check_model(name, want_fmt, is_vela):
    m = tflite.Model.GetRootAsModel((V5 / name).read_bytes(), 0)
    g = m.Subgraphs(0)
    shape, typ, s, zp = tensor_info(g, g.Inputs(0))
    check(shape == [1, 192, 192, 3] and typ == tflite.TensorType.INT8 and abs(s - 1 / 255) < 1e-6 and zp == -128,
          f"{name}: input [1,192,192,3] int8 scale 1/255 zp -128 (got {shape}, type {typ}, {s:.6f}, {zp})")
    check(g.OutputsLength() == 2, f"{name}: 2 outputs")
    b_shape, b_typ, b_s, b_zp = tensor_info(g, g.Outputs(0))
    c_shape, c_typ, c_s, c_zp = tensor_info(g, g.Outputs(1))
    check(b_shape == [1, 4, 756] and b_typ == tflite.TensorType.INT8, f"{name}: output 0 = boxes [1,4,756] int8 (got {b_shape})")
    check(c_shape == [1, 756, 4] and c_typ == tflite.TensorType.INT8, f"{name}: output 1 = scores [1,756,4] int8 (got {c_shape})")
    top = (127 - b_zp) * b_s
    fmt = "normalized" if top <= 2 else "pixel" if top >= 64 else "unknown"
    check(fmt == want_fmt, f"{name}: box format {fmt} (max {top:.1f}), expected {want_fmt}")
    check(abs((127 - c_zp) * c_s - 1.0) < 0.02, f"{name}: score range 0..1 (max {(127 - c_zp) * c_s:.3f})")
    if is_vela:
        ops = []
        for i in range(g.OperatorsLength()):
            oc = m.OperatorCodes(g.Operators(i).OpcodeIndex())
            ops.append(OPNAMES.get(max(oc.BuiltinCode(), oc.DeprecatedBuiltinCode())))
        check(ops == ["CUSTOM"], f"{name}: single ethos-u custom op, 100% NPU (ops: {ops})")


def main():
    print("[1] Artifact paths")
    for name in MODELS:
        check((V5 / name).is_file(), f"model/v5/{name} exists")
    for fw in FIRMWARE:
        check(fw.is_file() and fw.stat().st_size > 100_000, f"{fw.relative_to(ROOT)} exists")
    manifest = json.loads((V5 / "MANIFEST.json").read_text(encoding="utf-8"))
    for key in ("vela_model", "vela_model_himax_official", "tflite_model", "tflite_model_himax_official", "pytorch_weights"):
        path = manifest.get("files", {}).get(key)
        check(bool(path) and (V5 / path).is_file(), f"MANIFEST files.{key} -> {path}")

    spec = importlib.util.spec_from_file_location("flasher", ROOT / "flashing" / "flash_model_windows.py")
    flasher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(flasher)
    check(flasher.MODEL_PATH.is_file(), f"flasher default model resolves: {flasher.MODEL_PATH.relative_to(ROOT)}")
    for ftype, fw in flasher.FIRMWARE_PATHS.items():
        check(fw.is_file(), f"flasher {ftype} firmware resolves: {fw.relative_to(ROOT)}")
        check(flasher.firmware_type_of(fw) == ftype, f"flasher recognises {fw.relative_to(ROOT)} as {ftype} (SHA-256)")

    print("[2] Tensor contract")
    for name, (fmt, is_vela) in MODELS.items():
        check_model(name, fmt, is_vela)

    print("[3] Flasher box-format guard")
    check(flasher.model_box_format(flasher.MODEL_PATH) == flasher.EXPECTED_BOX_FORMAT["project"],
          "default model matches the project firmware (pixel boxes)")
    flasher.MODEL_PATH = V5 / "cat_emotion_v5_vela_himax_official.tflite"
    check(flasher.flash(port="NONE", firmware_type="project") is False,
          "normalized model is refused for the project firmware (before touching the serial port)")

    print("[3b] Flasher --with-firmware regression (--dry-run)")
    om = "model/v5/cat_emotion_v5_vela_himax_official.tflite"
    pf, of = "firmware/output.img", "firmware/himax_official_tflm_yolov8_od/output.img"
    cases = [
        # (description, args, expected exit code, text that must appear in the output)
        ("project defaults -> project firmware + pixel model", ["--with-firmware"], 0, "firmware" + "/output.img -> 0x"),
        ("official type, no --firmware -> official image picked automatically",
         ["--with-firmware", "--firmware-type", "official", "--model", om], 0, "himax_official_tflm_yolov8_od"),
        ("official type + project firmware image is refused",
         ["--with-firmware", "--firmware-type", "official", "--model", om, "--firmware", pf], 1, "is the project firmware"),
        ("project type + official firmware image is refused",
         ["--with-firmware", "--firmware-type", "project", "--firmware", of], 1, "is the official firmware"),
        ("official model + project firmware (defaults) is refused", ["--with-firmware", "--model", om], 1, "expects pixel"),
        ("unknown firmware image is refused", ["--with-firmware", "--firmware", "README.md"], 1, "not a known firmware image"),
    ]
    for desc, extra, want_code, want_text in cases:
        r = subprocess.run([sys.executable, "flashing/flash_model_windows.py", "--dry-run", *extra],
                           capture_output=True, text=True, encoding="utf-8", cwd=ROOT)
        out = (r.stdout + r.stderr).replace("\\", "/")
        check(r.returncode == want_code and want_text in out, f"{desc} (exit {r.returncode})")
        if not (r.returncode == want_code and want_text in out):
            print(out[-1500:])

    print("[4] Evaluation command (synthetic smoke test)")
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d / "images" / "test").mkdir(parents=True)
        (d / "labels" / "test").mkdir(parents=True)
        rng = np.random.default_rng(0)
        samples = {"background__a": "", "asian_face__b": "", "angry__c": "0 0.5 0.5 0.6 0.6\n"}
        for stem, label in samples.items():
            img = (rng.random((384, 384, 3)) * 255).astype(np.uint8)
            cv2.imwrite(str(d / "images" / "test" / f"{stem}.jpg"), img)
            (d / "labels" / "test" / f"{stem}.txt").write_text(label)
        for model in ("cat_emotion_v5_int8.tflite", "cat_emotion_v5_int8_himax_official.tflite"):
            for extra in ([], ["--camsim"]):
                cmd = [sys.executable, str(ROOT / "training" / "4_evaluate.py"), "--model", str(V5 / model),
                       "--data-dir", str(d), "--split", "test", "--score", "0.25", *extra]
                r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", cwd=ROOT / "training")
                check(r.returncode == 0 and "情緒正確率" in r.stdout,
                      f"4_evaluate.py {model} {' '.join(extra)} runs (exit {r.returncode})")
                if r.returncode != 0:
                    print(r.stdout[-2000:], r.stderr[-2000:])

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED")
        sys.exit(1)
    print("All checks passed")


if __name__ == "__main__":
    main()
