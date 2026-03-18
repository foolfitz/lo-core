# Phase 5: Overlay PaintBuffer 開發規格書

> 版本：0.1
> 日期：2026-03-19
> 狀態：Draft
> 前置需求：Phase 4 已完成 XDocumentOverlay（overlay paint callback lifecycle）
> 分支：ext-uno-api-26-2-1

---

## 1. 目標

為 Writer 的 extension overlay 繪製系統引入**獨立的 VirtualDevice paint buffer**，讓 overlay 內容不再與文件內容共享同一個繪製表面。

本 phase 要解決的問題是：

Phase 4 的 overlay 繪製直接畫在 `SwEditWin` 的 `RenderContext` 上。當 Writer 對文件進行局部重繪（例如游標移動、文字輸入）時，只有被 invalidate 的區域會觸發 `Paint()`，而 overlay 在未被 invalidate 的區域中的繪製結果會被 clip 掉，導致 overlay **暫時消失**。

### 1.1 具體失效場景

以下場景在 Phase 4 中被觀察到：

1. **游標移動**：Writer 只 invalidate 游標的舊位置和新位置附近的小矩形。如果 overlay 繪製在其他段落上，該區域不在 invalidation 範圍內，`paintOverlay()` 的繪製結果被 VCL clip 掉，overlay 視覺上消失
2. **文字輸入**：Writer invalidate 編輯行附近的區域。其他行上的 overlay 同樣被 clip 掉
3. **選取範圍變更**：選取高亮的 invalidation 可能只覆蓋部分 overlay 區域

在這些情境中，overlay 的消失是暫時的——下一次整頁 repaint（例如捲動、視窗切換）會讓 overlay 重新出現。但這種閃爍行為對使用者體驗來說是不可接受的。

### 1.2 Phase 4 的 workaround 及其局限

Phase 4 的 code review 中引入了一個 workaround：在 `CallOverlayPainters()` 中，extension 可以呼叫 `invalidateOverlay()` 強制擴大 invalidation 範圍。但這個方法有兩個根本問題：

1. **效能**：每次任何局部重繪都變成整個可見區域的全量重繪，抵銷了 VCL 局部 invalidation 的效能優勢
2. **架構**：overlay 和文件內容的繪製依然耦合——overlay 的正確性取決於 invalidation 範圍是否恰好覆蓋了所有 overlay 區域

### 1.3 本 phase 的最小有用成果

overlay 繪製結果存在於獨立的 buffer 中，文件的局部重繪不會清除 overlay 內容。`paintOverlay()` 仍然維持既有的 repaint callback 契約，但改為先畫到 buffer，再統一 composit 到畫面。

### 1.4 核心假設

- VCL 的 `VirtualDevice` 可以作為 overlay 的獨立繪製目標
- `VirtualDevice` 的內容可以在每次 `SwEditWin::Paint()` 後被 composit 到 `RenderContext` 上
- 這個 compositing 步驟不會破壞既有 UNO repaint callback contract，且其額外開銷可接受

---

## 2. 背景與先例

### 2.1 Phase 4 的現狀

Phase 4 的 overlay 繪製插入在 `SwEditWin::Paint()` 的末段：

```
SwEditWin::Paint(rRenderContext, rRect)
  → pWrtShell->setOutputToWindow(true)
  → pWrtShell->Paint(rRenderContext, rRect)
      → DLPrePaint2()
      → PaintDesktop()
      → GetLayout()->PaintSwFrame()     ← 文件內容
      → DLPostPaint2()                  ← 內部 overlay (selection etc.)
  → [extension overlay painting]         ← Phase 4 新增
  → pWrtShell->setOutputToWindow(false)
```

extension overlay 直接畫在 `rRenderContext` 上。由於 `rRenderContext` 的 clip region 被設為 `rRect`（本次 invalidation 的範圍），任何在 `rRect` 之外的 overlay 繪製都會被 clip 掉。

### 2.2 VCL VirtualDevice 作為 buffer

VCL 提供 `VirtualDevice`（`include/vcl/virdev.hxx`），這是一個不依附於任何實體視窗的 `OutputDevice`：

