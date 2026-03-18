# 擴充 UNO API 以支援 Extension 自訂繪製：研究報告與開發指引

> 版本：0.1 draft
> 日期：2026-03-06

---

## 1. 概述

### 1.1 目的

LibreOffice extension（.oxt）目前無法做到自訂繪製，因為 VCL 的繪製能力沒有透過 UNO API 充分暴露。本報告研究現有的 UNO-to-VCL 橋接模式，並規劃需要新增的 UNO API，使 extension 能夠：

1. 在 sidebar 面板中做自訂繪製（聊天氣泡、程式碼區塊等）
2. 在文件視圖上疊加繪製（段落高亮標籤）
3. 精確控制段落選取與導航

### 1.2 核心策略

```
LO 核心 (改一次)                    Extension (持續迭代)
┌──────────────────────┐           ┌──────────────────────┐
│ 新增 UNO IDL 介面    │           │ Python 或 C++ .oxt   │
│ 新增 VCL 橋接實作    │ ← UNO →  │ 實作回呼介面         │
│ 在 toolkit/ 和 sw/   │           │ 透過新 API 自訂繪製  │
│ 中實作               │           │ 獨立發佈、版本獨立   │
└──────────────────────┘           └──────────────────────┘
```

核心只需修改一次，產出通用 API。之後所有 extension 都能使用自訂繪製，extension 維持 .oxt 獨立發佈。

---

## 2. 現有先例：UNO-to-VCL 橋接模式

LibreOffice 已有成熟的 UNO-to-VCL 橋接架構。以下是三個關鍵先例。

### 2.1 XGraphics — 繪製操作橋接

**角色**：讓 UNO 端（extension / macro）能在視窗上繪製圖形和文字。

**IDL 定義**：`offapi/com/sun/star/awt/XGraphics.idl`

提供的繪製方法：

| 方法 | 說明 |
|------|------|
| `drawLine(x1, y1, x2, y2)` | 畫線段 |
| `drawRect(x, y, w, h)` | 畫矩形 |
| `drawRoundedRect(x, y, w, h, rx, ry)` | 畫圓角矩形 |
| `drawText(x, y, text)` | 繪製文字 |
| `drawGradient(x, y, w, h, gradient)` | 漸層填充 |
| `drawEllipse(x, y, w, h)` | 畫橢圓 |
| `drawPolyPolygon(...)` | 畫多邊形 |
| `setLineColor(color)` | 設定線條顏色 |
| `setFillColor(color)` | 設定填充顏色 |
| `setFont(font)` | 設定字體 |
| `push()` / `pop()` | 狀態堆疊 |

**C++ 實作**：`VCLXGraphics`（`toolkit/source/awt/vclxgraphics.cxx`）

橋接原理：

```
Extension 呼叫 XGraphics::drawRect(10, 20, 100, 50)
  → VCLXGraphics::drawRect()
    → SolarMutexGuard 取鎖（VCL 線程安全）
    → InitOutputDevice()（將快取的顏色/字體同步到 VCL）
    → mpOutputDevice->DrawRect(...)（呼叫 VCL 的 OutputDevice）
      → 最終由平台圖形後端繪製（Cairo/GDI/Metal）
```

**限制**：

- 只有整數像素座標（無浮點變換）
- 無旋轉/縮放/扭曲矩陣
- 無 alpha 透明度混合（只有基本 RasterOp）
- 無 Bezier 曲線
- 無路徑裁切（只有矩形 clip region）

這些限制是我們需要擴充 API 的原因。

### 2.2 XDevice — 圖形裝置抽象

**角色**：代表一個可繪製的表面（螢幕視窗、虛擬設備、印表機），是 XGraphics 的工廠。

**IDL 定義**：`offapi/com/sun/star/awt/XDevice.idl`

關鍵方法：

| 方法 | 說明 |
|------|------|
| `createGraphics()` | 建立 XGraphics 物件用於繪製 |
| `createDevice(w, h)` | 建立相容的虛擬設備（離屏繪製） |
| `getInfo()` | 取得設備資訊（解析度、色深） |
| `getFont(descriptor)` | 取得字體資訊 |
| `createBitmap(x, y, w, h)` | 從設備擷取點陣圖 |

**C++ 實作**：`VCLXDevice`（`toolkit/source/awt/vclxdevice.cxx`）

內部包裝 VCL 的 `OutputDevice`，提供 UNO 端存取。`createGraphics()` 內部建立 `VCLXGraphics` 並綁定到同一個 `OutputDevice`。

**關鍵設計模式**：XDevice 是 **工廠 + 資訊提供者**，不直接繪製。繪製透過它產出的 XGraphics 物件進行。

### 2.3 XWindowPeer — 視窗操作橋接

