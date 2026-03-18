# 擴充 LibreOffice UNO API 以支援 Extension 自訂 UI 繪製

> 版本：0.1 draft
> 日期：2026-03-27
> 作者：Jiajun
> 分支：ext-uno-api-26-2-1

---

## 1. 動機

### 1.1 為什麼要做這件事

我想開發一個 LibreOffice extension，功能類似於 VSCode 中的 Claude Code 或 Cline——一個**可以和 AI 互動的 sidebar**。使用者在 Writer 中撰寫文件時，能透過 sidebar 與 AI 對話，AI 能即時在文件上標注、高亮段落、顯示建議，提供類似於現代 AI 程式碼編輯器的體驗。

然而，在實際嘗試後發現，**現有的 UNO API 在 extension UI 自訂性上有嚴重不足**：

| 我需要的能力 | 現狀 |
|-------------|------|
| 在 sidebar 中自訂繪製聊天氣泡、程式碼區塊 | UNO 只提供標準 widget（Button、Edit 等），無法自訂繪製 |
| 在文件上疊加繪製段落高亮、標籤 | 沒有 overlay API，extension 無法在文件視圖上疊加內容 |
| 量測和自動換行繪製文字 | XGraphics/XGraphics2 只有單行 `drawText()`，沒有量測、沒有換行 |
| 查詢段落在畫面上的位置 | 沒有 paragraph geometry API |

**這些缺口不是單一 extension 的特殊需求**，而是所有想做進階 UI 的 extension 都會遇到的共同限制。因此，我選擇從核心程式碼下手，新增通用的 UNO API，讓任何 extension 都能利用這些能力。

### 1.2 設計策略

```
LO 核心 (改一次)                    Extension (持續迭代)
┌──────────────────────┐           ┌──────────────────────┐
│ 新增 UNO IDL 介面    │           │ Python 或 C++ .oxt   │
│ 新增 VCL 橋接實作    │ ← UNO →  │ 實作回呼介面         │
│ 在 toolkit/ 和 sw/   │           │ 透過新 API 自訂繪製  │
│ 中實作               │           │ 獨立發佈、版本獨立   │
└──────────────────────┘           └──────────────────────┘
```

核心修改一次，產出通用 API。之後所有 extension 都能使用，extension 維持 .oxt 獨立發佈。

### 1.3 涉及範圍

本提案涵蓋兩個層級的 API 擴充：

- **Toolkit 層**（通用，所有應用程式可用）：繪圖能力增強、自訂繪製視窗、分隔視窗
- **Writer 層**（Writer 專用）：段落導航與幾何查詢、文件 overlay 繪製

下面按開發順序分 5 個 phase 說明。

---

## 2. Phase 1 + 1.5：XGraphics3 — 補齊文字量測與自動換行繪製

### 問題

Extension 要做自訂繪製（例如 sidebar 中的聊天氣泡），需要量測文字大小和自動換行。但現有的 `XGraphics` / `XGraphics2` 只有：

- `drawText(x, y, text)`：只能畫單行文字
- `getFontMetric()`：只回傳 ascent / descent，無法量測具體字串

VCL 的 `OutputDevice` 早就有 `GetTextWidth()`、`GetTextHeight()`、`GetTextRect()`、`DrawText(rect, flags)` 等方法，但**從未透過 UNO API 暴露給 extension**。

另外，extension 要做 layout 計算，需要確保「量測」和「繪製」使用完全相同的輸入和演算法——`measureText(text, maxWidth)` 和 `drawTextInRect(rect, text, flags)` 的輸入不一致，無法保證 layout 一致性。

### 方案

新增 `XGraphics3` 介面，繼承 `XGraphics2`（沿用 LO 慣例），增加 5 個方法：

| 新方法 | 對應 VCL 方法 | 用途 |
|--------|-------------|------|
| `getTextWidth(text)` | `OutputDevice::GetTextWidth()` | 量測字串像素寬度 |
| `getTextHeight()` | `OutputDevice::GetTextHeight()` | 量測目前字體行高 |
| `measureText(text, maxWidth)` | `OutputDevice::GetTextRect()` | 計算自動換行後的實際矩形（以 maxWidth 為限） |
| `measureTextInRect(rect, text, flags)` | `OutputDevice::GetTextRect()` | 以與 `drawTextInRect()` **完全相同**的輸入計算 layout，保證結果一致 |
| `drawTextInRect(rect, text, flags)` | `OutputDevice::DrawText(rect, flags)` | 自動換行繪製多行文字 |