- 支援 `DeviceFormat::WITHOUT_ALPHA`（不透明）和 `DeviceFormat::WITH_ALPHA`（含 alpha 通道）
- 可設定任意大小（`SetOutputSizePixel()`）
- 可設定 MapMode（與 `SwEditWin` 的文件座標系一致）
- 內容在兩次繪製之間**持久保留**
- 可透過 `OutputDevice::DrawOutDev()` 將內容 blit 到其他 `OutputDevice`

`VirtualDevice` 在 LibreOffice 中被廣泛用作 off-screen buffer：

- `sw/source/uibase/docvw/AnnotationWin2.cxx`：sidebar annotation 使用 `ScopedVclPtrInstance<VirtualDevice>` 進行 off-screen 繪製
- `sw/source/uibase/sidebar/StylePresetsPanel.cxx`：樣式預覽使用 VirtualDevice
- `sdr::overlay::OverlayManager`：SVX 的 overlay 系統雖然不直接使用 buffer，但 `completeRedraw()` 接受 `pPreRenderDevice` 參數，顯示此模式已被考慮

### 2.3 DrawOutDev compositing

`OutputDevice::DrawOutDev()` 提供了 device-to-device blit：

```cpp
virtual void DrawOutDev(
    const Point& rDestPt, const Size& rDestSize,
    const Point& rSrcPt,  const Size& rSrcSize,
    const OutputDevice& rOutDev );
```

這可以將 `VirtualDevice` 的內容一次性 blit 到 `SwEditWin` 的 `RenderContext` 上，效能遠優於逐一呼叫每個 overlay painter。

### 2.4 alpha compositing 的可能性

`VirtualDevice` 支援 `DeviceFormat::WITH_ALPHA`。如果未來 overlay 需要半透明效果，alpha VirtualDevice 結合 `DrawTransparent()` 或 alpha-aware `DrawOutDev()` 可以提供支援。

但 Phase 5 暫不處理 alpha 透明度——先以不透明 buffer 驗證獨立 buffer 架構的可行性。

---

## 3. 非目標

本 phase 不處理以下事項：

1. **Alpha 透明度 compositing**：overlay buffer 使用不透明模式。半透明 overlay 留待後續擴充
2. **XGraphics 層級的 alpha API**：不新增 `setFillAlpha()` 或 alpha 混合方法
3. **overlay 互動事件**：click / hover / drag — 不在本 phase scope
4. **非 Writer 應用**：Calc / Draw / Impress 的獨立 buffer — 本 phase 只在 Writer 驗證，但設計時會考慮可移植性
5. **多 buffer 層級**：本 phase 使用單一 overlay buffer。多層 buffer（如分 background overlay 和 foreground overlay）留待需求出現時再評估
6. **buffer 大小自適應策略**：本 phase 的 buffer 跟隨 `SwEditWin` client area 大小。更精細的策略（如只分配可見區域大小、tile-based buffer）留待效能分析後決定
7. **overlay 動畫**：timer-based update、fade in/out

---

## 4. Public Contract

### 4.1 published UNO API 變更

Phase 5 的核心變更在 VCL / sw 內部實作層。**`XDocumentOverlay` 和 `XOverlayPainter` 的 published IDL 不需要修改。**

Extension 端的使用方式不變：

```python
handle = overlay.addOverlay(painter, 0)
overlay.invalidateOverlay(empty_rect)
# paintOverlay() 被呼叫時，座標系與 XGraphics 行為與 Phase 4 完全一致
```

這是刻意的設計決策：Phase 5 是一個**純內部實作改進**，外部 contract 保持穩定。Extension 升級到含 Phase 5 的 LO build 後，唯一可觀察的行為差異是「overlay 不再閃爍」。

### 4.2 語義調整（不改 IDL，但內部路徑改變）

以下行為在 Phase 5 之後會有變化。這些調整都應保持 published UNO contract 穩定：

#### `paintOverlay()` 呼叫時機

| | Phase 4 | Phase 5 |
|---|---|---|
| 文件局部重繪時 | 每次 `Paint()` 都呼叫所有 visible painter | 每次 `Paint()` 仍呼叫 visible painter，但改畫到 overlay buffer |
| `invalidateOverlay()` 後 | 排程下次 `Paint()` 呼叫 | 排程下次 `Paint()`；下次 paint 會重繪 buffer 並 composit |
| 捲動/縮放時 | 每次 repaint 重新繪製 | 每次 repaint 重新繪製 buffer，MapMode 由 view 自動同步 |

