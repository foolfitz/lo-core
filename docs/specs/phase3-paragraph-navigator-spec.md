# Phase 3: XParagraphNavigator 開發規格書

> 版本：0.2
> 日期：2026-03-18
> 狀態：Draft
> 前置需求：Phase 1.5 已補齊 text layout contract；Phase 2 已驗證 extension 端 custom paint 佈局
> 目前 repo 狀態：已有 Phase 3 的 IDL / `sw/` bridge 初版，且 `sw/qa/uibase/uno` 已補上對應 `7.2` 的 core 驗證；後續若要再擴充，重點會是增加更多 scope / layout 變體，而不是補齊空白 contract 項目

---

## 1. 目標

在 Writer 的 `TextDocumentView` 上新增一個**view-bound 的段落導航與幾何查詢 API**，讓 extension 能先證明「段落位置資訊可被穩定暴露」，再進入 Phase 4 的 overlay 繪製。

本 phase 要解決的最小問題是：

1. extension 能以 UNO 方式取得目前 Writer 視圖中的段落數量與目前段落索引
2. extension 能跳到指定段落、前一段、下一段，並選取目前段落
3. extension 能查詢某一段落在 Writer 目前版面中的幾何資訊
4. 幾何資訊必須足以支撐後續 overlay，但本 phase **不**負責 overlay 本身

### 1.1 本 phase 要回答的核心假設

Phase 3 的真正驗證問題不是「可不可以畫 overlay」，而是：

- Writer 是否能先以穩定的 published UNO contract 暴露段落層級的 geometry snapshot？
- 如果可以，Phase 4 才有理由把 `SwEditWin::Paint()` 與 overlay lifecycle 拉進來

也就是說，Phase 3 是 **paragraph geometry contract** 的驗證 phase，而不是 overlay phase。

---

## 2. 背景與先例

### 2.1 研究報告中的需求

`uno-api-extension-research.md` 將 Phase 3 定義為 `XParagraphNavigator`，目標是提供：

- 段落導航
- 段落選取
- 段落排版位置查詢

這是 Phase 4 `XDocumentOverlay` 的前置能力。若 extension 連段落在目前視圖中的幾何位置都拿不到，overlay 只能退回脆弱的猜測式定位。

### 2.2 現有公開 UNO 先例

#### `XParagraphCursor`

`offapi/com/sun/star/text/XParagraphCursor.idl` 已提供：

- `gotoNextParagraph()`
- `gotoPreviousParagraph()`
- `gotoStartOfParagraph()`
- `gotoEndOfParagraph()`

但這是一個**model text cursor**：

- 它知道段落邊界
- 它不知道目前 Writer 視圖的 layout geometry
- 它無法回報段落在頁面/視窗中的矩形

#### `TextDocumentView` / `XTextViewCursorSupplier`

`offapi/com/sun/star/text/TextDocumentView.idl` 與 `sw/source/uibase/uno/unotxvw.cxx` 已證明 Writer controller 可以暴露 view-bound 能力，例如：

- `getViewCursor()`
- `createTextRangeByPixelPosition()`

也就是說，現有架構已經能做：

- view -> model 的 pixel hit-test
- 目前 view cursor 的位置查詢

但缺的正是**paragraph-level 的 view geometry**

#### `SwXTextViewCursor::getPosition()`

`SwXTextViewCursor::getPosition()` 只回報 caret 位置，且其語義是 cursor point，不是段落範圍。  
這對 overlay 來說不夠，因為 overlay 需要的是 paragraph area，而不是單點。

### 2.3 現有內部先例

Writer 內部已經有類似能力，但都不是 published UNO contract：

- `SwCursorShell::GetCurrFrame()` 可取得 cursor 所在 frame
- `SwTextNode` / `SwContentFrame` 可用於取得 layout frame
- `SwEditWin::LogicToPixel()` 可將 document logic 座標轉成 view pixel
- `sw/source/uibase/sidebar/QuickFindPanel.cxx` 已用 layout frame/char rect 做排序與定位

這些都說明：**能力存在於內部，但 extension 目前沒有穩定的 UNO bridge 可用**

### 2.4 為什麼不能直接沿用研究報告中的單一 `Rectangle`

研究報告中的初步方向是：

