# Phase 4: XDocumentOverlay 開發規格書

> 版本：0.1
> 日期：2026-03-18
> 狀態：Draft
> 前置需求：Phase 3 已完成 XParagraphNavigator（paragraph geometry snapshot contract）
> 分支：ext-uno-api-26-2-1

---

## 1. 目標

在 Writer 的 `TextDocumentView` 上新增一個 **view-bound 的文件 overlay 繪製 API**，讓 extension 能在 Writer 編輯區域的文件內容上方疊加繪製，而不修改文件內容。

本 phase 要解決的最小問題是：

1. extension 能向 Writer view 註冊一個 overlay painter callback
2. Writer 在每次重繪時，於文件內容繪製完成後，呼叫已註冊的 painter
3. painter 接收一個可繪製的 `XGraphics` 和目前可見區域資訊，在文件座標系中繪製
4. extension 能控制 overlay 的可見性、觸發重繪、以及移除 overlay

### 1.1 本 phase 要回答的核心假設

Phase 4 的真正驗證問題是：

- Writer 的 `SwEditWin::Paint()` 是否能在不破壞現有繪製正確性的前提下，新增一個 UNO callback 階段？
- UNO cross-boundary 的 paint callback 是否在效能上可接受？
- extension 能否以 Phase 3 的 paragraph geometry 搭配 Phase 4 的 overlay，完成段落高亮標籤等視覺功能？

也就是說，Phase 4 是 **overlay paint lifecycle** 的驗證 phase。

### 1.2 使用情境

主要使用情境（以 `markdown-insert-ext` 為例）：

1. **段落高亮**：在目前選取的段落上繪製半透明背景色
2. **段落標籤**：在段落旁繪製樣式名稱或狀態標籤
3. **範圍標記**：在多個段落上繪製括號或邊框，標示選取範圍

這些繪製都需要：
- 知道段落的文件座標（Phase 3 已提供）
- 在文件座標系中繪製（Phase 4 提供）
- 隨捲動 / 縮放自動更新（VCL MapMode 處理）

---

## 2. 背景與先例

### 2.1 研究報告中的需求

`uno-api-extension-research.md` §3.3 將 Phase 4 定義為 `XDocumentOverlay`，目標是讓 extension 在 Writer 編輯區域上疊加繪製，承接 Phase 3 的 paragraph geometry contract。

### 2.2 Writer 現有 overlay 機制

Writer 內部已有成熟的 overlay 系統：

- `sw::overlay::OverlayRanges`（`sw/source/uibase/docvw/OverlayRanges.cxx`）：用於文字選取範圍 overlay
- `sw::sidebarwindows::ShadowOverlayObject`：用於 sidebar annotation 陰影
- `sw::sidebarwindows::AnchorOverlayObject`：用於浮動物件錨點標記

這些都繼承自 `sdr::overlay::OverlayObject`（svx），透過 drawinglayer primitive2d 渲染，在 `DLPostPaint2()` 中由 overlay manager 統一繪製。

#### 為什麼不能直接沿用 svx overlay 系統

- `sdr::overlay::OverlayObject` 要求實作 `createOverlayObjectPrimitive2DSequence()`，回傳 drawinglayer primitives
- drawinglayer primitive2d 是 LO 內部概念，不是 published UNO contract
- extension 無法直接建構 primitive2d 物件
- extension 需要的是一個基於 `XGraphics` 的簡單 callback 模型

因此 Phase 4 需要新建一個 **UNO-friendly overlay layer**，整合在 `SwEditWin::Paint()` 流程中，但使用 `XGraphics` callback 而非 drawinglayer primitives。

### 2.3 Phase 2 的 XCustomPaintWindow 先例

Phase 2 的 `XCustomPaintWindow` 已證明 UNO paint callback 模型可行：

- extension 實作 `XPaintListener`，在 `windowPaint()` 中接收 `XGraphics` 繪製
- VCL `Paint()` 呼叫 UNO callback，跨越 UNO 邊界
- `SolarMutexGuard` 保護整個繪製路徑

