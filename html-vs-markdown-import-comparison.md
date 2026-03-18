# LibreOffice HTML 與 Markdown 匯入架構比較分析

本文件比較 LibreOffice Writer 中 HTML 匯入過濾器與 Markdown 匯入過濾器的架構差異，基於 26.2 版原始碼分析。

---

## 1. 總覽

| 面向 | HTML 匯入 | Markdown 匯入 |
|---|---|---|
| 原始碼位置 | `sw/source/filter/html/` | `sw/source/filter/md/` |
| 程式碼規模 | ~47,645 行（43 個檔案，含匯出） | ~2,906 行（8 個檔案，含匯出） |
| 解析器 | 自製 tokenizer（`svtools` 內建） | 第三方 md4c 函式庫（外部靜態連結） |
| CSS 支援 | 完整的 CSS1 解析管線（3 層架構） | 無 |
| 歷史 | 可追溯至 StarOffice 時代（1990s） | LibreOffice 26.2 新增 |

HTML 過濾器的規模約為 Markdown 過濾器的 **16 倍**，反映了 HTML 規格本身的複雜度。

---

## 2. 解析器架構

### 2.1 HTML：自製 Token 驅動解析器

HTML 匯入使用 LibreOffice 自行實作的多層解析器，繼承鏈如下：

```
SvParser<HtmlTokenId>       (svtools/svparser.cxx)  — 泛型串流狀態機
  └── HTMLParser             (svtools/parhtml.cxx)   — HTML tokenizer
        └── SfxHTMLParser    (sfx2/sfxhtml.cxx)      — SFX 整合層
              └── SwHTMLParser (sw/source/filter/html/swhtml.cxx) — Writer 特化實作
```

**運作方式：**
1. `SvParser` 逐字元讀取位元組串流，透過 `rtl_TextToUnicodeConverter` 處理編碼轉換
2. `HTMLParser::GetNextToken_()` 對串流做 lexing，將 HTML 標籤名稱對照關鍵字表（`svtools/htmlkywd.hxx`），回傳 `HtmlTokenId` 列舉值
3. `HTMLParser::Continue()` 迴圈呼叫 `GetNextToken()` → `NextToken()`
4. `SwHTMLParser::NextToken()` 是核心分派中樞：一個巨大的 `switch` 依 `HtmlTokenId` 呼叫各個專用處理函式

### 2.2 Markdown：外部函式庫回呼驅動

Markdown 匯入委託給第三方 C 函式庫 md4c（CommonMark 相容解析器）：

```
MarkdownReader::Read()
  → SwMarkdownParser::CallParser()
    → md_parse(buffer, size, &parser, this)   // md4c 外部函式
      → enter_block_callback / leave_block_callback
      → enter_span_callback / leave_span_callback
      → text_callback
```

**運作方式：**
1. 整個檔案一次讀入記憶體（`m_pArr`）
2. 設定 `MD_PARSER` 結構體，包含 5 個靜態回呼函式指標
3. 呼叫 `md_parse()`，md4c 驅動解析，透過回呼通知 LibreOffice
4. 回呼函式建構 Writer 文件

### 2.3 關鍵差異

| 面向 | HTML | Markdown |
|---|---|---|
| 解析器來源 | 自行實作（~5,000+ 行） | 外部 md4c 函式庫 |
| 解析模式 | Token-by-token 逐步處理 | 整個檔案一次解析，回呼通知 |
| 非同步支援 | 支援（`SvParserState::Pending` 機制，可暫停/恢復） | 不支援（同步一次完成） |
| 解析器旗標 | 無特殊旗標 | `MD_DIALECT_GITHUB \| MD_FLAG_WIKILINKS` |
| 錯誤恢復 | Tokenizer 內建容錯（寬鬆解析不合規 HTML） | md4c 內部處理，失敗回傳 `ERRCODE_IO_GENERAL` |

---

## 3. 進入點與註冊方式

### HTML

```cpp
// fltini.cxx — Reader 建立
ReadHTML = new HTMLReader;

// swhtml.cxx — Reader::Read() 實作
ErrCode HTMLReader::Read(SwDoc& rDoc, ...) {
    // 建立 SwHTMLParser 並呼叫 CallParser()
}
```

Filter 名稱：`FILTER_HTML = "HTML (StarWriter)"_ustr`

### Markdown

```cpp
// fltini.cxx — Reader 建立
ReadMarkdown = new MarkdownReader;
SetFltPtr(READER_WRITER_MD, ReadMarkdown);

// swmd.cxx — Reader::Read() 實作
ErrCode MarkdownReader::Read(SwDoc& rDoc, ...) {
    // 建立 SwMarkdownParser 並呼叫 CallParser()
}
```