- `getParagraphBounds(long index) -> Rectangle`
- `getParagraphScreenBounds(long index) -> Rectangle`

但 published API 若只回傳單一矩形，會把真正重要的 layout 邊界藏起來：

- 段落可能跨頁
- 段落可能跨 column
- 段落可能有 follow frame

若 contract 只回傳單一 union rect，extension 會得到一個「看似簡單、其實語義很差」的矩形。  
因此本 spec 將幾何 contract 調整為：**回傳 fragment rect sequence，而不是單一 rect。**

---

## 3. 非目標

本 phase 不處理以下事項：

1. `XDocumentOverlay`、`SwEditWin::Paint()` 擴充、overlay z-order、overlay handle lifecycle
2. `setHighlight()` / `clearHighlight()` 這類把 geometry 與繪製行為綁在一起的 API
3. header/footer、footnote/endnote、text frame、shape text、comment/sidebar note 的 paragraph geometry
4. table cell 內段落的 published contract
5. layout listener、可訂閱的 geometry change event、或任何 live object
6. 單行級 / glyph 級的細部 geometry
7. 將多個 page/column fragment 強行壓成一個 union rect 的簡化語義

本 phase 的 scope 先收斂為：

- **Writer 主本文（main body text）**
- 由 `XTextDocument.getText()` 所代表的 root text paragraph 順序

這個範圍之外的 paragraph geometry，留待後續 phase 或 follow-up spec。

---

## 4. Public Contract

### 4.1 取得方式

建議新增：

- `offapi/com/sun/star/text/XParagraphNavigator.idl`

並在：

- `offapi/com/sun/star/text/TextDocumentView.idl`

加入一個新的 optional interface：

```idl
[optional] interface com::sun::star::text::XParagraphNavigator;
```

使用方式應是對目前 controller 做一般 UNO query，例如：

```cpp
uno::Reference<css::text::XParagraphNavigator> xNav(xController, uno::UNO_QUERY);
```

其他語言綁定可用對等的 query/cast 方式。  
若 query 失敗，表示目前 LibreOffice build 不支援 Phase 3 contract，extension 必須走 fallback。

### 4.2 建議 IDL 方向

```idl
published interface XParagraphNavigator : com::sun::star::uno::XInterface
{
    long getCount();

    long getCurrentIndex();

    void gotoIndex(
        [in] long Index,
        [in] boolean Select )
        raises (com::sun::star::lang::IndexOutOfBoundsException);

    boolean gotoNext( [in] boolean Select );
    boolean gotoPrevious( [in] boolean Select );

    void selectCurrentParagraph();

    sequence<com::sun::star::awt::Rectangle> getParagraphBounds(
        [in] long Index )
        raises (com::sun::star::lang::IndexOutOfBoundsException);

    sequence<com::sun::star::awt::Rectangle> getParagraphViewBounds(
        [in] long Index )
        raises (com::sun::star::lang::IndexOutOfBoundsException);

    boolean isVisible( [in] long Index )
        raises (com::sun::star::lang::IndexOutOfBoundsException);

    string getParagraphText( [in] long Index )
        raises (com::sun::star::lang::IndexOutOfBoundsException);

    string getParagraphStyleName( [in] long Index )
        raises (com::sun::star::lang::IndexOutOfBoundsException);
};
```

### 4.3 索引範圍與順序

`Index` 的 contract 必須明寫：

1. `0` 起算
2. 順序以 `XTextDocument.getText()` 的 paragraph model order 為準
3. 只涵蓋本 phase 支援範圍內的主本文 paragraph
4. 合法範圍是 `0 .. getCount()-1`

`getCurrentIndex()` 的 contract：

- 若目前 view cursor 位於本 phase 支援的主本文 paragraph，回傳其 index
- 若目前 view cursor 位於本 phase scope 之外，回傳 `-1`

這個 `-1` sentinel 必須寫進 IDL 文件，不能留給實作者自行解讀。

### 4.4 導航與選取語義

`gotoIndex()`：

- 將 Writer view cursor 移到目標段落
- `Select = false` 時，等同一般移動
- `Select = true` 時，沿用 Writer 現有 selection expand 語義，而不是定義一套新選取模型

`gotoNext()` / `gotoPrevious()`：

