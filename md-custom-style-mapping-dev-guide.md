# Markdown 過濾器自訂樣式映射 — 開發參考文件

## 1. 目標

將 Markdown 匯入/匯出過濾器中的硬編碼樣式映射，從 LibreOffice 內建 pool 樣式改為公司自訂樣式名稱。確保 Markdown ↔ Writer 文件之間的雙向轉換一致。

---

## 2. 需修改的檔案清單

| 檔案 | 角色 | 修改範圍 |
|---|---|---|
| `sw/source/filter/md/swmd.cxx` | 匯入主程式 | 段落樣式映射（標題、blockquote、code block、HR、預設段落） |
| `sw/source/filter/md/mdcallbcks.cxx` | md4c 回呼 | inline code 字型來源（從 `RES_POOLCOLL_HTML_PRE` 取字型） |
| `sw/source/filter/md/wrtmd.cxx` | 匯出主程式 | 段落樣式反向識別（pool ID → Markdown 語法） |
| `sw/qa/filter/md/md.cxx` | 單元測試 | 新增/修改測試驗證自訂樣式映射 |

---

## 3. 現有樣式映射總覽

### 3.1 匯入端（Markdown → Writer 樣式）

以下是目前所有 `GetTextCollFromPool()` 呼叫，即 pool ID 硬編碼的位置：

| Markdown 元素 | 目前 Pool ID | 程式碼位置 | 函式 |
|---|---|---|---|
| `# H1` | `RES_POOLCOLL_HEADLINE_BASE + 1` | `swmd.cxx:284-285` | `StartHeading()` |
| `## H2` | `RES_POOLCOLL_HEADLINE_BASE + 2` | `swmd.cxx:284-285` | `StartHeading()` |
| `### H3` | `RES_POOLCOLL_HEADLINE_BASE + 3` | `swmd.cxx:284-285` | `StartHeading()` |
| `#### H4` | `RES_POOLCOLL_HEADLINE_BASE + 4` | `swmd.cxx:284-285` | `StartHeading()` |
| `##### H5` | `RES_POOLCOLL_HEADLINE_BASE + 5` | `swmd.cxx:284-285` | `StartHeading()` |
| `###### H6` | `RES_POOLCOLL_HEADLINE_BASE + 6` | `swmd.cxx:284-285` | `StartHeading()` |
| `> blockquote` | `RES_POOLCOLL_HTML_BLOCKQUOTE` | `swmd.cxx:218` | `AddBlockQuote()` |
| `` ``` code ``` `` | `RES_POOLCOLL_HTML_PRE` | `swmd.cxx:583` | `BeginCodeBlock()` |
| `---` 水平線 | `RES_POOLCOLL_HTML_HR` | `swmd.cxx:249` | `AddHR()` |
| 一般段落 (fallback) | `RES_POOLCOLL_TEXT` | `swmd.cxx:236,257,297,598,845` | 多處 |
| inline `` `code` `` 字型來源 | `RES_POOLCOLL_HTML_PRE` | `mdcallbcks.cxx:209` | `enter_span_callback` |

### 3.2 匯出端（Writer 樣式 → Markdown 語法）

匯出是透過 `GetPoolFormatId()` 比對段落的 pool ID 來判斷要輸出什麼 Markdown 語法：

| 判斷條件 | 輸出 Markdown | 程式碼位置 | 說明 |
|---|---|---|---|
| `RES_POOLCOLL_HEADLINE1` ~ `6` | `#` ~ `######` | `wrtmd.cxx:673-696` | 遍歷樣式繼承鏈 `DerivedFrom()` |
| `RES_POOLCOLL_HTML_BLOCKQUOTE` | `> ` 前綴 | `wrtmd.cxx:660` | 直接比對 `pFormatColl` |
| `RES_POOLCOLL_HTML_PRE` | ` ``` ` 圍欄 | `wrtmd.cxx:740,746,861,866` | 前後段落比對，判斷 code block 邊界 |
| `RES_POOLCOLL_HTML_HR` | `___\n` | `wrtmd.cxx:697-703` | 在 heading switch 內 |
| `RES_POOLCOLL_HTML_PRE` 字型比對 | `` ` `` inline code | `wrtmd.cxx:275-287` | 比對字型是否與 HTML_PRE 樣式相同 |

