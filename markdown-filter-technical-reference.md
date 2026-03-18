# LibreOffice 26.2 Markdown Filter — Technical Architecture Reference

This document serves as a comprehensive technical reference for AI agents working on the Markdown import/export filter in LibreOffice Writer. All information is derived from the actual source code as of the 26.2 release branch.

---

## 1. File Map & Ownership

All Markdown filter source code lives under `sw/source/filter/md/`:

| File | Responsibility |
|---|---|
| `swmd.hxx` | Import class declaration, constants, enums, MDImage struct |
| `swmd.cxx` | Import implementation: `SwMarkdownParser`, `MarkdownReader`, template support |
| `mdcallbcks.cxx` | md4c callback implementations (block/span/text handlers) |
| `mdnum.hxx` / `mdnum.cxx` | `SwMdNumRuleInfo` — list numbering state management |
| `mdtab.cxx` | `MDTable` — table import helper, `StartTable`/`EndTable`/`StartRow`/`StartCell` |
| `wrtmd.hxx` | Export class declaration: `SwMDWriter`, `SwMDCellInfo`, `SwMDTableInfo`, `SwMDFly` |
| `wrtmd.cxx` | Export implementation: node traversal, formatting state machine, image/redline handling |

Related files outside `md/`:

| File | Role |
|---|---|
| `sw/source/filter/basflt/fltini.cxx` | Filter registration — instantiates `MarkdownReader`, maps `READER_WRITER_MD` |
| `sw/source/filter/basflt/iodetect.cxx` | Filter detection — registers `FILTER_MD` ("Markdown") in `aFilterDetect[]` |
| `sw/inc/iodetect.hxx` | Defines `FILTER_MD = "Markdown"_ustr` |
| `sw/inc/shellio.hxx` | Declares `MarkdownReader` class (inherits `Reader`) |
| `sw/Library_sw.mk` | Build: compiles the 5 md source files, links `md4c` external library |
| `external/md4c/` | Third-party md4c CommonMark parser (static library) |
| `sw/qa/filter/md/md.cxx` | CppUnit tests (~35 test cases) |
| `sw/qa/filter/md/data/` | Test fixtures (`.md`, `.fodt`, `.odt`, `.ott`, `.docx`, `.png`) |

---

## 2. External Dependency: md4c

md4c is a fast, CommonMark-compliant C library used exclusively for **import** (parsing Markdown text). LibreOffice builds it as a static library from `external/md4c/`.

**Build files:**
- `external/md4c/StaticLibrary_md4c.mk` — compiles `entity.c`, `md4c.c`, `md4c-html.c`
- `external/md4c/UnpackedTarball_md4c.mk` — fetches source tarball
- `external/md4c/0001-const-up-scheme_map.patch.1` — const-correctness patch

**API surface used by LibreOffice:**

```c
// Core parsing function
int md_parse(const MD_CHAR* text, MD_SIZE size, const MD_PARSER* parser, void* userdata);

// Parser configuration struct
typedef struct {
    unsigned flags;                    // MD_DIALECT_GITHUB | MD_FLAG_WIKILINKS
    int (*enter_block)(MD_BLOCKTYPE type, void* detail, void* userdata);
    int (*leave_block)(MD_BLOCKTYPE type, void* detail, void* userdata);
    int (*enter_span)(MD_SPANTYPE type, void* detail, void* userdata);
    int (*leave_span)(MD_SPANTYPE type, void* detail, void* userdata);
    int (*text)(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata);
} MD_PARSER;
```

**Parser flags:** `MD_DIALECT_GITHUB | MD_FLAG_WIKILINKS` — enables GFM tables, strikethrough, task lists, and `[[wiki]]` links.

---

## 3. Filter Registration

### 3.1 How LibreOffice finds the Markdown filter

The filter system uses two parallel arrays defined in `sw/source/filter/basflt/`:

**`iodetect.cxx` — detection array:**
```cpp
SwIoDetect aFilterDetect[] = {
    // ... other filters ...
    SwIoDetect(FILTER_MD)    // FILTER_MD = "Markdown"_ustr, index 11
};
```