Filter 名稱：`FILTER_MD = "Markdown"_ustr`

兩者都透過 `iodetect.cxx` 的 `aFilterDetect[]` 陣列和 `fltini.cxx` 的 `aReaderWriter[]` 陣列註冊，遵循相同的過濾器架構。

---

## 4. 屬性/格式堆疊機制

這是兩個過濾器差異最顯著的地方。

### 4.1 HTML：三層交錯資料結構

HTML 過濾器使用複雜的三層屬性管理系統：

**第一層 — `HTMLAttr`**（`swhtml.hxx:132`）：
- 代表一個開啟中（或剛關閉）的字元/段落屬性
- 儲存：`SfxPoolItem`（屬性值）、起始 `SwNodeIndex` + 偏移、結束位置、有效性旗標
- 透過 `m_pNext` 鏈結（處理同類型重疊屬性）和 `m_pPrev`（已關閉但未提交的屬性）

**第二層 — `HTMLAttrTable`**（`swhtml.hxx:83`）：
- 扁平結構體，每種屬性型別一個指標插槽（約 30 個：`pBold`、`pItalic`、`pUnderline`、`pFont`、`pAdjust` 等）
- 代表「目前開啟的屬性」表
- 可透過 `shared_ptr` 在進入表格儲存格時儲存/還原

**第三層 — `HTMLAttrContext`**（`swhtml.hxx:211`）：
- 追蹤特定 HTML 標籤所開啟的項目（段落樣式、邊距、間距、以及所屬的 `HTMLAttr*` 清單）
- 堆疊於 `m_aContexts`（`vector<unique_ptr<HTMLAttrContext>>`）

**生命週期：**
1. `NewAttr()` — 配置 `HTMLAttr`，鏈入對應插槽
2. `PushContext()` — 推入上下文
3. 關閉標籤時 — 設定 `HTMLAttr` 的結束位置，移入 `m_aSetAttrTab`（「已關閉、未套用」）
4. `SetAttr_()` — 遍歷 `m_aSetAttrTab`，呼叫 `SwDoc::InsertPoolItem()` 套用

**特殊處理：** 關閉字元屬性時，使用 ICU `BreakIterator` 在 CJK/CTL/Latin 文字邊界分割 run，套用對應的 `RES_CHRATR_CJK_*` / `RES_CHRATR_CTL_*` 變體。

### 4.2 Markdown：簡單向量堆疊

Markdown 使用單一的屬性堆疊：

```cpp
MDAttrStack m_aAttrStack;  // vector<unique_ptr<SfxPoolItem>>
```

**生命週期：**
1. `enter_span_callback` — 將對應的 `SfxPoolItem` 推入堆疊
2. `InsertText()` — 將堆疊中所有項目作為字元屬性套用到插入的文字
3. `leave_span_callback` — 從堆疊彈出

### 4.3 比較

| 面向 | HTML | Markdown |
|---|---|---|
| 資料結構 | 三層（HTMLAttr + HTMLAttrTable + HTMLAttrContext） | 單一 vector 堆疊 |
| 同類型重疊 | 支援（鏈結串列處理） | 不需要（Markdown 語法不允許） |
| CJK/CTL 文字分割 | 支援（ICU BreakIterator） | 不支援 |
| 表格儲存格隔離 | 支援（save/restore 機制） | 不需要（md4c 回呼自然隔離） |
| 延遲套用 | 支援（先記錄範圍，後批次套用） | 即時套用（插入文字時直接套用） |

---

## 5. 區塊層級元素處理

### 5.1 HTML 支援的區塊元素

| HTML 元素 | 處理函式 | 映射目標 |
|---|---|---|
| `<P>` | `NewPara()` / `EndPara()` | 依 `class=` 決定段落樣式 |
| `<H1>` – `<H6>` | `NewHeading()` / `EndHeading()` | `RES_POOLCOLL_HEADLINE1` – `6` |
| `<BLOCKQUOTE>` | `NewTextFormatColl()` | `RES_POOLCOLL_HTML_BLOCKQUOTE` |
| `<PRE>`, `<LISTING>`, `<XMP>` | `NewTextFormatColl()` | `RES_POOLCOLL_HTML_PRE` |
| `<UL>`, `<OL>`, `<DIR>`, `<MENU>` | `NewNumberBulletList()` | `SwNumRule` |
| `<LI>` | `NewNumberBulletListItem()` | 套用 numbering |
| `<DL>`, `<DT>`, `<DD>` | `NewDefList()` / `NewDefListItem()` | 定義清單樣式 |
| `<TABLE>` | `BuildTable()` | `SwDoc::InsertTable()` |
| `<DIV>`, `<CENTER>` | `NewDivision()` / `EndDivision()` | 區段/對齊 |
| `<HR>` | `InsertHorzRule()` | 水平線 |
| `<ADDRESS>` | `NewTextFormatColl()` | `RES_POOLCOLL_SEND_ADDRESS` |
| `<FORM>`, `<INPUT>`, `<SELECT>` 等 | 表單控制項系列函式 | UNO 表單控制項 |
| `<APPLET>`, `<OBJECT>`, `<EMBED>` | `InsertObject()` 系列 | OLE 嵌入物件 |

