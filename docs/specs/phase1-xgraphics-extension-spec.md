# Phase 1: XGraphics3 擴充開發規格書

> 版本：0.2
> 日期：2026-03-12
> 狀態：**程式碼已完成，尚未編譯驗證**（repo 未執行 autogen.sh）

---

## 1. 目標

擴充現有的 `XGraphics2` 介面，新增文字量測和自動換行繪製方法，為後續 Phase 2（XCustomPaintWindow）的自訂 sidebar 元件繪製提供足夠的「畫筆」能力。

### 1.1 現有介面缺口

| Extension 需要做的事 | VCL OutputDevice 已有 | UNO XGraphics/XGraphics2 暴露了嗎？ |
|---------------------|----------------------|-----------------------------------|
| 量測文字寬度 | `GetTextWidth()` | 否 |
| 量測文字高度 | `GetTextHeight()` | 否（只有 `getFontMetric()` 回傳 ascent/descent） |
| 計算換行後的文字矩形 | `GetTextRect(rect, str, WordBreak)` | 否 |
| 自動換行繪製多行文字 | `DrawText(rect, str, WordBreak\|MultiLine)` | 否（只有單行 `drawText(x, y, str)`） |
| 文字截斷加省略號 | `GetEllipsisString()` | 否 |
| Alpha 透明度 | `SetLineColor()`/`SetFillColor()` 支援 alpha | 已支援（現有 `setFillColor(sal_Int32)` 使用 `Color(ColorTransparency, nColor)`，top byte 即為透明度） |

### 1.2 設計決策

| 決策 | 理由 |
|------|------|
| 新增 `XGraphics3` 繼承 `XGraphics2` | 沿用 LO 慣例（XGraphics → XGraphics2），`VCLXGraphics` 只需改繼承 |
| **不新增 alpha 方法** | 現有 `setFillColor(sal_Int32)` 已用 `Color(ColorTransparency, nColor)` 建構，top byte 就是透明度，不需要新方法 |
| **範圍為 4 個新方法** | `getTextWidth`、`getTextHeight`、`measureText`、`drawTextInRect` |
| `measureText()` 回傳 struct | IDL struct 語義更清晰，Python 端使用更方便 |
| `drawTextInRect()` 用 `long Flags` | 與 VCL `DrawTextFlags` 位元值一致，值在 IDL doc 中記載 |
| 需修改 VCL `TextRectInfo` | 新增 `GetLineCount()` public getter（`mnLineCount` 原本是 private 無 getter） |

### 1.3 繼承架構

```
XInterface
  └─ XGraphics      (32 methods, 原始介面)
       └─ XGraphics2 (+2 methods: clear, drawImage, LO 4.1+)
            └─ XGraphics3 (+4 methods, 本次新增)
```

---

## 2. 已完成的變更

### 2.1 修改檔案清單

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 編輯 | `include/vcl/textrectinfo.hxx` | 加 `GetLineCount()` getter |
| 2 | 新建 | `offapi/com/sun/star/awt/TextMetrics.idl` | 量測結果 struct |
| 3 | 新建 | `offapi/com/sun/star/awt/XGraphics3.idl` | 新介面定義 |
| 4 | 編輯 | `offapi/UnoApi_offapi.mk` | 註冊兩個新 IDL |
| 5 | 編輯 | `toolkit/inc/awt/vclxgraphics.hxx` | 改繼承 XGraphics3 + 宣告新方法 |
| 6 | 編輯 | `toolkit/source/awt/vclxgraphics.cxx` | 實作 4 個新方法 + 新增 includes |

### 2.2 TextRectInfo 變更

**檔案**：`include/vcl/textrectinfo.hxx:43`

新增一行 public getter：
```cpp
sal_uInt16          GetLineCount() const { return mnLineCount; }
```

### 2.3 TextMetrics IDL struct

**檔案**：`offapi/com/sun/star/awt/TextMetrics.idl`（新建）