**角色**：UNO 端操作視窗的入口。每個 VCL 視窗都有一個對應的 XWindowPeer。

**IDL 定義**：`offapi/com/sun/star/awt/XWindowPeer.idl`

關鍵方法：

| 方法 | 說明 |
|------|------|
| `getToolkit()` | 取得 Toolkit |
| `setPointer(pointer)` | 設定滑鼠游標 |
| `setBackground(color)` | 設定背景色 |
| `invalidate(flags)` | 觸發重繪 |
| `invalidateRect(rect, flags)` | 觸發區域重繪 |

**C++ 實作**：`VCLXWindow`（`toolkit/source/awt/vclxwindow.cxx`）

繼承關係：

```
VCLXWindow
  └─ extends VCLXDevice        ← 所以每個視窗也是一個 Device
       └─ wraps OutputDevice    ← 實際上包裝的是 vcl::Window
```

這表示 **extension 已經可以從視窗取得 XGraphics**：

```python
peer = window.getPeer()           # XWindowPeer
graphics = peer.createGraphics()  # XGraphics（透過繼承的 XDevice）
graphics.drawRect(10, 10, 100, 50)
```

但問題是：extension **無法控制繪製時機**。`drawRect()` 會立即繪製，但 VCL 的重繪機制隨時可能覆蓋這些內容。Extension 需要的是「在重繪事件中被回呼」，而不是「主動繪製」。

### 2.4 XCanvas — 進階繪製 API（已存在但用途不同）

**IDL 定義**：`offapi/com/sun/star/rendering/XCanvas.idl`

XCanvas 是一個更現代的繪製 API，提供：

- 浮點座標 + 仿射變換矩陣
- Bezier 路徑
- 任意路徑裁切
- Alpha 混合
- 無狀態設計（每次呼叫帶完整 ViewState + RenderState）

**實作**：`canvas/source/vcl/canvas.cxx`（VCL 後端）

XCanvas 主要用於 Impress 的投影片渲染和過場動畫，不用於一般 UI 繪製。但它的設計理念（無狀態、變換矩陣、路徑）值得參考。

### 2.5 UNO Wrapper 工廠模式

所有橋接物件由 `UnoWrapper`（`toolkit/source/helper/unowrapper.cxx`）統一管理：

```
UnoWrapper::CreateGraphics(OutputDevice*)  → new VCLXGraphics
UnoWrapper::GetWindowInterface(vcl::Window*)  → find/create VCLXWindow
UnoWrapper::ReleaseAllGraphics(OutputDevice*)  → disconnect all graphics
```

這個工廠確保 VCL 物件和 UNO 包裝物件的生命週期正確同步。

### 2.6 線程安全模式

所有 VCLXGraphics / VCLXWindow 的方法開頭都是：

```cpp
SolarMutexGuard aGuard;  // 取得 SolarMutex
if (mpOutputDevice)       // 檢查底層 VCL 物件是否仍存活
{
    // 安全操作 VCL
}
```

任何新增的 UNO API 都必須遵循此模式。

---

## 3. 需要新增的 UNO API

基於現有先例的分析，以下是需要擴充的五組 API：

| # | 新增 API | 用途 | 實作位置 | 複雜度 |
|:-:|---------|------|----------|:------:|
| 3.1 | `XCustomPaintWindow` | 自訂繪製視窗（聊天介面等） | `toolkit/` | 高 |
| 3.2 | `XGraphics` 擴充 | 文字量測、多行文字、alpha | `toolkit/` | 中 |
| 3.3 | `XDocumentOverlay` | 文件視圖上疊加繪製 | `sw/` | 高 |
| 3.4 | `XParagraphNavigator` | 段落導航與排版位置查詢 | `sw/` | 中 |
| 3.5 | `XSplitterWindow` | 可拖拉分隔線（佈局調整） | `toolkit/` | 低 |

### 3.1 XCustomPaintWindow — 自訂繪製視窗

**目的**：讓 extension 能提供繪製回呼，在 VCL 重繪時被呼叫。

**解決的問題**：目前 extension 只能「主動繪製」（呼叫 XGraphics 方法），但這些繪製會被 VCL 的重繪覆蓋。需要一個機制讓 extension 「被動繪製」——VCL 觸發重繪時回呼 extension 的繪製邏輯。

**建議 IDL 位置**：`offapi/com/sun/star/awt/XCustomPaintWindow.idl`

**介面設計方向**：

```
XCustomPaintWindow
├── setPaintHandler(XPaintHandler handler)  // 設定繪製回呼
├── invalidate()                             // 觸發重繪
├── invalidateRect(Rectangle area)           // 觸發區域重繪
├── getPreferredSize() → Size                // 取得建議大小
└── setScrollOffset(long x, long y)          // 捲動偏移

XPaintHandler
├── paint(XGraphics2 graphics, Rectangle updateArea)  // 被呼叫進行繪製
├── getContentHeight() → long                          // 回報內容總高度
└── mouseClicked(long x, long y, short button)         // 滑鼠事件轉發
```