`measureTextInRect()` 解決了 layout 一致性問題：extension 先呼叫它量測，再呼叫 `drawTextInRect()` 繪製，兩者輸入參數完全相同，layout 演算法一致，不會有位移或截斷誤差。

C++ 實作在 `VCLXGraphics` 中，橋接模式與現有方法完全相同：取 SolarMutex → 同步狀態 → 呼叫 OutputDevice。

### 狀態

程式碼已完成。IDL 定義於 `offapi/com/sun/star/awt/XGraphics3.idl`，VCL 橋接實作於 `toolkit/source/awt/vclxgraphics.cxx`。

---

## 3. Phase 2：XCustomPaintWindow + XSplitterWindow — Sidebar 自訂繪製

### 問題

Extension 的 sidebar panel 只能用 UNO 標準 widget 組合（Button、Edit、Label 等），無法做自訂繪製。要實現聊天氣泡、程式碼區塊、語法高亮等 UI，需要一個能接收 paint callback 的自訂視窗。

### 方案

新增兩個 UNO 元件：

**XCustomPaintWindow**：支援繪製回呼的自訂視窗
- Extension 實作 `XCustomPaintHandler`，在 VCL paint 週期中被回呼
- 使用 `XGraphics3` 繪製
- 支援滑鼠和鍵盤事件轉發
- 另提供明確的重繪控制（`repaint()`、`repaintRect()`）和 content-coordinate 捲動偏移管理（`setScrollOffset()`、`getScrollOffsetX()`、`getScrollOffsetY()`）

**XSplitterWindow**：可拖拉分隔線
- 包裝 VCL `Splitter`
- 使用者可拖拉調整相鄰區域大小

完成後的 sidebar 佈局：

```
┌──────────────────────┐
│  ChatView            │ ← XCustomPaintWindow（自訂繪製聊天記錄）
│                      │
├══ splitter ══════════┤ ← XSplitterWindow（使用者可拖拉）
│  ChatInput           │ ← 現有 UnoControlEdit
├──────────────────────┤
│  [Send] [Settings]   │ ← 現有 UnoControlButton
└──────────────────────┘
```

### 狀態

程式碼已完成。`XCustomPaintWindow` 實作於 `toolkit/source/awt/`，`XSplitterWindow` 同。IDL 定義於 `offapi/com/sun/star/awt/`。

---

## 4. Phase 3：XParagraphNavigator — 段落導航與幾何查詢

### 問題

Extension 要在文件上標注特定段落（高亮、標籤），首先需要知道段落在畫面上的位置。但現有 UNO API 只有 `XText` / `XTextRange` 等文件模型 API，**沒有 view 層的段落幾何查詢**。

### 方案

在 Writer 的 `TextDocumentView` 上新增 `XParagraphNavigator` 介面：

| 方法 | 用途 |
|------|------|
| `getCount()` | 取得段落數量 |
| `getCurrentIndex()` | 取得游標所在段落的索引 |
| `gotoIndex(index)` | 跳到指定段落 |
| `gotoNext(select)` / `gotoPrevious(select)` | 前後導航，可選擇性延伸選取範圍 |
| `selectCurrentParagraph()` | 以 view 的正常選取行為選取目前段落 |
| `getParagraphBounds(index)` | 段落的版面片段矩形（document twips）；回傳 `sequence<Rectangle>`，因為段落可能跨越多個版面片段 |
| `getParagraphViewBounds(index)` | 同 `getParagraphBounds()`，但以螢幕像素座標回傳 |
| `isVisible(index)` | 段落是否有任何部分在目前可視區域中 |
| `getParagraphText(index)` | 段落的純文字內容 |
| `getParagraphStyleName(index)` | 段落套用的段落樣式名稱 |

**範圍限制**：本介面只覆蓋**本文主體**的段落。表格、文字框、頁首、頁尾、註腳等非主體容器中的段落不在目前範圍內。

幾何資訊來自 Writer 的排版引擎（`SwFrame` 樹），以 document twips 為單位，與文件座標系一致。

### 狀態

程式碼已完成，CppUnit 測試通過。實作於 `sw/source/uibase/uno/unotxvw.cxx`，IDL 定義於 `offapi/com/sun/star/text/XParagraphNavigator.idl`。

---

## 5. Phase 4：XDocumentOverlay — 文件 Overlay 繪製

### 問題

Extension 需要在 Writer 文件的編輯區域上方疊加繪製（段落高亮、邊框、標籤等），且不修改文件內容。這需要一個 view-bound 的 overlay paint API。

### 方案

在 `TextDocumentView` 上新增 `XDocumentOverlay` 介面：