---

## 4. 修改方案：逐項說明

### 4.1 匯入 — `swmd.cxx`

#### 4.1.1 標題（`StartHeading`）

**檔案**: `sw/source/filter/md/swmd.cxx`，第 277-287 行

**現有程式碼**:
```cpp
void SwMarkdownParser::StartHeading(sal_uInt8 nLvl)
{
    if (m_pPam->GetPoint()->GetContentIndex())
        AppendTextNode(AM_SPACE);
    else
        AddParSpace();

    SwTextFormatColl* pColl = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(
        RES_POOLCOLL_HEADLINE_BASE + nLvl);
    m_xDoc->SetTextFormatColl(*m_pPam, pColl);
}
```

**修改方式**: 改為按名稱查找自訂樣式，找不到時 fallback 到原本的 pool 樣式。

```cpp
void SwMarkdownParser::StartHeading(sal_uInt8 nLvl)
{
    if (m_pPam->GetPoint()->GetContentIndex())
        AppendTextNode(AM_SPACE);
    else
        AddParSpace();

    // 嘗試查找自訂樣式，例如 "custom-h1", "custom-h2", ...
    OUString aCustomName = u"custom-h"_ustr + OUString::number(nLvl);
    SwTextFormatColl* pColl = m_xDoc->FindTextFormatCollByName(aCustomName);
    if (!pColl)
    {
        // Fallback 到內建 pool 樣式
        pColl = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(
            RES_POOLCOLL_HEADLINE_BASE + nLvl);
    }
    m_xDoc->SetTextFormatColl(*m_pPam, pColl);
}
```

> **注意**: `FindTextFormatCollByName()` 定義在 `sw/inc/doc.hxx:818`，接受 `UIName` 參數，在文件的 `mpTextFormatCollTable` 中查找。如果自訂樣式尚未存在於文件中（例如沒有透過範本載入），此函式會回傳 `nullptr`。

#### 4.1.2 Blockquote（`AddBlockQuote`）

**檔案**: `sw/source/filter/md/swmd.cxx`，第 210-224 行

**現有程式碼**:
```cpp
void SwMarkdownParser::AddBlockQuote()
{
    // ...
    SwTextFormatColl* pColl
        = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_BLOCKQUOTE);
    m_nBlockQuoteDepth++;
    m_xDoc->SetTextFormatColl(*m_pPam, pColl);
}
```

**修改方式**: 同樣模式，替換為自訂樣式名稱。

```cpp
    SwTextFormatColl* pColl = m_xDoc->FindTextFormatCollByName(u"custom-blockquote"_ustr);
    if (!pColl)
        pColl = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_BLOCKQUOTE);
```

#### 4.1.3 Code Block（`BeginCodeBlock`）

**檔案**: `sw/source/filter/md/swmd.cxx`，第 575-588 行

**現有程式碼**:
```cpp
void SwMarkdownParser::BeginCodeBlock()
{
    // ...
    SwTextFormatColl* pColl
        = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
    m_xDoc->SetTextFormatColl(*m_pPam, pColl);

    SvxBrushItem aBrushItem(COL_CODE_BLOCK, RES_BACKGROUND);
    m_xDoc->getIDocumentContentOperations().InsertPoolItem(*m_pPam, aBrushItem);
}
```

**修改方式**:
```cpp
    SwTextFormatColl* pColl = m_xDoc->FindTextFormatCollByName(u"custom-code-block"_ustr);
    if (!pColl)
        pColl = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
```

> **注意**: 如果自訂樣式已包含背景色設定，可考慮移除後面的 `SvxBrushItem` 設定，避免覆蓋自訂樣式的背景色。

#### 4.1.4 水平線（`AddHR`）