**`fltini.cxx` — reader/writer array:**
```cpp
SwReaderWriterEntry aReaderWriter[] = {
    // ... other filters at indices 0-10 ...
    SwReaderWriterEntry(nullptr, &GetMDWriter, false)   // index 11 = READER_WRITER_MD
};
```

**Reader initialization (`fltini.cxx`, `Filters` constructor):**
```cpp
ReadMarkdown = new MarkdownReader;
SetFltPtr(READER_WRITER_MD, ReadMarkdown);
```

**Writer factory (`wrtmd.cxx`):**
```cpp
void GetMDWriter(std::u16string_view, const OUString& rBaseURL, WriterRef& xRet) {
    xRet = new SwMDWriter(rBaseURL);
}
```

### 3.2 Invocation path

- **Import:** User opens `.md` file → `SwReader` → `MarkdownReader::Read()` → `SwMarkdownParser::CallParser()`
- **Export:** User saves as Markdown → `SwWriter` → `SwMDWriter::WriteStream()` → `Out_SwDoc()`

---

## 4. Import Architecture

### 4.1 Constants & Enums (`swmd.hxx`)

```cpp
constexpr tools::Long MD_PARSPACE = o3tl::toTwips(5, o3tl::Length::mm);  // 5mm paragraph spacing
constexpr tools::Long MD_MIN_IMAGE_WIDTH_IN_TWIPS  = 500;   // ~1.75cm
constexpr tools::Long MD_MIN_IMAGE_HEIGHT_IN_TWIPS = 500;
constexpr tools::Long MD_MAX_IMAGE_WIDTH_IN_TWIPS  = 5000;  // ~17.5cm
constexpr tools::Long MD_MAX_IMAGE_HEIGHT_IN_TWIPS = 5000;
constexpr Color COL_CODE_BLOCK = { 225, 225, 225 };  // code block background

constexpr tools::Long MD_NUMBER_BULLET_MARGINLEFT = o3tl::toTwips(125, o3tl::Length::mm10);  // 12.5mm
constexpr tools::Long MD_NUMBER_BULLET_INDENT = -o3tl::toTwips(5, o3tl::Length::mm);         // -5mm hanging

// Table alignment mapping (compile-time frozen map)
constexpr frozen::unordered_map<MD_ALIGN, SvxAdjust, 4> adjustMap = {
    { MD_ALIGN_DEFAULT, SvxAdjust::Left },
    { MD_ALIGN_LEFT,    SvxAdjust::Left },
    { MD_ALIGN_CENTER,  SvxAdjust::Center },
    { MD_ALIGN_RIGHT,   SvxAdjust::Right }
};

enum SwMdAppendMode {
    AM_NORMAL,      // No spacing handling
    AM_NOSPACE,     // Force spacing = 0
    AM_SPACE,       // Force spacing = MD_PARSPACE
    AM_SOFTNOSPACE, // Record 0 but don't set
    AM_NONE         // Don't append
};
```

### 4.2 MDImage struct (`swmd.hxx`)

```cpp
struct MDImage {
    OUString url;    // Image source URL
    OUString title;  // Title attribute
    OUString desc;   // Alt text (description)
    OUString link;   // Hyperlink wrapping the image
    void Reset();    // Clears all fields
};
```

### 4.3 SwMarkdownParser class (`swmd.hxx` / `swmd.cxx`)

**Key member variables:**

```cpp
rtl::Reference<SwDoc> m_xDoc;                    // Target document
SwPaM* m_pPam;                                   // Insertion cursor
SvStream& m_rInput;                              // Input stream
std::unique_ptr<char[]> m_pArr;                  // Entire file in memory
tools::Long m_nFilesize;                         // File size
std::unique_ptr<SwMdNumRuleInfo> m_pNumRuleInfo; // List state
MDAttrStack m_aAttrStack;                        // Formatting stack (vector<unique_ptr<SfxPoolItem>>)
sal_Int32 m_nBlockQuoteDepth;                    // Blockquote nesting (-1 = none)
bool m_bNewDoc;                                  // Creating vs. inserting
bool m_bNoParSpace;                              // Suppress spacing
bool m_bInsideImage;                             // Inside image alt-text
MDImage m_aImg;                                  // Current image data
std::vector<MDTable*> m_aTables;                 // Tracked tables
std::shared_ptr<MDTable> m_xTable;               // Current table
OUString m_sBaseURL;                             // For relative URL resolution
OUString m_htmlData;                             // Accumulated HTML block content
```