### 5.2 Markdown 支援的區塊元素

| Markdown 語法 | md4c 區塊型別 | 處理方法 | 映射目標 |
|---|---|---|---|
| 段落 | `MD_BLOCK_P` | `StartPara()` / `EndPara()` | `RES_POOLCOLL_TEXT` |
| `#` – `######` | `MD_BLOCK_H` | `StartHeading()` / `EndHeading()` | `RES_POOLCOLL_HEADLINE1` – `6` |
| `>` | `MD_BLOCK_QUOTE` | `AddBlockQuote()` / `EndBlockQuote()` | `RES_POOLCOLL_HTML_BLOCKQUOTE` |
| `` ``` `` | `MD_BLOCK_CODE` | `BeginCodeBlock()` / `EndCodeBlock()` | `RES_POOLCOLL_HTML_PRE` + 灰色背景 |
| `- ` / `1. ` | `MD_BLOCK_UL/OL` | `StartNumberedBulletList()` | `SwNumRule` |
| `- [x]` | `MD_BLOCK_LI` | `StartNumberedBulletListItem()` | `SwContentControl`（核取方塊） |
| 表格 | `MD_BLOCK_TABLE` | `StartTable()` / `EndTable()` | `SwDoc::InsertTable()` |
| `---` | `MD_BLOCK_HR` | `AddHR()` | 水平線 |
| HTML 區塊 | `MD_BLOCK_HTML` | `BeginHtmlBlock()` / `EndHtmlBlock()` | 委託給 HTML 過濾器 `ReadHTML` |

### 5.3 比較

| 面向 | HTML | Markdown |
|---|---|---|
| 支援元素數量 | ~20+ 種區塊元素 | 8 種區塊元素 |
| 表單控制項 | 完整支援（input、select、textarea 等） | 僅 task list checkbox |
| OLE/外掛 | 支援（applet、object、embed） | 不支援 |
| 定義清單 | 支援（dl/dt/dd） | 不支援 |
| 巢狀表格 | 原生支援 | 不支援（匯出時攤平） |
| HTML 回退 | — | 支援（`MD_BLOCK_HTML` 委託給 HTML 過濾器） |

---

## 6. 行內格式處理

### 6.1 HTML 支援的行內格式

| HTML 標籤 | 映射的 `SfxPoolItem` |
|---|---|
| `<B>`, `<STRONG>` | `SvxWeightItem(WEIGHT_BOLD)` + CJK/CTL 變體 |
| `<I>`, `<EM>`, `<CITE>` | `SvxPostureItem(ITALIC_NORMAL)` + CJK/CTL 變體 |
| `<U>`, `<INS>` | `SvxUnderlineItem(LINESTYLE_SINGLE)` |
| `<S>`, `<STRIKE>`, `<DEL>` | `SvxCrossedOutItem(STRIKEOUT_SINGLE)` |
| `<SUP>` | `SvxEscapementItem(HTML_ESC_SUPER)` |
| `<SUB>` | `SvxEscapementItem(HTML_ESC_SUB)` |
| `<CODE>`, `<TT>` | `SvxFontItem`（等寬字型） |
| `<FONT>` | `SvxFontItem` / `SvxFontHeightItem` / `SvxColorItem` |
| `<A href="...">` | `SwFormatINetFormat` |
| `<IMG>` | Fly frame + graphic node |
| `<SPAN style="...">` | 依 CSS 解析結果動態決定 |
| `<BR>` | 換行符號 |
| `<RUBY>`, `<RBC>`, `<RT>` | Ruby 標注（旁注）屬性 |

### 6.2 Markdown 支援的行內格式

| Markdown 語法 | md4c Span 型別 | 映射的 `SfxPoolItem` |
|---|---|---|
| `**bold**` | `MD_SPAN_STRONG` | `SvxWeightItem(WEIGHT_BOLD)` |
| `*italic*` | `MD_SPAN_EM` | `SvxPostureItem(ITALIC_NORMAL)` |
| `` `code` `` | `MD_SPAN_CODE` | 從 `RES_POOLCOLL_HTML_PRE` 樣式複製字型 |
| `~~del~~` | `MD_SPAN_DEL` | `SvxCrossedOutItem(STRIKEOUT_SINGLE)` |
| `<u>text</u>` | `MD_SPAN_U` | `SvxUnderlineItem(LINESTYLE_SINGLE)` |
| `[text](url)` | `MD_SPAN_A` | `SwFormatINetFormat` |
| `![alt](url)` | `MD_SPAN_IMG` | `InsertImage()` + 大小限制 |
| `[[wiki]]` | `MD_SPAN_WIKILINK` | `SwFormatINetFormat`（Wikipedia URL） |

### 6.3 比較

| 面向 | HTML | Markdown |
|---|---|---|
| 格式種類 | ~15+ 種（含 CJK/CTL 變體） | 8 種 |
| CJK/CTL 變體 | 粗體、斜體各有 3 個變體（Latin/CJK/CTL） | 僅 Latin 變體 |
| 上下標 | 支援 | 不支援 |
| 自訂字型/大小/顏色 | 支援（`<FONT>` + CSS） | 不支援 |
| Ruby 標注 | 支援 | 不支援 |
| Wikilink | 不支援 | 支援（`[[...]]` → Wikipedia URL） |

---

## 7. CSS 支援

這是兩者間最大的功能差距。

### HTML：完整的 CSS1 解析管線

HTML 過濾器有一個 **3 層 CSS 處理架構**：

```
CSS1Parser (parcss1.cxx)         — 純 CSS lexer/parser
  └── SvxCSS1Parser (svxcss1.cxx)  — CSS 屬性 → SfxPoolItem 轉換
        └── SwCSS1Parser (htmlcss1.cxx) — Writer 樣式系統整合