Phase 4 的 overlay callback 應延續相同模式，差異在於：

| | Phase 2 (sidebar) | Phase 4 (overlay) |
|---|---|---|
| 繪製目標 | 獨立 VCL Window | SwEditWin 文件區域 |
| 座標系 | window client pixel | document twips |
| 觸發時機 | window Paint event | SwEditWin::Paint() 後段 |
| MapMode | pixel (identity) | 文件 MapMode（由 VCL 處理捲動/縮放） |

---

## 3. 非目標

本 phase 不處理以下事項：

1. **互動事件**：overlay 的 mouse click、hover、drag 等互動 — 留待 Phase 5
2. **overlay z-order 精細控制**：本 phase 只提供簡單 layer 優先級，不做完整 z-order 管理
3. **overlay 動畫**：timer-based animation、fade in/out
4. **overlay 命中測試**：判斷某個 pixel 座標是否在 overlay 繪製範圍內
5. **overlay 跨 view 共享**：一個 overlay handle 只屬於一個 view
6. **非 Writer 應用**：Calc / Draw / Impress 的 overlay 不在本 phase scope
7. **document content overlay**：overlay 不修改文件內容，也不影響列印輸出
8. **XCanvas / drawinglayer 整合**：overlay 使用 XGraphics，不使用 canvas 或 drawinglayer primitives

---

## 4. Public Contract

### 4.1 取得方式

建議新增：

- `offapi/com/sun/star/text/XDocumentOverlay.idl`
- `offapi/com/sun/star/text/XOverlayPainter.idl`

並在 `TextDocumentView.idl` 中加入 optional interface：

```idl
[optional] interface com::sun::star::text::XDocumentOverlay;
```

取得方式與 Phase 3 一致：

```cpp
uno::Reference<css::text::XDocumentOverlay> xOverlay(xController, uno::UNO_QUERY);
```

若 query 失敗，表示目前 LibreOffice build 不支援 Phase 4 contract，extension 必須走 fallback。

### 4.2 介面定義

#### XDocumentOverlay（從 TextDocumentView 取得）

```idl
published interface XDocumentOverlay : com::sun::star::uno::XInterface
{
    /** 註冊一個 overlay painter。

        <p>The painter's <code>paintOverlay()</code> method will be called during
        every view repaint, after the document content has been rendered. Multiple
        painters can be registered; they are called in the order of their
        <code>Layer</code> value (lower values first), and in registration order
        within the same layer.</p>

        @param Painter
            the overlay painter callback to register

        @param Layer
            painting order hint; lower values paint first (closer to document
            content), higher values paint last (closer to user). Typical
            values: 0 for background highlights, 100 for foreground labels.

        @returns
            a handle that can be used to control or remove the overlay
     */
    long addOverlay(
        [in] com::sun::star::text::XOverlayPainter Painter,
        [in] long Layer );

    /** 移除一個已註冊的 overlay。

        <p>After removal, the painter will no longer be called during repaint.
        The affected area is automatically invalidated.</p>

        @throws com::sun::star::lang::IllegalArgumentException
            if Handle does not refer to a registered overlay
     */
    void removeOverlay( [in] long Handle )
        raises (com::sun::star::lang::IllegalArgumentException);

    /** 觸發 overlay 區域重繪。

        <p>Invalidates the visible area so that all registered overlay painters
        will be called again during the next repaint cycle. Use this when the
        overlay content has changed but the document itself has not been
        modified.</p>

        <p>If <code>Area</code> is an empty rectangle (all fields zero), the
        entire visible area is invalidated.</p>

        @param Area
            the document-coordinate region to invalidate, in twips; or an empty
            rectangle to invalidate the entire visible area
     */
    void invalidateOverlay(
        [in] com::sun::star::awt::Rectangle Area );

    /** 設定指定 overlay 的可見性。

        <p>When set to <code>FALSE</code>, the painter will not be called during
        repaint. The affected area is automatically invalidated.</p>

        @throws com::sun::star::lang::IllegalArgumentException
            if Handle does not refer to a registered overlay
     */
    void setOverlayVisible(
        [in] long Handle,
        [in] boolean Visible )
        raises (com::sun::star::lang::IllegalArgumentException);

    /** 查詢指定 overlay 是否可見。

        @throws com::sun::star::lang::IllegalArgumentException
            if Handle does not refer to a registered overlay
     */
    boolean isOverlayVisible( [in] long Handle )
        raises (com::sun::star::lang::IllegalArgumentException);
};
```