```
Extension                           Writer 核心
┌─────────────────┐                ┌─────────────────────┐
│ 實作             │                │                     │
│ XOverlayPainter  │◄── callback ──│ paint 週期中回呼     │
│ ├ paintOverlay() │                │                     │
│ └ getOverlayBounds()│             │                     │
└─────────────────┘                └─────────────────────┘
```

**XDocumentOverlay** 方法：

| 方法 | 用途 |
|------|------|
| `addOverlay(painter, layer)` | 註冊一個 overlay painter，回傳 handle |
| `removeOverlay(handle)` | 移除 overlay |
| `invalidateOverlay(area)` | 觸發 overlay 重繪。IDL 中的 `area` 參數為未來的區域精確重繪預留；目前實作會對整個 overlay 物件執行全域失效，忽略傳入的 area。 |
| `setOverlayVisible(handle, visible)` | 控制可見性 |
| `isOverlayVisible(handle)` | 查詢目前可見性狀態 |

**XOverlayPainter**（Extension 實作）：

| 方法 | 用途 |
|------|------|
| `paintOverlay(graphics, visibleArea)` | 在給定的 XGraphics 上繪製 overlay |
| `getOverlayBounds()` | 回報 overlay 佔用的矩形範圍（document twips）。允許回傳空 sequence；此時 view 會 fallback 到整個可見區域，並仍正常呼叫 `paintOverlay()`。 |

### 狀態

IDL 已完成並發布，CppUnit 測試通過。Phase 4 驗證了 overlay paint lifecycle 的基本可行性。

---

## 6. Phase 5：OverlayObject 橋接 — 解決 Overlay 存活性問題

### 問題

Phase 4 的實作在游標閃爍和 partial repaint 時，overlay 會被 VCL 的 `OverlayManagerBuffered` 擦除。這是因為 VCL 維護一個 background save/restore 循環，**只有註冊到 OverlayManager 的 OverlayObject 實例**才能在此循環中存活。Phase 4 使用的 hook-based 直接繪圖不在此系統保護範圍內。

這個問題經歷了多次嘗試（方向 F1–F4、G1–G3），窮盡所有「在 paint pipeline 中直接畫到 OutputDevice」的方案，全部失敗後，才確認必須走 OverlayObject 路線。

### 方案

建立 `OverlayExtensionPainter` 類別，繼承 `sdr::overlay::OverlayObject`，遵循 Writer 內部 `OverlayRanges` 等類別的成熟模式：

1. **OverlayObject 註冊**：透過 `OverlayManager::add()` 註冊，讓 VCL 知道這個物件需要在 background restore 循環中重繪
2. **Bitmap 橋接**：在 `createOverlayObjectPrimitive2DSequence()` 中，建立 VirtualDevice → 呼叫 `XOverlayPainter::paintOverlay()` → 擷取 Bitmap → 包裝為 `BitmapPrimitive2D` 回傳
3. **透明度**：使用 `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` 背景，未繪製區域保持透明
4. **MapMode 追蹤**：override `getOverlayObjectPrimitive2DSequence()` 偵測 zoom/scroll 時 MapMode 的變化，自動清除 primitive cache 並重建

### 開發過程中解決的關鍵 Bug

| Bug | 根因 | 修正 |
|-----|------|------|
| Zoom 後 overlay 位移 | OverlayManager 被銷毀/重建，但 `m_bOverlayRegistered` 仍為 true | 檢查實際 OverlayManager pointer 而非只看 flag；偵測 MapMode 變化時重置 primitive cache |
| GetBitmap 取得空白圖片 | `GetBitmap()` 以 MapMode 的邏輯座標解讀參數 | 呼叫前 `EnableMapMode(false)`，以 pixel 座標傳入 |
| 白色背景遮蓋文字 | `DeviceFormat::WITHOUT_ALPHA` 無透明通道 | 改用 `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` |
| Transform 矩陣錯誤 | `B2DHomMatrix::scale().translate()` 產生後乘效果 | 改用 `set()` 直接設定矩陣元素 |
| 右方/下方邊框被裁切 | VirtualDevice 邊界上的像素落在有效範圍外 | Bounding rect 加 40 twips margin |

### 狀態

**已完成並驗證**。Overlay 在游標閃爍、打字、捲動、zoom 變更時保持可見、位置正確、背景透明。

技術規格詳見 `docs/phase5-study/H2A-overlay-object-bridge-spec.md`（rev.4）。

---

## 7. 變更檔案總覽

### Toolkit 層（Phase 1, 2）