**Constructor:**

```cpp
SwMarkdownParser(SwDoc& rD, SwPaM& rCursor, SvStream& rIn,
                 OUString aBaseURL, bool bReadNewDoc);
```

Reads the entire file into `m_pArr`, initializes numbering info, sets initial paragraph style.

**Main entry — `CallParser()`:**

1. Sets up `MD_PARSER` struct with 5 static callback pointers
2. Calls `md_parse(m_pArr.get(), m_nFilesize, &parser, this)`
3. Returns `ERRCODE_NONE` on success, `ERRCODE_IO_GENERAL` on failure

**Block-level methods:**

| Method | Triggered by | What it does |
|---|---|---|
| `StartHeading(sal_uInt8 nLvl)` | `MD_BLOCK_H` enter | Sets `RES_POOLCOLL_HEADLINE_BASE + nLvl` style |
| `EndHeading()` | `MD_BLOCK_H` leave | Reverts to text body style |
| `StartPara()` | `MD_BLOCK_P` enter | Appends text node with spacing |
| `EndPara()` | `MD_BLOCK_P` leave | Appends text node |
| `StartNumberedBulletList(MD_BLOCKTYPE)` | `MD_BLOCK_UL/OL` enter | Creates/reuses `SwNumRule`, increments depth |
| `EndNumberedBulletList()` | `MD_BLOCK_UL/OL` leave | Decrements depth, clears rule if depth=0 |
| `StartNumberedBulletListItem(detail)` | `MD_BLOCK_LI` enter | Applies numbering, handles task list checkboxes via `SwContentControl` |
| `EndNumberedBulletListItem()` | `MD_BLOCK_LI` leave | Appends text node |
| `AddBlockQuote()` | `MD_BLOCK_QUOTE` enter | Sets blockquote style, increments depth |
| `EndBlockQuote()` | `MD_BLOCK_QUOTE` leave | Decrements depth, reverts style |
| `BeginCodeBlock()` | `MD_BLOCK_CODE` enter | Sets `RES_POOLCOLL_HTML_PRE` style + gray background |
| `EndCodeBlock()` | `MD_BLOCK_CODE` leave | Reverts style |
| `AddHR()` | `MD_BLOCK_HR` enter | Inserts horizontal rule |
| `StartTable(nRow, nCol)` | `MD_BLOCK_TABLE` enter | Calls `SwDoc::InsertTable()`, creates `MDTable` |
| `EndTable()` | `MD_BLOCK_TABLE` leave | Moves cursor past table |
| `StartRow()` | `MD_BLOCK_TR` enter | Increments row counter in `MDTable` |
| `StartCell(MD_ALIGN)` | `MD_BLOCK_TH/TD` enter | Positions cursor in cell, applies alignment |
| `BeginHtmlBlock()` | `MD_BLOCK_HTML` enter | Begins accumulating HTML |
| `InsertHtmlData()` | Internal | Parses accumulated HTML via `ReadHTML` |
| `EndHtmlBlock()` | `MD_BLOCK_HTML` leave | Calls `InsertHtmlData()` |

**Inline/text methods:**

| Method | Purpose |
|---|---|
| `InsertText(OUString&)` | Inserts text at cursor, applies all items from `m_aAttrStack` |
| `SetAttrs(SwPaM&)` | Applies attribute stack items as character attributes on range |
| `ClearAttrs()` | Clears attributes at cursor position |
| `InsertImage(const MDImage&)` | Inserts graphic object with size constraints and anchor |
| `InsertCurImage()` | Calls `InsertImage(m_aImg)`, resets `m_aImg` |
| `AppendTextNode(SwMdAppendMode, bool)` | Creates new paragraph, controls spacing |

### 4.4 md4c Callbacks (`mdcallbcks.cxx`)

All callbacks are `static` methods on `SwMarkdownParser`. The `userdata` pointer is `this`.