**檔案**: `sw/source/filter/md/swmd.cxx`，第 243-251 行

```cpp
    SwTextFormatColl* pColl = m_xDoc->FindTextFormatCollByName(u"custom-hr"_ustr);
    if (!pColl)
        pColl = m_xDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_HR);
```

#### 4.1.5 一般段落 Fallback

一般段落（`RES_POOLCOLL_TEXT`）出現在多處：
- `EndBlockQuote()` — `swmd.cxx:236`
- `EndHR()` — `swmd.cxx:257`
- `EndHeading()` — `swmd.cxx:297`
- `EndCodeBlock()` — `swmd.cxx:598`
- 建構子初始化 — `swmd.cxx:845`

**如果需要**將預設段落也對應到自訂樣式，需修改上述所有位置。建議抽取為一個輔助函式（見第 5 節）。

### 4.2 匯入 — `mdcallbcks.cxx`（Inline Code 字型）

**檔案**: `sw/source/filter/md/mdcallbcks.cxx`，第 205-211 行

**現有程式碼**:
```cpp
case MD_SPAN_CODE:
{
    SwDoc* pDoc = parser->m_xDoc.get();
    SwTextFormatColl* pColl
        = pDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
    pItem.reset(new SvxFontItem(pColl->GetFont()));
    break;
}
```

**說明**: 這裡從 `HTML_PRE` 樣式取字型來套用到 inline code span。如果您的自訂 code block 樣式使用了不同的字型，這裡也要跟著改：

```cpp
case MD_SPAN_CODE:
{
    SwDoc* pDoc = parser->m_xDoc.get();
    SwTextFormatColl* pColl = pDoc->FindTextFormatCollByName(u"custom-code-block"_ustr);
    if (!pColl)
        pColl = pDoc->getIDocumentStylePoolAccess().GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
    pItem.reset(new SvxFontItem(pColl->GetFont()));
    break;
}
```

### 4.3 匯出 — `wrtmd.cxx`

匯出端需要做**反向映射**：根據段落的樣式名稱或 pool ID 判斷要輸出什麼 Markdown 語法。

#### 4.3.1 標題識別（`OutMarkdown_SwTextNode`）

**檔案**: `sw/source/filter/md/wrtmd.cxx`，第 666-705 行

**現有程式碼**:
```cpp
int nHeadingLevel = 0;
for (const SwFormat* pFormat = &rNode.GetAnyFormatColl(); pFormat;
     pFormat = pFormat->DerivedFrom())
{
    sal_uInt16 nPoolId = pFormat->GetPoolFormatId();
    switch (nPoolId)
    {
        case RES_POOLCOLL_HEADLINE1:
            if (!nHeadingLevel) nHeadingLevel = 1;
            break;
        case RES_POOLCOLL_HEADLINE2:
            if (!nHeadingLevel) nHeadingLevel = 2;
            break;
        // ... 3, 4, 5, 6 ...
        case RES_POOLCOLL_HTML_HR:
            if (rNodeText.isEmpty()) {
                rWrt.Strm().WriteUnicodeOrByteText(u"___\n");
                return;
            }
            break;
    }
}
```

**修改方式**: 在 pool ID 比對之前，先用樣式名稱比對自訂樣式。

```cpp
int nHeadingLevel = 0;
for (const SwFormat* pFormat = &rNode.GetAnyFormatColl(); pFormat;
     pFormat = pFormat->DerivedFrom())
{
    // 先嘗試自訂樣式名稱比對
    const OUString& rName = pFormat->GetName();
    if (!nHeadingLevel)
    {
        if (rName == u"custom-h1") { nHeadingLevel = 1; continue; }
        if (rName == u"custom-h2") { nHeadingLevel = 2; continue; }
        if (rName == u"custom-h3") { nHeadingLevel = 3; continue; }
        if (rName == u"custom-h4") { nHeadingLevel = 4; continue; }
        if (rName == u"custom-h5") { nHeadingLevel = 5; continue; }
        if (rName == u"custom-h6") { nHeadingLevel = 6; continue; }
    }
    if (rName == u"custom-hr" && rNodeText.isEmpty())
    {
        rWrt.Strm().WriteUnicodeOrByteText(u"___\n");
        return;
    }

    // 原有的 pool ID fallback
    sal_uInt16 nPoolId = pFormat->GetPoolFormatId();
    switch (nPoolId)
    {
        case RES_POOLCOLL_HEADLINE1: /* ... 保留原有邏輯 ... */
    }
}
```