**C++ 實作方向**：

在 `toolkit/` 中建立一個新的 VCL 視窗子類別（如 `CustomPaintWindow : public vcl::Window`），override `Paint()` 方法。在 `Paint()` 內部，取得 OutputDevice 的 XGraphics 包裝，呼叫 extension 提供的 `XPaintHandler::paint()`。

**關鍵設計考量**：

- **繪製時機**：`XPaintHandler::paint()` 在 VCL 的 `Paint()` 事件中被呼叫，此時 SolarMutex 已被持有，extension 可以安全地呼叫 XGraphics 方法
- **事件轉發**：滑鼠、鍵盤事件需要轉發給 extension（用於按鈕點擊、捲動等互動）
- **捲動**：內建捲動支援（`setScrollOffset`），避免 extension 自己管理 canvas 偏移
- **與 XDL 的關係**：此視窗可作為 XDL dialog 中的一個控制項使用，或獨立建立

**參考先例**：VCL 內部的 `PaintHelper` 機制（`vcl/source/window/paint.cxx`）

### 3.2 XGraphics2D — 擴充繪製能力

**目的**：擴充現有 XGraphics/XGraphics2 的繪製能力，補足自訂 UI 繪製所需的功能。

**解決的問題**：現有 XGraphics 缺少圓角矩形填充控制、alpha 透明度、文字量測、多行文字繪製等 UI 繪製必需的功能。

**建議 IDL 位置**：`offapi/com/sun/star/awt/XGraphics2D.idl`

**需要新增的方法分類**：

**文字量測與繪製**（目前最大的缺口）：

```
measureText(text, font, maxWidth) → TextMetrics
  // 回傳：寬度、高度、行數、每行斷點位置
  // 這是 Python 版 ScrollableChatView 最痛苦的部分——估算文字高度

drawTextWrapped(x, y, text, maxWidth)
  // 自動換行繪製多行文字，使用目前字體

drawTextWithFormat(x, y, parts[])
  // 單次呼叫繪製混合格式文字（粗體+普通混排）
```

**圖形增強**：

```
drawRoundedRectFilled(x, y, w, h, rx, ry)
  // 填充的圓角矩形（聊天氣泡背景）
  // XGraphics 有 drawRoundedRect 但受 fill/line 狀態控制不直覺

setAlpha(alpha)
  // 全局 alpha 值（0-255），影響後續所有繪製
  // 目前 XGraphics 完全沒有 alpha 支援

drawImage(x, y, w, h, XGraphic)
  // XGraphics2 已有此方法，確保可用
```

**設計選擇**：

- **方案 A**：直接擴充 `XGraphics2`，新增方法（新 IDL `XGraphics3`）
  - 優點：沿用現有架構
  - 缺點：stateful API，不適合複雜繪製
- **方案 B**：新建獨立的 `XGraphics2D` 介面，可從 XGraphics 取得
  - 優點：乾淨的介面分離
  - 缺點：需要額外的橋接層
- **建議**：方案 A（擴充 `XGraphics`），因為與現有架構一致，且 `XCustomPaintWindow` 的 `paint()` 回呼已經傳入 XGraphics，extension 可以直接使用擴充方法

**C++ 實作方向**：

在 `VCLXGraphics` 中新增方法，委派到 VCL 的 `OutputDevice`。例如 `measureText()` 可以委派到 `OutputDevice::GetTextWidth()` / `GetTextHeight()` / `GetTextBreakInfo()`，這些 VCL 已有完整實作。

#### 3.2.1 驗證後補充：需要一個更強的 Text Layout Contract

2026-03-14 在 `markdown-insert-ext` 的 staged validation 中，已完成：

- Phase 1：`XGraphics3.measureText()` + `drawTextInRect()`
- Phase 2：`XCustomPaintWindow`

但驗證結果顯示，sidebar 自繪 block 的高度雖已明顯優於 `UnoControlFixedText` 路徑，仍存在可見的殘留誤差：某些長段落在 custom paint 視圖中仍會出現 block 高度略高或略低於實際文字佔用區域的情況。

這表示問題**不只是 control peer 差異**，而是目前 API contract 仍有缺口：

- `measureText(text, maxWidth)` 只接受最大寬度，**不知道最終繪製使用的 rect 與 flags**
- `drawTextInRect(rect, text, flags)` 則在另一個呼叫中才拿到完整資訊
- extension 無法用單一 API 取得「這段文字在這個 rect、這組 flags 下，實際繪製會佔用的 used bounds」