**`enter_span_callback`** pushes `SfxPoolItem` objects onto `m_aAttrStack`:

| Span type | Pool item created |
|---|---|
| `MD_SPAN_EM` | `SvxPostureItem(ITALIC_NORMAL, RES_CHRATR_POSTURE)` |
| `MD_SPAN_STRONG` | `SvxWeightItem(WEIGHT_BOLD, RES_CHRATR_WEIGHT)` |
| `MD_SPAN_CODE` | `SvxFontItem` copied from `RES_POOLCOLL_HTML_PRE` style's font |
| `MD_SPAN_DEL` | `SvxCrossedOutItem(STRIKEOUT_SINGLE, RES_CHRATR_CROSSEDOUT)` |
| `MD_SPAN_U` | `SvxUnderlineItem(LINESTYLE_SINGLE, RES_CHRATR_UNDERLINE)` |
| `MD_SPAN_A` | `SwFormatINetFormat(href, "")` |
| `MD_SPAN_IMG` | Populates `m_aImg` fields, sets `m_bInsideImage = true` |
| `MD_SPAN_WIKILINK` | `SwFormatINetFormat("https://{lang}.wikipedia.org/wiki/{target}", "")` |

**`leave_span_callback`** pops from `m_aAttrStack`. Special case: `MD_SPAN_IMG` triggers `InsertCurImage()`.

**`text_callback`** dispatches on `MD_TEXTTYPE`:

| Text type | Action |
|---|---|
| `MD_TEXT_NORMAL` | `InsertText()` (or set `m_aImg.desc` if inside image) |
| `MD_TEXT_CODE` | Same as NORMAL |
| `MD_TEXT_HTML` | Append to `m_htmlData` |
| `MD_TEXT_BR` | Insert `"\n"` |
| `MD_TEXT_SOFTBR` | Insert hard blank character |
| `MD_TEXT_ENTITY` | No-op |

### 4.5 MDTable class (`mdtab.cxx`)

Internal (file-scope) class for tracking table position during import:

```cpp
class MDTable {
    const SwTable* m_pTable;       // Writer table object
    SwMarkdownParser* m_pParser;   // Parser back-reference
    sal_Int32 m_nRow, m_nCol;      // Total rows/cols
    sal_Int32 m_nCurRow, m_nCurCol; // Current position (0-based)
};
```

Registers itself with `SwMarkdownParser::RegisterTable()` on construction, deregisters on destruction.

### 4.6 SwMdNumRuleInfo (`mdnum.hxx` / `mdnum.cxx`)

Manages list numbering state during both import and export:

```cpp
class SwMdNumRuleInfo {
    sal_uInt16 m_aNumStarts[MAXLEVEL]; // Start value per level
    SwNumRule* m_pNumRule;             // Current numbering rule
    sal_uInt16 m_nDeep;                // Depth (1-based)
    bool m_bRestart;                   // Should restart numbering?
    bool m_bNumbered;                  // Is numbered (vs. bulleted)?
};
```

Key methods: `IncDepth()`, `DecDepth()`, `GetLevel()` (returns `min(m_nDeep - 1, MAXLEVEL - 1)`).

### 4.7 Template support (`swmd.cxx`, `MarkdownReader::SetupFilterOptions`)

Import accepts a JSON filter option:

```json
{ "TemplateURL": "file:///path/to/template.ott" }
```

The reader loads the template document via `Desktop::loadComponentFromURL()`, then copies styles into the target document via `pDocShell->LoadStyles()`. This allows users to customize the visual rendering of Markdown content.

---

## 5. Export Architecture

### 5.1 Data Structures (`wrtmd.hxx`)

**`SwMDCellInfo`** — per-node cell boundary tracking:

```cpp
struct SwMDCellInfo {
    bool bCellStart = false;     // Is this node a cell start?
    bool bCellEnd = false;       // Is this node a cell end?
    bool bRowStart = false;      // First cell in row?
    bool bRowEnd = false;        // Last cell in row?
    bool bFirstRowEnd = false;   // End of header row? (triggers separator line)
    size_t nFirstRowBoxCount = 0;
    std::vector<SvxAdjust> aFirstRowBoxAdjustments;  // Column alignments
};
```