- 以目前 paragraph index 為基準
- 到達邊界時回傳 `false`
- 若目前 cursor 不在支援範圍內，也回傳 `false`

`selectCurrentParagraph()`：

- 目標是提供一個明確的「選取目前整段」入口
- 實作應忠於 Writer 既有 paragraph selection 行為
- 不另行定義 paragraph mark 是否包含在選取內的全新規則

也就是說，這個方法應是**既有 Writer 段落選取行為的 UNO bridge**，而不是新 selection model。

### 4.5 幾何語義

#### `getParagraphBounds()`

回傳值是：

- `sequence<awt::Rectangle>`
- 單位：**twips**
- 座標系：**Writer document logic space**

這個座標系應與以下邏輯一致：

- `SwView::GetVisArea()`
- `SwEditWin::LogicToPixel()`

也就是說，它不是：

- page-local 座標
- global screen 座標
- mm100

#### `getParagraphViewBounds()`

回傳值是：

- `sequence<awt::Rectangle>`
- 單位：**pixels**
- 座標系：**目前 `SwEditWin` client area 左上角為原點**

它不是 global screen pixel。  
若未來 extension 需要 screen pixel，應在 extension 端再做視窗層轉換，而不是把這個 phase 的 contract 綁到全域螢幕座標。

#### fragment 的定義

每個 `Rectangle` 代表一個**paragraph layout fragment**，最小保證是：

- paragraph 若分裂成多個 page/column/follow frame，必須回傳多個 rect
- 不可將多 fragment 強制壓成一個 union rect

本 phase 不承諾 line-by-line rect。  
也就是說，fragment 粒度是 paragraph layout fragment，而不是每一行文字。

#### 順序

rect sequence 的順序必須穩定：

- 以目前版面的 visual order 排列
- 一般情況下是從上到下

#### 空結果

對於合法 index，若 paragraph 目前沒有可回報的 layout fragment，回傳空 sequence 是合法的。典型情況包括：

- hidden paragraph
- layout 尚未 materialize
- 該 paragraph 在目前 view/layout 條件下沒有 frame

此時：

- `getParagraphBounds()` / `getParagraphViewBounds()` 回傳空 sequence
- `isVisible()` 回傳 `false`

### 4.6 Snapshot、生命週期與 threading

這些 API 回傳的是**snapshot**，不是 live object。

因此 caller 不可假設以下條件成立：

- scroll 後舊 rect 仍有效
- zoom 後舊 rect 仍有效
- edit 後舊 rect 仍有效
- page style / layout 變更後舊 rect 仍有效

caller 必須在 view 狀態改變後重新查詢。

所有方法都必須遵循既有 Writer UNO bridge 模式：

```cpp
SolarMutexGuard aGuard;
if (!m_pView)
    throw uno::RuntimeException();
```

Phase 3 不引入 listener/callback，因此沒有 event ordering contract。  
這反而是刻意的，因為 geometry 先以 snapshot contract 驗證，較容易控制相容性風險。

### 4.7 相容性邊界

一旦 `XParagraphNavigator` 變成 published UNO API，下列語義就很難再改：

1. index 的定義範圍
2. document twips vs view pixel 的座標系
3. fragment rect sequence 的粒度
4. `getCurrentIndex() == -1` 的約定

因此這些都必須在 Phase 3 spec 明講，不能等 C++ 實作完成後再讓 reviewer 從 code 推論。

---

## 5. 實作形狀

### 5.1 建議修改檔案

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 新建 | `offapi/com/sun/star/text/XParagraphNavigator.idl` | 新 published interface |
| 2 | 編輯 | `offapi/com/sun/star/text/TextDocumentView.idl` | 將 `XParagraphNavigator` 加為 optional interface |
| 3 | 編輯 | `offapi/UnoApi_offapi.mk` | 註冊新 IDL，並維持 alphabetical order |
| 4 | 編輯 | `sw/source/uibase/inc/unotxvw.hxx` | 將 `SwXTextView` 擴充為實作 `XParagraphNavigator` |
| 5 | 編輯 | `sw/source/uibase/uno/unotxvw.cxx` | 主要 bridge 實作 |
| 6 | 選擇性新建 | `sw/source/uibase/inc/unoparagnav.hxx` | 若需要抽 helper 類別 |
| 7 | 選擇性新建 | `sw/source/uibase/uno/unoparagnav.cxx` | geometry / index mapping helper |
| 8 | 視情況編輯 | `sw/Library_sw.mk` | 若新增 helper `.cxx` 檔，需納入 build |

