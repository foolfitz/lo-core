# LibreOffice Toolkit CppUnit Practical Guide

本文件整理這次針對 `toolkit` 模組新增 API 的實際單元測試經驗，重點是讓之後的人能快速重現、定位、縮小問題。

---

## 1. 相關檔案

- 測試定義：
  - `toolkit/CppunitTest_toolkit.mk`
- 本次新增測試：
  - `toolkit/qa/cppunit/CustomWindow.cxx`
- 執行 log：
  - `workdir/CppunitTest/toolkit.test.log`

本次新增的 testcase：

- `testCustomPaintWindowPaintNotifications`
- `testSplitterWindowBasicState`
- `testTextMetricsLineCountType`

---

## 2. 基本執行方式

### 2.1 跑整包 toolkit cppunit

```bash
cd /home/jiajun/LibreOffice/LO-core
make -j8 CppunitTest_toolkit
```

### 2.2 只跑單一 testcase

LibreOffice 的 gbuild 支援 `CPPUNIT_TEST_NAME=`，這對縮小問題非常重要。

```bash
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testTextMetricsLineCountType
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testSplitterWindowBasicState
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testCustomPaintWindowPaintNotifications
```

### 2.3 看執行 log

```bash
tail -f workdir/CppunitTest/toolkit.test.log
```

---

## 3. 這次實測到的結果

### 3.1 可穩定通過的測試

以下兩支單測已實際跑過，會在目前環境正常完成：

```bash
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testTextMetricsLineCountType
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testSplitterWindowBasicState
```

實際觀察：

- `testTextMetricsLineCountType` 很快結束，適合當基本 smoke test
- `testSplitterWindowBasicState` 約數十毫秒，可作為 `splitter` 相關改動的快速回歸測試

### 3.2 目前會卡住的測試

```bash
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testCustomPaintWindowPaintNotifications
```

這支在目前 headless `svp` 測試環境會卡住，沒有正常結束。  
整包 `CppunitTest_toolkit` 目前也會停在這一支。

從 log 可看到最後停在：

```text
[_RUN_____] (anonymous namespace)::ToolkitCustomWindowTest::testCustomPaintWindowPaintNotifications
```

---

## 4. 為什麼會卡住

`CppunitTest_toolkit` 不是直接用互動式桌面環境跑，而是由 `cppunittester` 以 headless 方式啟動。  
實際執行環境中可看到：

- `--headless`
- `SAL_USE_VCLPLUGIN=svp`

這表示：

- 沒有一般桌面 plugin
- paint / invalidate / visibility / event loop 的行為可能與真實 UI 環境不同

因此，任何依賴「真的收到 repaint callback」的測試，都可能在 `svp` 下卡住或不穩定。

---

## 5. 實務建議

### 5.1 先跑穩定的 smoke test

改 `splitter`、`TextMetrics`、IDL 或 bridge 後，先跑：

```bash
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testTextMetricsLineCountType
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testSplitterWindowBasicState
```

這樣可以先確認：

- 型別變更沒有壞掉
- splitter 基本 state round-trip 正常

### 5.2 對 GUI repaint 類測試使用 timeout

如果懷疑 testcase 會卡住，不要直接無限等：

```bash
timeout 40s make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testCustomPaintWindowPaintNotifications
```

這樣比較容易辨識是：

- assertion fail
- crash
- 還是純粹 hang

### 5.3 先看 log 再決定下一步

```bash
tail -n 80 workdir/CppunitTest/toolkit.test.log
```

如果 log 只停在 `[_RUN_____]`，通常代表：

- testcase 已開始
- 但 event loop、callback、等待條件沒有完成

---

## 6. Debug 方式

### 6.1 用 gdb 跑單一 testcase

```bash
make CppunitTest_toolkit CPPUNIT_TEST_NAME=testSplitterWindowBasicState CPPUNITTRACE="gdb --args"
```

也可用在卡住的 `custom paint` 測試：

```bash
make CppunitTest_toolkit CPPUNIT_TEST_NAME=testCustomPaintWindowPaintNotifications CPPUNITTRACE="gdb --args"
```

### 6.2 確認測試程序是否仍存活

```bash
ps -ef | rg 'cppunittester .*libtest_toolkit.so'
```

如果程序還在，但 log 沒再前進，通常是 hang，不是 compile/link 問題。

---

## 7. Build 經驗教訓

### 7.1 初次 build 不要只跑 `offapi toolkit`

這次從尚未完整建好的 tree 出發時，直接跑：

```bash
make -j8 offapi toolkit
```

會在 link 階段缺：

- `libsvllo.so`
- `libvcllo.so`

原因不是 patch 本身，而是整體依賴尚未先建齊。

### 7.2 第一次應先做完整基線 build

較穩妥的流程：

```bash
make -j8
```

等完整 build 跑過一次後，再進行增量開發：

```bash
make -j8 offapi toolkit
make -j8 CppunitTest_toolkit
```

---

## 8. 對未來測試設計的建議

### 8.1 `custom paint` 測試應避免過度依賴實際 repaint 發生

如果目標是驗證 contract，而不是桌面 plugin 行為，測試可以考慮：

- 拆成較小的 headless-safe 檢查
- 避免等待真實 paint 事件
- 必要時針對 `svp` 做 skip 或改寫等待條件

### 8.2 對 toolkit 測試要優先設計「可 headless 穩定跑」的版本

因為 CI 與本地單測都傾向 headless 路徑，若測試依賴完整 GUI repaint，之後維護成本會很高。

---

## 9. 建議的日常回歸流程

1. 修改 IDL 或 bridge 後先重新編譯：

```bash
make -j8 offapi toolkit
```

2. 跑穩定單測：

```bash
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testTextMetricsLineCountType
make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testSplitterWindowBasicState
```

3. 若要碰 `custom paint`，先用 timeout 探測：

```bash
timeout 40s make -j8 CppunitTest_toolkit CPPUNIT_TEST_NAME=testCustomPaintWindowPaintNotifications
```

4. 若失敗或卡住，先看 log，再決定是否進 gdb。