**`SwMDTableInfo`** — per-table export state:

```cpp
struct SwMDTableInfo {
    std::map<SwNodeOffset, SwMDCellInfo> aCellInfos;  // Node offset → cell info
    const SwEndNode* pEndNode = nullptr;               // Table end sentinel
};
```

**`SwMDFly`** — anchored object location for export:

```cpp
struct SwMDFly {
    SwNodeOffset m_nAnchorNodeOffset;
    sal_Int32 m_nAnchorContentOffset;
    const SwFrameFormat* m_pFrameFormat;
    bool operator<(const SwMDFly&) const;  // Sorted by node, then content offset
};
```

### 5.2 SwMDWriter class (`wrtmd.hxx` / `wrtmd.cxx`)

Inherits from `Writer` (defined in `shellio.hxx`).

**Member variables:**

```cpp
bool m_bOutTable = false;
SwNodeOffset m_nStartNodeIndex{ 0 };
std::map<int, int> m_aListLevelPrefixSizes;  // level → prefix character count
std::stack<SwMDTableInfo> m_aTableInfos;     // Nested table stack
int m_nTaskListItems = 0;                   // Open task list checkboxes
o3tl::sorted_vector<SwMDFly> m_aFlys;       // All anchored objects
```

**Entry point — `WriteStream()`:**

1. `CollectFlys()` — gathers all `SwFlyFrameFormat` objects from the document
2. Checks if document starts with a table node (sets `m_bOutTable`)
3. Calls `Out_SwDoc(m_pCurrentPam)` — main traversal loop
4. Returns `ERRCODE_NONE`

### 5.3 Internal Data Structures (`wrtmd.cxx`, anonymous namespace)

**`SwMDImageInfo`** — extracted image data for output:

```cpp
struct SwMDImageInfo {
    OUString aURL;          // Absolute, relative, or data: URL
    OUString aTitle;        // Image title
    OUString aDescription;  // Alt text
    OUString aLink;         // Wrapping hyperlink URL
};
```

**`FormattingStatus`** — tracks active formatting state at any text position:

```cpp
struct FormattingStatus {
    int nCrossedOutChange = 0;    // Strikethrough level
    int nPostureChange = 0;       // Italic level
    int nUnderlineChange = 0;     // Underline level
    int nWeightChange = 0;        // Bold level
    int nCodeChange = 0;          // Code span level
    std::unordered_map<OUString, int> aHyperlinkChanges;           // URL → active level
    std::unordered_map<const SwRangeRedline*, int> aRedlineChanges; // Redline → active level
    std::set<SwMDImageInfo> aImages;                                // Images at this position
    std::unordered_map<bool, int> aTaskListItemChanges;            // Checked? → active level
    std::set<const SwFrameFormat*> aFlys;                          // Fly frames at this position
};
```

State values: `> 0` = active/open, `0` = inactive/closed, `< 0` = closed in next paragraph (redlines only).

**`PosData<T>`** — sorted position-to-pointer mapping:

```cpp
template <typename T> struct PosData {
    std::vector<std::pair<sal_Int32, const T*>> table;
    size_t cur = 0;  // Cursor for single-pass iteration
    void add(sal_Int32 pos, const T* val);
    void sort();
    const value_type* current() const;
    const value_type* next();
};
```

**`NodePositions`** — all formatting boundaries in a single text node:

```cpp
struct NodePositions {
    PosData<SfxPoolItem> hintStarts;      // Character attribute starts
    PosData<SfxPoolItem> hintEnds;        // Character attribute ends
    PosData<SwRangeRedline> redlineStarts; // Redline starts
    PosData<SwRangeRedline> redlineEnds;   // Redline ends
    PosData<SwFrameFormat> flys;           // Anchored object positions
    sal_Int32 getEndOfCurrent(sal_Int32 end);  // Next boundary position
};
```

### 5.4 Core Export Functions (`wrtmd.cxx`)

**`Out_SwDoc(SwPaM*)`** — document traversal loop:

