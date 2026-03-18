# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案簡介

LibreOffice 是一個基於 copyleft 授權的整合辦公套件，由 The Document Foundation 支援。此程式碼庫包含了 LibreOffice 的核心程式碼，歷史可追溯至 1990 年代的 StarOffice。

## 建置系統

### 基本建置流程

LibreOffice 使用自訂的 gbuild 建置系統（基於 GNU Make）：

1. **初始設定**：
   ```bash
   ./autogen.sh
   ```
   - 此腳本會讀取 `autogen.input` 檔案（如果存在）中的設定參數
   - 執行 configure 腳本並進行初始化

2. **完整編譯**：
   ```bash
   make
   ```
   - 編譯整個 LibreOffice 專案
   - 首次編譯可能需要數小時

3. **執行編譯後的 LibreOffice**：
   ```bash
   ./instdir/program/soffice
   ```
   或使用完整路徑執行主程式

### 測試相關指令

- **執行所有測試**：
  ```bash
  make check
  ```

- **執行特定的 CppUnit 測試**：
  ```bash
  make CppunitTest_<module>_<testname>
  ```
  例如：`make CppunitTest_sw_core`

- **在 GDB 除錯器中執行 LibreOffice**：
  ```bash
  make debugrun
  ```
  注意：這會啟動 GDB 除錯環境，主要用於開發者除錯。一般使用請直接執行 `./instdir/program/soffice`

- **執行 UI 測試**：
  ```bash
  make UITest_<module>_<testname>
  ```

### 其他有用指令

- **只編譯特定模組**：
  ```bash
  make <module>
  ```
  例如：`make sw`（編譯 Writer）

- **清理建置**：
  ```bash
  make clean
  ```

- **增量編譯（只重建變更的部分）**：
  ```bash
  make build-nocheck
  ```

## 程式碼架構

### 核心模組

LibreOffice 包含約 200 個模組，以下是最重要的核心模組：

#### 基礎層