| 類別 | 檔案 | 說明 |
|------|------|------|
| IDL | `offapi/com/sun/star/awt/XGraphics3.idl` | 文字量測、自動換行、精確 layout contract |
| IDL | `offapi/com/sun/star/awt/TextLayoutMetrics.idl` | `measureTextInRect()` 的回傳型別 |
| IDL | `offapi/com/sun/star/awt/XCustomPaintWindow.idl` | 自訂繪製視窗 |
| IDL | `offapi/com/sun/star/awt/XCustomPaintHandler.idl` | paint 回呼介面 |
| IDL | `offapi/com/sun/star/awt/XSplitterWindow.idl` | 分隔線視窗 |
| C++ | `toolkit/inc/awt/vclxgraphics.hxx` | XGraphics3 橋接 header |
| C++ | `toolkit/source/awt/vclxgraphics.cxx` | XGraphics3 橋接實作 |
| C++ | `toolkit/source/awt/` | XCustomPaintWindow / XSplitterWindow 實作 |
| Test | `toolkit/qa/cppunit/CustomWindow.cxx` | Toolkit CppUnit 測試 |

### Writer 層（Phase 3, 4, 5）

| 類別 | 檔案 | 說明 |
|------|------|------|
| IDL | `offapi/com/sun/star/text/XParagraphNavigator.idl` | 段落導航與幾何查詢 |
| IDL | `offapi/com/sun/star/text/XDocumentOverlay.idl` | Overlay 管理 |
| IDL | `offapi/com/sun/star/text/XOverlayPainter.idl` | Overlay 繪製回呼 |
| IDL | `offapi/com/sun/star/text/TextDocumentView.idl` | Service 宣告（新介面掛載點） |
| C++ | `sw/source/uibase/uno/unotxvw.cxx` | XParagraphNavigator / XDocumentOverlay 橋接 |
| C++ | `sw/source/uibase/docvw/OverlayExtensionPainter.cxx` | OverlayObject 橋接子類別 |
| C++ | `sw/source/uibase/inc/OverlayExtensionPainter.hxx` | 同上 header |
| C++ | `sw/source/uibase/inc/unotxvw.hxx` | SwXTextView 擴充 |
| C++ | `sw/Library_sw.mk` | 將 OverlayExtensionPainter 加入建置 |
| C++ | `sw/CppunitTest_sw_uibase_uno.mk` | 測試建置設定 |
| Test | `sw/qa/uibase/uno/uno.cxx` | CppUnit 測試 |

---

## 8. 目前狀態與未來方向

### 目前狀態

- Phase 1–4 的 IDL 和 C++ 橋接已完成
- Phase 5 的 OverlayObject 橋接已完成並驗證（overlay 存活性、透明度、zoom 追蹤）
- CppUnit 測試覆蓋 overlay 的 add/remove/invalidate/paint callback/partial repaint 等場景；另有重入（reentrancy）防護測試，驗證實作在誤用時不崩潰（注意：published contract 明確禁止在 `paintOverlay()` 內呼叫 `addOverlay`/`removeOverlay`/`setOverlayVisible`，重入測試是防禦性驗證而非背書的使用模式）
- 分支 `ext-uno-api-26-2-1` 上可建置、可測試

### 未來改善方向

1. **Bitmap → 向量 primitive**（中期）：目前的 bitmap 橋接在高 DPI 或大幅放大時可能有解析度問題。可改用 GDIMetaFile 錄製 → 轉換為向量 Primitive2D，天生透明且放大不失真。

2. **Overlay 滑鼠互動**：目前 overlay 僅供顯示，未來可加入 hit-testing 支援點擊和 hover 事件。

3. **其他應用程式模組**：目前 Writer 層 API（Phase 3–5）只在 `sw/` 中實作。類似的 overlay 和導航 API 可擴展到 Calc 和 Impress。

4. **Extension 端參考實作**：完成一個展示用的 Python extension，示範如何使用上述所有 API 建構 AI sidebar + 段落標注功能。

---

## 10. 關於開發方式的說明

這個專案的開發大量借助了 AI 工具（Claude Code）。作為一個非傳統社群開發者，我對 LibreOffice 的核心程式碼並不像資深開發者那麼熟悉，但透過 AI 輔助的研究、程式碼閱讀、和迭代開發，能夠深入理解 VCL / OverlayManager / drawinglayer 等內部機制，並產出可運作的實作。

每個 phase 都遵循「研究 → 撰寫規格 → 確認 → 實作 → 記錄結果」的流程，確保每一步都有文件可追溯。所有的 spec 文件都在 `docs/` 目錄下。

我歡迎任何關於 API 設計、實作方式、或社群流程的建議和回饋。