Iterates over all `SwNode` objects within the PaM range. For each node:
- `SwTextNode` → `OutMarkdown_SwTextNode()`
- `SwTableNode` → `OutMarkdown_SwTableNode()`, push `SwMDTableInfo`
- `SwEndNode` → pop `SwMDTableInfo` if it matches the table's end node
- `SwSectionNode` → skip if hidden (unless `IncludeHiddenText` config is true)

**`OutMarkdown_SwTableNode(SwMDWriter&, const SwTableNode&)`:**

Scans the table structure by walking through start/end nodes of each cell. Builds a `SwMDTableInfo` with per-node `SwMDCellInfo` entries. For the header row, records column count and alignment. Pushes the info onto the table stack.

**`OutMarkdown_SwTextNode(SwMDWriter&, const SwTextNode&, SwNodeOffset)`:**

This is the most complex function (~300 lines). Handles:

1. **Table cell delimiters** — outputs `| ` at cell start, ` |` at cell end, newline at row end, separator line (`| - | :-: | -: |`) after header row
2. **Block-level prefixes** — `> ` for blockquotes, `# ` for headings, `- ` / `1. ` for lists, ```` ``` ```` for code blocks, `___\n` for horizontal rules
3. **Character-level formatting** — builds `NodePositions` from SwpHints (character attributes), redlines, and fly frames, then iterates character by character:
   - At each position, calls `OutFormattingChange()` to emit opening/closing markup
   - Calls `OutEscapedChars()` for the actual text content
4. **List indentation** — tracks prefix sizes per list level in `m_aListLevelPrefixSizes` for correct nested indentation

**`OutFormattingChange(SwMDWriter&, NodePositions&, sal_Int32, FormattingStatus&)`:**

The formatting state machine. At each text position:

1. Calls `CalculateFormattingChange()` to compute new `FormattingStatus`
2. **Closes** any formatting that transitioned from active to inactive:
   - Strikethrough: `~~`
   - Code: `` ` ``
   - Italic: `*`
   - Bold: `**`
   - Hyperlink: `](url)`
   - Redline insert: `</ins>`
   - Redline delete: `</del>`
   - Task list: (closing handled implicitly)
3. **Outputs fly-frame images** at this position
4. **Opens** any formatting that transitioned from inactive to active:
   - Redline insert: `<ins title="Author: name" datetime="ISO8601">`
   - Redline delete: `<del title="Author: name" datetime="ISO8601">`
   - Task list: `[x] ` or `[ ] `
   - Hyperlink: `[`
   - Bold: `**`
   - Italic: `*`
   - Code: `` ` ``
   - Strikethrough: `~~`
5. **Outputs inline images** (`![desc](url "title")` or `[![desc](url "title")](link)`)

Decision logic:
```cpp
bool ShouldCloseIt(int prev, int curr) { return prev != curr && prev >= 0 && curr <= 0; }
bool ShouldOpenIt(int prev, int curr)  { return prev != curr && prev <= 0 && curr > 0; }
```

**`ApplyItem(FormattingStatus&, const SfxPoolItem&, ...)`:**

Maps Writer attribute items to `FormattingStatus` fields:

| `Which()` | FormattingStatus field |
|---|---|
| `RES_CHRATR_CROSSEDOUT` | `nCrossedOutChange` |
| `RES_CHRATR_POSTURE` | `nPostureChange` |
| `RES_CHRATR_UNDERLINE` | `nUnderlineChange` |
| `RES_CHRATR_WEIGHT` | `nWeightChange` |
| `RES_CHRATR_FONT` | `nCodeChange` (if font matches `RES_POOLCOLL_HTML_PRE` style) |
| `RES_TXTATR_INETFMT` | `aHyperlinkChanges[url]` |
| `RES_TXTATR_AUTOFMT` | Recurses on nested item set |
| `RES_TXTATR_CHARFMT` | Recurses on character format item set |
| `RES_TXTATR_FLYCNT` | Calls `ApplyFlyFrameFormat()` → `aFlys` |
| `RES_TXTATR_CONTENTCONTROL` | `aTaskListItemChanges` (checkbox type only) |

**`ApplyFlyFrameFormat(FormattingStatus&, const SwFrameFormat*, ...)`:**

Extracts image data from a fly frame format:
- Linked graphics → uses `GetGrfNode()->GetFileFilterNms()` for URL
- Embedded graphics → converts to base64 data URL via `XOutBitmap::GraphicToBase64()`
- OLE objects → same base64 conversion via replacement graphic
- Image metadata → title from alt text, link from `SwFormatURL`
- URL handling → converts absolute URLs to relative using `URIHelper::simpleNormalizedMakeRelative()`

**`OutEscapedChars(SwMDWriter&, std::u16string_view)`:**

Escapes markdown special characters: `\ ` `` ` `` `* _ { } [ ] < > #`. Converts line breaks to two-spaces-plus-newline. Strips dummy characters (`CH_TXTATR_BREAKWORD`, `CH_TXTATR_INWORD`, `CH_TXTATR_NEWATTR`).