因此，這個問題**不是 Phase 3（`XParagraphNavigator`）或 Phase 4（`XDocumentOverlay`）的範圍**，而是 Phase 1 的延伸缺口。建議在開發順序中插入一個 **Phase 1.5: Text Layout Contract**：

- 在 `XGraphics3` 之上新增一個小幅但關鍵的擴充
- 讓 extension 可以用與 `drawTextInRect()` 相同的 rect + flags 進行量測
- 回傳 used rect / line count / max line width / ellipsis 等結果

建議 API 方向：

```idl
XGraphics4 : XGraphics3
└── measureTextInRect(Rectangle Rect, string Text, long Flags)
    → TextLayoutMetrics
```

其中 `TextLayoutMetrics` 至少應包含：

- `UsedRect`
- `Width`
- `Height`
- `MaxLineWidth`
- `LineCount`
- `IsEllipsis`

這一層的目標不是引入完整的 reusable text layout object，而是先補齊 extension 端目前最需要的「量測與繪製使用同一組輸入條件」。

### 3.3 XDocumentOverlay — 文件視圖疊加繪製

**目的**：讓 extension 能在文件視圖（Writer 的編輯區域）上疊加繪製，而不修改文件內容。

**解決的問題**：段落高亮標籤（「已選取段落」）需要顯示在文件內容上方。目前 extension 只能用 TextFrame（修改文件）或 `Toolkit.createWindow()` 子視窗（需手動追蹤座標），兩者都不理想。

**建議 IDL 位置**：`offapi/com/sun/star/text/XDocumentOverlay.idl`

**介面設計方向**：

```
XDocumentOverlay（從 XTextDocument 或 XController 取得）
├── addOverlay(XOverlayPainter painter) → XOverlayHandle
├── removeOverlay(XOverlayHandle handle)
└── invalidateOverlays()

XOverlayPainter（extension 實作）
├── paint(XGraphics2 graphics,
│         Rectangle visibleArea,
│         XTextViewCursor viewCursor)
│   // visibleArea：目前可見的文件區域座標
│   // viewCursor：目前游標位置（供 extension 計算相對位置）
└── getLayer() → short
    // 繪製層級（低於選取高亮、高於文字）

XOverlayHandle（控制 handle）
├── setVisible(boolean visible)
├── invalidate()
└── dispose()
```

**C++ 實作方向**：

這部分需要修改 Writer 的編輯視窗。`SwEditWin`（`sw/source/uibase/docvw/edtwin.cxx`）是 Writer 的主要編輯區域，它的 `Paint()` 方法中可以新增一個 overlay 繪製階段：

```
SwEditWin::Paint()
  → 繪製文件內容（現有邏輯）
  → 繪製選取高亮（現有邏輯）
  → 繪製 overlay（新增：遍歷已註冊的 XOverlayPainter，逐一呼叫 paint()）
```

**關鍵設計考量**：

- **座標系統**：overlay 的座標應該是文件座標（twips），不是視窗像素座標。VCL 的 `MapMode` 會處理轉換
- **捲動**：overlay 繪製時 VCL 已設好 MapMode，extension 畫在文件座標上的東西會自動隨捲動移動
- **效能**：`Paint()` 是高頻呼叫，extension 的 `paint()` 實作必須快速。如果需要複雜計算（如段落位置查詢），應在 overlay 外部完成，`paint()` 只做繪製
- **段落位置查詢**：extension 需要知道段落在文件中的位置才能定位 overlay。這可以透過下方的 `XParagraphNavigator` 提供

### 3.4 XParagraphNavigator — 段落導航與資訊

**目的**：提供段落層級的導航、選取和排版資訊查詢。

**解決的問題**：目前 UNO 的 `XParagraphCursor` 可以做段落導航，但無法查詢段落在畫面上的排版位置（需要存取 Writer 內部的 `SwFrame`）。Extension 需要知道段落的螢幕座標才能定位 overlay。

**建議 IDL 位置**：`offapi/com/sun/star/text/XParagraphNavigator.idl`

**介面掛載位置（研究後補充）**：

這個能力較適合作為 Writer 現有 `TextDocumentView` / controller 的一個 optional interface，而不是新的可實例化 UNO service。也就是說，extension 應從：

```
doc.getCurrentController()
```

對 controller 做 `UNO_QUERY` 取得 `XParagraphNavigator`。  
這與現有 `XTextViewCursorSupplier` 的掛載模式一致，也能避免額外的 service registration 與 lifecycle 複雜度。

**介面設計方向（修正版）**：