```idl
published struct TextMetrics
{
    long Width;          // 換行後文字的總寬度（像素）
    long Height;         // 換行後文字的總高度（像素）
    long MaxLineWidth;   // 所有行中最大的行寬（像素）
    short LineCount;     // 行數
    boolean IsEllipsis;  // 是否被截斷加省略號
};
```

### 2.4 XGraphics3 IDL 介面

**檔案**：`offapi/com/sun/star/awt/XGraphics3.idl`（新建）

繼承 `XGraphics2`，4 個新方法：

| 方法 | 回傳 | 參數 | 委派到 OutputDevice |
|------|------|------|-------------------|
| `getTextWidth` | `long` | `[in] string Text` | `GetTextWidth(str)` |
| `getTextHeight` | `long` | （無） | `GetTextHeight()` |
| `measureText` | `TextMetrics` | `[in] string Text, [in] long MaxWidth` | `GetTextRect(rect, str, WordBreak\|MultiLine, &info)` |
| `drawTextInRect` | `void` | `[in] Rectangle Rect, [in] string Text, [in] long Flags` | `DrawText(rect, str, flags)` |

`drawTextInRect` 的 Flags 常數值（與 VCL `DrawTextFlags` 一致）：

| 值 | 意義 |
|----|------|
| `0x0010` | Left（靠左，預設） |
| `0x0020` | Center（水平置中） |
| `0x0040` | Right（靠右） |
| `0x0080` | Top（靠上，預設） |
| `0x0100` | VCenter（垂直置中） |
| `0x0200` | Bottom（靠下） |
| `0x0400` | EndEllipsis（截斷加 "..."） |
| `0x1000` | MultiLine（多行） |
| `0x2000` | WordBreak（在單字邊界斷行） |

### 2.5 建置系統變更

**檔案**：`offapi/UnoApi_offapi.mk`

- `TextMetrics` 插入在 `SystemPointer` 和 `TextAlign` 之間（按字母序）
- `XGraphics3` 插入在 `XGraphics2` 和 `XImageButton` 之間（按字母序）

使用 `gb_UnoApi_add_idlfiles` macro。不需要 component 註冊（VCLXGraphics 由 `createGraphics()` 內部建立）。

### 2.6 C++ 實作

**檔案**：`toolkit/inc/awt/vclxgraphics.hxx`
- `#include <com/sun/star/awt/XGraphics2.hpp>` → `XGraphics3.hpp`
- `WeakImplHelper<css::awt::XGraphics2>` → `WeakImplHelper<css::awt::XGraphics3>`
- 新增 4 個方法宣告

**檔案**：`toolkit/source/awt/vclxgraphics.cxx`

新增 includes：
```cpp
#include <vcl/textrectinfo.hxx>
#include <com/sun/star/awt/TextMetrics.hpp>
```

每個方法遵循現有模式（`SolarMutexGuard` → `if(mpOutputDevice)` → `InitOutputDevice(flags)` → 委派）：

- `getTextWidth` / `getTextHeight`：`InitOutputDevice(FONT)` → 直接委派
- `measureText`：`InitOutputDevice(FONT)` → `GetTextRect(Rectangle(0,0,maxW,0x7FFFFFFF), text, WordBreak|MultiLine, &info)` → 填充 TextMetrics
- `drawTextInRect`：`InitOutputDevice(FONT|COLORS)` → `DrawText(Rectangle(Point,Size), text, static_cast<DrawTextFlags>(flags))`

---

## 3. 編譯與測試

> **目前狀態**：此 repo 尚未執行 `autogen.sh`，無 `config_host.mk` 和 `workdir/`，無法編譯。
> 以下為部署到有完整建置環境後的驗證步驟。

### 3.1 編譯

```bash
make offapi    # 產生 IDL headers
make toolkit   # 編譯 C++ 實作
```