**`CollectFlys()`:**

Calls `m_pDoc->GetAllFlyFormats()`, creates `SwMDFly` entries with anchor node offset and content offset, inserts into `m_aFlys` (sorted vector).

### 5.5 Configuration

Export reads one configuration value:
```cpp
officecfg::Office::Writer::FilterFlags::Markdown::IncludeHiddenText::get()
```
Controls whether hidden text sections are included in the Markdown output.

---

## 6. Writer Document Model Interfaces Used

The filter interacts with Writer's document model through these key interfaces:

| Interface / Class | Usage |
|---|---|
| `SwDoc` | Central document class. Used for `InsertTable()`, `MakeNumRule()`, `GetNodes()`, `GetAttrPool()` |
| `IDocumentContentOperations` | `InsertString()`, `AppendTextNode()`, `InsertPoolItem()`, `InsertGraphic()` |
| `IDocumentStylePoolAccess` | `GetTextCollFromPool()` — retrieves built-in paragraph styles |
| `IDocumentRedlineAccess` | `GetRedlineTable()` — access tracked changes for export |
| `SwPaM` | Position and Mark — cursor for insertion (import) and traversal (export) |
| `SwTextNode` | Text paragraph node. `SetAttr()`, `GetNumRule()`, `GetActualListLevel()`, `m_pSwpHints` |
| `SwTableNode` | Table start node. `GetTable()` returns `SwTable` |
| `SwTable` | `GetTabSortBoxes()` — access cells for import positioning |
| `SwNumRule` | Numbering/list rule. `Set()` to configure level formats |
| `SwContentControl` | Checkbox content controls for task list items |
| `SwFormatINetFormat` | Hyperlink character attribute |

**Pool style IDs used:**

| ID | Markdown element |
|---|---|
| `RES_POOLCOLL_TEXT` | Normal paragraph (default) |
| `RES_POOLCOLL_HEADLINE1` – `HEADLINE6` | `#` through `######` headings |
| `RES_POOLCOLL_HTML_PRE` | Code blocks and inline code font source |
| `RES_POOLCOLL_HTML_BLOCKQUOTE` | `>` blockquotes |

---

## 7. Security Considerations

- **External URL access:** `InsertImage()` calls `allowAccessLink()` to validate the referer before loading external images
- **Exotic protocols:** Checks `INetURLObject::IsExoticProtocol()` before creating hyperlinks
- **Relative URLs:** Resolved via `INetURLObject::GetAbsURL(m_sBaseURL, url)` during import
- **Data URLs:** Embedded images use `data:image/png;base64,...` format, bypassing network access
- **Template loading:** Uses `Desktop::loadComponentFromURL()` with hidden document mode

---

## 8. Error Handling