注意：Phase 4 IDL 對 `paintOverlay()` 呼叫頻率的描述是「during every **view** repaint」。Phase 5 必須保留這個語義；改變的是繪製目標與 compositing 路徑，而不是 callback 是否觸發。

#### `invalidateOverlay()` 語義

Phase 4：invalidate 指定文件區域，觸發 `SwEditWin` 重繪，進而呼叫所有 painter。
Phase 5：invalidate 指定文件區域，觸發 `SwEditWin` 對應區域重繪；下次 `Paint()` 會重繪 overlay buffer，然後 composit 到畫面。

對 extension 來說，效果相同（指定區域的 overlay 被更新），但內部路徑不同。

#### `XGraphics` 的 OutputDevice

Phase 4：`paintOverlay()` 收到的 `XGraphics` 指向 `SwEditWin` 的 `RenderContext`。
Phase 5：`paintOverlay()` 收到的 `XGraphics` 指向 overlay `VirtualDevice`。

由於 IDL 只承諾「a graphics context for drawing, with MapMode set to document coordinate space (twips)」，不承諾底層 OutputDevice 的類型，這個變更不破壞 contract。

### 4.3 未來 IDL 擴充的預留空間

雖然 Phase 5 不修改 IDL，但未來可能有以下擴充方向。Phase 5 的內部設計應不阻塞這些方向：

| 可能的未來擴充 | 說明 |
|---------------|------|
| `addOverlay()` 增加 `Flags` 參數 | 控制 buffer 模式（如 `ALPHA_BUFFER`）|
| `setOverlayAlpha(handle, percent)` | 控制整個 overlay 的透明度 |
| `XDocumentOverlay2` 或 optional 擴充 | 新增 buffer 控制方法 |

Phase 5 的內部架構應讓這些擴充在不破壞現有 extension 的前提下可行。

---

## 5. 實作形狀

### 5.1 架構概覽

```
SwEditWin::Paint(rRenderContext, rRect)
  → pWrtShell->setOutputToWindow(true)
  → pWrtShell->Paint(rRenderContext, rRect)
      → document content + internal overlays
  → [Phase 5: composit overlay buffer onto rRenderContext]
  → pWrtShell->setOutputToWindow(false)
```

overlay buffer 的生命週期：

```
SwXTextView 建構
  → (初始不建立 buffer)

首次 addOverlay()
  → 建立 VirtualDevice (lazy init)
  → 設定大小、MapMode

SwEditWin::Paint()
  → 如果 overlay buffer 存在且非空
    → 清除 buffer 並呼叫 visible painter 重繪可見 overlay
    → 將 buffer composit 到 rRenderContext

SwEditWin resize
  → 調整 buffer 大小；下次 paint 會以新的大小重繪

invalidateOverlay(Area)
  → Invalidate SwEditWin 中對應區域
  → 下次 paint 會重繪 overlay buffer 並 composit

removeOverlay() / setOverlayVisible()
  → Invalidate SwEditWin

view dispose
  → 釋放 VirtualDevice
```

### 5.2 核心資料結構

在 `SwXTextView` 中新增：

```cpp
// Overlay paint buffer (Phase 5)
VclPtr<VirtualDevice>       m_pOverlayBuffer;
MapMode                    m_aOverlayBufferMapMode;
Size                       m_aOverlayBufferSize;
```

Phase 5 保留 buffer 的 persistent lifetime，但為了維持 published repaint callback contract，不做「clean buffer 就跳過 painter」這種語義變更。buffer 主要解決的是 clip/compositing 問題，而不是 callback 頻率問題。

### 5.3 VirtualDevice 的建立與管理

```cpp
void SwXTextView::EnsureOverlayBuffer()
{
    if (m_pOverlayBuffer)
        return;

    SwEditWin& rEditWin = m_pView->GetEditWin();
    Size aPixelSize = rEditWin.GetOutputSizePixel();
    if (aPixelSize.Width() <= 0 || aPixelSize.Height() <= 0)
        return;

    m_pOverlayBuffer = VclPtr<VirtualDevice>::Create(
        rEditWin, DeviceFormat::WITHOUT_ALPHA);
    m_pOverlayBuffer->SetOutputSizePixel(aPixelSize);

    // 與 SwEditWin 使用相同的 MapMode（document twips）
    m_pOverlayBuffer->SetMapMode(/* document map mode */);

}
```