#### XOverlayPainter（extension 實作）

```idl
published interface XOverlayPainter : com::sun::star::uno::XInterface
{
    /** 繪製 overlay 內容。

        <p>Called by the view during repaint, after document content has been
        rendered. The implementation must paint only within the coordinate space
        of the provided graphics context.</p>

        <p>The graphics context's coordinate system is document twips, with the
        MapMode already configured to account for scrolling and zooming. This
        means the caller can use coordinates from
        <code>XParagraphNavigator.getParagraphBounds()</code> directly.</p>

        <p><strong>Threading:</strong> This method is always called under the
        SolarMutex. Implementations must not acquire additional locks that could
        lead to deadlock, and should return as quickly as possible to avoid
        blocking the UI repaint.</p>

        <p><strong>Performance:</strong> This method is called on every repaint
        cycle (including during scrolling). Expensive computation should be
        performed outside this callback and cached; this method should only
        perform drawing operations.</p>

        @param Graphics
            the graphics context for drawing, with MapMode set to document
            coordinate space (twips)

        @param VisibleArea
            the currently visible document area in twips; painters may use
            this to skip drawing for off-screen content
     */
    void paintOverlay(
        [in] com::sun::star::awt::XGraphics Graphics,
        [in] com::sun::star::awt::Rectangle VisibleArea );
};
```

### 4.3 Handle 語義

- `addOverlay()` 回傳 `long` handle，是一個不透明識別碼
- handle 在 view 生命週期內唯一
- handle 不可跨 view 使用
- handle 在 `removeOverlay()` 後失效

選擇 `long` handle 而非 `XOverlayHandle` interface 的理由：

1. 避免為每個 overlay 建立額外的 UNO 物件
2. 避免 handle 自身的 lifecycle 管理問題（誰持有 reference？何時 dispose？）
3. 與 Phase 2 的 listener registration 模式一致
4. Python 端操作更簡潔：`handle = overlay.addOverlay(painter, 0)` → `overlay.removeOverlay(handle)`

### 4.4 Layer 語義

`Layer` 參數控制繪製順序：

- 數值小的先繪製（更接近文件內容）
- 數值大的後繪製（更接近使用者）
- 同一 Layer 值的 overlay 按註冊順序繪製
- Layer 值沒有上下限，但建議使用範圍：

| 建議值 | 用途 |
|:---:|------|
| 0 | 背景色 / 段落高亮 |
| 50 | 邊框 / 範圍標記 |
| 100 | 文字標籤 / 前景標記 |

Layer 的語義是 **paint order hint**，不是嚴格的 z-order stack。不保證 overlay 之間的 clip 或合成行為。

### 4.5 座標系與 MapMode

#### 繪製座標系

overlay painter 收到的 `XGraphics` 已經設好 MapMode：

- 單位：**twips**
- 座標系：**Writer document logic space**
- 原點：文件左上角
- 與 `XParagraphNavigator.getParagraphBounds()` 回傳的座標系相同

這意味著：

```python
# 取得段落的文件座標
bounds = navigator.getParagraphBounds(idx)
rect = bounds[0]  # 第一個 fragment

# 在 overlay painter 中直接使用這些座標
def paintOverlay(self, graphics, visible_area):
    graphics.setFillColor(0x80CCDDEE)  # 半透明藍
    graphics.drawRect(rect.X, rect.Y, rect.Width, rect.Height)
```