### 5.2 為什麼不需要新 UNO service

本 phase 建議**不**建立新的可實例化 UNO service，也不需要 `.component` XML 註冊。

原因：

- 能力本質上屬於現有 Writer controller (`TextDocumentView`)
- 這是 view helper interface，不是獨立 service
- 現有 precedent（`XTextViewCursorSupplier`）也是直接掛在 controller 上

這可降低 service registration 與 lifecycle 複雜度。

### 5.3 `SwXTextView` 作為 bridge 入口

`sw/source/uibase/uno/unotxvw.cxx` 已經是 `TextDocumentView` 的實作核心。  
因此 Phase 3 最自然的掛點是直接擴充 `SwXTextView`。

這樣可直接重用：

- `SwView* m_pView`
- `SwWrtShell& GetWrtShell()`
- `SwEditWin`
- 既有 `Invalidate()` lifecycle

### 5.4 paragraph index mapping

本 phase 最敏感的內部問題不是矩形轉換，而是：

**如何把 published 的 paragraph index 穩定映射到主本文 paragraph。**

建議原則：

1. 以主本文 paragraph 的 model order 建立 index
2. mapping 的定義要與 `XTextDocument.getText()` 一致
3. 不要偷偷把 header/footer/footnote/table cell 混進同一組 index

也就是說，先讓 index scope 清楚、可測，再考慮擴大覆蓋範圍。

### 5.5 geometry 取得方向

幾何查詢的典型路徑可參考：

```text
SwXTextView
  -> SwView
    -> SwWrtShell
      -> layout / text node / content frame
        -> one or more paragraph fragments
          -> SwRect
            -> UNO awt::Rectangle
```

對 pixel 版本：

```text
document twip rect
  -> SwEditWin::LogicToPixel()
    -> awt::Rectangle
```

關鍵原則：

- published contract 先以 document twip rect 為主
- pixel rect 是 view convenience API
- 兩者都必須來自**同一份 layout snapshot**

### 5.6 selection/navigation 實作方向

導航與選取應盡量沿用 Writer 既有 cursor / selection 路徑，而不是自己發明新行為：

- `gotoIndex()` 對齊現有 view cursor 移動模式
- `selectCurrentParagraph()` 對齊 Writer 現有 paragraph selection 行為

這樣可以減少：

- UNO 與 UI 行為不一致
- reviewer 難以判斷 contract 是否破壞既有 selection semantics

---

## 6. 驗證計畫

### 6.1 Phase 3 的 stage hypothesis

**Hypothesis**  
Writer 可以在不修改 overlay 繪製路徑的前提下，先穩定暴露 paragraph-level geometry snapshot contract。

**Entry condition**

- Phase 1.5 已讓 extension 有可依賴的 text layout measurement contract
- Phase 2 已證明 extension custom paint/sidebar path 可工作

**Fallback**

- 若 `XParagraphNavigator` 不存在，extension 只使用既有 `XParagraphCursor`
- paragraph geometry / overlay anchoring 功能停用
- 不嘗試用脆弱的 pixel guess 代替正式 geometry contract

**Exit note**

- Phase 3 完成只代表 paragraph geometry contract 成立
- 不代表 overlay paint ordering、invalidations、z-order 已經被驗證
- 這些仍屬於 Phase 4

### 6.2 自動化驗證

建議最小 build / test 組合：

```bash
make offapi
make sw
make CppunitTest_sw_uibase_uno
make CppunitTest_sw_core_crsr
make CppunitTest_sw_unowriter
```

#### 建議新增的 CppUnit 驗證點

放置方向：

- `sw/qa/uibase/uno/uno.cxx`
- `sw/qa/core/crsr/crsr.cxx`
- 或 `sw/qa/extras/unowriter/unowriter.cxx`

至少應覆蓋：

