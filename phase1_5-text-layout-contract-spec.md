# Phase 1.5: Text Layout Contract 擴充開發規格書

> 版本：0.1
> 日期：2026-03-14
> 狀態：Draft
> 前置需求：Phase 1（`XGraphics3`）已完成；Phase 2 驗證已暴露 residual layout mismatch

---

## 1. 目標

在現有 `XGraphics3` 之上補一層更精確的文字 layout contract，讓 extension 能以**與 `drawTextInRect()` 完全相同的輸入條件**進行量測，取得實際 used bounds。

### 1.1 要解決的問題

目前 API 組合是：

- `measureText(text, maxWidth)`
- `drawTextInRect(rect, text, flags)`

這對 extension 來說仍有一個缺口：

- `measureText()` 不知道最終繪製使用的 `Rectangle`
- `measureText()` 不知道 `DrawTextFlags`
- extension 無法得到「這段文字在這個 rect + flags 下，實際會占用多少高度」

實際驗證顯示：

- Stage 1 (`ScrollableChatView + FixedText`) 有可見誤差
- Stage 2 (`XCustomPaintWindow`) 已大幅改善，但仍有 residual mismatch

因此，這個問題不是 `XParagraphNavigator` 或 `XDocumentOverlay` 的範圍，而是 **Phase 1 的延伸缺口**。

### 1.2 非目標

本 phase 不處理：

- 文件 overlay
- 段落螢幕座標查詢
- rich text / mixed-format inline layout
- reusable UNO text layout object
- 字元級 hit-test / caret positioning

若未來仍需要更細緻的 line/glyph 資訊，再另開後續 phase。

---

## 2. 設計方向

### 2.1 核心決策

| 決策 | 理由 |
|------|------|
| 直接擴充 `XGraphics3` | `XGraphics3` 仍在研究/驗證階段，直接補齊 contract 較精簡 |
| 新增 `measureTextInRect(Rect, Text, Flags)` | 對齊 `drawTextInRect()` 的輸入條件 |
| 回傳 `TextLayoutMetrics` struct | 讓 Python extension 易於使用，不引入額外 UNO object lifecycle |
| 沿用 `DrawTextFlags` 整數值 | 與現有 `drawTextInRect()` 一致，學習成本最低 |
| 先不做 reusable `XTextLayout` object | 先用最小 scope 解決 sidebar height/layout mismatch |

### 2.2 新增 IDL

建議新增：

- `offapi/com/sun/star/awt/TextLayoutMetrics.idl`

#### `TextLayoutMetrics`

```idl
published struct TextLayoutMetrics
{
    long Width;                // used rect width
    long Height;               // used rect height
    long MaxLineWidth;         // widest laid-out line
    unsigned short LineCount;  // total laid-out lines
    boolean IsEllipsis;        // whether ellipsis was applied
    Rectangle UsedRect;        // actual used text rect in device coordinates
};
```

`Width` / `Height` 與 `UsedRect.Width` / `UsedRect.Height` 故意保留重複，讓 Python 端可直接讀常用欄位而不必額外拆 `Rectangle`。

#### `XGraphics3` 新增方法

```idl
published interface XGraphics3 : com::sun::star::awt::XGraphics2
{
    TextLayoutMetrics measureTextInRect(
        [in] Rectangle Rect,
        [in] string Text,
        [in] long Flags );
};
```

### 2.3 contract 定義

`measureTextInRect()` 必須滿足以下語義：

1. 使用與 `drawTextInRect()` 相同的 `Rect` 和 `Flags`
2. 使用目前已選取的 font 與 text color state
3. 回傳的 `UsedRect` 必須對應於 `drawTextInRect()` 在同條件下實際使用的 layout bounds
4. `LineCount`、`MaxLineWidth`、`IsEllipsis` 應與實際繪製路徑一致

也就是說，extension 應能：

1. `metrics = graphics.measureTextInRect(rect, text, flags)`
2. 用 `metrics.Height` 決定 block height
3. 再用同一組 `rect + text + flags` 呼叫 `drawTextInRect()`

並預期兩者高度一致，而不是只「大致接近」。

---

## 3. C++ 實作方向

### 3.1 主要修改檔案

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 新建 | `offapi/com/sun/star/awt/TextLayoutMetrics.idl` | 新量測結果 struct |
| 2 | 編輯 | `offapi/com/sun/star/awt/XGraphics3.idl` | 補 `measureTextInRect()` |
| 3 | 編輯 | `offapi/UnoApi_offapi.mk` | 註冊新 IDL |
| 4 | 編輯 | `toolkit/inc/awt/vclxgraphics.hxx` | 宣告方法 |
| 5 | 編輯 | `toolkit/source/awt/vclxgraphics.cxx` | 實作 `measureTextInRect()` |

### 3.2 實作原則

`measureTextInRect()` 應直接委派到 VCL：