#### 捲動與縮放

- MapMode 已處理 viewport offset 和 zoom factor
- painter 不需要自行計算捲動或縮放
- painter 繪製的內容會隨文件自動捲動和縮放

#### VisibleArea

`paintOverlay()` 的 `VisibleArea` 參數：

- 單位：twips
- 代表目前 SwEditWin 中可見的文件區域
- painter 應用此資訊跳過不可見內容的繪製，以提升效能
- 與 `SwView::GetVisArea()` 語義一致

### 4.6 XGraphics 能力範圍

overlay painter 收到的 `XGraphics` 是標準的 `awt::XGraphics`，支援：

- `drawRect()` / `drawRoundedRect()` / `drawEllipse()`
- `drawLine()` / `drawPolyLine()` / `drawPolyPolygon()`
- `drawText()` / `drawTextArray()`
- `drawGradient()`
- `setLineColor()` / `setFillColor()` / `setFont()`
- `push()` / `pop()` — 狀態堆疊
- Phase 1.5 新增的 `measureTextInRect()` 等方法（若可用）

**已知限制**（與 Phase 2 sidebar paint 相同）：

- 無 alpha 透明度混合（只有基本 RasterOp）
- 無旋轉 / 縮放矩陣
- 無 Bezier 曲線
- 無路徑裁切

> **注意**：半透明 overlay 是最常見的需求之一。目前 XGraphics 缺乏 alpha 支援。如果在 Phase 4 實作中發現這是核心阻礙，可能需要在 XGraphics 上補充 alpha 相關方法（如 `setFillAlpha()`），但這屬於 Phase 4 的實作階段決策，不在本 spec 預設解決。

### 4.7 Lifecycle 與 view 的關係

overlay 的生命週期綁定在 view（`SwXTextView`）上：

1. **註冊**：`addOverlay()` 將 painter 註冊到目前 view
2. **繪製**：每次 `SwEditWin::Paint()` 後段呼叫已註冊的 painter
3. **暫停**：`setOverlayVisible(handle, false)` 暫停呼叫
4. **移除**：`removeOverlay(handle)` 移除並 invalidate
5. **view 關閉**：view dispose 時，所有已註冊的 overlay 自動移除

extension 不需要（也不應該）在 view dispose 時手動 removeOverlay。但如果 extension 持有 painter reference 形成循環引用，extension 有責任在 view dispose 前斷開。

#### dispose 順序

```
SwXTextView::dispose()
  → 遍歷已註冊的 overlay，逐一移除
  → 清空 overlay 登記簿
  → 繼續既有 dispose 流程
```

不會對 painter 呼叫 dispose（painter 的 lifecycle 由 extension 管理）。

### 4.8 Threading

- `addOverlay()` / `removeOverlay()` / `invalidateOverlay()` / `setOverlayVisible()` 全部需要 SolarMutex
- `paintOverlay()` callback 在 SolarMutex 保護下呼叫
- painter 實作**不得**嘗試取得其他可能造成死鎖的鎖
- painter 實作**不得**在 callback 中呼叫 `addOverlay()` / `removeOverlay()`（避免 reentrancy）

### 4.9 Invalidation

- `invalidateOverlay(Area)` 觸發指定區域的 view 重繪
- `Area` 為空矩形（0, 0, 0, 0）時，invalidate 整個可見區域
- `removeOverlay()` 和 `setOverlayVisible()` 自動 invalidate
- 文件自身的編輯、捲動、縮放會觸發正常 repaint，overlay 自然隨之重繪

extension 不需要在 scroll / zoom 後手動 invalidate — 這些情況下 `SwEditWin::Paint()` 會被正常觸發，overlay painter 自然被呼叫。

### 4.10 相容性邊界

一旦 `XDocumentOverlay` 變成 published UNO API，下列語義就很難再改：

1. `paintOverlay()` callback 的座標系（document twips）
2. `VisibleArea` 的語義
3. handle 的不透明性與唯一性
4. Layer 的繪製順序約定（小先大後）
5. overlay 不出現在列印輸出中

