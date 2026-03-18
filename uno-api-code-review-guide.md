# LibreOffice UNO API Code Review Guide

本文件整理這次針對 LibreOffice core 新增 C++ / UNO API 的實際 review 經驗，目標是提供之後可重複使用的審查方法。

適用範圍：

- `offapi/` 新增或修改 IDL
- `toolkit/`、`vcl/`、`sw/` 等模組中，UNO interface 到 C++/VCL 實作的橋接
- 新 API 對 extension、macro、跨語言 bindings 的相容性風險

---

## 1. 核心方法論

這類 review 最有效的方法不是單看 diff，而是做 `spec-driven review`：

1. 先讀 spec，建立預期行為
2. 再看 IDL，確認 published contract 是否真的表達了 spec
3. 再看 C++ bridge 與 VCL/內部實作，確認行為是否忠於 contract
4. 最後看測試，確認有沒有證明這些 contract 與行為真的成立

這次 review 的最佳輸入組合是：

- 規格文件：
  - `phase1-xgraphics-extension-spec.md`
  - `phase2-custom-paint-splitter-spec.md`
- 相關 commits：
  - `0a87da3addab`
  - `4bf909c4cd31`

---

## 2. 建議的 Review 流程

### 2.1 先從 API contract 開始

優先看：

- `offapi/com/sun/star/awt/XGraphics3.idl`
- `offapi/com/sun/star/awt/TextMetrics.idl`
- `offapi/com/sun/star/awt/XCustomPaintHandler.idl`
- `offapi/com/sun/star/awt/XCustomPaintWindow.idl`
- `offapi/com/sun/star/awt/XSplitterWindow.idl`

主要問題不是「能不能編譯」，而是：

- 介面型別是否與 spec 一致
- 文件是否精確描述方向、座標系、單位、範圍與邊界條件
- API 是否留下模糊地帶，導致不同實作者做出不同結果
- published UNO API 一旦進入 stable branch，之後修改成本是否過高

### 2.2 再看 UNO 到 VCL/C++ 的橋接

優先看：

- `toolkit/source/awt/vclxgraphics.cxx`
- `toolkit/source/awt/vclxcustompaintwindow.cxx`
- `toolkit/source/awt/vclxsplitter.cxx`
- `toolkit/source/awt/vclxtoolkit.cxx`
- `include/toolkit/helper/listenermultiplexer.hxx`

這一輪要特別檢查：

- 是否正確使用 `SolarMutexGuard`
- window / peer / listener lifetime 是否穩定
- `dispose()` 後是否仍可能 callback
- event ordering 是否穩定
- state 是持久狀態還是只在當下生效一次
- 是否意外破壞既有 UNO listener 行為

### 2.3 最後看驗證證據

重點問題：

- 有沒有 `qa/` 或 `CppUnit` 測試
- 測試是否覆蓋 spec 的關鍵行為，而不是只測 getter/setter
- 如果沒有測試，這個 patch 目前靠什麼證明它是對的

---

## 3. 這次 Review 實際抓到的重點

### 3.1 `XSplitterWindow::setRange()` 不是持久狀態

原本 `setRange()` 只在呼叫當下計算一次 drag rect，之後若：

- 視窗 resize
- 方向切換
- peer/window 重新綁定

range 不會被重新套用，導致 API 表面上有 state，實際上卻是 one-shot 行為。

這類問題很典型，因為 UNO API 看起來像 property，但 bridge 其實只做了一次性副作用。

### 3.2 splitter 文件與實作的方向語意不一致

IDL 文件若寫反，外部使用者就會在「完全合法」的情況下調錯方向。  
這類問題比一般 bug 更麻煩，因為一旦外部程式碼照文件寫了，之後很難再改。

### 3.3 `custompaintwindow` 破壞標準 `XPaintListener` 路徑

原本實作為了自訂 paint callback，直接略過標準 paint 通知路徑。  
結果是：

- `XCustomPaintHandler` 有被呼叫
- 但一般 `XPaintListener` 永遠收不到事件

這是很典型的「新功能做到了，但不小心破壞既有 contract」。

### 3.4 `XCustomPaintHandler.paint()` 型別與實際 contract 不一致

spec 與實作想提供的是 `XGraphics3` 能力，但 callback 參數型別卻仍是 `XGraphics`。  
這會造成：

- 文件與 signature 不一致
- 靜態型別語言的使用者必須每次自行 query/cast

如果 API 還很新，應優先在早期修正，而不是等使用者依賴後再背相容性包袱。

### 3.5 `TextMetrics.LineCount` 的 signed/unsigned 選型問題

內部值是非負計數，但 published UNO struct 卻用 `short`。  
這會讓大值時出現負數，屬於小但真實的 ABI/contract 問題。

---

## 4. UNO API Review Checklist

### 4.1 IDL / contract

- 名稱是否清楚，外部讀者能否只靠 IDL 理解用途
- 參數與回傳型別是否真的表達 spec
- `short` / `unsigned short` / `long` / `hyper` 是否合理
- 座標、尺寸、範圍、單位是否定義清楚
- 是否定義清楚 invalid input 或 boundary behavior
- 是否需要例外，但現在卻靜默吞掉錯誤
- 文件是否與實作方向、軸、矩形定義一致

### 4.2 Bridge / lifecycle

- 是否持有弱參考還是裸指標
- listener 註冊與移除是否對稱
- `dispose()` 後是否還能觸發 callback
- 是否有 reentrancy 風險
- 是否有 `SolarMutexGuard`
- 是否有 VCL window 被替換後 state 遺失

### 4.3 Compatibility

- 這個 API 若進 stable branch，之後是否還能改
- 是否會影響既有 UNO interface 的預設行為
- 外部 extension、Basic、Python、Java bindings 是否會受影響

### 4.4 Verification

- 是否有最小 smoke test
- 是否有 headless 可跑的測試
- 是否只測 compile，沒有測實際行為

---

## 5. 推薦的 AI Review Prompt

可直接對 AI 使用以下格式：

```text
請 review LibreOffice core 的 commits <commit1> 與 <commit2>。

Context:
- 規格：<spec1>, <spec2>
- 請以 published UNO API reviewer 的角度審查
- 優先找：
  1. API contract 與 spec 不一致
  2. published UNO API 的相容性風險
  3. UNO/VCL bridge 的 lifecycle、threading、listener、dispose 問題
  4. 缺少或不足的測試
- 忽略純風格與命名 nit

輸出格式：
- Findings only
- 每個 finding 要有 severity、file、reason、minimal fix
- 如果不確定，明說假設
```

這種 prompt 比「幫我看看有沒有 bug」有效很多，因為它會把 AI 聚焦到真正重要的 review 維度。

---

## 6. 這次 Review 後的工程結論

對 LibreOffice 這種大型老專案，新 UNO API 的 review 重點通常不是演算法，而是 contract quality：

- 文件是否正確
- 型別是否合適
- 新 API 是否破壞既有 listener / event contract
- state 是否持久
- 是否有 headless 可執行的驗證方式

如果只看「有沒有 crash」或「功能能不能 demo」，很容易漏掉之後最難修的 API 問題。