```
XParagraphNavigator（從 XTextDocument 的 XController 取得）
├── getCount() → long                           // 總段落數（主本文 paragraph）
├── getCurrentIndex() → long                     // 目前段落 index（0-based，scope 外回傳 -1）
├── gotoIndex(long index, boolean select)        // 跳到指定段落
├── gotoNext(boolean select) → boolean           // 下一段（scope 外回傳 false）
├── gotoPrevious(boolean select) → boolean       // 上一段（scope 外回傳 false）
├── selectCurrentParagraph()                     // 選取目前整段（scope 外為 no-op）
│
│  // 排版資訊查詢（關鍵新功能）
├── getParagraphBounds(long index) → sequence<Rectangle>
│   // 段落在文件中的 fragment 矩形（twips）
├── getParagraphViewBounds(long index) → sequence<Rectangle>
│   // 段落在目前 Writer 視圖中的 fragment 矩形（pixels）
├── isVisible(long index) → boolean              // 段落是否在可見範圍內
│
│  // 段落內容
├── getParagraphText(long index) → string        // 取得展開後的段落文字
└── getParagraphStyleName(long index) → string   // 取得 UNO programmatic 段落樣式名稱
```

> **實作回饋（2026-03-18）**：`getParagraphStyleName()` 必須回傳 UNO programmatic name（如 `"Heading 1"`），而非 UI localized name（如中文環境下的「標題 1」）。實作上需透過 `SwStyleNameMapper::FillProgName()` 轉換，否則回傳值會隨 locale 改變而不穩定。初版實作曾直接用 `GetTextColl()->GetName()` 回傳 UI name，在 code review 中被修正。

#### 3.4.1 contract 收斂：Phase 3 先只做 geometry snapshot

2026-03-18 補充：原始研究方向把 paragraph geometry 與 highlight / overlay handle 放在同一個 phase 中，但進一步比對 Writer 現有 controller bridge 後，較穩定的切法應是：

- **Phase 3**：只提供 paragraph navigation + geometry snapshot
- **Phase 4**：再加入 overlay / highlight / repaint lifecycle

原因是 geometry contract 與 overlay contract 的風險完全不同：

- geometry contract 的核心風險是 index、座標系、fragment 粒度、scope
- overlay contract 的核心風險則是 `SwEditWin::Paint()` ordering、invalidations、z-order、callback lifecycle

若兩者同時引入，published API 的 review 與 staged validation 都會變得過大。

因此，Phase 3 應先明確承諾：

1. **scope 只限主本文（main body text）**
   - 以 `XTextDocument.getText()` 的 paragraph 順序為準
   - 不含 table cell、header/footer、footnote/endnote、text frame、shape text
2. **index 為 0-based**，合法範圍 `0 .. getCount()-1`
3. `getCurrentIndex()` 若目前 view cursor 不在支援範圍內，回傳 `-1`
4. `selectCurrentParagraph()` 若目前 view cursor 不在支援範圍內，為 **no-op**（不 throw）
5. `getParagraphBounds()` / `getParagraphViewBounds()` 回傳的是 **snapshot**
   - scroll / zoom / edit / layout change 後需重新查詢
6. geometry 應是 **fragment rect sequence**，而不是單一 union rect
   - 因為同一段落可能跨 page / column / follow frame
7. `getParagraphViewBounds()` 的 pixel 座標應定義為
   - **目前 `SwEditWin` client area 左上角為原點**
   - 不是 global screen pixel
8. `getParagraphStyleName()` 回傳的是 **UNO programmatic name**，不是 UI localized name

這些邊界若不先寫進研究報告與 spec，之後實作很容易長成「看似能用、但 contract 很難維護」的版本。

**C++ 實作方向**：

在 `sw/source/uibase/uno/` 中新增或擴充實作，首選掛點是既有 `SwXTextView`（`TextDocumentView` 的 UNO bridge）。段落排版資訊的取得方式：

```
XController (SwView)
  → SwWrtShell
    → 找到對應的 SwTextNode（主本文 paragraph）
      → SwTextNode::getLayoutFrame()
        → SwTextFrame::getFrameArea()      // fragment 的排版矩形（文件座標）
        → SwTextFrame::GetFollow()          // 後續 fragment

視圖座標轉換：
  SwEditWin::LogicToPixel(frameArea)  // 文件座標 → 視窗像素座標
```

**設計補充**：

- `XParagraphCursor` 仍可作為 model 端段落移動先例，但不能直接解決 view geometry
- 若 paragraph 目前沒有對應 layout frame（例如 hidden paragraph 或 layout 尚未 materialize），`getParagraphBounds()` / `getParagraphViewBounds()` 應回傳空 sequence，`isVisible()` 回傳 `false`
- Phase 3 不需要新增新的 UNO service 或 `.component` 註冊，只需在 `TextDocumentView` 上加 optional interface
- 段落高亮不應在本 phase 直接做成 `setHighlight()`；它應延後到 `XDocumentOverlay`，避免讓 geometry phase 背上 repaint / handle lifetime contract

