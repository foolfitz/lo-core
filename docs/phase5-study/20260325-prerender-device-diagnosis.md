# PreRenderDevice 診斷與實驗記錄（2026-03-25）

> 前置文件：`20260323-direction-f-experiment-log.md`
> 分支：ext-uno-api-26-2-1

---

## 背景

方向 F（實驗 F1–F4）反覆失敗：overlay 邊框在打字時消失。
即使把 PaintOverlays 移到 EndDrawLayers 前（F4），overlay 仍然被覆蓋。

---

## 診斷：確認 PreRenderDevice 存在

### 方法

在 `viewsh.cxx` 的 hook 呼叫點加入 SAL_WARN 診斷。

### 結果

```
OverlayDiag PaintWin[0]: PreRenderDev=YES BufferedOutput=1 BufferedOverlay=1
  OutDev=0 rRenderCtx=0 sameDevice=1
```

**每次 paint 都有 PreRenderDevice。** 這是方向 F 所有實驗失敗的根因。

### VCL Paint 流程（完整理解）

```
SwEditWin::Paint(rRenderContext = window):
  edtwin2.cxx: hook 設定 → pWrtShell->Paint()

SwViewShell::Paint():
  DLPrePaint2()
    → BeginDrawLayers() → PreparePreRenderDevice() → 建立 PreRenderDevice(VirtualDevice)
  PaintDesktop(rRenderContext)       ← 畫到 window
  PaintSwFrame(rRenderContext)       ← 畫到 window
  [hook 位置]
  DLPostPaint2()
    → EndDrawLayers() → EndCompleteRedraw():
        1. FormLayer / TextEdit drawing
        2. DrawOverlay() → ImpSaveBackground(PreRenderDevice) + ImpDrawMembers(PreRenderDevice)
        3. OutputPreRenderDevice() → flush PreRenderDevice 到 window（opaque DrawOutDev）
```

關鍵：
- `PaintSwFrame` 畫到 **window**（rRenderContext）
- `ImpSaveBackground` 從 **PreRenderDevice** 讀取
- `OutputPreRenderDevice` 將 PreRenderDevice **覆蓋**到 window
- 游標閃爍 `ImpBufferTimerHandler` 從 PreRenderDevice buffer 恢復背景

---

## 實驗 G1：Hook 導向 PreRenderDevice

### 假說

畫到 PreRenderDevice → `ImpSaveBackground` 擷取 overlay → 游標閃爍不會擦除。

### 變更

```cpp
if (m_fnPreEndDrawLayersHook)
{
    SdrPaintWindow* pPW = pDV->FindPaintWindow(rRenderContext);
    if (pPW && pPW->GetPreRenderDevice())
        pTarget = &pPW->GetPreRenderDevice()->GetPreRenderDevice();
    m_fnPreEndDrawLayersHook(*pTarget);
}
```

### 結果

- **Overlay 不消失**（打字時保持可見）
- 但顯示為**一個小綠點**，而非段落邊框
- 座標/大小不正確

### 裝置診斷

```
OverlayDev: type=2 sizePx=(1543x825) dpi=(96x96) mapUnit=9
  origin=(0,-21600) scaleX=1 scaleY=1 visArea=(0,21600 23131x12361)
```

- PreRenderDevice 的 size、DPI、MapMode 均與視窗一致
- MapMode 已經是 doc twips，不需要再 SetMapMode

### 追加實驗：跳過 VirtualDevice 的 SetMapMode

```cpp
if (rRenderContext.GetOutDevType() != OUTDEV_VIRDEV)
    rRenderContext.SetMapMode(rDocMapMode);
```

**結果**：段落邊框正確顯示（紅色，位置正確），打字時仍消失。

分析：overlay 在 PreRenderDevice 上座標正確，但 `OutputPreRenderDevice`
flush 時似乎不包含我們畫的 overlay（或被後續操作覆蓋）。

---

## 實驗 G2：Hook 移到 DLPostPaint2 之後（畫到 window）

### 假說

`PaintSwFrame` 畫到 window，overlay 也應該畫到 window，
在 `OutputPreRenderDevice` flush 之後。

### 變更

```cpp
DLPostPaint2(true);  // EndDrawLayers + flush

// 在 flush 之後畫 overlay 到 window
if (m_fnPreEndDrawLayersHook)
    m_fnPreEndDrawLayersHook(rRenderContext);
```

### 結果

- Overlay 位置正確、可見
- **打字時仍消失**
- 原因：`ImpBufferTimerHandler`（游標閃爍）從 PreRenderDevice buffer
  恢復背景，overlay 不在 buffer 中