#### Lazy 初始化

- buffer 只在第一次 `addOverlay()` 時建立，不是在 `SwXTextView` 建構時
- 最後一個 overlay 被 `removeOverlay()` 後，可選擇釋放 buffer（節省記憶體）或保留（避免重建成本）。建議 Phase 5 先採用釋放策略，因為 overlay 少的場景佔多數

#### Resize 處理

- `SwEditWin` 的大小變更需要同步到 overlay buffer
- 偵測方式：在 `SwEditWin::Paint()` 的 compositing 階段，比對 buffer 大小與 `SwEditWin` 大小，若不同則重建 buffer

### 5.4 Buffer 重繪

```cpp
void SwXTextView::RepaintOverlayBuffer(const css::awt::Rectangle& rVisibleArea)
{
    if (!m_pOverlayBuffer)
        return;

    // 每次 repaint 都清除並重繪 buffer，保持與既有 UNO callback
    // 語義一致
    m_pOverlayBuffer->Erase();

    // 建立指向 buffer 的 XGraphics
    uno::Reference<awt::XGraphics> xGraphics
        = m_pOverlayBuffer->CreateUnoGraphics();
    if (!xGraphics.is())
        return;

    // 呼叫 painter（與 Phase 4 的 CallOverlayPainters 相同邏輯）
    CallOverlayPaintersIntoBuffer(xGraphics, rVisibleArea);
}
```

### 5.5 Compositing

在 `SwEditWin::Paint()` 中，替換 Phase 4 的直接繪製為 compositing：

```cpp
// Phase 5: composit overlay buffer onto rRenderContext
{
    SwXTextView* pTextView = dynamic_cast<SwXTextView*>(
        GetView().GetController().get());
    if (pTextView && pTextView->HasOverlays()
        && rRenderContext.GetOutDevType() != OUTDEV_PRINTER
        && rRenderContext.GetOutDevType() != OUTDEV_PDF)
    {
        const tools::Rectangle& rVisArea = GetView().GetVisArea();
        css::awt::Rectangle aVisArea(
            rVisArea.Left(), rVisArea.Top(),
            rVisArea.GetWidth(), rVisArea.GetHeight());

        // 確保 buffer 是最新的
        pTextView->RepaintOverlayBuffer(aVisArea);

        // composit: blit buffer 到 rRenderContext
        pTextView->CompositOverlayBuffer(rRenderContext);
    }
}
```

Compositing 的關鍵：**不受 `rRect` clip region 限制**。overlay buffer 的 blit 必須覆蓋整個可見區域，而非只覆蓋本次 invalidation 的 `rRect`。

```cpp
void SwXTextView::CompositOverlayBuffer(vcl::RenderContext& rRenderContext)
{
    if (!m_pOverlayBuffer)
        return;

    // 暫時移除 clip region，讓 overlay 完整呈現
    auto scopedPush = rRenderContext.ScopedPush(vcl::PushFlags::CLIPREGION);
    rRenderContext.SetClipRegion();  // 清除 clip

    Size aSize = m_pOverlayBuffer->GetOutputSizePixel();
    rRenderContext.DrawOutDev(
        Point(0, 0), aSize,
        Point(0, 0), aSize,
        *m_pOverlayBuffer);
}
```

**注意**：這裡的 `DrawOutDev` 是在 pixel 空間操作（buffer 和 SwEditWin 都是 pixel 空間的 blit），但 buffer 內部的繪製使用 document twips MapMode。這個兩層 MapMode 需要仔細管理。

### 5.6 捲動最佳化（可選）

捲動時，overlay buffer 的大部分內容只是平移。可以用 `CopyArea()` 或 `DrawOutDev()` 將 buffer 內容平移，只重繪新曝露的邊緣區域。

此最佳化在 Phase 5 中為**可選**。若不做，捲動時仍沿用既有 repaint callback 路徑、整個 buffer 重新繪製，效能是否需要再優化留待量測後決定。

### 5.7 Erase 策略：背景色問題

overlay buffer 的「透明」區域（overlay 沒有繪製的地方）需要在 composit 時**不遮擋文件內容**。