### 3.5 XSplitter — 可拖拉分隔線

**目的**：讓 extension 能在 sidebar 面板中放置可拖拉的分隔線，使用者可以拖拉來調整相鄰區域的大小（例如聊天記錄區和輸入框的高度比例）。

**解決的問題**：目前 extension 的 sidebar 佈局是固定比例的。輸入框高度寫死在程式碼中（如 `SIDEBAR_INPUT_HEIGHT = 50`），使用者無法根據需求調整。VCL 內部已有完整的 `Splitter` 控制項（`include/vcl/split.hxx`），支援滑鼠拖拉、鍵盤操作、拖拉範圍限制，但沒有透過 UNO 暴露。

**VCL Splitter 已有的能力**（直接可橋接）：

| VCL 方法 | 功能 |
|---------|------|
| `SetHorizontal(bool)` | 設定水平或垂直分割 |
| `SetSplitPosPixel(pos)` | 設定分隔線位置 |
| `GetSplitPosPixel()` | 取得分隔線位置 |
| `SetDragRectPixel(rect)` | 設定拖拉範圍（限制最小/最大位置） |
| `SetKeyboardStepSize(step)` | 設定鍵盤操作步進量 |
| `SetSplitHdl(handler)` | 拖拉時的回呼 |
| `SetStartSplitHdl(handler)` | 開始拖拉時的回呼 |
| `SetEndSplitHdl(handler)` | 結束拖拉時的回呼 |
| 滑鼠拖拉追蹤 | `MouseButtonDown` + `Tracking` 已完整實作 |
| 鍵盤操作 | `KeyInput` 已完整實作 |
| 拖拉預覽線 | `Paint` 中繪製分隔線 |

**建議 IDL 位置**：`offapi/com/sun/star/awt/XSplitterWindow.idl`

**介面設計方向**：

```
XSplitterWindow
├── setHorizontal(boolean bHorizontal)       // true=水平分割, false=垂直分割
├── isHorizontal() → boolean
│
├── setSplitPosition(long nPos)              // 設定分隔線位置（像素）
├── getSplitPosition() → long                // 取得分隔線位置
│
├── setRange(long nMin, long nMax)           // 設定拖拉範圍（最小/最大位置）
├── setKeyboardStepSize(long nStep)          // 鍵盤操作步進量
│
├── addSplitListener(XSplitListener)         // 註冊拖拉事件監聽
└── removeSplitListener(XSplitListener)

XSplitListener
├── splitStarted(SplitEvent event)           // 使用者開始拖拉
├── splitting(SplitEvent event)              // 拖拉過程中（即時更新）
├── splitFinished(SplitEvent event)          // 拖拉結束
└── disposing(EventObject)

SplitEvent (struct)
├── Position : long                          // 分隔線新位置（像素）
└── Source : XInterface                      // 事件來源
```

**C++ 實作方向**：

在 `toolkit/` 中建立 UNO 包裝層，包裝 VCL 的 `Splitter` 類別。實作模式與 `VCLXWindow` 包裝 `vcl::Window` 完全相同：

```
Extension 呼叫 XSplitterWindow::setSplitPosition(200)
  → VCLXSplitter::setSplitPosition()
    → SolarMutexGuard
    → m_pSplitter->SetSplitPosPixel(200)

使用者拖拉分隔線
  → VCL Splitter::Split() 觸發回呼
    → VCLXSplitter 內部 handler
      → 呼叫 XSplitListener::splitting(event)
        → Extension 收到新位置
          → 重新計算上下區域大小
          → setPosSize() 更新 ChatView 和 ChatInput
```

**實作複雜度**：低。VCL `Splitter` 已經完整實作了所有拖拉邏輯，UNO 層只需要做薄薄的包裝和事件轉發。

**Extension 端使用範例（sidebar 面板佈局）**：

```
┌──────────────────────┐
│  ChatView            │  ← XCustomPaintWindow（自訂繪製聊天記錄）
│  （聊天記錄區域）     │
│                      │
├══ XSplitter ═════════┤  ← 使用者可拖拉調整上下比例
│  ChatInput           │  ← 現有 UnoControlEdit（多行文字輸入）
│  （輸入框）           │
├──────────────────────┤
│  [Send] [Settings]   │  ← 現有 UnoControlButton
└──────────────────────┘
```

Extension 在 `XSplitListener::splitting()` 中收到新位置後，呼叫 `ChatView.setPosSize()` 和 `ChatInput.setPosSize()` 更新兩者的高度。拖拉範圍透過 `setRange()` 限制，避免使用者把某個區域拉到消失。

---

## 4. 整體架構圖