因此這些都必須在本 spec 明講，不能等 C++ 實作完成後再讓 reviewer 從 code 推論。

---

## 5. 實作形狀

### 5.1 建議修改檔案

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 新建 | `offapi/com/sun/star/text/XDocumentOverlay.idl` | overlay 管理 interface |
| 2 | 新建 | `offapi/com/sun/star/text/XOverlayPainter.idl` | extension 實作的 callback interface |
| 3 | 編輯 | `offapi/com/sun/star/text/TextDocumentView.idl` | 加入 `[optional] XDocumentOverlay` |
| 4 | 編輯 | `offapi/UnoApi_offapi.mk` | 註冊新 IDL |
| 5 | 編輯 | `sw/source/uibase/inc/unotxvw.hxx` | `SwXTextView` 增加 `XDocumentOverlay` 實作 |
| 6 | 編輯 | `sw/source/uibase/uno/unotxvw.cxx` | overlay 管理方法實作 |
| 7 | 編輯 | `sw/source/uibase/docvw/edtwin2.cxx` | `SwEditWin::Paint()` 新增 overlay callback 階段 |
| 8 | 可能新建 | `sw/source/uibase/inc/overlaymanager.hxx` | overlay 登記簿 helper |
| 9 | 可能新建 | `sw/source/uibase/uno/overlaymanager.cxx` | overlay 管理邏輯 |
| 10 | 視情況編輯 | `sw/Library_sw.mk` | 若新增 `.cxx` 檔 |

### 5.2 SwEditWin::Paint() 整合

目前 `SwEditWin::Paint()` 的流程：

```
SwEditWin::Paint(rRenderContext, rRect)
  → pWrtShell->setOutputToWindow(true)
  → pWrtShell->Paint(rRenderContext, rRect)
      → DLPrePaint2()
      → PaintDesktop()
      → GetLayout()->PaintSwFrame()     ← 文件內容
      → DLPostPaint2()                  ← 現有 overlay (selection etc.)
  → pWrtShell->setOutputToWindow(false)
```

Phase 4 新增的 overlay callback 應在 `DLPostPaint2()` **之後**、`setOutputToWindow(false)` **之前**：

```
SwEditWin::Paint(rRenderContext, rRect)
  → pWrtShell->setOutputToWindow(true)
  → pWrtShell->Paint(rRenderContext, rRect)
      → DLPrePaint2()
      → PaintDesktop()
      → GetLayout()->PaintSwFrame()     ← 文件內容
      → DLPostPaint2()                  ← 現有 overlay
  → PaintExtensionOverlays(rRenderContext)  ← 【新增】extension overlay
  → pWrtShell->setOutputToWindow(false)
```

這樣 extension overlay 會畫在所有文件內容和內部 overlay（如選取高亮）之上。

#### 為什麼在 DLPostPaint2() 之後

- extension overlay 不應被文件內容或內部 overlay 遮擋
- extension overlay 的座標系需要與 Phase 3 geometry 一致
- 把 extension overlay 放在最上層，避免干擾內部 overlay 的渲染邏輯

#### PaintExtensionOverlays 草案

```cpp
void SwEditWin::PaintExtensionOverlays(vcl::RenderContext& rRenderContext)
{
    SwView* pView = GetView().GetDocShell() ? &GetView() : nullptr;
    if (!pView)
        return;

    SwXTextView* pTextView = /* get from view */;
    if (!pTextView || !pTextView->HasOverlays())
        return;

    // 設定 MapMode 為文件座標
    // (此時 MapMode 應已是文件 logic space)

    // 取得目前可見區域
    tools::Rectangle aVisArea = pView->GetVisArea();
    awt::Rectangle aUnoVisArea = /* convert */;

    // 建立 XGraphics
    uno::Reference<awt::XGraphics> xGraphics = /* create from rRenderContext */;

    // 依 Layer 順序呼叫已註冊的 painter
    pTextView->CallOverlayPainters(xGraphics, aUnoVisArea);
}
```