Phase 5（不透明 buffer）有兩種處理方式：

**方式 A：只繪製有 overlay 的區域**

- composit 時不 blit 整個 buffer，而是只 blit overlay 覆蓋的矩形列表
- 需要維護 overlay 的 bounding rect 列表
- 複雜度較高，但不遮擋文件

**方式 B：使用 WITH_ALPHA VirtualDevice**

- buffer 使用 `DeviceFormat::WITH_ALPHA`
- buffer 清除為全透明
- composit 時用 alpha-aware blit
- 簡單優雅，但 `DrawOutDev` 的 alpha 支援在不同平台上可能不一致

**建議**：Phase 5 優先嘗試方式 B（alpha VirtualDevice）。若平台相容性有問題，退回方式 A。

> **風險標記**：這是本 spec 中最需要 prototype 驗證的部分。在正式實作前，應先寫一個最小 prototype 確認 `DeviceFormat::WITH_ALPHA` + `DrawOutDev` 在 Linux/Windows/macOS 上的行為。

### 5.8 建議修改檔案

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 編輯 | `sw/source/uibase/inc/unotxvw.hxx` | 新增 buffer 成員、RepaintOverlayBuffer、CompositOverlayBuffer |
| 2 | 編輯 | `sw/source/uibase/uno/unotxvw.cxx` | buffer 管理、每次 repaint 的 buffer 重繪邏輯 |
| 3 | 編輯 | `sw/source/uibase/docvw/edtwin2.cxx` | `SwEditWin::Paint()` 改為 compositing 模式 |
| 4 | 不變 | `offapi/com/sun/star/text/XDocumentOverlay.idl` | IDL 不修改 |
| 5 | 不變 | `offapi/com/sun/star/text/XOverlayPainter.idl` | IDL 不修改 |
| 6 | 編輯 | `sw/qa/uibase/uno/uno.cxx` | 新增或修改 overlay 測試，驗證 buffer 行為 |

### 5.9 與 Phase 4 的差異摘要

| 面向 | Phase 4 | Phase 5 |
|------|---------|---------|
| 繪製目標 | `SwEditWin` RenderContext | overlay VirtualDevice |
| clip 影響 | 受 `rRect` clip | 不受 clip（buffer 獨立） |
| painter 呼叫頻率 | 每次 `Paint()` | 每次 `Paint()`（維持 UNO contract） |
| compositing | 無（直接繪製） | `DrawOutDev` blit |
| 記憶體 | 無額外記憶體 | VirtualDevice buffer |
| IDL | Phase 4 IDL | 不變 |

---

## 6. 跨應用擴展性考量

### 6.1 設計原則

Phase 5 的實作應遵循以下原則，以便未來擴展到其他 LO 應用：

1. **buffer 管理邏輯與 Writer 解耦**：buffer 的建立、MapMode 同步、compositing 邏輯應可抽取為獨立的 helper class，不直接依賴 `SwXTextView` 或 `SwEditWin` 的特定 API
2. **paint 插入點抽象化**：每個應用的 `Paint()` 流程不同，但「在文件繪製之後 composit overlay buffer」這個步驟的邏輯是通用的
3. **IDL 保持應用中立**：`XDocumentOverlay` 和 `XOverlayPainter` 的 IDL 已經不含 Writer 特有的概念（如 paragraph、page），可直接用於其他應用

### 6.2 各應用的 paint 插入點

| 應用 | 編輯視窗 | Paint 流程 | overlay 插入點 |
|------|----------|-----------|----------------|
| Writer | `SwEditWin` | `SwEditWin::Paint()` → `pWrtShell->Paint()` | 在 `pWrtShell->Paint()` 之後 |
| Impress | `sd::Window` | `sd::Window::Paint()` → view paint | 在 view paint 之後 |
| Calc | `ScGridWindow` | `ScGridWindow::Paint()` → cell rendering | 在 cell rendering 之後 |
| Draw | `sd::Window`（共用 Impress） | 同 Impress | 同 Impress |

### 6.3 建議的 helper class 方向