驗證產生的 headers 存在：
- `workdir/UnoApiHeadersTarget/offapi/normal/com/sun/star/awt/TextMetrics.hpp`
- `workdir/UnoApiHeadersTarget/offapi/normal/com/sun/star/awt/XGraphics3.hpp`

### 3.2 Python macro 測試

編譯成功後，啟動 `./instdir/program/soffice`，開啟 Writer 文件，在 **Tools → Macros → Organize → Edit** 或 APSO 中執行以下 Python macro：

```python
import uno

def test_xgraphics3(args=None):
    ctx = uno.getComponentContext()
    desktop = ctx.ServiceManager.createInstanceWithContext(
        "com.sun.star.frame.Desktop", ctx)
    doc = desktop.getCurrentComponent()
    controller = doc.getCurrentController()

    # 取得視窗的 graphics
    frame = controller.getFrame()
    window = frame.getContainerWindow()
    peer = window.getPeer()
    graphics = peer.createGraphics()

    # 測試 1: getTextWidth / getTextHeight
    width = graphics.getTextWidth("Hello World")
    height = graphics.getTextHeight()
    print(f"Text width: {width}, height: {height}")
    assert width > 0, "getTextWidth should return positive value"
    assert height > 0, "getTextHeight should return positive value"

    # 測試 2: measureText（文字夠長應換行）
    metrics = graphics.measureText(
        "This is a long text that should wrap to multiple lines "
        "when the maximum width is narrow enough", 100)
    print(f"Wrapped: {metrics.Width}x{metrics.Height}, "
          f"{metrics.LineCount} lines, maxLineW={metrics.MaxLineWidth}")
    assert metrics.LineCount > 1, "Should wrap to multiple lines"
    assert metrics.Height > height, "Wrapped height > single line"

    # 測試 3: drawTextInRect
    FLAGS_WORDBREAK_MULTILINE = 0x1000 | 0x2000
    rect = uno.createUnoStruct("com.sun.star.awt.Rectangle")
    rect.X, rect.Y, rect.Width, rect.Height = 50, 50, 200, 300
    graphics.drawTextInRect(rect, "This is wrapped text.", FLAGS_WORDBREAK_MULTILINE)
    print("drawTextInRect completed (check window for visual output)")

    print("All tests passed!")
```

### 3.3 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | `make offapi` 成功 | 檢查 workdir 輸出 |
| 2 | `make toolkit` 成功，無編譯錯誤 | 建置日誌 |
| 3 | `getTextWidth("Hello")` 回傳正整數 | Python macro |
| 4 | `getTextHeight()` 回傳正整數 | Python macro |
| 5 | `measureText(longText, 100).LineCount > 1` | Python macro |
| 6 | `drawTextInRect()` 在視窗上可見繪製換行文字 | 目視確認 |

---

## 4. 已知風險與緩解

| 風險 | 影響 | 緩解 |
|------|------|------|
| `DrawTextFlags` 數值若未來 VCL 端修改 | Extension 行為異常 | 在 IDL 文件中固定數值定義；後續可考慮改用 constants group IDL |
| `VCLXGraphics` 改繼承 `XGraphics3` | 理論上可能影響其他程式碼 | `XGraphics3` 繼承 `XGraphics2`，`queryInterface` 同時回傳兩者，完全向後相容 |
| `TextRectInfo` 新增 public getter | 極低風險 | 只加了一個 getter，不改變類別佈局或行為 |
| `drawTextInRect` 繪製到視窗表面會被重繪覆蓋 | 預期行為 | 這是已知限制，Phase 2 的 XCustomPaintWindow 會透過 paint 回呼解決 |

---

## 5. 後續（Phase 2 銜接）

Phase 1 完成後，Phase 2 的 `XCustomPaintWindow` 的 `paint()` 回呼將傳入 `XGraphics3` 而非 `XGraphics2`，extension 在繪製回呼中即可直接使用 `measureText()` 和 `drawTextInRect()` 等新方法。