### 5.3 Overlay 登記簿

`SwXTextView` 內部維護一個 overlay 登記簿：

```cpp
struct OverlayEntry
{
    sal_Int32 nHandle;
    sal_Int32 nLayer;
    uno::Reference<text::XOverlayPainter> xPainter;
    bool bVisible;
};

std::vector<OverlayEntry> m_aOverlays;
sal_Int32 m_nNextHandle = 1;
```

`CallOverlayPainters()` 按 (Layer, 註冊順序) 排序後逐一呼叫 `paintOverlay()`。

### 5.4 XGraphics 的建立

overlay callback 需要一個 `XGraphics`，指向 `SwEditWin` 的 `OutputDevice`。

建立方式與 Phase 2 的 sidebar paint 類似：

```cpp
// 重用 VCLXGraphics 包裝
rtl::Reference<VCLXGraphics> pGraphics(new VCLXGraphics);
pGraphics->Init(rRenderContext);  // or SetOutputDevice
```

**關鍵**：此 XGraphics 的 `OutputDevice` 指標在 callback 結束後即失效。painter 不得保存 XGraphics reference 供後續使用。

### 5.5 MapMode 設定

`SwEditWin::Paint()` 流程中，在 `pWrtShell->Paint()` 呼叫時，MapMode 已被設為文件 logic 座標（twips）。但在 `DLPostPaint2()` 結束後，MapMode 狀態需要確認。

實作時需確保：

1. 進入 `PaintExtensionOverlays()` 前，MapMode 為文件 logic space
2. 如果 `DLPostPaint2()` 改變了 MapMode，需要在 overlay 繪製前恢復
3. overlay 繪製完成後，恢復原始 MapMode

### 5.6 為什麼不需要新 UNO service

與 Phase 3 相同的理由：

- overlay 能力本質上屬於 view（`TextDocumentView`）
- 不是獨立 service
- 不需要 `.component` XML 註冊
- 直接掛在 `SwXTextView` 上，與 `XParagraphNavigator` 平行

---

## 6. 驗證計畫

### 6.1 Phase 4 的 stage hypothesis

**Hypothesis**
Writer 可以在 `SwEditWin::Paint()` 中新增一個 UNO callback 階段，讓 extension 在文件座標系中繪製 overlay，且不影響現有繪製正確性和效能。

**Entry condition**

- Phase 3 已提供穩定的 paragraph geometry snapshot contract
- Phase 2 已證明 UNO paint callback 模型可行（sidebar）

**Fallback**

- 若 `XDocumentOverlay` 不存在，extension 不使用 overlay 功能
- 段落導航和選取仍可透過 Phase 3 的 `XParagraphNavigator` 運作
- 不嘗試用子視窗疊加或 TextFrame 等 workaround 模擬 overlay

**Exit note**

- Phase 4 完成代表 overlay paint callback lifecycle 成立
- 不代表 overlay 互動（click / hover / drag）已被驗證
- 不代表 alpha 透明度問題已完全解決

### 6.2 自動化驗證

建議最小 build / test 組合：

```bash
make offapi
make sw
make CppunitTest_sw_uibase_uno
```

#### 建議新增的 CppUnit 驗證點

放置方向：`sw/qa/uibase/uno/uno.cxx`

至少應覆蓋：

1. `CurrentController` 可 `UNO_QUERY` 到 `XDocumentOverlay`
2. `addOverlay()` 回傳有效 handle（> 0）
3. `removeOverlay()` 後再次 remove 同一 handle 丟出 `IllegalArgumentException`
4. `addOverlay()` + `removeOverlay()` 不崩潰（lifecycle roundtrip）
5. `setOverlayVisible(handle, false)` + `isOverlayVisible(handle)` 回傳 `false`
6. `invalidateOverlay()` 以空矩形呼叫不崩潰
7. 多個 overlay 可同時註冊且不互相干擾
8. view dispose 後，已註冊的 overlay 自動清理（不 double-free、不崩潰）