1. `CurrentController` 可 `UNO_QUERY` 到 `XParagraphNavigator`
2. `getCount()` 與主本文 paragraph enumeration 一致
3. `gotoIndex()` 會把 view cursor 移到預期段落
4. `gotoNext()` / `gotoPrevious()` 在邊界返回 `false`
5. `getCurrentIndex()` 在支援範圍內回傳穩定 index
6. `getParagraphBounds()` 對跨多 fragment 的 paragraph 回傳多個 rect，而不是單一 union rect
7. `getParagraphViewBounds()` 與 `getParagraphBounds()` 來自同一份 layout snapshot
8. hidden / 無 layout frame 的 paragraph 回傳空 sequence

### 6.3 extension-side / macro 驗證

建議用最小 Python macro 驗證 capability probe 與基本查詢：

```python
import uno

def test_phase3(args=None):
    ctx = uno.getComponentContext()
    desktop = ctx.ServiceManager.createInstanceWithContext(
        "com.sun.star.frame.Desktop", ctx)
    doc = desktop.getCurrentComponent()
    controller = doc.getCurrentController()

    xNav = controller  # Python 端以動態屬性 / queryInterface 取得
    if not hasattr(xNav, "getParagraphBounds"):
        print("XParagraphNavigator not available; fallback path should be used")
        return

    count = xNav.getCount()
    print("paragraph count =", count)
    assert count >= 1

    idx = xNav.getCurrentIndex()
    print("current index =", idx)

    rects = xNav.getParagraphBounds(0)
    view_rects = xNav.getParagraphViewBounds(0)
    print("doc rects =", len(rects), "view rects =", len(view_rects))
```

### 6.4 手動驗證

至少做以下情境：

1. 一般單段文字：回傳 1 個 doc rect + 1 個 view rect
2. 長段落自動換行：仍可回傳穩定 geometry
3. 跨頁或 follow frame 段落：回傳多個 fragment rect
4. scroll 後重新查詢：doc rect 不因 viewport 改變而改語義，view rect 會更新
5. zoom 後重新查詢：舊 rect 不可重用，重新查詢可得到新 snapshot

---

## 7. 驗收標準

| # | 標準 | 驗證方式 |
|:-:|------|----------|
| 1 | `XParagraphNavigator` 可從 Writer `CurrentController` 取得 | CppUnit / Python macro |
| 2 | `getCount()` 與主本文 paragraph 順序一致 | CppUnit |
| 3 | `gotoIndex()`、`gotoNext()`、`gotoPrevious()` 忠於既有 Writer 導航語義 | CppUnit |
| 4 | `getCurrentIndex()` 在 scope 內穩定，scope 外回傳 `-1` | CppUnit |
| 5 | `getParagraphBounds()` 的單位與座標系明確為 document twips | IDL 文件 + CppUnit |
| 6 | `getParagraphViewBounds()` 明確為 view client pixel，而非 global screen pixel | IDL 文件 + 手動驗證 |
| 7 | 多 fragment paragraph 不會被壓成單一 union rect | CppUnit / 手動驗證 |
| 8 | empty-sequence 行為與 `isVisible()` 一致 | CppUnit |
| 9 | extension capability probe 與 fallback path 已在文件中明確定義 | spec + macro |
| 10 | Phase 3 與 Phase 4 的邊界清楚，沒有把 overlay/highlight contract 偷渡進來 | spec review |

### 7.1 驗收責任切分

Phase 3 的驗收不應只靠單一證據來源關帳。  
由於 `XParagraphNavigator` 是 **published UNO contract**，但它的設計初衷同時又是給 Python extension 消費，因此驗收應分成兩條線：

- `LO-core`：負責關帳 public contract correctness
- `markdown-insert-ext`：負責關帳 consumer-side capability probe、fallback 與 workflow viability

建議切分如下：