---

## 實驗 G3：兩邊都畫（PreRenderDevice + window）

### 假說

同時畫到 PreRenderDevice（讓 ImpSaveBackground 擷取）和
window（在 flush 後保持可見）。

### 變更

```cpp
// Before EndDrawLayers: 畫到 PreRenderDevice
if (m_fnPreEndDrawLayersHook && pPW->GetPreRenderDevice())
    m_fnPreEndDrawLayersHook(pPW->GetPreRenderDevice()->GetPreRenderDevice());

DLPostPaint2(true);

// After EndDrawLayers: 畫到 window
if (m_fnPreEndDrawLayersHook)
    m_fnPreEndDrawLayersHook(rRenderContext);
```

### 結果

- **仍然消失。** 與 G2 相同行為。
- PreRenderDevice 上的 overlay 似乎沒有被 ImpSaveBackground 正確擷取，
  或者被 EndDrawLayers 內部的其他操作覆蓋。

---

## 總結：所有嘗試過的方向

| 方向 | 策略 | Overlay 可見 | 打字時保持 | 問題 |
|------|------|:---:|:---:|------|
| F1 | Idle 直接畫到 window | 短暫 | ✗ | 下次 paint 覆蓋 |
| F2 | 三層診斷繪製 | 部分 | ✗ | partial paint 區域內消失 |
| F3 | 降低 Idle 優先級 | 短暫 | ✗ | 同 F1 |
| F4 | Hook 在 EndDrawLayers 前 | 短暫 | ✗ | PreRenderDevice flush 覆蓋 |
| G1 | Hook 畫到 PreRenderDevice | ✗（小點） | ✓ | 座標錯誤 |
| G1b | G1 + 跳過 SetMapMode | ✓ | ✗ | 打字觸發 repaint 後消失 |
| G2 | Hook 在 EndDrawLayers 後畫到 window | ✓ | ✗ | ImpBufferTimerHandler 覆蓋 |
| G3 | 兩邊都畫 | ✓ | ✗ | 同 G2 |

### 根本困境

VCL 的 `OverlayManagerBuffered` 設計為管理自己的 `OverlayObject` 實例。
任何在此系統外直接畫到 OutputDevice 的像素，都會在 background
save/restore 循環中被擦除。而 `PaintSwFrame`（文字內容）之所以不受影響，
是因為文字在每次 paint 時都被完整重繪。

### 可能的出路

1. **方向 G（VCL OverlayObject）**：建立 C++ `OverlayObject` 子類別，
   在 `createOverlayObjectPrimitive2DSequence()` 中呼叫 extension 的
   `paintOverlay()`，輸出為 `BitmapPrimitive2D`。這是唯一能正確存活於
   VCL overlay buffer 循環的方式。

2. **放棄即時 overlay，改用其他 UI 方式**：
   - 使用 Writer 內建的段落背景色 / 邊框屬性（透過 UNO 設定，不需要 overlay）
   - 使用 sidebar / tooltip / status bar 顯示段落資訊
   - 使用 annotation (comment) 機制標記段落
   - 使用 text field / bookmark 做視覺標記

3. **停用 PreRenderDevice / BufferedOverlay**：
   呼叫 `SetBufferedOutputAllowed(false)` 和 `SetBufferedOverlayAllowed(false)`，
   讓 VCL 不使用 PreRenderDevice 和 OverlayManagerBuffered。
   這可能影響效能和其他功能，但可以作為 extension 啟用時的 workaround。

---

## 啟動方式備忘

```bash
# 安裝 extension（首次或 profile 變更後）
/home/jiajun/LibreOffice/LO-core/instdir/program/unopkg add --force \
  ~/LibreOffice/markdown-insert-ext/markdown-insert.oxt

# 啟動測試
pkill -f soffice; sleep 2
SAL_LOG='+WARN.sw.uno' \
MD_SIDEBAR_OVERLAY_DEMO=1 \
MD_SIDEBAR_SEED_PARAGRAPH_NAV_SAMPLE=1 \
MD_SIDEBAR_VALIDATE_OVERLAY=1 \
/home/jiajun/LibreOffice/LO-core/instdir/program/soffice --writer 2>/tmp/lo-overlay-diag.log
```

注意：不要使用 `-env:UserInstallation=`，目前 dev build 的 bootstraprc
不支援此參數（會報 "bootstraprc is corrupt"）。直接使用預設 profile。