#### 4.3.2 Blockquote 識別

**檔案**: `sw/source/filter/md/wrtmd.cxx`，第 659-664 行

**現有程式碼**:
```cpp
const SwFormatColl* pFormatColl = rNode.GetFormatColl();
if (pFormatColl && pFormatColl->GetPoolFormatId() == RES_POOLCOLL_HTML_BLOCKQUOTE)
{
    rWrt.Strm().WriteUnicodeOrByteText(u"> ");
}
```

**修改方式**: 新增名稱比對。

```cpp
const SwFormatColl* pFormatColl = rNode.GetFormatColl();
if (pFormatColl
    && (pFormatColl->GetPoolFormatId() == RES_POOLCOLL_HTML_BLOCKQUOTE
        || pFormatColl->GetName() == u"custom-blockquote"))
{
    rWrt.Strm().WriteUnicodeOrByteText(u"> ");
}
```

#### 4.3.3 Code Block 識別（4 處）

**檔案**: `sw/source/filter/md/wrtmd.cxx`，出現在第 740、746、861、866 行

需要將所有 `GetPoolFormatId() == RES_POOLCOLL_HTML_PRE` 的比對，都加上自訂樣式名稱的 OR 條件。建議抽取一個 helper function：

```cpp
namespace {
bool IsCodeBlockStyle(const SwFormatColl* pColl)
{
    if (!pColl)
        return false;
    return pColl->GetPoolFormatId() == RES_POOLCOLL_HTML_PRE
           || pColl->GetName() == u"custom-code-block";
}
}
```

然後將 4 處比對替換為：

```cpp
// 第 740 行
if (pFormatColl && IsCodeBlockStyle(pFormatColl))

// 第 746 行
if (!pPrevColl || !IsCodeBlockStyle(pPrevColl))

// 第 861 行
if (pFormatColl && IsCodeBlockStyle(pFormatColl))

// 第 866 行
if (!pNextColl || !IsCodeBlockStyle(pNextColl))
```

#### 4.3.4 Inline Code 字型比對

**檔案**: `sw/source/filter/md/wrtmd.cxx`，第 275-287 行

**現有程式碼**:
```cpp
case RES_CHRATR_FONT:
{
    const SvxFontItem& rFontItem = rItem.StaticWhichCast(RES_CHRATR_FONT);
    SwDoc* pDoc = rWrt.m_pDoc;
    IDocumentStylePoolAccess& rIDSPA = pDoc->getIDocumentStylePoolAccess();
    SwTextFormatColl* pColl = rIDSPA.GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
    if (rFontItem == pColl->GetFont())
    {
        rChange.nCodeChange += increment;
    }
    break;
}
```

**修改方式**: 如果自訂 code block 樣式使用不同字型，這裡也要對應修改比對來源。

```cpp
case RES_CHRATR_FONT:
{
    const SvxFontItem& rFontItem = rItem.StaticWhichCast(RES_CHRATR_FONT);
    SwDoc* pDoc = rWrt.m_pDoc;
    SwTextFormatColl* pColl = pDoc->FindTextFormatCollByName(u"custom-code-block"_ustr);
    if (!pColl)
    {
        IDocumentStylePoolAccess& rIDSPA = pDoc->getIDocumentStylePoolAccess();
        pColl = rIDSPA.GetTextCollFromPool(RES_POOLCOLL_HTML_PRE);
    }
    if (rFontItem == pColl->GetFont())
    {
        rChange.nCodeChange += increment;
    }
    break;
}
```

---

## 5. 建議的重構：集中管理樣式名稱

