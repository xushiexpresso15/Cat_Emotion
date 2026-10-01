# 貓咪情緒模型 - Windows 桌機訓練流程

目錄結構（`D:\catphotoclaudetraining`）：

```
dataset/            原始照片：angry/ focus/ relax/ scared/（貓）+ background/ asian face/（負樣本）
training/training/  這裡的腳本
train_env/          訓練環境（Python 3.12、PyTorch CUDA、ultralytics、tensorflow 2.19、onnx2tf）
vela_env/           Vela 3.9.0 專用環境（需要 numpy<2，用 MinGW 從原始碼編譯）
```

以下指令都在 `training/training/` 底下執行。

## 1. 自動標註

```bat
..\..\train_env\Scripts\python.exe 1_auto_label.py --input ../../dataset --output yolo_dataset
```

- 用 COCO 預訓練的 `yolov8x.pt` 抓貓的位置，框標成資料夾名稱的情緒。
- `background/`、`asian face/` 是**負樣本**，標籤檔是空的，代表「這張圖沒有貓」。不會新增類別，模型輸出仍是 4 類。人臉在訓練集重複 2 次，加強「人不是貓」。
- 所有圖片會**拉伸成 384x384 正方形**。板子韌體就是把鏡頭畫面直接拉伸成 192x192，這樣訓練看到的形狀跟板子一致。
- `EXCLUDE` 清單是人工檢查後排除的圖，例如背景照裡有貓、偵測器抓不到而且貓只佔畫面一小部分的全身照。
- 沒抓到貓但是特寫的照片，用整張圖當框，存一份在 `yolo_dataset/missed/` 可以抽查。

## 2. 訓練（RTX 3060 約 25 分鐘）

```bat
..\..\train_env\Scripts\python.exe 2_train.py --data yolo_dataset/data.yaml --device 0
```

- **第一階段**：從 COCO `yolov8n.pt` 訓練 250 epochs。
  - SGD、Cosine 學習率 0.01→0.001，最後 35 epochs 關 mosaic。
  - 加上困難負樣本懲罰：不是貓的位置分數超過 0.2 就加平方懲罰，壓低背景和人臉的誤判。
- **第二階段**：低學習率微調，關閉 mosaic。
- `--recipe github`（預設）：再加上同學 GitHub 的做法，第三版就是用這個設定：
  - assigner alpha=0.6、beta=4.5
  - 情緒互斥 loss
  - 第二階段 SGD、25 epochs
- `--recipe v2`：第二版的設定。
- 最終權重在 `runs/cat_emotion_stage2/weights/best.pt`。

| 版本 | mAP50 | 板子門檻 0.5 下的貓咪偵測率 | 情緒正確率 | 背景／人臉誤判 |
|---|---|---|---|---|
| v2（`cat_emotion_vela_v2.tflite`） | 85.8% | **93.0%** | 77.5% | 0% / 0% |
| v3（`cat_emotion_vela_v3.tflite`） | **89.3%** | 88.9% | 77.5% | 0% / 0% |

## 3. 匯出 + 量化 + Vela 編譯

```bat
..\..\train_env\Scripts\python.exe 3_export_and_compile.py --weights runs/cat_emotion_stage2/weights/best.pt
```

產生 `cat_emotion_vela.tflite`，格式跟板子韌體（`cvapp_yolov8n_ob.cpp`）相容：

| | shape | 內容 |
|---|---|---|
| 輸入 | [1,192,192,3] int8 | RGB |
| 輸出 0 | [1,4,756] int8 | 框 cx,cy,w,h，192x192 上的**像素座標** |
| 輸出 1 | [1,756,4] int8 | 4 種情緒分數（0~1） |

- 匯出時把 YOLOv8 的偵測頭改寫成「全程 NHWC」，圖裡**沒有任何 Transpose**，Vela 3.9.0 編譯後 **100% 跑在 NPU**（韌體只註冊了 Transpose + Ethos-U，不支援其他 CPU op）。
- 腳本會自動比對改寫的偵測頭跟原本 YOLOv8 的輸出一致，並檢查量化前後的結果。
- Vela 一定用 **3.9.0** 加 Himax 設定檔 `himax_vela.ini`（My_Sys_Cfg / My_Mem_Mode_Parent）。

## 4. 模擬板子評估

```bat
..\..\train_env\Scripts\python.exe 4_evaluate.py --model export_out/cat_emotion_int8.tflite
```

用 int8 模型、跟韌體一樣的門檻 0.5 和 NMS 0.45，在驗證集上算背景誤判、人臉誤判、貓咪偵測率、情緒正確率和混淆矩陣。

## 5. 燒錄（Mac）

把 `cat_emotion_vela.tflite` 傳到 Mac，覆蓋 `my_model/cat_emotion_v8_sota_vela.tflite`（先備份原本的），然後：

```bash
./flash_my_model.sh
```

**只燒模型，不要加 `--with-firmware`。** 燒完執行 `python3 server.py` 開網頁測試。