```cpp
// 可移植的 overlay buffer helper（方向性草案，不是 Phase 5 必須實作）
class OverlayBufferHelper
{
    VclPtr<VirtualDevice> m_pBuffer;
    bool m_bFullDirty = true;
    tools::Rectangle m_aDirtyRect;

public:
    void EnsureBuffer(OutputDevice& rRefDevice, const Size& rPixelSize);
    void MarkFullDirty();
    void MarkDirty(const tools::Rectangle& rRect);
    bool IsDirty() const;
    void Repaint(/* painter callback */);
    void Composit(vcl::RenderContext& rTarget);
    void Dispose();
};
```

Phase 5 可以先將這些邏輯直接寫在 `SwXTextView` 中，待 Impress / Calc 也需要時再抽取為共用 helper。

### 6.4 Impress 播放模式的特殊考量

Impress 在播放模式下的繪製路徑與編輯模式不同。overlay 在播放模式下有獨立的應用場景（即時投票、字幕），但 paint 插入點和 buffer 管理需要額外設計。這不在 Phase 5 scope 內，但記錄在此以備後續。

---

## 7. 驗證計畫

### 7.1 Phase 5 的 stage hypothesis

**Hypothesis**
Writer 可以使用 VirtualDevice 作為 overlay 的獨立 paint buffer，消除局部重繪導致的 overlay 閃爍，且不破壞現有 overlay UNO contract。

**Entry condition**
- Phase 4 已提供可工作的 overlay paint callback lifecycle
- overlay 閃爍問題已被觀察並記錄

**Fallback**
- 若 `VirtualDevice` + `DrawOutDev` compositing 在某些平台上表現不佳，退回 Phase 4 的直接繪製模式
- Phase 4 的 UNO contract 不受影響——fallback 只影響內部實作

**Exit note**
- Phase 5 完成代表 overlay 在 Writer 中具有穩定的視覺呈現
- 不代表 alpha 透明度問題已完全解決
- 不代表 overlay 已擴展到其他 LO 應用

### 7.2 前置 prototype

在正式實作前，建議先驗證以下關鍵假設：

```cpp
// 最小 prototype：在 SwEditWin::Paint() 尾端
// 建立一個 VirtualDevice、畫一個紅色矩形、blit 到 rRenderContext
// 確認：
// 1. 紅色矩形在文件局部重繪時不消失
// 2. 紅色矩形在捲動時正確移動
// 3. DeviceFormat::WITH_ALPHA 的 VirtualDevice 是否能正確 composit
```

此 prototype 不涉及 UNO，純粹驗證 VCL buffer compositing 的可行性。

### 7.3 自動化驗證

建議最小 build / test 組合：

```bash
make sw
make CppunitTest_sw_uibase_uno
```

#### 建議新增或修改的 CppUnit 驗證點

放置方向：`sw/qa/uibase/uno/uno.cxx`

| # | 驗證點 | 說明 |
|:-:|--------|------|
| 1 | 既有 overlay 測試不回歸 | Phase 4 的所有 overlay 測試必須繼續通過 |
| 2 | `paintOverlay()` 仍在每次 repaint 被呼叫 | 連續兩次 `PaintImmediately()` 之間無 `invalidateOverlay()`，painter 的 call_count 仍應增加 |
| 3 | compositing 不受局部 repaint clip 影響 | 部分 invalidate 後，可視區右下角 overlay 仍能在畫面上讀到 |
| 4 | zoom / resize 後 compositing 幾何正確 | 模擬縮放或大小變更後，overlay 位置與尺寸仍正確 |

### 7.4 extension-side 驗證

使用現有的 `markdown-insert-ext` 驗證環境：

```bash
MD_SIDEBAR_OVERLAY_DEMO=1 \
MD_SIDEBAR_SEED_PARAGRAPH_NAV_SAMPLE=1 \
soffice --writer
```

驗證項目：

1. **游標移動不閃爍**：在段落間移動游標，overlay 邊框持續可見
2. **文字輸入不閃爍**：在文件中打字，其他段落上的 overlay 不消失
3. **捲動正確**：overlay 隨捲動正確移動
4. **縮放正確**：zoom in/out 後 overlay 大小和位置正確
5. **可見性切換**：`setOverlayVisible()` 仍然正常

### 7.5 手動驗證