為避免自訂樣式名稱散落在多個檔案中，建議在 `swmd.hxx` 中集中定義所有映射：

```cpp
// swmd.hxx — 新增自訂樣式名稱常數

// 自訂段落樣式名稱
inline constexpr OUString MD_CUSTOM_HEADING1 = u"custom-h1"_ustr;
inline constexpr OUString MD_CUSTOM_HEADING2 = u"custom-h2"_ustr;
inline constexpr OUString MD_CUSTOM_HEADING3 = u"custom-h3"_ustr;
inline constexpr OUString MD_CUSTOM_HEADING4 = u"custom-h4"_ustr;
inline constexpr OUString MD_CUSTOM_HEADING5 = u"custom-h5"_ustr;
inline constexpr OUString MD_CUSTOM_HEADING6 = u"custom-h6"_ustr;
inline constexpr OUString MD_CUSTOM_BLOCKQUOTE = u"custom-blockquote"_ustr;
inline constexpr OUString MD_CUSTOM_CODE_BLOCK = u"custom-code-block"_ustr;
inline constexpr OUString MD_CUSTOM_HR = u"custom-hr"_ustr;
inline constexpr OUString MD_CUSTOM_TEXT = u"custom-text"_ustr;  // 可選
```

並提供輔助函式以統一查找邏輯：

```cpp
// swmd.hxx 或 swmd.cxx — 輔助函式

/// 按名稱查找自訂樣式，找不到時 fallback 到 pool 樣式
inline SwTextFormatColl* GetMdStyleOrPool(SwDoc& rDoc, const OUString& rCustomName,
                                          sal_uInt16 nPoolId)
{
    SwTextFormatColl* pColl = rDoc.FindTextFormatCollByName(rCustomName);
    if (!pColl)
        pColl = rDoc.getIDocumentStylePoolAccess().GetTextCollFromPool(nPoolId);
    return pColl;
}
```

使用範例：
```cpp
// StartHeading 變得很簡潔
void SwMarkdownParser::StartHeading(sal_uInt8 nLvl)
{
    // ...
    static constexpr OUString aCustomHeadings[] = {
        MD_CUSTOM_HEADING1, MD_CUSTOM_HEADING2, MD_CUSTOM_HEADING3,
        MD_CUSTOM_HEADING4, MD_CUSTOM_HEADING5, MD_CUSTOM_HEADING6,
    };
    SwTextFormatColl* pColl = GetMdStyleOrPool(
        *m_xDoc, aCustomHeadings[nLvl - 1], RES_POOLCOLL_HEADLINE_BASE + nLvl);
    m_xDoc->SetTextFormatColl(*m_pPam, pColl);
}
```

---

## 6. 完整修改點位清單（Checklist）

### 匯入端（swmd.cxx）

- [ ] `StartHeading()` — 第 284-285 行 → 6 級標題
- [ ] `AddBlockQuote()` — 第 218 行 → blockquote
- [ ] `BeginCodeBlock()` — 第 583 行 → code block
- [ ] `AddHR()` — 第 249 行 → 水平線
- [ ] `EndBlockQuote()` — 第 236 行 → fallback 到一般段落
- [ ] `EndHR()` — 第 257 行 → fallback 到一般段落
- [ ] `EndHeading()` — 第 297 行 → fallback 到一般段落
- [ ] `EndCodeBlock()` — 第 598 行 → fallback 到一般段落
- [ ] 建構子 — 第 845 行 → 初始段落樣式

### 匯入端（mdcallbcks.cxx）

- [ ] `enter_span_callback` `MD_SPAN_CODE` — 第 209 行 → inline code 字型來源

### 匯出端（wrtmd.cxx）

- [ ] 標題 switch — 第 670-704 行 → 識別自訂標題樣式
- [ ] blockquote 判斷 — 第 660 行 → 識別自訂 blockquote 樣式
- [ ] code block 開始判斷 — 第 740 行 → 識別自訂 code block 樣式
- [ ] code block 前段比對 — 第 746 行 → 判斷前一段是否為 code block
- [ ] code block 結束判斷 — 第 861 行 → 識別自訂 code block 樣式
- [ ] code block 後段比對 — 第 866 行 → 判斷後一段是否為 code block
- [ ] inline code 字型比對 — 第 280 行 → 取字型的樣式來源

