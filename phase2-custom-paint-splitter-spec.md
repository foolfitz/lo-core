# Phase 2: XCustomPaintWindow + XSplitterWindow 開發規格書

> 版本：0.2
> 日期：2026-03-12
> 前置需求：Phase 1（XGraphics3）已完成
> 狀態：**程式碼已完成，尚未編譯驗證**（repo 未執行 autogen.sh）

---

## 1. 目標

提供兩個新的 UNO 元件，使 extension 能在 sidebar 面板中建立：

1. **XCustomPaintWindow**：支援繪製回呼的自訂視窗，extension 在 VCL 重繪週期中被回呼並使用 XGraphics3 繪製
2. **XSplitterWindow**：可拖拉分隔線，包裝 VCL `Splitter`，使用者拖拉調整相鄰區域大小

完成後 extension 可實現以下佈局：

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

---

## 2. 設計決策

| 決策 | 理由 |
|------|------|
| `XCustomPaintHandler` 回呼介面 | PaintEvent 不帶 XGraphics，需專用回呼讓 extension 在 paint 中拿到 XGraphics3 |
| `CustomPaintVCLWindow` 子類別 | Override `Paint()` 直接回呼 handler，不呼叫 base（不需要 WindowPaint event） |
| 手動捲動（`setScrollOffset`） | 先不做內建 ScrollBar，extension 自行管理 offset，Paint() 中透過 MapMode 偏移 |
| 滑鼠/鍵盤事件用現有 `XMouseListener`/`XKeyListener` | VCLXWindow 已支援，不需額外介面 |
| `WindowType::TOOLKIT_CUSTOMPAINTWINDOW = 0x1002` | 使用 toolkit 專用範圍，不改 VCL 主 enum |
| `VCLXSplitter` 利用現有 `WindowType::SPLITTER` | 只需在 switch 中補上 `*ppNewComp` |
| `VCLXWindow + ImplHelper1` 雙繼承模式 | 參考 `VCLXSpinButton`，使用 `IMPLEMENT_FORWARD_XINTERFACE2` |
| `SplitListenerMultiplexer` 在 `listenermultiplexer.hxx/.cxx` | 使用現有 DECL/IMPL 巨集，與所有其他 multiplexer 一致 |
| VCL `Link<>` → UNO listener | Override `SetWindow()` 連接 IMPL_LINK handlers |

---

## 3. 已完成的變更

### 3.1 修改檔案清單

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 新建 | `offapi/com/sun/star/awt/SplitEvent.idl` | 分隔線事件 struct |
| 2 | 新建 | `offapi/com/sun/star/awt/XSplitListener.idl` | 分隔線事件 listener |
| 3 | 新建 | `offapi/com/sun/star/awt/XSplitterWindow.idl` | 分隔線視窗介面 |
| 4 | 新建 | `offapi/com/sun/star/awt/XCustomPaintHandler.idl` | 繪製回呼介面 |
| 5 | 新建 | `offapi/com/sun/star/awt/XCustomPaintWindow.idl` | 自訂繪製視窗介面 |
| 6 | 編輯 | `offapi/UnoApi_offapi.mk` | 註冊 5 個新 IDL |
| 7 | 編輯 | `include/toolkit/helper/listenermultiplexer.hxx` | 宣告 SplitListenerMultiplexer |
| 8 | 編輯 | `toolkit/source/helper/listenermultiplexer.cxx` | 實作 SplitListenerMultiplexer |
| 9 | 新建 | `toolkit/inc/awt/vclxsplitter.hxx` | VCLXSplitter header |
| 10 | 新建 | `toolkit/source/awt/vclxsplitter.cxx` | VCLXSplitter 實作 |
| 11 | 新建 | `toolkit/inc/awt/vclxcustompaintwindow.hxx` | CustomPaintVCLWindow + VCLXCustomPaintWindow header |
| 12 | 新建 | `toolkit/source/awt/vclxcustompaintwindow.cxx` | 兩者實作 |
| 13 | 編輯 | `include/vcl/wintypes.hxx` | 新增 `TOOLKIT_CUSTOMPAINTWINDOW = 0x1002` |
| 14 | 編輯 | `toolkit/source/awt/vclxtoolkit.cxx` | window type mapping + switch cases + includes |
| 15 | 編輯 | `toolkit/Library_tk.mk` | 新增 2 個 .cxx |

### 3.2 IDL 定義

**SplitEvent** — 繼承 `EventObject`：

| 欄位 | 型別 | 說明 |
|------|------|------|
| `SplitPos` | `long` | 分隔線位置（像素） |

**XSplitListener** — 繼承 `XEventListener`，3 個方法：

| 方法 | 說明 |
|------|------|
| `splitStarted(SplitEvent)` | 開始拖拉 |
| `splitting(SplitEvent)` | 拖拉中 |
| `splitEnded(SplitEvent)` | 拖拉結束 |

**XSplitterWindow** — 繼承 `XInterface`，7 個方法：

| 方法 | 說明 |
|------|------|
| `addSplitListener` / `removeSplitListener` | listener 管理 |
| `setSplitPosition(long)` / `getSplitPosition()` | 分隔線位置 |
| `setHorizontal(boolean)` / `getHorizontal()` | 方向 |
| `setRange(long nMin, long nMax)` | 拖拉範圍 |

**XCustomPaintHandler** — 繼承 `XInterface`，1 個方法：

| 方法 | 說明 |
|------|------|
| `paint(XGraphics, Rectangle)` | 在重繪時被回呼，XGraphics 僅在呼叫期間有效 |

**XCustomPaintWindow** — 繼承 `XInterface`，7 個方法：