```cpp
tools::Rectangle aInput = vcl::unohelper::ConvertToVCLRect(Rect);
DrawTextFlags nFlags = static_cast<DrawTextFlags>(Flags);
TextRectInfo aInfo;
tools::Rectangle aResult = mpOutputDevice->GetTextRect(aInput, rText, nFlags, &aInfo);
```

然後將結果填入 `TextLayoutMetrics`：

- `Width = aResult.GetWidth()`
- `Height = aResult.GetHeight()`
- `UsedRect = aResult`
- `MaxLineWidth = aInfo.GetMaxLineWidth()`
- `LineCount = aInfo.GetLineCount()`
- `IsEllipsis = aInfo.IsEllipses()`

### 3.3 為何補進 `XGraphics3`

Phase 1 的 `measureText(text, maxWidth)` 仍保留原語義。  
Phase 1.5 的目標不是改寫 `measureText()`，而是補一個 **更精確、與 rect + flags 對齊的量測 API**。由於 `XGraphics3` 仍在研究/驗證階段，這次直接補進 `XGraphics3`，避免多開一個只差一個方法的新世代介面。

---

## 4. Extension 端驗證目標

以 `markdown-insert-ext` 為主要驗證對象：

### 4.1 Stage 1 路徑

- `ScrollableChatView` 若仍需保留 `FixedText` fallback
- 可用 `measureTextInRect()` 比對 `FixedText.getPreferredSize()` 與實際顯示高度差異
- 幫助定位 peer padding / text area 差異

### 4.2 Stage 2 路徑

- `CustomPaintChatView` 直接以 `measureTextInRect()` 回傳的 `Height` 與 `UsedRect` 決定 block bounds
- 使用與 `drawTextInRect()` 完全相同的 `TextRect` 和 `Flags`
- 驗證長段落 / code block / mixed CJK 時，空白區塊明顯縮小

### 4.3 驗證重點

- 長段落在窄 sidebar 下不再出現明顯過高 block
- resize 後重算高度仍與實際繪製一致
- user / ai / insertable 三種 block 都能穩定對齊
- scrollbar 範圍與內容總高度誤差縮小

---

## 5. Python macro 驗證

編譯成功後，可用以下 macro 做基本驗證：

```python
import uno

FLAGS = 0x0010 | 0x0080 | 0x1000 | 0x2000

def test_xgraphics3(args=None):
    ctx = uno.getComponentContext()
    desktop = ctx.ServiceManager.createInstanceWithContext(
        "com.sun.star.frame.Desktop", ctx)
    doc = desktop.getCurrentComponent()
    frame = doc.getCurrentController().getFrame()
    peer = frame.getContainerWindow().getPeer()
    graphics = peer.createGraphics()

    rect = uno.createUnoStruct("com.sun.star.awt.Rectangle")
    rect.X = 0
    rect.Y = 0
    rect.Width = 180
    rect.Height = 1000

    text = (
        "This is a long paragraph that should wrap across multiple "
        "lines and return the same used bounds that drawTextInRect() "
        "will use during rendering."
    )

    metrics = graphics.measureTextInRect(rect, text, FLAGS)
    print(
        "used=%dx%d lineCount=%d maxLine=%d ellipsis=%s" % (
            metrics.Width,
            metrics.Height,
            metrics.LineCount,
            metrics.MaxLineWidth,
            metrics.IsEllipsis,
        )
    )

    assert metrics.Height > 0
    assert metrics.LineCount > 1
```

---

## 6. 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | `make offapi` 成功 | 新 IDL header 產生 |
| 2 | `make toolkit` 成功 | 無編譯錯誤 |
| 3 | `measureTextInRect()` 可由 Python 呼叫 | Python macro |
| 4 | `measureTextInRect()` 對同一組 rect + flags 回傳穩定 `LineCount` / `Height` | Python macro |
| 5 | `markdown-insert-ext` Stage 2 block 高度比 Phase 1/2 現況更貼近實際繪製 | 手動驗證 |
| 6 | 此問題被明確歸類為 text layout contract，而非 paragraph geometry / overlay 問題 | 文件與驗證結論 |

---

## 7. 風險與後續

| 風險 | 說明 | 緩解 |
|------|------|------|
| `GetTextRect()` 與 `DrawText()` 在特定 backend 仍有微差 | 可能仍殘留小誤差 | 先以 `measureTextInRect()` 驗證；若仍不足，再規劃 reusable text layout object |
| `DrawTextFlags` 被 extension 傳入不合理組合 | 量測與繪製結果不易解讀 | 沿用既有 `drawTextInRect()` 的 flags contract，不新增新旗標體系 |
| Python extension 仍需自行決定 block padding | API 只回傳文字 used rect，不含 UI padding | 將 padding 明確留在 extension 端控制 |

若 Phase 1.5 完成後仍有明顯 mismatch，下一步才考慮更大的設計：

- `XGraphics4` / `XTextLayout`
- reusable layout object
- line-level / glyph-level metrics
- text hit-test / caret support

也就是說，**Phase 1.5 是一個小而關鍵的補洞 phase，而不是取代後續 Phase 3 / 4。**
