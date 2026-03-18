# 方向 F 實驗記錄（2026-03-23）

> 前置文件：`phase5-overlay-idle-direct-draw-spec.md`
> 分支：ext-uno-api-26-2-1

---

## 實驗目標

實作方向 F（Idle Direct Draw）：在 Idle handler 中直接繪製到 OutputDevice，
不呼叫 `Invalidate()`，照搬 `OverlayManagerBuffered` 的策略。

---

## 實驗 F1：基本 Idle Direct Draw

**變更**：
- `OverlayRepaintIdleHdl`：從 `Invalidate()` 改為直接在 `rEditWin.GetOutDev()`
  上透過 `CreateUnoGraphics()` + `CallOverlayPainters()` 繪製 overlay
- 移除捲動偵測（`m_aLastOverlayVisibleArea`）
- 優先級：`TaskPriority::POST_PAINT`

**結果**：overlay 打字時消失

**診斷 log**：
- Idle 正確觸發（每次 partial paint 後都觸發）
- `inPaint=0`（確認在 Paint() 外）
- `dblBuf=0`（無 double buffering）
- `xGraphics=1`（成功取得 XGraphics）
- Flush 完成

**結論**：Idle handler 執行正確，但繪製的像素不可見或被覆蓋。

---

## 實驗 F2：三層診斷繪製

在 Idle handler 中加入三個 debug 矩形：

| 測試 | 方法 | 座標系 | 位置 | 結果 |
|------|------|--------|------|------|
| A（紅） | `OutputDevice::DrawRect` | pixel (50,50) | 頁面邊緣 | **穩定可見** |
| B（藍） | `XGraphics::drawLine` | pixel (300,50) | 頁面邊緣 | **穩定可見**（重複出現） |
| C（綠） | `XGraphics::drawLine` | doc-twips (visArea+100) | 文件區域 | **可見** |
| overlay | `CallOverlayPainters` | doc-twips | 段落區域 | **消失** |

**關鍵發現**：
1. OutputDevice 直接繪製在 Paint() 外有效（紅色矩形證實）
2. XGraphics 在 Paint() 外也有效（藍色、綠色矩形證實）
3. debug 矩形在固定像素位置（頁面邊緣），**不在** partial paint 覆蓋範圍
4. overlay 邊框在文字段落區域，**在** partial paint 覆蓋範圍

---

## 實驗 F3：降低 Idle 優先級

**假說**：VCL 的 `OverlayManagerBuffered` 也使用 POST_PAINT Idle，
在我們之後觸發時恢復「背景」覆蓋我們的 overlay。

**變更**：Idle 優先級從 `POST_PAINT` 降為 `DEFAULT_IDLE`

**結果**：overlay 仍然消失。降低優先級無效。

---

## 實驗 F4：Paint 順序調整（PaintOverlays 移至 EndDrawLayers 前）

**根因分析**（基於 VCL 源碼研究）：

Paint 順序：
```
SwViewShell::Paint():
  DLPrePaint2() → BeginDrawLayers()
  PaintDesktop()
  PaintSwFrame()                    ← 內容繪製
  DLPostPaint2() → EndDrawLayers() ← VCL OverlayManagerBuffered 儲存背景
                                      （此時沒有我們的 overlay！）
SwEditWin::Paint():
  PaintOverlays()                   ← 我們的 overlay（太晚，背景已儲存）
```

VCL 的 `OverlayManagerBuffered::completeRedraw()`（在 EndDrawLayers 中）：
1. `ImpSaveBackground`：從視窗複製像素到 `mpBufferDevice`（此時沒有 overlay）
2. `ImpDrawMembers`：在視窗上畫 VCL overlay（游標等）

之後游標閃爍時，`ImpBufferTimerHandler` 恢復「背景」→ 我們的 overlay 被擦掉。

**解法嘗試**：
1. 在 `SwViewShell` 加 `m_fnPreEndDrawLayersHook`（`std::function<void(RenderContext&)>`）
2. 在 `edtwin2.cxx` 中設定 hook，讓 PaintOverlays 在 `Paint()` 呼叫前注入
3. 在 `viewsh.cxx` 的 `PaintSwFrame` 和 `DLPostPaint2` 之間呼叫 hook

**第一版**：hook 在 `DLPostPaint2` 中呼叫，使用 `mpPrePostOutDev`
- 結果：失敗（可能 `mpPrePostOutDev` ≠ `rRenderContext`）

**第二版**：hook 在 `SwViewShell::Paint()` 中呼叫，使用 `rRenderContext`
- 結果：仍然失敗。overlay 邊框打字時消失。

---

## VCL 研究摘要

### VCLXGraphics clip 行為（非問題）

`VCLXGraphics::InitOutputDevice()` 在每次 draw 前呼叫 `SetClipRegion()`
（無參數 → 清除 clip）。之後 `InitClipRegion()` 在 `mbInPaint=false` 時
使用 `ImplGetWinChildClipRegion()`（整個視窗 clip）。

結論：XGraphics 在 Paint() 外的 clip 正確，不是問題。

### 已確認的事實

1. OutputDevice 直接繪製在 Paint() 外有效
2. XGraphics 透過 OutputDevice 在 Paint() 外有效
3. 繪製的像素在 partial paint 不涵蓋的區域可以持久存在
4. 繪製的像素在 partial paint 涵蓋的區域會被下次 paint 覆蓋
5. `SupportsDoubleBuffering()` = false
6. Idle handler 正確觸發且不產生無限循環
7. 捲動正常（移除捲動偵測後滾輪/捲軸功能正常）

### 未解之謎

overlay 邊框透過 `CallOverlayPainters` + XGraphics 在 Idle 中繪製，
所有條件都正確（clip、MapMode、XGraphics 有效），但視覺上看不到效果。
即使把 PaintOverlays 移到 EndDrawLayers 前（理論上應被包含在 VCL 背景 buffer），
overlay 仍然消失。

**可能方向**：
- VCL 的 `PaintBufferGuard` 可能在 frame level 有額外的 buffer 機制
- `OverlayManagerBuffered::completeRedraw` 的 `ImpSaveBackground` 可能在
  PaintOverlays 繪製區域之外操作
- overlay painter（Python extension）的繪製可能在某些條件下無效
- 需要更深入研究 `SdrPaintWindow` 和 `PaintBufferGuard` 的 buffer 生命週期

---

## 目前代碼狀態

已修改的檔案：

| 檔案 | 變更 |
|------|------|
| `sw/inc/viewsh.hxx` | 加 `#include <functional>`、`m_fnPreEndDrawLayersHook` 成員、`SetPreEndDrawLayersHook()` |
| `sw/source/core/view/viewsh.cxx` | 在 `PaintSwFrame` 和 `DLPostPaint2` 之間呼叫 hook |
| `sw/source/uibase/inc/unotxvw.hxx` | 移除 `m_aLastOverlayVisibleArea`，更新註解 |
| `sw/source/uibase/uno/unotxvw.cxx` | 簡化 PaintOverlays Step2、Idle handler 改為直接繪製 |
| `sw/source/uibase/docvw/edtwin2.cxx` | PaintOverlays 改用 hook 注入 |

CppUnit 測試 `sw_uibase_uno` 通過。