#### painter callback 驗證

由於 CppUnit 在 headless 環境中可能不觸發完整 Paint()，callback 驗證可能需要：

- 在 CppUnit 中手動觸發 `SwEditWin::Invalidate()` + `Update()`
- 或使用 mock painter 記錄呼叫次數

需要驗證：

9. `paintOverlay()` 在 repaint 時被呼叫（至少一次）
10. `paintOverlay()` 接收到非 null 的 `XGraphics` 和非零的 `VisibleArea`
11. `setOverlayVisible(handle, false)` 後 `paintOverlay()` 不再被呼叫
12. 多個 painter 按 Layer 順序被呼叫

### 6.3 extension-side 驗證

建議用 Python macro 驗證基本 overlay 功能：

```python
import uno
from com.sun.star.text import XOverlayPainter

class HighlightPainter(unohelper.Base, XOverlayPainter):
    def __init__(self, navigator, para_index):
        self.navigator = navigator
        self.para_index = para_index

    def paintOverlay(self, graphics, visible_area):
        bounds = self.navigator.getParagraphBounds(self.para_index)
        if len(bounds) > 0:
            rect = bounds[0]
            graphics.setFillColor(0xCCDDEE)
            graphics.drawRect(rect.X, rect.Y, rect.Width, rect.Height)

def test_phase4(args=None):
    doc = XSCRIPTCONTEXT.getDocument()
    controller = doc.getCurrentController()

    # capability probe
    overlay = controller  # queryInterface XDocumentOverlay
    navigator = controller  # queryInterface XParagraphNavigator

    painter = HighlightPainter(navigator, 0)
    handle = overlay.addOverlay(painter, 0)

    # 觸發重繪以觀察效果
    import time
    time.sleep(2)

    overlay.removeOverlay(handle)
```

### 6.4 手動驗證

至少做以下情境：

1. **基本 overlay**：在第一段落上繪製矩形，確認位置正確
2. **捲動**：overlay 隨文件捲動正確移動
3. **縮放**：zoom in/out 後 overlay 大小和位置正確更新
4. **可見性切換**：`setOverlayVisible(false)` 後 overlay 消失，`true` 後出現
5. **多個 overlay**：同時註冊背景色和標籤，確認 Layer 順序正確
6. **效能**：快速捲動時不明顯卡頓
7. **列印**：overlay 不出現在列印輸出中
8. **view 關閉**：關閉文件後不崩潰

---

## 7. 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | `XDocumentOverlay` 可從 Writer `CurrentController` 取得 | CppUnit / Python macro |
| 2 | `addOverlay()` / `removeOverlay()` lifecycle 正確 | CppUnit |
| 3 | `paintOverlay()` 在 repaint 時被呼叫 | CppUnit (mock painter) |
| 4 | painter 收到的座標系為 document twips，與 `getParagraphBounds()` 一致 | CppUnit + 手動驗證 |
| 5 | overlay 隨捲動和縮放正確更新 | 手動驗證 |
| 6 | `setOverlayVisible(false)` 後 painter 不被呼叫 | CppUnit |
| 7 | overlay 不出現在列印輸出中 | 手動驗證 |
| 8 | 快速捲動時效能可接受（無明顯卡頓） | 手動驗證 |
| 9 | view dispose 不崩潰 | CppUnit |
| 10 | extension capability probe 與 fallback path 可行 | Python macro |
| 11 | Phase 4 與 Phase 5 的邊界清楚（無 overlay 互動 contract） | spec review |

### 7.1 驗收責任切分