| # | 情境 | 預期結果 |
|:-:|------|----------|
| 1 | 游標在段落間移動 | overlay 不閃爍、不消失 |
| 2 | 在文件中間打字 | 其他位置的 overlay 持續可見 |
| 3 | 選取文字範圍 | overlay 不被選取高亮覆蓋 |
| 4 | 快速捲動 | overlay 流暢跟隨 |
| 5 | 縮放 50%→200% | overlay 大小正確更新 |
| 6 | 列印 | overlay 不出現在列印輸出 |
| 7 | 開啟 10 個 overlay | 效能無明顯下降 |
| 8 | 視窗大小調整 | overlay 正確重繪 |

---

## 8. 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | Phase 4 的所有 overlay CppUnit 測試繼續通過 | `make CppunitTest_sw_uibase_uno` |
| 2 | 游標移動時 overlay 不閃爍 | 手動驗證 + extension demo |
| 3 | 文字輸入時 overlay 不閃爍 | 手動驗證 + extension demo |
| 4 | `paintOverlay()` 仍符合 published repaint callback contract | CppUnit (call_count) |
| 5 | `invalidateOverlay()` 與局部 repaint 下 compositing 仍正確 | CppUnit + extension demo |
| 6 | overlay 不出現在列印輸出 | 手動驗證 |
| 7 | `XDocumentOverlay` IDL 未修改 | diff 確認 |
| 8 | view dispose 不崩潰、buffer 正確釋放 | CppUnit |
| 9 | 在 Linux 上 compositing 行為正確 | 手動驗證 |

### 8.1 驗收責任切分

| 驗收項 | 主要關帳位置 | 原因 |
|--------|--------------|------|
| `#1` 測試不回歸 | `LO-core` | 屬於 public contract 正確性 |
| `#2` `#3` 不閃爍 | extension 為主 | 真正可驗的是使用者視角行為 |
| `#4` repaint callback 語義 | `LO-core` | 屬於 published UNO contract |
| `#5` invalidateOverlay | `LO-core` + extension | core 關帳排程，extension 關帳效果 |
| `#6` 列印排除 | `LO-core` | buffer composit 不應出現在 print path |
| `#7` IDL 不變 | diff | 相容性保證 |
| `#8` lifecycle | `LO-core` | buffer 記憶體管理 |

---

## 9. 風險與後續

| 風險 | 說明 | 緩解 |
|------|------|------|
| `DeviceFormat::WITH_ALPHA` 平台一致性 | alpha VirtualDevice 在 svp (headless)、X11、Wayland、Windows 上的行為可能不同 | 先做 prototype 驗證；準備不透明 fallback（方式 A） |
| 記憶體消耗 | overlay buffer 佔用額外的 pixel buffer 記憶體（與 SwEditWin 大小相同） | lazy init；最後一個 overlay 移除後釋放 buffer |
| compositing 效能 | 每次 `Paint()` 都有一次 `DrawOutDev` blit | blit 通常是 GPU 加速的 memcpy，效能影響極小；可量測 |
| clip region 管理 | composit 時需暫時移除 clip region | 使用 `ScopedPush(PushFlags::CLIPREGION)` 保護 |
| MapMode 一致性 | buffer 的 MapMode 與 `DrawOutDev()` source units 必須正確對齊 | compositing 時以 source device 的 logical size 做 blit，並在每次 paint 前同步 MapMode |
| 捲動時全量重繪 | callback contract 要求每次 repaint 都呼叫 painter，捲動時無法跳過重繪 | Phase 5 先接受；若要改 callback 頻率需另行設計 API/contract |

### 9.1 Deferred Work

以下議題不在本 phase 解決：

- **Alpha 透明度完整支援**：若 `DeviceFormat::WITH_ALPHA` + `DrawOutDev` 不可行，需要在 VCL 層級補充 alpha compositing API
- **捲動最佳化**：buffer 內容平移 + 邊緣重繪
- **多 buffer 層級**：為不同 Layer 的 overlay 分配獨立 buffer
- **非 Writer 應用的 overlay buffer**：Impress / Calc / Draw
- **overlay 互動事件**：Phase 6 或後續 phase
- **OverlayBufferHelper 抽取**：待第二個應用（如 Impress）也需要 buffer 時再進行

換句話說，**Phase 5 完成後，overlay 應該在 Writer 中視覺穩定（不閃爍）；跨應用支援、alpha 透明度、互動事件都留待後續。**