| 驗收項 | 主要關帳位置 | 原因 |
|--------|--------------|------|
| `#1` `CurrentController` 可取得 `XParagraphNavigator` | `LO-core` + extension | core 要證明 controller bridge 存在；extension 要證明 Python 端真的拿得到 |
| `#2` `getCount()` 與主本文順序一致 | `LO-core` | 這是 paragraph scope / enumeration contract，不應只靠 consumer 外觀推定 |
| `#3` `gotoIndex()` / `gotoNext()` / `gotoPrevious()` 忠於 Writer 導航語義 | `LO-core` 為主，extension 輔助 | core 關帳 cursor semantics；extension 補真實 workflow 手感 |
| `#4` scope 外 `getCurrentIndex() == -1` | `LO-core` | 必須覆蓋 table/header/footer/footnote 等邊界 |
| `#5` `getParagraphBounds()` 為 document twips | `LO-core` | 屬於公開座標語義，需由 IDL + CppUnit 關帳 |
| `#6` `getParagraphViewBounds()` 為 view client pixel | extension 為主，`LO-core` 輔助 | 真正容易驗的是 scroll / zoom / re-query 後的使用者視角行為 |
| `#7` 多 fragment paragraph 不壓成 union rect | `LO-core` 為主，extension 輔助 | core 要覆蓋 follow frame / 跨頁；extension 補真實重查與視覺定位 |
| `#8` empty-sequence 與 `isVisible()` 一致 | `LO-core` | hidden / no-layout-frame 是 contract 邊界，不應只靠 UI 觀察 |
| `#9` capability probe 與 fallback path | extension | 這是 consumer integration contract，應在真實 Python 路徑驗證 |
| `#10` Phase 3 / 4 邊界清楚 | spec review | 屬於規格與設計邊界，不是執行時驗證 |

### 7.2 `LO-core` 必關帳項

以下項目若沒有對應 CppUnit / core 測試，Phase 3 不應宣告完整驗收：

1. main body paragraph enumeration 與 index mapping
2. `gotoIndex()` / `gotoNext()` / `gotoPrevious()` 的邊界與 selection 語義
3. scope 外 `getCurrentIndex() == -1`
4. `IndexOutOfBoundsException` 的 range contract
5. document twips / view pixel 的 public 座標語義
6. multi-fragment paragraph 回傳多個 rect
7. hidden / no-layout-frame paragraph 的 empty-sequence + `isVisible() == false`

### 7.3 extension 必關帳項

以下項目應在 `markdown-insert-ext` 內以 capability-based 驗證入口關帳：

1. `CurrentController` 對 Python `queryInterface()` 可穩定取得 `XParagraphNavigator`
2. 新 API 缺席時不崩潰，且 extension 走明確 fallback
3. sample document / 真實文件下可查到 paragraph count、current index、doc/view rect
4. scroll 後重新 query：view rect 更新、doc rect 不改語義
5. zoom 後重新 query：舊 snapshot 不重用，新 query 反映新 view geometry

建議在 extension 端提供一個 developer-only 驗證入口，例如：

- `pythonpath/paragraph_navigator_validation.py`
- 開發旗標控制 sample document、capability probe、log 輸出
- 使用同一份 `/tmp/md-sidebar-debug.log` 與既有 staged validation 流程對齊

---

## 8. 風險與後續

| 風險 | 說明 | 緩解 |
|------|------|------|
| index scope 定義過寬 | 若一開始把 table cell、header/footer、footnote 全混進來，之後很難維持穩定 mapping | 先限制在主本文 paragraph |
| geometry 粒度定義不清 | 若只說「paragraph bounds」卻沒說 fragment sequence，之後使用者會依賴錯誤語義 | 在 IDL 文件中明寫 fragment rect sequence |
| doc / view 座標系混淆 | `screen bounds` 用語太模糊，容易誤導成 global screen pixel | 改稱 `getParagraphViewBounds()`，明寫以 `SwEditWin` client area 為原點 |
| hidden paragraph / 無 frame 情況 | valid paragraph 不一定總有 geometry | 明定 empty-sequence contract |
| Phase 3 偷混 overlay | 若本 phase 加入 highlight handle，API 邊界會變得太大 | 把所有繪製與 lifecycle 相關 contract 留到 Phase 4 |

### 8.1 Deferred Work

以下議題不在本 phase 解決：

- table cell paragraph geometry
- header/footer/footnote/endnote paragraph geometry
- paragraph geometry change listener
- richer fragment metadata（例如 page number、column id、fragment kind）
- paragraph overlay/highlight handle
- `SwEditWin::Paint()` overlay 階段

換句話說，**Phase 3 完成後，extension 應該能穩定「知道段落在哪裡」；Phase 4 才負責「在那裡畫東西」。**