```
Extension (.oxt, Python or C++)
┌───────────────────────────────────────────────────────────┐
│                                                           │
│  實作 XPaintHandler   實作 XOverlayPainter                │
│      ↑ paint()             ↑ paint()                      │
│      │                     │          實作 XSplitListener  │
│      │                     │               ↑ splitting()   │
│      │                     │               │               │
└──────┼─────────────────────┼───────────────┼──────────────┘
       │ UNO 回呼            │ UNO 回呼      │ UNO 事件
       │                     │               │
─── UNO 邊界 ────────────────────────────────────────────────
       │                     │               │
┌──────┼─────────────────────┼───────────────┼──────────────┐
│ LO 核心 (toolkit/ + sw/)   │               │              │
│      │                     │               │              │
│  XCustomPaintWindow   XDocumentOverlay  XSplitterWindow   │
│  (toolkit/)           (sw/uibase/)      (toolkit/)        │
│      │                     │               │              │
│  CustomPaintWindow    SwEditWin::Paint  VCL Splitter      │
│  : vcl::Window             │ overlay     : vcl::Window    │
│      │                     │               │              │
│      ↓                     ↓               │              │
│  VCL OutputDevice     VCL OutputDevice     │              │
│      │                     │               │              │
│      ↓                     ↓               │              │
│  XGraphics / XGraphics2D (擴充)            │              │
│  (VCLXGraphics, toolkit/)                  │              │
│      │                                     │              │
│      ↓                                     │              │
│  平台圖形後端 (Cairo / GDI / Metal)         │              │
└─────────────────────────────────────────────┘              │
                                                            │
  Sidebar 面板佈局範例：                                     │
  ┌──────────────────────┐                                  │
  │  ChatView            │ ← XCustomPaintWindow             │
  ├══ splitter ══════════┤ ← XSplitterWindow ───────────────┘
  │  ChatInput           │ ← 現有 UnoControlEdit
  ├──────────────────────┤
  │  [Send] [Settings]   │ ← 現有 UnoControlButton
  └──────────────────────┘
```

---

## 5. 開發指引

### 5.1 新增 UNO IDL 介面的步驟

1. **撰寫 IDL 檔案**：放在 `offapi/com/sun/star/awt/`（通用 UI）或 `offapi/com/sun/star/text/`（Writer 專用）
2. **更新 `offapi` 的建置檔**：在對應的 `module.mk` 中加入新 IDL
3. **執行 `make offapi`**：產生 C++ header（由 `cppumaker` 自動處理）
4. **撰寫 C++ 實作**：在 `toolkit/` 或 `sw/` 中
5. **註冊 UNO service**：讓 extension 可以透過 `ServiceManager` 取得實例

### 5.2 新增 IDL 的範本

```idl
// offapi/com/sun/star/awt/XPaintHandler.idl

#ifndef com_sun_star_awt_XPaintHandler_idl
#define com_sun_star_awt_XPaintHandler_idl

#include <com/sun/star/uno/XInterface.idl>
#include <com/sun/star/awt/XGraphics2.idl>
#include <com/sun/star/awt/Rectangle.idl>

module com { module sun { module star { module awt {

interface XPaintHandler : com::sun::star::uno::XInterface
{
    void paint(
        [in] com::sun::star::awt::XGraphics2 xGraphics,
        [in] com::sun::star::awt::Rectangle aUpdateArea
    );

    long getContentHeight();
};

}; }; }; };

#endif
```

### 5.3 C++ 實作的範本

```cpp
// toolkit/source/awt/custompaintwindow.cxx

class CustomPaintWindow : public vcl::Window
{
    css::uno::Reference<css::awt::XPaintHandler> m_xHandler;

public:
    void SetPaintHandler(const css::uno::Reference<css::awt::XPaintHandler>& xHandler)
    {
        m_xHandler = xHandler;
    }

    virtual void Paint(vcl::RenderContext& rRenderContext,
                       const tools::Rectangle& rRect) override
    {
        if (!m_xHandler.is())
            return;

        // 取得 OutputDevice 的 XGraphics 包裝
        css::uno::Reference<css::awt::XGraphics2> xGraphics
            = rRenderContext.CreateUnoGraphics();

        // 轉換更新區域
        css::awt::Rectangle aUnoRect(
            rRect.Left(), rRect.Top(), rRect.GetWidth(), rRect.GetHeight());

        // 回呼 extension
        m_xHandler->paint(xGraphics, aUnoRect);
    }
};
```

### 5.4 Extension 端使用範例（Python）