| 方法 | 說明 |
|------|------|
| `setPaintHandler` / `getPaintHandler` | 設定/取得 paint 回呼 |
| `repaint()` / `repaintRect(Rectangle)` | 觸發重繪 |
| `setScrollOffset(long, long)` | 設定捲動偏移（觸發重繪） |
| `getScrollOffsetX()` / `getScrollOffsetY()` | 取得捲動偏移 |

### 3.3 C++ 實作架構

**VCLXSplitter**（`namespace toolkit`）：
- `VCLXWindow + ImplHelper1<XSplitterWindow>` 雙繼承
- `SplitListenerMultiplexer maSplitListeners`
- Override `SetWindow()` 連接 VCL Splitter 的 3 個 Link callbacks
- `IMPL_LINK_NOARG` handlers → 建構 `SplitEvent` → 呼叫 multiplexer
- `setRange()` → 轉換 min/max 為 `SetDragRectPixel(Rectangle, parent)`

**CustomPaintVCLWindow**（`namespace toolkit`，public class）：
- 繼承 `vcl::Window`
- Override `Paint()` — 核心流程：
  1. MapMode origin 偏移（scroll offset）
  2. `new VCLXGraphics; Init(&rRenderContext)` 建臨時 graphics
  3. `mxHandler->paint(xGraphics, aUnoRect)` — try/catch
  4. `SetOutputDevice(nullptr)` 清理
  5. 還原 MapMode

**VCLXCustomPaintWindow**（`namespace toolkit`）：
- `VCLXWindow + ImplHelper1<XCustomPaintWindow>` 雙繼承
- 所有方法透過 `dynamic_cast<CustomPaintVCLWindow*>(GetWindow())` 委派

### 3.4 vclxtoolkit.cxx 組裝

- `constComponentTypeMapping` 新增 `{ u"custompaintwindow", WindowType::TOOLKIT_CUSTOMPAINTWINDOW }`
- `ImplCreateWindow` switch：
  - `SPLITTER` case 加 `*ppNewComp = new toolkit::VCLXSplitter`
  - 新增 `TOOLKIT_CUSTOMPAINTWINDOW` case

---

## 4. 編譯與測試

> **目前狀態**：此 repo 尚未執行 `autogen.sh`，無法編譯。

### 4.1 編譯

```bash
make offapi    # 產生 IDL headers
make toolkit   # 編譯 C++ 實作
```

### 4.2 Python macro 測試

```python
import uno
import unohelper
from com.sun.star.awt import XCustomPaintHandler, Rectangle, WindowDescriptor, WindowClass

class TestPaintHandler(unohelper.Base, XCustomPaintHandler):
    def paint(self, graphics, updateArea):
        graphics.setFillColor(0xE3F2FD)
        graphics.drawRect(10, 10, 200, 100)
        rect = Rectangle()
        rect.X, rect.Y, rect.Width, rect.Height = 20, 30, 180, 60
        graphics.drawTextInRect(rect, "Hello from custom paint!", 0x3000)

def test_phase2(args=None):
    ctx = uno.getComponentContext()
    smgr = ctx.ServiceManager
    toolkit = smgr.createInstanceWithContext("com.sun.star.awt.Toolkit", ctx)
    desktop = smgr.createInstanceWithContext("com.sun.star.frame.Desktop", ctx)
    doc = desktop.getCurrentComponent()
    frame = doc.getCurrentController().getFrame()
    parent = frame.getContainerWindow()

    # Test 1: Create splitter
    desc = WindowDescriptor()
    desc.Type = WindowClass.SIMPLE
    desc.WindowServiceName = "splitter"
    desc.Parent = parent
    splitter_peer = toolkit.createWindow(desc)
    splitter_peer.setHorizontal(True)
    splitter_peer.setSplitPosition(200)
    assert splitter_peer.getSplitPosition() == 200
    print("Splitter: OK")

    # Test 2: Create custompaintwindow
    desc2 = WindowDescriptor()
    desc2.Type = WindowClass.SIMPLE
    desc2.WindowServiceName = "custompaintwindow"
    desc2.Parent = parent
    paint_peer = toolkit.createWindow(desc2)
    handler = TestPaintHandler()
    paint_peer.setPaintHandler(handler)
    assert paint_peer.getPaintHandler() is not None
    paint_peer.setPosSize(50, 50, 300, 200, 15)
    paint_peer.setVisible(True)
    print("CustomPaintWindow: OK (check window for visual output)")

    print("All Phase 2 tests passed!")
```

### 4.3 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | `make offapi` 成功 | 5 個新 IDL header 產生 |
| 2 | `make toolkit` 成功 | 無編譯錯誤 |
| 3 | `toolkit.createWindow("splitter")` 建立成功 | Python macro |
| 4 | `setSplitPosition / getSplitPosition` 正常運作 | Python macro |
| 5 | 拖拉 splitter 時 `splitting()` 被回呼 | Python macro + 手動拖拉 |
| 6 | `toolkit.createWindow("custompaintwindow")` 建立成功 | Python macro |
| 7 | `paint()` 在視窗顯示時被回呼 | 目視確認 |
| 8 | `setScrollOffset()` 改變繪製位置 | 目視確認 |

---

## 5. 後續（Phase 3 銜接）

Phase 2 完成後，extension 可以建立完整的 sidebar 佈局：
- ChatView（`custompaintwindow`）使用 XGraphics3 繪製聊天氣泡
- Splitter 讓使用者調整上下比例
- Input（`multilineedit`）用於文字輸入

Phase 3 將新增 `XParagraphNavigator`，提供段落排版位置查詢，為文件 overlay 功能做準備。