### 測試（sw/qa/filter/md/md.cxx）

- [ ] 新增匯入測試：驗證 Markdown 標題映射到自訂樣式
- [ ] 新增匯出測試：驗證自訂樣式匯出為正確的 Markdown 語法
- [ ] 新增 fallback 測試：自訂樣式不存在時正常使用內建樣式

---

## 7. 測試策略

### 7.1 建置與測試指令

```bash
# 編譯 Writer 模組
make sw

# 執行所有 Markdown 過濾器測試
make CppunitTest_sw_filter_md

# 執行單一測試
make CppunitTest_sw_filter_md CPPUNIT_TEST_NAME=testHeading
```

### 7.2 測試案例建議

**匯入測試**：建立一個包含自訂樣式的 `.ott` 範本，搭配 `.md` 測試檔，驗證匯入後段落的樣式名稱。

```cpp
CPPUNIT_TEST_FIXTURE(Test, testCustomStyleHeadingImport)
{
    // 需準備含自訂樣式的文件或範本
    setImportFilterName(TestFilter::MD);
    createSwDoc("heading.md");

    // 驗證段落樣式名稱是否為自訂樣式
    // （前提：自訂樣式已預先存在於文件中）
    SwDoc* pDoc = getSwDoc();
    SwTextNode* pNode = pDoc->GetNodes()[/* 適當的 node index */]->GetTextNode();
    CPPUNIT_ASSERT_EQUAL(u"custom-h1"_ustr, pNode->GetFormatColl()->GetName());
}
```

**匯出測試**：建立含自訂樣式段落的 `.fodt` 測試檔，匯出為 Markdown 後驗證輸出內容。

```cpp
CPPUNIT_TEST_FIXTURE(Test, testCustomStyleHeadingExport)
{
    createSwDoc("custom-styles.fodt");  // 段落使用 "custom-h1" 樣式
    save(TestFilter::MD);
    std::string aActual = TempFileToString();
    // 驗證輸出包含 "# " 標題前綴
    CPPUNIT_ASSERT(aActual.find("# ") != std::string::npos);
}
```

### 7.3 自訂樣式的前置條件

`FindTextFormatCollByName()` 只能找到**已存在於文件中**的樣式。確保自訂樣式可用的方式：

1. **透過範本**：匯入時使用 `{"TemplateURL": "file:///path/to/template.ott"}` filter option 載入含自訂樣式的範本
2. **程式碼建立**：在 `SwMarkdownParser` 的建構子中，若找不到自訂樣式就用 `MakeTextFormatColl()` 建立
3. **預設文件範本**：將自訂樣式加入公司的預設文件範本

---

## 8. 風險與注意事項

| 風險 | 說明 | 緩解措施 |
|---|---|---|
| 自訂樣式不存在 | 使用者沒有載入含自訂樣式的範本 | 所有地方都必須有 fallback 到原本的 pool 樣式 |
| 樣式名稱大小寫/空格差異 | `FindTextFormatCollByName` 做精確比對 | 統一使用常數定義的樣式名稱 |
| 匯出端樣式繼承鏈 | 匯出遍歷 `DerivedFrom()` 鏈，自訂樣式可能繼承自內建樣式 | 先比對名稱，再比對 pool ID，避免重複匹配 |
| inline code 字型不一致 | 自訂 code block 樣式若使用不同字型，匯入/匯出的字型比對邏輯必須同步 | 確保 `mdcallbcks.cxx` 和 `wrtmd.cxx` 使用同一個樣式來源取字型 |
| 測試資料相依性 | 測試需要含自訂樣式的 `.fodt` / `.ott` 檔案 | 在 `sw/qa/filter/md/data/` 新增測試用範本和文件 |