```python
# Extension 實作 XPaintHandler
class MyChatPaintHandler(unohelper.Base, XPaintHandler):
    def paint(self, graphics, updateArea):
        # 繪製聊天氣泡背景
        graphics.setFillColor(0xE3F2FD)
        graphics.drawRoundedRect(10, 10, 200, 60, 8, 8)

        # 繪製文字
        graphics.setTextColor(0x1A5276)
        graphics.drawText(20, 25, "[You] Hello!")

    def getContentHeight(self):
        return 500  # 內容總高度

# 取得 CustomPaintWindow 並設定回呼
paint_window = smgr.createInstanceWithContext(
    "com.sun.star.awt.CustomPaintWindow", ctx)
paint_window.setPaintHandler(MyChatPaintHandler())
```

### 5.5 開發順序建議

```
Phase 1: XGraphics 擴充
  ├── 新增 measureText() 到 VCLXGraphics
  ├── 新增 drawTextWrapped()
  └── 驗證：Python macro 呼叫新方法，確認可用

Phase 1.5: Text Layout Contract
  ├── 新增 measureTextInRect(Rect, Text, Flags)
  ├── 暴露 used rect / line count / ellipsis 等結果
  ├── 驗證：custom paint sidebar 用 returned height 佈局
  └── 目標：縮小 drawTextInRect() 與量測結果之間的殘差

Phase 2: XCustomPaintWindow + XSplitterWindow
  ├── XCustomPaintWindow：IDL 定義 + VCL 實作 + UNO 包裝
  ├── XSplitterWindow：IDL 定義 + VCL Splitter 包裝（複雜度低，可同步進行）
  ├── 註冊為 UNO service
  └── 驗證：Python extension 建立可拖拉分隔的自訂繪製面板
      （ChatView + Splitter + ChatInput 的基本佈局）

Phase 3: XParagraphNavigator ✓ 已完成
  ├── 已完成 IDL 定義（offapi/com/sun/star/text/XParagraphNavigator.idl）
  ├── 掛在 TextDocumentView 上（optional interface）
  ├── 在 sw/source/uibase/uno/unotxvw.cxx 中實作 bridge
  ├── geometry 回傳 fragment rect sequence，而不是單一 union rect
  ├── CppUnit 驗證覆蓋：enumeration / navigation / scope / geometry / hidden
  ├── 實作 commits：d5892a688105, bd9f422e2c8f
  ├── spec：phase3-paragraph-navigator-spec.md
  ├── review：260318-phase3-xparagraphnavigator-review.md
  ├── 實作教訓：
  │   ├── getParagraphStyleName 初版誤用 UI name，需用 SwStyleNameMapper::FillProgName
  │   ├── EnterStdMode() 會清除 selection，navigation helper 中不應假設 HasMark() 成立
  │   └── 負 index 與正越界的 IndexOutOfBoundsException 都需要測試覆蓋
  └── 非目標：highlight / overlay handle / SwEditWin::Paint() 擴充

Phase 4: XDocumentOverlay
  ├── 新增 IDL 定義
  ├── 修改 SwEditWin::Paint() 加入 overlay 階段
  ├── 實作 overlay handle 管理
  ├── 承接 paragraph highlight / annotation 等視圖上層繪製需求
  └── 驗證：Python extension 在文件上繪製 overlay
```

### 5.6 上游提交策略

這些 API 擴充如果設計得當，對 LO 社群有普遍價值（所有 extension 開發者都受益）。建議：

1. **先在本地開發和驗證**，確保 API 設計穩定
2. **在 LO 開發者郵件列表討論**（libreoffice@lists.freedesktop.org），徵求 API 設計回饋
3. **分批提交 patch**：先提交 XGraphics 擴充（最小改動），再逐步提交其他部分
4. **提供完整的測試和文件**，提高被接受的機率

---

## 6. 風險與待決事項

| 項目 | 風險 | 緩解策略 |
|------|------|----------|
| UNO 回呼效能 | `paint()` 每次重繪都跨 UNO 邊界呼叫，可能有開銷 | 先量測。XGraphics 的現有方法也是 UNO 呼叫，LO 已在用 |
| SolarMutex 與 extension 線程 | Extension 的 `paint()` 在 SolarMutex 下執行，如果 extension 內部有鎖可能死鎖 | 文件中明確說明 paint() 的線程限制 |
| IDL 版本相容 | 新增 IDL 後，舊版 LO 無法使用新 API 的 extension | Extension 在初始化時檢查 API 是否存在，提供 fallback |
| SwEditWin 修改風險 | overlay 繪製階段可能影響 Writer 的繪製效能或正確性 | 只在有註冊 overlay 時才執行額外繪製 |
| XCanvas 重複 | 新 API 可能與 XCanvas 功能重疊 | XCanvas 面向進階渲染（投影片動畫），新 API 面向 UI 控制項繪製，用途不同 |
| 社群接受度 | LO 社群可能對 API 設計有不同意見 | 先在郵件列表提案討論，收集回饋後再動手 |