```

**支援的 CSS 屬性分類：**

| 分類 | 屬性 |
|---|---|
| 字型 | `font-family`, `font-style`, `font-variant`, `font-weight`, `font-size`, `font` |
| 顏色/背景 | `color`, `background`, `background-color` |
| 文字 | `letter-spacing`, `text-decoration`, `text-align`, `text-indent`, `line-height`, `text-transform`, `white-space` |
| 盒模型 | `margin-*`, `padding-*`, `border-*`, `width`, `max-width`, `height`, `float` |
| 排版 | `position`, `left`, `top`, `column-count`, `display`, `visibility` |
| 分頁 | `page-break-before`, `page-break-after`, `page-break-inside`, `size`, `widows`, `orphans` |
| 其他 | `direction`, `list-style-type` |

**CSS 套用途徑：**
1. `<STYLE>` 區塊 — 儲存後於 `<BODY>` 時解析
2. `<LINK>` 外部樣式表
3. 行內 `style=` 屬性

### Markdown：無 CSS 支援

Markdown 過濾器不解析任何 CSS。所有視覺呈現完全依賴 Writer 的內建 pool 樣式（`RES_POOLCOLL_*`）。

唯一的外觀自訂方式是透過 **模板功能**：匯入時指定 `.ott` 或 `.docx` 模板檔案，覆蓋 pool 樣式的視覺定義。

```json
{ "TemplateURL": "file:///path/to/template.ott" }
```

---

## 8. 表格處理

### HTML

- 實作於 `htmltab.cxx`（**5,227 行**，單一檔案）
- 支援巢狀表格
- 支援 `colspan`、`rowspan` 合併儲存格
- 支援 `<CAPTION>`
- 支援 CSS 邊框、背景色、寬度
- 支援 `<THEAD>`、`<TBODY>`、`<TFOOT>`
- 使用 `HTMLTable` 中間資料結構追蹤合併和巢狀
- 屬性表在進入/離開儲存格時 save/restore

### Markdown

- 實作於 `mdtab.cxx`（~80 行）
- 僅支援 GFM（GitHub Flavored Markdown）表格
- 不支援合併儲存格
- 支援欄位對齊（左/中/右，透過 `MD_ALIGN` → `SvxAdjust` 映射）
- 使用 `MDTable` 追蹤目前列/欄位置
- md4c 在第一次解析前就已算出總列數和欄數

| 面向 | HTML | Markdown |
|---|---|---|
| 程式碼量 | ~5,200 行 | ~80 行 |
| 合併儲存格 | 支援 | 不支援 |
| 巢狀表格 | 支援 | 不支援 |
| 欄位對齊 | CSS + `align` 屬性 | `:`-separator 語法 |
| 表頭 | `<TH>` / `<THEAD>` | 第一列自動為表頭 |

---

## 9. 編碼處理

### HTML：多層編碼偵測

依優先順序：
1. HTTP `Content-Type` 標頭的 `charset` 參數
2. 明確的 UTF-8 旗標（由過濾器匯入器設定）
3. 預設 UTF-8
4. `<meta charset="...">` 或 `<meta http-equiv="Content-Type" content="...; charset=...">` — 但僅在新舊編碼都是單位元組時才切換

使用 `rtl_TextToUnicodeConverter` 逐字元即時轉換。

### Markdown：僅 UTF-8

Markdown 過濾器將整個檔案讀入 `char[]` 陣列，直接作為 UTF-8 傳給 md4c。不做編碼偵測或轉換。

---

## 10. 圖片處理

### HTML

- `InsertImage()`（`htmlgrin.cxx:323`）
- 讀取 `src`、`width`、`height`、`align`、`alt`、`border`、`usemap` 屬性
- 支援 CSS `float` 定位
- 支援影像地圖（`usemap`）
- 建立 fly frame + graphic node
- 無明確的大小上下限

### Markdown

- `InsertImage()`（`swmd.cxx`）
- 讀取 `url`、`title`、`desc`（alt text）、`link`（包裹的超連結）
- 有明確的大小限制：
  - 最小：500 twips（~1.75cm）
  - 最大：5000 twips（~17.5cm）
- 支援 base64 data URI 嵌入圖片
- 呼叫 `allowAccessLink()` 驗證 referer

---

## 11. 特殊功能比較

| 功能 | HTML | Markdown |
|---|---|---|
| 表單控制項 | 完整支援 | 僅 task list（`SwContentControl` checkbox） |
| 追蹤修訂（Redline） | 不處理（匯入時不保留） | 不處理（匯入時不保留） |
| 嵌入式 HTML | — | 支援（`MD_BLOCK_HTML` → 委託 `ReadHTML`） |
| 模板套用 | 不支援 | 支援（JSON `TemplateURL` 選項） |
| Wikilink | 不支援 | 支援（`[[target]]` → Wikipedia URL） |
| 安全檢查 | URL 過濾 | `allowAccessLink()` + `IsExoticProtocol()` |
| 非同步載入 | 支援（`SvParserState::Pending`） | 不支援 |
| 表單欄位 | `<SDFIELD>` LibreOffice 專用欄位 | 不支援 |
| 書籤/錨點 | `<A name="...">` | 不支援 |
| 腳註/尾註 | 不支援（HTML 本身不支援） | 不支援 |

---

## 12. 效能特性

| 面向 | HTML | Markdown |
|---|---|---|
| 記憶體模式 | 串流式逐字元處理 | 整個檔案載入記憶體 |
| 解析複雜度 | 較高（tokenizer + 狀態機 + CSS 解析） | 較低（md4c 單次掃描） |
| 屬性管理開銷 | 較高（三層資料結構 + save/restore） | 較低（簡單堆疊） |
| 非同步能力 | 支援暫停/恢復（適用於大檔案） | 不支援 |

---

## 13. 錯誤處理

| 情境 | HTML | Markdown |
|---|---|---|
| 解析失敗 | Tokenizer 內建容錯，寬鬆處理 | 回傳 `ERRCODE_IO_GENERAL` |
| 圖片載入失敗 | 靜默跳過 | 靜默跳過 |
| 不認識的標籤/語法 | 忽略標籤，保留文字內容 | md4c 視為普通文字 |
| 編碼錯誤 | 使用替代字元 | 不處理（假設 UTF-8） |

---

## 14. 總結

HTML 匯入過濾器是一個成熟、功能完整但龐大的系統，歷經數十年演化，支援幾乎所有 HTML4/CSS1 功能。它的架構複雜度主要來自三個方面：(1) 自製解析器的維護成本、(2) 完整 CSS 支援的分層架構、(3) 表格合併/巢狀的處理邏輯。

Markdown 匯入過濾器則是一個精簡、現代的實作。它透過委託第三方 md4c 函式庫處理解析，專注在 md4c 回呼與 Writer 文件模型之間的映射。程式碼量僅為 HTML 過濾器的 1/16，但覆蓋了 Markdown 語法的所有主要元素。遇到 HTML 區塊時，它甚至能委託給 HTML 過濾器處理，實現了務實的功能擴展。

兩者共用相同的 Writer 文件模型介面（`SwDoc`、`SwPaM`、`IDocumentContentOperations` 等）和相同的 pool 樣式系統（`RES_POOLCOLL_*`），這確保了不同格式匯入後文件的一致性。