- **sal/**：System Abstraction Layer（系統抽象層）
  - 提供跨平台的基礎 API
  - 包含基本資料型別和作業系統抽象

- **tools/**：基礎內部型別
  - 提供 `Rectangle`、`Color` 等基本型別
  - 實用工具函式

- **vcl/**：Visual Components Library（視覺元件函式庫）
  - 跨平台的 widget toolkit
  - 提供渲染抽象層
  - 處理視窗、對話框、控制項等 UI 元素

- **framework/**：UNO framework
  - 負責建立工具列、選單、狀態列
  - 使用 VCL widgets 和 `/uiconfig/` 中的 XML 描述檔

- **sfx2/**：Legacy core framework
  - Writer/Calc/Draw 使用的核心框架
  - 處理文件模型、載入/儲存、動作訊號等

- **svx/**：繪圖模型相關輔助程式碼
  - 包含大部分 Draw/Impress 的功能

#### 應用程式模組

- **desktop/**：應用程式主程式
  - 包含 `main()` 函式
  - 處理初始化和啟動程序

- **sw/**：Writer（文書處理器）
  - 詳見下方 Writer 架構說明

- **sc/**：Calc（試算表）
  - 處理儲存格、公式、圖表等

- **sd/**：Draw / Impress（繪圖/簡報）
  - 處理向量圖形和投影片

#### 圖形相關模組

- **basegfx/**：圖形演算法和資料型別
  - 用於 canvas 的圖形處理

- **canvas/**：新的 UNO canvas 渲染模型
  - 提供多種後端實作

- **cppcanvas/**：使用 UNO canvas 的 C++ 輔助類別

- **drawinglayer/**：渲染可繪製物件的檢視程式碼
  - 將複雜物件分解為更易渲染的基本圖元

### Writer (sw/) 架構

Writer 是 LibreOffice 的文書處理器，程式碼歷史可追溯至 1990 年。

#### 目錄結構

- `sw/inc/`：模組內所有原始檔可用的標頭檔
- `sw/source/core/`：Writer 核心（文件模型、排版、UNO API 實作）
- `sw/source/filter/`：Writer 內部過濾器
  - `ascii/`：純文字過濾器
  - `basflt/`：基礎過濾器輔助程式碼
  - `docx/`：UNO DOCX 匯入過濾器的包裝（用於 autotext）
  - `html/`：HTML 過濾器
  - `indexing/`：文件索引匯出
  - `md/`：**Markdown 過濾器**（詳見下方）
  - `rtf/`：UNO RTF 匯入過濾器的複製/貼上輔助
  - `writer/`：Writer 格式過濾器
  - `ww8/`：DOC 匯入、DOC/DOCX/RTF 匯出
  - `xml/`：ODF 匯入/匯出
- `sw/source/uibase/`：使用者介面（總是載入的部分）
- `sw/source/ui/`：使用者介面（按需載入的部分）
- `sw/qa/`：單元測試、慢速測試和後續測試

#### 核心概念

**SwDoc**：
- 代表一個文件的中心類別
- 功能分散到各個 Manager 類別中，每個實作一個 `IDocument*` 介面
- 透過 `SwDoc::getIDocument*()` 方法檢索 managers

**SwNodes**：
- 基本上是 `SwNode` 指標的陣列
- 使用特殊的 `SwStartNode` 和 `SwEndNode` 子類別編碼巢狀樹狀結構
- 包含五個頂層區段：空白、註腳內容、框架/頁首/頁尾內容、已刪除的變更追蹤內容、本文內容

**文字屬性**：
- 段落的子結構儲存在 `SwTextNode::m_pSwpHints` (SwpHintsArray) 中
- 包含格式屬性、巢狀屬性（超連結、Ruby、Meta）、無結尾的屬性（欄位、註腳）

**樣式系統**：
- 使用者定義樣式：有使用者定義的名稱
- 內建樣式：由 `inc/poolfmt.hxx` 中的 `RES_POOL*` 常數識別
  - ProgName（程式化名稱）：永不改變，用於 UNO API 和 ODF
  - UIName（翻譯名稱）：可翻譯的字串，用於 UI

**排版系統**：
- 排版是 `SwFrame` 子類別的樹狀結構
- 可透過 upper、lower、next 和 previous 指標遍歷
- 使用 follow 和 precede 指標處理跨頁流動

### Markdown 過濾器 (sw/source/filter/md/)

LibreOffice Writer 包含 Markdown 匯入和匯出功能：

#### 檔案結構

- **swmd.hxx / swmd.cxx**：Markdown 匯入器
  - `SwMarkdownParser` 類別：解析 Markdown 並轉換為 Writer 文件
  - 使用 md4c 函式庫作為解析器
  - 處理段落、標題、列表、表格、程式碼區塊、圖片等

- **wrtmd.hxx / wrtmd.cxx**：Markdown 匯出器
  - `SwMDWriter` 類別：將 Writer 文件匯出為 Markdown
  - 處理表格結構（`SwMDTableInfo`、`SwMDCellInfo`）
  - 支援錨定物件（`SwMDFly`）

- **mdnum.hxx / mdnum.cxx**：編號規則處理
  - `SwMdNumRuleInfo` 類別：管理 Markdown 列表的編號

- **mdtab.cxx**：表格處理
  - `MDTable` 類別：處理 Markdown 表格的匯入

- **mdcallbcks.cxx**：md4c 回呼函式
  - 實作 md4c 解析器的回呼介面

#### 主要功能

**匯入功能（swmd.cxx）**：
- 解析各種 Markdown 語法元素
- 處理區塊引用（blockquote）
- 處理編號和項目符號列表
- 處理表格（包含對齊）
- 處理程式碼區塊（帶背景色）
- 處理圖片（支援大小限制）
- 處理 HTML 區塊
- 使用屬性堆疊管理文字格式（粗體、斜體、刪除線等）

**匯出功能（wrtmd.cxx）**：
- 將 Writer 格式轉換為 Markdown
- 保持表格結構和對齊
- 處理內嵌圖片
- 處理各種文字格式
- 追蹤列表層級和前綴大小

#### 相關常數

```cpp
constexpr tools::Long MD_PARSPACE = o3tl::toTwips(5, o3tl::Length::mm);
constexpr tools::Long MD_MIN_IMAGE_WIDTH_IN_TWIPS = 500;
constexpr tools::Long MD_MIN_IMAGE_HEIGHT_IN_TWIPS = 500;
constexpr tools::Long MD_MAX_IMAGE_WIDTH_IN_TWIPS = 5000;
constexpr tools::Long MD_MAX_IMAGE_HEIGHT_IN_TWIPS = 5000;
constexpr Color COL_CODE_BLOCK = { 225, 225, 225 };  // 程式碼區塊背景色
```

## #include 指令規則

- 使用 `"..."` 形式：當且僅當被包含的檔案與包含檔案在相同目錄中
- 使用 `<...>` 形式：其他所有情況
- UNO API 標頭檔應一致使用雙引號（為外部使用者著想）
- `loplugin:includeform` 會強制執行這些規則

## 建置基準

### 作業系統和編譯器

- **Windows**：
  - 執行環境：Windows 10+
  - 建置：WSL + Visual Studio 2022

- **macOS**：
  - 執行環境：macOS 11+
  - 建置：macOS 13+ + Xcode 14.3+

- **Linux**：
  - 執行環境：RHEL 9 / CentOS 9 或相容版本
  - 建置：GCC 12 或 Clang 18（配合 libstdc++ 11）

- **Java**：JDK 17 或更新版本（許多模組需要 Java）
- **Python**：3.11（跟隨 SUSE/RHEL 的版本）

## 跨平台編譯

LibreOffice 支援跨平台編譯至 iOS、Android 和 Raspbian。使用標準的 `--build` 和 `--host` 選項：

- iOS 和 Android 上，函式庫會編譯為靜態函式庫（因平台限制）
- 需要平台特定的工具鏈（NDK for Android, Xcode for iOS）
- 詳細設定見 `README.cross`

## 開發資源

- **文件**：https://docs.libreoffice.org/
- **Wiki**：https://wiki.documentfoundation.org/
- **API 文件**：https://api.libreoffice.org/
- **郵件列表**：libreoffice@lists.freedesktop.org
- **IRC**：#libreoffice-dev on irc.libera.chat

## 重要提醒

- 每個模組應該有自己的 `README.md` 檔案提供更詳細的說明
- TDF 的 configure 開關位於 `distro-configs/` 目錄
- 使用 LODE (LibreOffice Development Environment) 腳本可簡化 Windows 和 macOS 的初始建置環境設定
- 編譯器外掛程式需要 Clang 18.1.8+（macOS 上需要自行編譯 Clang）
