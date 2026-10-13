# Live board artifacts 2026-10-13

Dataset images are not in this repo.

Dest is dest 92% + QAT-last 8%. Official VAL minD 0.786, CM 0.829/0.837/0.786/0.800, empty20=0.

Flash model-only to 0xB7B000. Firmware image is the 256x192 JPEG crop build.

| file | bytes | md5 |
|---|---:|---|
| best.pt | 6216049 | 964677a60853b2807541f1b5fe7babe5 |
| cat_emotion_v8_generalized_acc_int8.tflite | 3223904 | 4eca33120e137dd99da075953b2f64d0 |
| cat_emotion_v8_generalized_acc_vela.tflite | 2730848 | 13c147d5afe65abe2a7464e05964a894 |

firmware/live_256x192_output.img 364544 md5 2f5a44eb7abcefa31fbaefcfdc15aeee