| 驗收項 | 主要關帳位置 | 原因 |
|--------|--------------|------|
| `#1` `CurrentController` 可取得 `XDocumentOverlay` | `LO-core` + extension | core 證明 bridge 存在；extension 證明 Python 端可用 |
| `#2` lifecycle roundtrip | `LO-core` | handle 語義屬於 public contract |
| `#3` callback 被呼叫 | `LO-core` | 屬於 paint integration contract |
| `#4` 座標系一致 | `LO-core` 為主，extension 輔助 | core 關帳 MapMode；extension 補真實 paragraph highlight |
| `#5` 捲動/縮放 | extension 為主 | 真正容易驗的是使用者視角行為 |
| `#6` 可見性控制 | `LO-core` | 屬於 handle state contract |
| `#7` 列印排除 | `LO-core` + 手動 | overlay callback 只在 screen paint 呼叫 |
| `#8` 效能 | extension 為主 | 需在真實使用情境評估 |
| `#9` view dispose | `LO-core` | lifecycle 邊界 |
| `#10` capability probe | extension | consumer integration contract |
| `#11` Phase 邊界 | spec review | 設計邊界 |

### 7.2 `LO-core` 必關帳項

Phase 4 不應宣告完整驗收，除非以下都有對應 CppUnit / core 測試：

1. `XDocumentOverlay` 可從 `CurrentController` 取得
2. `addOverlay()` 回傳有效 handle
3. `removeOverlay()` 後 handle 失效（double remove 丟 exception）
4. `paintOverlay()` callback 在 repaint 時被呼叫
5. `setOverlayVisible(false)` 阻止 callback
6. overlay 繪製時 MapMode 為 document twips
7. view dispose 時 overlay 自動清理

### 7.3 extension 必關帳項

以下應在 `markdown-insert-ext` 內驗證：

1. Python `queryInterface()` 可穩定取得 `XDocumentOverlay`
2. 新 API 缺席時不崩潰，走 fallback
3. 段落高亮 overlay 位置與 `getParagraphBounds()` 座標一致
4. 捲動 / 縮放後 overlay 正確更新
5. `setOverlayVisible()` 切換可見性
6. `removeOverlay()` 後 overlay 消失

---

## 8. 風險與後續

| 風險 | 說明 | 緩解 |
|------|------|------|
| UNO callback 效能 | `paintOverlay()` 每次 repaint 都跨 UNO 邊界呼叫 | 先量測。若不可接受，考慮 batch invalidation 或只在 dirty 時呼叫 |
| MapMode 不一致 | `DLPostPaint2()` 可能改變 MapMode | 在 overlay 繪製前明確恢復 document MapMode |
| Alpha 透明度 | XGraphics 不支援 alpha，段落高亮需要半透明 | Phase 4 先用不透明色驗證 contract；alpha 支援可作為 XGraphics 擴充 |
| SolarMutex 死鎖 | painter 內部若有鎖可能造成死鎖 | IDL 文件明確警告；extension 端 lint / 文件 |
| 列印路徑洩漏 | overlay callback 可能在列印 paint 路徑也被呼叫 | 實作中檢查 output device 類型或 `setOutputToWindow` flag |
| Painter reference 循環 | extension 持有 handle，painter 持有 extension state，形成循環 | 文件說明 lifecycle 責任；view dispose 時清理 overlay 不呼叫 painter.dispose() |
| headless 測試困難 | svp 後端可能不觸發完整 Paint() | 使用 `Invalidate()` + `PaintImmediately()` 觸發；或在 CppUnit 中直接呼叫 `PaintExtensionOverlays()` |

### 8.1 Deferred Work

以下議題不在本 phase 解決：

- **overlay 互動**：mouse click / hover / drag on overlay — Phase 5
- **overlay hit test**：判斷某 pixel 座標是否命中某個 overlay
- **overlay 動畫**：timer-based update、fade in/out
- **alpha 透明度**：XGraphics 層級的 alpha 支援（可能需要 XGraphics 擴充）
- **非 Writer overlay**：Calc / Draw / Impress 的 overlay
- **overlay 跨 view 共享**
- **overlay 序列化**：overlay 不儲存在文件中，不隨文件傳播

換句話說，**Phase 4 完成後，extension 應該能穩定「在段落位置上畫東西」；Phase 5 才負責「讓使用者與畫上去的東西互動」。**