| Situation | Behavior |
|---|---|
| md4c parse failure | Returns `ERRCODE_IO_GENERAL` |
| Image load failure | Silently skips the image |
| HTML block parse failure | Continues without the HTML content |
| Missing template file | Import proceeds without template styles |
| Invalid filter options JSON | Catches `boost::property_tree::json_parser_error`, continues |
| OLE without graphic | Skips the object (doesn't crash — covered by test) |

---

## 9. Test Coverage (`sw/qa/filter/md/md.cxx`)

### Import tests

| Test | What it verifies |
|---|---|
| `testHeading` | Heading levels map to correct `OutlineLevel` property |
| `testList` | Ordered/unordered lists, nesting levels |
| `testTables` | Table structure, cell alignment (left/center/right), images in cells |
| `testBlockQuoteMdImport` | Blockquote paragraph style applied |
| `testImageLinkMdImport` | Image with wrapping hyperlink |
| `testTastListItemsMdImport` | Task list checkboxes as `SwContentControl` |
| `testEmbeddedImageMdImport` | Base64 data URI images |
| `testTemplateMdImport` | `.ott` template style application |
| `testDocxTemplateMdImport` | `.docx` template style application |

### Export tests

| Test | What it verifies |
|---|---|
| `testExportingBasicElements` | Headings, bold, italic, special char escaping |
| `testExportingCodeSpan` | Code formatting detection via font matching |
| `testExportingList` | Nested bullets/numbering with correct indentation |
| `testExportingImage` | Inline image markdown syntax |
| `testExportingTable` | Table structure with `\|` delimiters |
| `testExportingRedlines` | Track changes as `<ins>`/`<del>` with author/datetime |
| `testBlockQuoteMdExport` | `> ` prefix output |
| `testCodeBlockMdExport` | Fenced code block output |
| `testTableColumnAdjustMdExport` | Column alignment in separator line (`:---:`, `---:`) |
| `testImageLinkMdExport` | `[![alt](src)](link)` syntax |
| `testNewlineMdExport` | Two-spaces-plus-newline for hard breaks |
| `testImageDescTitleExport` | `![desc](url "title")` syntax |
| `testMultiParaTableMdExport` | Multi-paragraph cells merged inline |
| `testNestedTableMdExport` | Nested tables flattened |
| `testTastListItemsMdExport` | `[x]` / `[ ]` checkbox syntax |
| `testEmbeddedImageMdExport` | Base64 data URI preservation |
| `testEmbeddedAnchoredImageMdExport` | `FLY_AT_CHAR` anchored images |

### Crash/robustness tests

| Test | What it verifies |
|---|---|
| `testExportFormula` | Formula objects don't crash export |
| `testExportTableFrame` | Frames in tables don't crash export |
| `testOLEWithoutGraphicMdExport` | OLE without graphic doesn't crash |

### Test data files (`sw/qa/filter/md/data/`)

```
basic-elements.fodt          heading.md             list.md
quote.md                     tables.md              task-list-items.md
image-and-link.md            embedded-image.md      template.md
template.ott                 template.docx          table.odt
tdf168572.odt                redlines-and-comments.odt
ole-without-graphic.odt      test.png
```

---

## 10. Build Commands

```bash
# Compile only the sw module (includes md filter)
make sw

# Run all Markdown filter tests
make CppunitTest_sw_filter_md

# Run with verbose output
make CppunitTest_sw_filter_md CPPUNIT_TEST_NAME=testHeading

# Full build including md4c external
make
```

---

## 11. Key Design Patterns

1. **Callback-driven import:** md4c drives the parsing; LibreOffice only reacts to callbacks. This separates grammar recognition from document construction.

2. **Attribute stack (import):** Character formatting is accumulated in a `vector<unique_ptr<SfxPoolItem>>` stack. Entering a span pushes, leaving pops. All stacked items are applied when text is inserted. Naturally handles nesting.

3. **State machine (export):** `FormattingStatus` tracks ~10 parallel formatting dimensions. At each character position, the old/new state comparison determines which markup to close/open. The `ShouldCloseIt`/`ShouldOpenIt` functions encode the transition logic.

4. **Pool style reuse:** Import maps Markdown elements to Writer's built-in `RES_POOL*` styles rather than creating new ones, ensuring consistency with the rest of Writer.

5. **Single-pass traversal:** Both import and export process the content in a single forward pass. Import reads the file once into memory; export walks the node tree once. `PosData<T>` with cursor enables O(n) processing of formatting boundaries.

6. **Table info stack:** `std::stack<SwMDTableInfo>` handles nested tables. Each table pushes its cell boundary map; when the table's end node is reached, the info is popped.

7. **Sorted fly collection:** Anchored objects are collected once upfront into a sorted set (`o3tl::sorted_vector<SwMDFly>`), then matched to text positions during node traversal.
