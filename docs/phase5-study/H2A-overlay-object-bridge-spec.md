# H2A：OverlayObject 橋接 XOverlayPainter 技術規格

> 日期：2026-03-25（rev.4 更新 2026-03-26）
> 分支：ext-uno-api-26-2-1
> 前置文件：`20260325-prerender-device-diagnosis.md`
> 狀態：**rev.4**（方案 A 已實作，zoom 修正完成）

---

## 1. 目標

### 要解決的問題

Python extension 透過 `XDocumentOverlay::addOverlay()` 註冊的 overlay painter，
在打字、游標閃爍、partial repaint 時被 VCL 的 `OverlayManagerBuffered` 擦除。
方向 F（F1–F4）和方向 G（G1–G3）已窮盡所有「在 paint pipeline 中直接畫到
OutputDevice」的嘗試，全部失敗。

### 根因

VCL 的 `OverlayManagerBuffered` 維護一個 background save/restore 循環：
`ImpSaveBackground` 擷取 PreRenderDevice 上的背景，`ImpBufferTimerHandler`
在游標閃爍時恢復背景再重繪 overlay。**只有註冊到 `OverlayManager` 的
`OverlayObject` 實例**才會在此循環中被重繪（透過 `ImpDrawMembers`）。
任何在此系統外直接畫到 OutputDevice 的像素，都在下一次 restore 時被擦除。

### 最小可用成果

Extension 註冊的 overlay（段落邊框、高亮等）在打字、游標閃爍、捲動、
partial repaint 時保持可見，不閃爍、不消失。

---

## 2. 背景與先例

### 當前的 workaround（失敗）

目前的實作使用 `m_fnPreEndDrawLayersHook`（`SwViewShell` 中的
`std::function<void(vcl::RenderContext&)>`），在 `DLPostPaint2` 前後直接
將 overlay 畫到 PreRenderDevice 和 window。overlay 在首次 paint 時可見，
但在游標閃爍時被 `ImpBufferTimerHandler` 的 background restore 擦除。

### 最接近的 LibreOffice 先例

Writer 內部已有多個 `OverlayObject` 子類別：

| 類別 | 位置 | 用途 |
|------|------|------|
| `sw::overlay::OverlayRanges` | `sw/source/uibase/docvw/OverlayRanges.cxx` | 文字選取高亮 |
| `ShadowOverlayObject` | `sw/source/uibase/docvw/ShadowOverlayObject.cxx` | 批註側邊陰影 |
| `AnchorOverlayObject` | `sw/source/uibase/docvw/AnchorOverlayObject.cxx` | 批註錨點連線 |
| `OverlayRangesOutline` | `sw/source/core/crsr/overlayrangesoutline.hxx` | 游標範圍輪廓 |

這些全部遵循相同模式：

1. 繼承 `sdr::overlay::OverlayObject`
2. Override `createOverlayObjectPrimitive2DSequence()` 回傳 `Primitive2DContainer`
3. 透過 `xOverlayManager->add()` 註冊
4. 資料變更時呼叫 `objectChange()`（protected）觸發重繪
5. 銷毀前手動呼叫 `OverlayManager::remove()` 移除，再銷毀物件
   （解構函式**不會**自動移除，只做 assert 檢查）

### 先例不足之處

上述先例全部是 C++ 原生程式碼，直接建構 `Primitive2D` 物件。
`XOverlayPainter` 使用 `XGraphics`（命令式繪圖 API），無法直接產生
`Primitive2D`。需要一個橋接層將命令式繪圖轉換為宣告式幾何描述。

---

## 3. 非目標

以下問題有意推遲，不在本 spec 範圍內：

| 非目標 | 歸屬 |
|--------|------|
| 替換 `XOverlayPainter` 為宣告式 API（回傳幾何資料） | 未來 Phase：方案 B |
| Overlay 的滑鼠互動（點擊、hover） | 未來 Phase：overlay hit-testing |
| 列印/PDF 匯出中包含 overlay | 設計決策：overlay 為螢幕專用 |
| 多視窗同步 overlay 狀態 | 當前 API 已定義為 view-local |
| Extension 自訂 overlay 透明度 | 可在未來版本的 IDL 中加入 |
| ~~Overlay 背景透明（alpha channel）~~ | ~~已在 rev.4 以方案 A 解決~~ |

---

## 4. 公開合約

### 4.1 UNO 介面：無變更

本 spec **不變更任何已發布的 UNO 介面**。`XOverlayPainter`、
`XDocumentOverlay` 的簽章和語意保持不變。

Extension 端呼叫 `addOverlay()` / `removeOverlay()` / `invalidateOverlay()` 的
行為與當前文件化的合約一致。唯一的差異是**可觀察行為改善**：overlay 不再
在游標閃爍和 partial repaint 時消失。

### 4.2 `XOverlayPainter::paintOverlay()` 呼叫情境變更

**變更前**（hook 方式）：
- `paintOverlay()` 在 `SwViewShell::Paint()` 內被呼叫
- 目標裝置是 window 或 PreRenderDevice
- 每次 VCL paint 循環呼叫一次

**變更後**（OverlayObject 方式）：
- `paintOverlay()` 在 `OverlayObject::createOverlayObjectPrimitive2DSequence()`
  內被呼叫
- 目標裝置是暫時的 VirtualDevice（用於 bitmap 擷取）
- 呼叫時機由 VCL OverlayManager 控制（可能在 paint 循環、游標閃爍、
  或 `objectChange()` 觸發時）
- 呼叫頻率可能比之前低（VCL 會快取 `Primitive2DContainer`）或高
  （OverlayManager 在游標閃爍時重繪）

**合約相容性**：`XOverlayPainter` IDL 已聲明：
> "The XGraphics reference is valid only for the duration of this call."

因此 extension 不應依賴繪圖目標的具體型別。此變更符合已發布合約。

### 4.3 座標系統

不變。所有座標仍為 document twips，與 `XParagraphNavigator::getParagraphBounds()`
一致。VirtualDevice 的 MapMode 將設定為與原 paint context 相同的
document twips MapMode。

### 4.4 `invalidateOverlay()` 語意變更

**變更前**：呼叫 `EditWin::Invalidate()` 觸發完整 VCL paint 循環。

**變更後**：呼叫 `OverlayObject::objectChange()` 觸發 OverlayManager
的重繪。這更高效（只重繪 overlay 層，不重繪文件內容），但語意等效。

### 4.5 執行緒與重入

- `paintOverlay()` 仍在 SolarMutex 下呼叫
- 在 `paintOverlay()` 內呼叫 `addOverlay()` / `removeOverlay()` 仍被禁止
  （IDL 已文件化此限制）
- 新增限制：`paintOverlay()` 可能在 `ImpBufferTimerHandler`（Timer 回呼）
  中被間接呼叫，而非僅在 `SwEditWin::Paint()` 中。extension 不應依賴
  呼叫堆疊。

---

## 5. 實作形狀

### 5.1 新增類別：`sw::overlay::OverlayExtensionPainter`

```
檔案：sw/source/uibase/docvw/OverlayExtensionPainter.hxx
      sw/source/uibase/docvw/OverlayExtensionPainter.cxx
```

```cpp
namespace sw::overlay
{
    class OverlayExtensionPainter final : public sdr::overlay::OverlayObject
    {
    public:
        OverlayExtensionPainter(
            css::uno::Reference<css::text::XOverlayPainter> xPainter,
            const MapMode& rDocMapMode);

        virtual ~OverlayExtensionPainter() override;

        /// 更新可見區域（捲動/縮放時）
        void updateVisibleArea(const css::awt::Rectangle& rVisibleArea);

        /// 標記幾何已變更，觸發 Primitive2D 重建
        void invalidate();

    private:
        virtual drawinglayer::primitive2d::Primitive2DContainer
            createOverlayObjectPrimitive2DSequence() override;

        css::uno::Reference<css::text::XOverlayPainter> m_xPainter;
        MapMode     m_aDocMapMode;
        css::awt::Rectangle m_aVisibleArea;
    };
}
```

### 5.2 `createOverlayObjectPrimitive2DSequence()` 的 Bitmap 橋接流程

這是本 spec 的核心。流程如下：

```
createOverlayObjectPrimitive2DSequence()
  │
  ├── 1. 呼叫 m_xPainter->getOverlayBounds()
  │      取得所有 overlay 矩形（document twips）
  │      如果為空 → 回傳空 Primitive2DContainer
  │
  ├── 2. 計算聯合邊界框（union bounding box）
  │      將所有 bounds 合併為一個 bounding rect
  │
  ├── 3. 建立 VirtualDevice
  │      尺寸 = bounding rect 轉換為 pixel 後的大小
  │      MapMode = document twips, origin 偏移到 bounding rect 左上角
  │      背景 = 不透明白色（見下方「透明度推遲」說明）
  │
  ├── 4. 呼叫 m_xPainter->paintOverlay(xGraphics, visibleArea)
  │      painter 在 VirtualDevice 上繪製
  │      使用 document twips 座標（與之前相同）
  │
  ├── 5. 從 VirtualDevice 取得 Bitmap
  │      vDev->GetBitmap(Point(0,0), sizePixel)
  │
  ├── 6. 建立 BitmapPrimitive2D
  │      transform = 將 unit square 映射到 bounding rect
  │      (document twips 空間中的位置和大小)
  │
  └── 7. 回傳 Primitive2DContainer { pBitmapPrimitive }
```

#### 決策：透明度（已解決 — rev.4）

~~**本階段不處理 alpha 透明。**~~ **已採用方案 A 解決。**

VirtualDevice 使用 `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` 背景。
Painter 繪製的線條和填充正常顯示，未繪製區域保持透明。

**實際實作（方案 A，2 行變更）**：
```cpp
ScopedVclPtrInstance<VirtualDevice> pVDev(DeviceFormat::WITH_ALPHA);
pVDev->SetBackground(Wallpaper(COL_TRANSPARENT));
pVDev->Erase();
```

**已驗證**：
- Painter 繪製的紅色邊框正常顯示
- 邊框內部文字可透視（不再被白色背景遮蓋）
- 帶 alpha 的 Bitmap 在 drawinglayer processor 中正確渲染

**未來改善路徑**（詳見附錄 D）：
1. ~~方案 A（已實作）~~
2. **方案 B（建議中期方案）**：GDIMetaFile 錄製 + `MetafilePrimitive2D`
   或 `wmfemfhelper::interpretMetafile()` 轉為向量 primitive，天生透明，
   放大不失真
3. **方案 C（遠期）**：自訂 XGraphics 實作，直接產生向量 primitive

#### 關鍵決策：Primitive2D 座標空間

`OverlayManager::ImpDrawMembers()` 使用 `BaseProcessor2D` 渲染
primitive。Processor 的 ViewInformation2D 包含 ViewTransformation，
它將 overlay 的邏輯座標轉換為裝置座標。

**OverlayRanges 先例**：`OverlayRanges::createOverlayObjectPrimitive2DSequence()`
直接使用 `B2DRange`（邏輯座標）建構 polygon，由 processor 的
ViewTransformation 處理轉換。

因此 `BitmapPrimitive2D` 的 transform 矩陣應在**邏輯座標空間**
（document twips）中定義位置和大小。Processor 會負責轉換到裝置座標。

**診斷建議**：Stage 2 應在日誌中輸出 transform matrix 的值，方便
座標問題調試：
```
SAL_WARN("sw.uno", "OverlayExtPainter: transform=("
    << boundLeft << "," << boundTop << " "
    << boundWidth << "x" << boundHeight << ")");
```

### 5.3 `SwXTextView` 的變更

#### `OverlayEntry` 結構擴充

```cpp
struct OverlayEntry
{
    sal_Int32 nHandle;
    sal_Int32 nLayer;
    css::uno::Reference<css::text::XOverlayPainter> xPainter;
    bool bVisible;
    std::unique_ptr<sw::overlay::OverlayExtensionPainter> pOverlayObj; // 新增
    bool bRegistered = false; // 新增：是否已註冊到 OverlayManager
};
```

#### `addOverlay()` 變更

```
addOverlay(xPainter, nLayer):
  1. 建立 OverlayExtensionPainter 實例
  2. 嘗試從 SdrView 取得 OverlayManager：
     SwView* pSwView = GetView();
     SdrView* pDV = pSwView ? pSwView->GetDrawView() : nullptr;
     if (pDV && pDV->PaintWindowCount() > 0)
         SdrPaintWindow* pPW = pDV->GetPaintWindow(0);
         const auto& xMgr = pPW->GetOverlayManager();
         if (xMgr.is())
             xMgr->add(*pOverlayObj);
             OverlayEntry.bRegistered = true;
  3. 儲存到 OverlayEntry（即使未註冊也保存，標記 bRegistered = false）
  4. 回傳 handle
```

**延遲註冊**：如果 `addOverlay()` 時 OverlayManager 尚不可用
（view 未完全初始化），OverlayEntry 以 `bRegistered = false`
保存。在首次 `invalidateOverlay()` 或其他適當時機重新嘗試註冊。

#### `removeOverlay()` 變更

**重要**：`OverlayObject` 的解構函式**不會**自動從 OverlayManager
移除（只做 assert 檢查）。必須在銷毀前手動呼叫
`getOverlayManager()->remove()`。

```
removeOverlay(nHandle):
  1. 找到 OverlayEntry
  2. 如果 pOverlayObj 已註冊到 OverlayManager：
     if (pOverlayObj->getOverlayManager())
         pOverlayObj->getOverlayManager()->remove(*pOverlayObj);
  3. 銷毀 pOverlayObj（此時 mpOverlayManager 應為 nullptr）
  4. 從 m_aOverlays 移除
```

**`~SwXTextView()` 清理**：解構函式中也必須對所有存活的
OverlayEntry 執行相同的 remove → destroy 流程。

#### `invalidateOverlay()` 變更

```
invalidateOverlay(rArea):
  1. 對每個 OverlayEntry:
     a. 如果 !bRegistered，嘗試延遲註冊（同 addOverlay 步驟 2）
     b. 如果 bVisible && bRegistered:
        pOverlayObj->invalidate()  // 內部呼叫 objectChange()
```

**`invalidate()` 方法說明**：`objectChange()` 是 `OverlayObject` 的
**protected** 方法，外部無法直接呼叫。`OverlayExtensionPainter::invalidate()`
作為 public wrapper，內部呼叫繼承到的 `objectChange()`。

#### `setOverlayVisible()` 變更

```
setOverlayVisible(nHandle, bVisible):
  1. 找到 OverlayEntry
  2. pOverlayObj->setVisible(bVisible)  // OverlayObject 的內建方法
```

### 5.4 移除的程式碼

以下現有機制將被**完全移除**：

| 被移除的元素 | 檔案 | 原因 |
|-------------|------|------|
| `m_fnPreEndDrawLayersHook` 成員 | `sw/inc/viewsh.hxx` | 不再需要 hook |
| `SetPreEndDrawLayersHook()` 方法 | `sw/inc/viewsh.hxx` | 同上 |
| Hook 呼叫點（G3 雙重繪製） | `sw/source/core/view/viewsh.cxx:2097-2120` | 同上 |
| Hook 安裝程式碼 | `sw/source/uibase/docvw/edtwin2.cxx:770-791` | 同上 |
| `PaintOverlays()` 方法 | `sw/source/uibase/uno/unotxvw.cxx` | 被 OverlayObject 取代 |
| `m_aOverlayRepaintIdle` 及其 handler | `sw/source/uibase/uno/unotxvw.cxx` | 不再需要 Idle recovery |

`CallOverlayPainters()` 和 `HasOverlays()` 保留但語意調整：
- `HasOverlays()` 保留，用於其他查詢需求
- `CallOverlayPainters()` 可能被 `OverlayExtensionPainter` 內部使用，
  或重構為 private helper

### 5.5 受影響的檔案清單

| 檔案 | 變更類型 |
|------|---------|
| `sw/source/uibase/docvw/OverlayExtensionPainter.hxx` | **新增** |
| `sw/source/uibase/docvw/OverlayExtensionPainter.cxx` | **新增** |
| `sw/source/uibase/inc/unotxvw.hxx` | 修改：OverlayEntry 加 pOverlayObj |
| `sw/source/uibase/uno/unotxvw.cxx` | 修改：addOverlay/removeOverlay/invalidate 重寫 |
| `sw/inc/viewsh.hxx` | 修改：移除 hook 成員 |
| `sw/source/core/view/viewsh.cxx` | 修改：移除 hook 呼叫點 |
| `sw/source/uibase/docvw/edtwin2.cxx` | 修改：移除 hook 安裝 |
| `sw/Library_sw.mk` | 修改：加入新 .cxx |

### 5.6 Build 整合

```makefile
# sw/Library_sw.mk — 在 uibase/docvw section 加入：
$(call gb_Library_add_exception_objects,sw,\
    sw/source/uibase/docvw/OverlayExtensionPainter \
)
```

---

## 6. 驗證計劃

### Stage 1：OverlayObject 基本存活測試

**假說**：在 OverlayManager 中註冊的 OverlayObject 能在游標閃爍
和 partial repaint 中存活。

**進入條件**：現有 `OverlayRanges` 的行為確認（它做為 Writer 選取
高亮，確實在游標閃爍時存活）。

**驗證步驟**：
1. 實作 `OverlayExtensionPainter`，先用硬編碼的紅色矩形
   （不呼叫 XOverlayPainter），直接建構 `PolyPolygonColorPrimitive2D`
2. 在 `addOverlay()` 中建立並註冊到 OverlayManager
3. 啟動 Writer，註冊 overlay，觀察：
   - 打字時 overlay 是否保持
   - 游標閃爍時是否保持
   - 捲動後是否保持

**證據**：overlay 矩形在以上三個操作中持續可見。

**回退路徑**：如果 OverlayObject 在 Writer 的 paint pipeline 中有
其他問題（例如 MapMode 不匹配），可回退到方向 H1（停用 BufferedOverlay）。

**退出備註**：此階段不涉及 XOverlayPainter 呼叫和 bitmap 橋接。

### Stage 2：Bitmap 橋接整合

**假說**：透過 VirtualDevice 擷取 XOverlayPainter 的繪圖結果，包裝為
`BitmapPrimitive2D`，能正確顯示在 overlay 層。

**進入條件**：Stage 1 的硬編碼矩形成功存活。

**驗證步驟**：
1. 實作完整的 `createOverlayObjectPrimitive2DSequence()`（含 VirtualDevice
   建立、XOverlayPainter 呼叫、Bitmap 擷取）
2. VirtualDevice 使用 `DeviceFormat::WITHOUT_ALPHA`（不透明白底）
3. 使用現有的 `document_overlay_validation.py` 做為測試 painter
4. 觀察：
   - 段落邊框是否正確顯示（位置、顏色、大小）
   - 白色背景是否與文件白底融合（視覺可接受）
   - 打字時是否保持

**證據**：
- 紅色段落邊框在打字、游標閃爍時保持可見
- 邊框位置與段落位置匹配
- 白色背景在白底文件上視覺可接受

**診斷記錄**：
```
SAL_LOG='+WARN.sw.uno'
```
在 `createOverlayObjectPrimitive2DSequence()` 中加入：
```
SAL_WARN("sw.uno", "OverlayExtPainter: bounds=" << nBounds
    << " vdev=" << nPixW << "x" << nPixH
    << " transform=(" << boundLeft << "," << boundTop
    << " " << boundWidth << "x" << boundHeight << ")"
    << " bitmap=" << (bitmapOk ? "ok" : "FAIL"));
```

**回退路徑**：如果 bitmap 橋接本身有問題（座標偏移、渲染異常），
可回退到 Stage 1 的純 primitive 方式（用固定幾何代替 XOverlayPainter
呼叫），確認是 bitmap 擷取問題還是 overlay 架構問題。

**退出備註**：此階段完成後應移除 hook 機制。

### Stage 3：清理與回歸測試

**假說**：移除 hook 機制後，所有現有功能（文字選取、批註、游標）不受影響。

**進入條件**：Stage 2 完成。

**驗證步驟**：
1. 移除所有 hook 相關程式碼（§5.4 清單）
2. 執行：
   ```bash
   make CppunitTest_sw_uibase_uno
   ```
3. 手動驗證：
   - 批註側邊欄仍正常顯示
   - 文字選取高亮仍正常
   - Extension overlay 在各種操作中保持可見

**CppUnit 測試擴充**：

現有的 `sw/qa/uibase/uno/uno.cxx` 中有 `MockOverlayPainter` 和
`CornerOverlayPainter`。需要擴充測試：

```cpp
// 驗證 overlay 在 addOverlay 後確實註冊到 OverlayManager
CPPUNIT_TEST(testOverlayObjectRegistration);

// 驗證 removeOverlay 後 OverlayObject 從 OverlayManager 移除
CPPUNIT_TEST(testOverlayObjectRemoval);

// 驗證 invalidateOverlay 觸發 objectChange
CPPUNIT_TEST(testOverlayInvalidation);
```

注意：headless 測試環境中 OverlayManager 可能不存在（無 SdrView），
需要檢查 guard。

---

## 7. 驗收標準

| # | 行為 | 證據 |
|---|------|------|
| A1 | Extension overlay 在首次顯示時位置正確 | 邊框與段落邊界吻合（白底可接受） |
| A2 | 打字時 overlay 保持可見 | 連續輸入 10 字，overlay 無閃爍 |
| A3 | 游標閃爍時 overlay 保持可見 | 靜止 5 秒，overlay 不消失 |
| A4 | Partial repaint 不擦除 overlay | 在 overlay 區域外操作，overlay 保持 |
| A5 | 捲動後 overlay 跟隨文件位置 | 向下捲動再回來，overlay 位置正確 |
| A6 | `removeOverlay()` 後 overlay 消失 | 移除後 overlay 不再顯示 |
| A7 | `setOverlayVisible(false)` 後 overlay 隱藏 | 隱藏後 overlay 不顯示 |
| A8 | Extension overlay 不出現在列印/PDF 中 | 匯出 PDF 檢查無 overlay |
| A9 | 批註側邊欄、文字選取仍正常 | 手動驗證其他 overlay 功能 |
| A10 | `CppunitTest_sw_uibase_uno` 通過 | 自動化測試通過 |

---

## 8. 風險與推遲的工作

### 已知風險

| 風險 | 影響 | 緩解 |
|------|------|------|
| **Bitmap 效能**：每次 overlay 重繪需建立 VirtualDevice + rasterize | Overlay 區域大時可能卡頓 | `getOverlayBounds()` 回傳的邊界框盡量小；VCL 的 primitive 快取機制會減少不必要的重建 |
| ~~**不透明背景**~~ | ~~已在 rev.4 以方案 A 解決~~ | ~~使用 `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT`~~ |
| **座標精度**：pixel ↔ twips 轉換可能有 ±1 pixel 偏差 | overlay 邊框偏移 | 使用與 OverlayRanges 相同的座標路徑 |
| **OverlayManager 不存在**：headless 或 view 初始化前呼叫 `addOverlay()` | 無法註冊 OverlayObject | `addOverlay()` 以 `bRegistered=false` 保存；`invalidateOverlay()` 時重新嘗試註冊（延遲註冊機制，見 §5.3） |
| **Primitive2D 快取**：VCL 可能快取 primitive，不每次呼叫 `createOverlayObjectPrimitive2DSequence()` | overlay 內容不更新 | `invalidateOverlay()` 呼叫 `objectChange()` → `resetPrimitive2DSequence()` 強制重建 |

### 已接受的限制

- **Bitmap 橋接是間接路徑**：每個 overlay painter 產生一整張 bitmap，
  而非精確的向量幾何。這對細線條（1px 邊框）在高 DPI 下可能有
  anti-aliasing 差異。本 spec 接受此限制，未來可透過方案 B
  （宣告式 API）解決。

- **單一 bounding rect**：每個 painter 的所有 overlay 區域合併為一個
  bounding rect 的 bitmap。如果 painter 在文件頂部和底部各畫一個
  小矩形，中間的大片空白也會被包含在 bitmap 中。未來可優化為
  multi-region bitmap 或分拆為多個 OverlayObject。

### 推遲的工作

| 推遲項目 | 歸屬 |
|---------|------|
| ~~Alpha 透明支援~~ | ~~**已實作**（方案 A：Alpha VDev）~~ |
| 方案 B：GDIMetaFile 錄製 + 向量 Primitive | **研究完成**：見附錄 D §D.2 方案 B，建議作為中期優化 |
| 方案 C：自訂 XGraphics（宣告式 API） | 見附錄 D §D.2 方案 C，遠期方案 |
| Multi-region 優化（多個小 bitmap 代替一個大 bitmap） | 效能優化 Phase |
| Overlay hit-testing（滑鼠點擊 overlay 元素） | 互動功能 Phase |
| Overlay 動畫支援（閃爍、漸變） | 進階功能 Phase |

---

## 附錄 A：VCL Overlay 系統的關鍵流程

```
SwEditWin::Paint(rRenderContext)
  └── SwViewShell::Paint()
        ├── DLPrePaint2()
        │     └── BeginDrawLayers()
        │           └── PreparePreRenderDevice() → 建立 PreRenderDevice
        ├── PaintSwFrame(rRenderContext)  → 畫文件內容到 window
        └── DLPostPaint2()
              └── EndDrawLayers()
                    └── EndCompleteRedraw()
                          ├── DrawOverlay(redrawRegion)
                          │     └── OverlayManager::completeRedraw()
                          │           ├── ImpSaveBackground()  ← 擷取背景
                          │           └── ImpDrawMembers()     ← 繪製所有 OverlayObject
                          └── OutputPreRenderDevice()          ← flush 到 window

ImpBufferTimerHandler() (游標閃爍 Timer):
  ├── ImpRestoreBackground()  ← 恢復之前儲存的背景
  └── ImpDrawMembers()        ← 重繪所有 OverlayObject
```

我們的 `OverlayExtensionPainter` 作為 OverlayObject 註冊後，會在
`ImpDrawMembers()` 中被呼叫，因此能自然存活於整個 buffer 循環。

## 附錄 B：與方向 H1 的比較

| 面向 | H1（停用 BufferedOverlay） | H2A（OverlayObject 橋接） |
|------|---------------------------|--------------------------|
| 工時 | 1-2 小時 | 1-2 天 |
| 架構正確性 | Workaround | 正解 |
| 游標閃爍品質 | 可能閃爍（失去 double-buffering） | 無閃爍 |
| 影響範圍 | 影響所有 overlay（選取、批註等） | 只影響 extension overlay |
| 效能 | 無 overhead | bitmap rasterize overhead |
| 未來可擴充性 | 有限 | 可擴展為宣告式 API |

建議：先做 Stage 1 快速驗證 OverlayObject 在 Writer 中能存活，
如果遇到無法解決的問題，再退回 H1。

---

## 附錄 C：實作偏差記錄（rev.2）

> 以下記錄實際實作與原始 spec 的差異。原因多為 VCL `OverlayManager`
> 內部行為（primitive caching、`objectChange()` → `getBaseRange()` 的
> 隱式 primitive 重建等）在實作過程中才完全暴露。

### C.1 單一共享 OverlayObject（最大偏差）

**Spec 設計**：每個 `XOverlayPainter` 擁有獨立的
`OverlayExtensionPainter` 實例，各自註冊到 OverlayManager。

**實際實作**：所有 extension overlay 共享**一個** `OverlayExtensionPainter`
實例。`SwXTextView` 持有 `std::unique_ptr<OverlayExtensionPainter> m_pOverlayObj`
（在首個 overlay 加入時建立，最後一個 overlay 移除時銷毀）。

**原因**：每個 painter 獨立的 OverlayObject 導致三個無法解決的問題：
1. **Z-order 不可控**：OverlayManager 依 `add()` 順序繪製，無法依
   layer 排序
2. **Visibility 旁路**：`objectChange()` 內部呼叫 `getBaseRange()` →
   `createOverlayObjectPrimitive2DSequence()`，此呼叫不受
   `isVisible()` 檢查保護，導致隱藏的 painter 仍被呼叫
3. **相互干擾**：多個 OverlayObject 的 `objectChange()` 互相觸發
   window invalidation，造成不可預測的 repaint 次數

### C.2 `OverlayExtensionPainter` 介面變更

**Spec 設計**：
```cpp
OverlayExtensionPainter(
    css::uno::Reference<css::text::XOverlayPainter> xPainter,
    const MapMode& rDocMapMode);
void updateVisibleArea(const css::awt::Rectangle& rVisibleArea);
```

**實際實作**：
```cpp
using PaintFunc = std::function<Primitive2DContainer(
    const css::awt::Rectangle& rVisibleArea)>;
explicit OverlayExtensionPainter(PaintFunc fnPaint);
```

不再持有 `XOverlayPainter` 引用或 `MapMode` 成員。改為接受
`std::function` callback，由 `SwXTextView::paintAllOverlays()`
提供實際的繪製邏輯。`updateVisibleArea()` 已移除——visible area
在 `createOverlayObjectPrimitive2DSequence()` 內從 OverlayManager
的 output device 即時計算。

### C.3 MapMode 取得時機

**Spec 設計**：在 `addOverlay()` 時從 `getPrePostMapMode()` 取得
MapMode，儲存在 `m_aDocMapMode` 成員中。

**實際實作**：在 `paintAllOverlays()` 執行時從
`GetView()->GetEditWin().GetMapMode()` 即時取得。

**原因**：`getPrePostMapMode()` 只在 `DLPrePaint2()` 時被設定。
如果 `addOverlay()` 在第一次 paint cycle 前呼叫（常見於初始化階段），
`getPrePostMapMode()` 回傳預設值 `MapPixel`，導致 VirtualDevice 的
MapMode 錯誤。`EditWin.GetMapMode()` 在 view 初始化後即正確。

### C.4 Bitmap 橋接邏輯位置

**Spec 設計**：bitmap 橋接（VirtualDevice → `BitmapPrimitive2D`）
在 `OverlayExtensionPainter::createOverlayObjectPrimitive2DSequence()`
內實作。

**實際實作**：bitmap 橋接邏輯在
`SwXTextView::paintAllOverlays()` 中實作。
`OverlayExtensionPainter::createOverlayObjectPrimitive2DSequence()`
只計算 visible area 並呼叫 callback。

**原因**：由 C.1 的單一 OverlayObject 設計導致。paintAllOverlays
需要遍歷所有 visible entry、檢查個別 visibility、依 layer 排序，
這些邏輯屬於 SwXTextView 的職責。

### C.5 Reentrancy 防護（Spec 未涵蓋）

**新增機制**：
- `m_bPaintingOverlays` flag：防止 `paintAllOverlays()` 遞迴呼叫
  （`objectChange()` → `getBaseRange()` → `createOverlayObjectPrimitive2DSequence()`
  → `paintAllOverlays()` 的隱式遞迴路徑）
- Snapshot-based 迭代：在 `paintAllOverlays()` 開始時對 visible
  painter 做 snapshot，callbacks 中的 `addOverlay()`/`removeOverlay()`
  修改 `m_aOverlays` 不影響當次迭代
- `addOverlay()` 在 `m_bPaintingOverlays=true` 時跳過 `invalidate()`
  呼叫，避免巢狀 `objectChange()`
- `removeOverlay()` 在 `m_bPaintingOverlays=true` 時延遲
  `destroyOverlayObj()` 呼叫（OverlayObject 正在執行中，不能銷毀）

**原因**：`objectChange()` 內部呼叫 `getBaseRange()` 會觸發
primitive 重建，形成間接遞迴。原 spec §4.5 聲明 paintOverlay 內
呼叫 addOverlay/removeOverlay「被禁止」，但實際測試
（`testDocumentOverlayCallbackReentrancy`）要求支援此情境。

### C.6 `removeOverlay()` 的解構方式

**Spec 設計**：移除時需手動呼叫 `getOverlayManager()->remove()`，
因為「解構函式不會自動移除」。

**實際實作**：`OverlayExtensionPainter` 解構函式呼叫
`getOverlayManager()->remove(*this)`（仿 `OverlayRanges` 模式）。
`removeOverlay()` 只需從 `m_aOverlays` erase entry，共享
OverlayObject 在最後一個 entry 移除時 `m_pOverlayObj.reset()` →
dtor 自動 remove。

**原因**：實際查閱 `OverlayRanges` 和其他 Writer overlay 子類別後
發現，dtor 中呼叫 `remove()` 是通用做法。Spec 基於 base class
的 assert 檢查做出保守判斷，但 assert 只在 dtor 中檢查
`mpOverlayManager == nullptr`，在呼叫 `remove()` 後該條件即滿足。

### C.7 `OverlayEntry` 結構簡化

**Spec 設計**：
```cpp
struct OverlayEntry {
    ...
    std::unique_ptr<OverlayExtensionPainter> pOverlayObj; // per-entry
    bool bRegistered = false;
};
```

**實際實作**：
```cpp
struct OverlayEntry {
    sal_Int32 nHandle;
    sal_Int32 nLayer;
    css::uno::Reference<css::text::XOverlayPainter> xPainter;
    bool bVisible;
};
// 共享 OverlayObject 在 SwXTextView 層級：
std::unique_ptr<OverlayExtensionPainter> m_pOverlayObj;
bool m_bOverlayRegistered = false;
bool m_bPaintingOverlays = false;
```

### C.8 `setOverlayVisible()` 實作差異

**Spec 設計**：呼叫 per-entry 的 `pOverlayObj->setVisible(bVisible)`
（OverlayObject 內建方法）。

**實際實作**：更新 entry 的 `bVisible` flag，然後呼叫共享
`m_pOverlayObj->invalidate()` 觸發 primitive 重建。在
`paintAllOverlays()` 中檢查 `bVisible` 決定是否呼叫該 painter。

**原因**：單一共享 OverlayObject 的 `setVisible(false)` 會隱藏
**所有** overlay。個別 visibility 必須在 `paintAllOverlays()` 中
per-entry 檢查。

### C.9 `paintOverlay()` 呼叫頻率變更

**Spec §4.2 描述**：呼叫頻率由 VCL OverlayManager 控制，
「可能比之前低或高」。

**實際行為**：`paintOverlay()` 只在 `invalidateOverlay()` 呼叫後
（清除 primitive cache）才會重新被呼叫。Window 的 `Invalidate()` +
`PaintImmediately()` 在 cache 有效時只渲染 cached bitmap，不會
觸發 `paintOverlay()`。

**測試影響**：原測試 `testDocumentOverlayPaintCallbackEveryRepaint`
預期每次 window repaint 都呼叫 painter，已修改為只驗證
`invalidateOverlay()` 後的重繪。

### C.10 已移除的程式碼（與 Spec §5.4 一致，另有額外移除）

Spec §5.4 列出的所有 hook 相關程式碼均已移除。額外移除：
- `CallOverlayPainters()`（Spec 說「可能保留」，實際已移除——
  bitmap 橋接邏輯由 `paintAllOverlays()` 取代）
- `PaintOverlays()`（已確認移除）
- `m_aOverlayRepaintIdle` 及其 handler（已確認移除）

`HasOverlays()` 如 Spec 所述保留。

---

## 附錄 D：透明度方案研究（rev.3）

> 日期：2026-03-26
> 狀態：**研究完成，待選定方案**

### D.1 問題陳述

§5.2「透明度推遲」中使用 `DeviceFormat::WITHOUT_ALPHA` + 白色背景的
bitmap 橋接方案，在視覺測試中確認：overlay bounding rect 的白色背景
會遮蓋文件文字。紅色邊框可見，但段落文字被白色矩形覆蓋。

此問題使 bitmap 橋接方案無法用於任何需要透視文件內容的 overlay（包括
段落邊框——因為邊框內部的文字被遮住）。必須解決透明度問題才能達到
§1「最小可用成果」。

### D.2 研究的三個方案

#### 方案 A：Alpha VirtualDevice + BitmapPrimitive2D

**原理**：使用帶 alpha 通道的 VirtualDevice 渲染，取得帶透明度的 Bitmap。

**關鍵發現**：
1. `DeviceFormat::WITH_ALPHA` 建立支援 alpha 的 VirtualDevice
2. 設定透明背景：`SetBackground(Wallpaper(COL_TRANSPARENT))` + `Erase()`
3. `GetBitmap(Point, Size)` 回傳的 `Bitmap` 物件自動攜帶 alpha 資訊
   （`HasAlpha()` 為 true），**無需**呼叫 `GetBitmapEx()`
4. `BitmapPrimitive2D` 儲存 `Bitmap`；processor 呼叫
   `DrawTransformedBitmapEx(aLocalTransform, aBitmap)` 渲染，
   VCL 會自動處理帶 alpha 的 Bitmap
5. 已有測試驗證此路徑：`vcl/qa/cppunit/bitmaprender/BitmapRenderTest.cxx`

**實作變更（最小）**：
```cpp
// 變更 1：DeviceFormat
ScopedVclPtrInstance<VirtualDevice> pVDev(DeviceFormat::WITH_ALPHA);
// 變更 2：透明背景取代白色背景
pVDev->SetBackground(Wallpaper(COL_TRANSPARENT));
pVDev->Erase();
// 其餘不變
```

**優點**：
- 改動量最小（只改 2 行）
- 保留現有 bitmap 橋接架構
- Extension painter 的 XGraphics 繪圖介面不變
- 帶 alpha 的 Bitmap 在 drawinglayer processor 中已有處理路徑

**缺點/風險**：
- 每次 repaint 建立 VirtualDevice + 分配 bitmap 記憶體（效能開銷）
- Pre-multiplied alpha 在某些後端不完美 round-trip（測試中觀察到）
- 繪圖解析度受 VirtualDevice 像素大小限制（放大時可能出現鋸齒）
- `GetBitmap()` 需在 MapMode 關閉狀態下呼叫（已修正的 bug）

#### 方案 B：GDIMetaFile 錄製 + MetafilePrimitive2D

**原理**：在 VirtualDevice 上啟用 GDIMetaFile 錄製，捕獲 painter 的繪圖
操作，然後用 `MetafilePrimitive2D` 或 `wmfemfhelper::interpretMetafile()`
轉換為向量 primitive。天生透明——只重播有明確繪製的內容。

**關鍵發現**：
1. GDIMetaFile 只記錄**明確的繪圖操作**（DrawLine、DrawRect 等），
   不會產生任何背景——天生透明
2. `MetafilePrimitive2D` 有完整的 `create2DDecomposition()` 實作，
   呼叫 `wmfemfhelper::interpretMetafile()` 將 MetaAction 轉為 Primitive2D
3. `wmfemfhelper` 是 3000+ 行的成熟程式碼，支援 50+ 種 MetaAction 類型
4. Transform 模型：MetafilePrimitive2D 接受一個 B2DHomMatrix，
   配合 GDIMetaFile 的 PrefMapMode/PrefSize 計算最終座標
5. 內容超出邊界時自動用 MaskPrimitive2D 裁切（tdf#113197 修正）

**實作流程**：
```cpp
// 1. 建立 VirtualDevice（不需要 alpha）
ScopedVclPtrInstance<VirtualDevice> pVDev(DeviceFormat::WITHOUT_ALPHA);
pVDev->SetMapMode(MapMode(MapUnit::MapTwip));
// 2. 開始錄製
GDIMetaFile aMetaFile;
aMetaFile.Record(pVDev.get());
// 3. 建立 UNO Graphics，讓 painter 繪製
auto xGraphics = pVDev->CreateUnoGraphics();
xPainter->paintOverlay(xGraphics, rVisibleArea);
// 4. 停止錄製
aMetaFile.Stop();
aMetaFile.SetPrefMapMode(MapMode(MapUnit::MapTwip));
aMetaFile.SetPrefSize(Size(nBoundW, nBoundH));
// 5. 方式 a：直接用 MetafilePrimitive2D
basegfx::B2DHomMatrix aTransform;
aTransform.scale(nBoundW, nBoundH);
aTransform.translate(nLeft, nTop);
aResult.push_back(new MetafilePrimitive2D(aTransform, aMetaFile));
// 或方式 b：用 interpretMetafile 轉為向量 primitive
// auto aPrimitives = wmfemfhelper::interpretMetafile(aMetaFile, aViewInfo);
```

**優點**：
- 天生透明——不需要處理 alpha 通道問題
- 產生向量 primitive，不受解析度限制（放大時保持清晰）
- 不需要分配 bitmap 記憶體（效能更好）
- 利用已有的成熟基礎設施（wmfemfhelper 3000+ 行）

**缺點/風險**：
- MetafilePrimitive2D 的 transform 模型較複雜
  （PrefMapMode + PrefSize + primitive transform 三層映射）
- `wmfemfhelper::interpretMetafile()` 不在 sw 模組的公開標頭中
  （位於 `drawinglayer/inc/wmfemfhelper.hxx`，非 public include）
- GDIMetaFile 錄製可能捕獲 VCLXGraphics 的 `InitOutputDevice()` 副作用
  （SetLineColor 等狀態設定也會被錄製為 MetaAction）
- 可能觸發 MetafilePrimitive2D decomposition 的效能開銷
  （每個 MetaAction 轉為一個 Primitive2D）
- VirtualDevice 需要實際的 OutputDevice 用於座標計算，但 `EnableOutput(false)`
  可以避免實際繪圖（只錄製 MetaAction）

#### 方案 C：自訂 XGraphics 實作（純向量 Primitive 生成器）

**原理**：建立一個自訂的 `XGraphics` UNO 實作，不繪製到任何 OutputDevice，
而是將每個 drawLine/drawRect 呼叫直接轉換為對應的 drawinglayer Primitive2D。

**關鍵發現**：
1. XGraphics 介面定義在 `offapi/com/sun/star/awt/XGraphics.idl`
2. 方法集合有限：drawLine, drawRect, drawRoundedRect, drawPolyLine,
   drawPolygon, drawPolyPolygon, drawEllipse, drawArc, drawPie,
   drawChord, drawGradient, drawText, drawTextArray
3. 對應的 Primitive2D 都已存在：
   - drawLine → PolygonHairlinePrimitive2D
   - drawRect → PolyPolygonColorPrimitive2D（filled）
     + PolyPolygonHairlinePrimitive2D（border）
   - drawText → TextSimplePortionPrimitive2D
4. 需維護狀態（line color, fill color, clip region, font）

**優點**：
- 天生透明
- 向量品質（放大清晰）
- 不需要 VirtualDevice、GDIMetaFile 等中間層
- 效能最佳（直接生成 primitive，無轉換開銷）
- 完全掌控 primitive 生成（可做效能優化）

**缺點/風險**：
- 實作工作量最大（需實作 XGraphics 介面的所有方法）
- 需處理 push/pop 狀態堆疊、clip region 等
- drawText 轉 Primitive2D 較複雜（字型度量、佈局）
- 座標系統轉換需要仔細處理（XGraphics 用 OutputDevice 邏輯座標，
  Primitive2D 用 basegfx 座標）
- 功能上與 `wmfemfhelper::interpretMetafile()` 重複

### D.3 方案比較

| 維度 | A：Alpha VDev | B：MetaFile | C：自訂 XGraphics |
|------|:---:|:---:|:---:|
| 改動量 | ★★★ 最小 | ★★ 中等 | ★ 最大 |
| 透明品質 | ★★ per-pixel alpha | ★★★ 天生透明 | ★★★ 天生透明 |
| 放大清晰度 | ★ 受像素限制 | ★★★ 向量 | ★★★ 向量 |
| 效能 | ★ bitmap 分配 | ★★ metafile 轉換 | ★★★ 直接生成 |
| 風險 | ★★ alpha round-trip | ★★ transform 複雜 | ★ 實作工作量大 |
| 成熟度 | ★★★ 路徑已驗證 | ★★★ wmfemfhelper 成熟 | ★ 全新程式碼 |
| 對現有架構的侵入 | ★★★ 幾乎不動 | ★★ 需改 primitive 類型 | ★ 需新增 class |

### D.4 建議方案

**短期（立即可用）：方案 A — Alpha VirtualDevice**

理由：
- 改動量最小（2 行），可立即驗證「最小可用成果」
- 風險最低：Bitmap alpha 路徑在 VCL 中已被 OverlayBitmapEx 等使用
- 允許快速進入手動測試驗證階段

**中期（效能/品質優化）：方案 B — GDIMetaFile 錄製**

理由：
- 向量渲染品質優於 bitmap（放大不失真）
- 不需要分配大塊 bitmap 記憶體
- 天生透明，不需要 alpha 通道管理
- 基礎設施已成熟（wmfemfhelper）

**遠期（不建議現階段投入）：方案 C — 自訂 XGraphics**

理由：
- 效能最佳但工作量大
- 與方案 B 功能重複（本質上是重新實作 wmfemfhelper 的子集）
- 只有在方案 B 遇到效能瓶頸時才值得投入

### D.5 方案 A 實作注意事項

1. **`GetBitmap()` 的 MapMode 問題**（已在 rev.2 實作中修正）：
   必須在呼叫 `GetBitmap()` 前 `EnableMapMode(false)`，否則
   Point/Size 會被 MapMode 二次轉換
2. **VirtualDevice 的 DPI**：應使用 `ScopedVclPtrInstance<VirtualDevice>`
   的預設建構（繼承螢幕 DPI），不要傳入 reference device
3. **pre-multiplied alpha**：某些 VCL 後端（特別是 Cairo）使用
   pre-multiplied alpha，完全透明的像素可能不會完美 round-trip
   為 `(0,0,0,0)`。但這只影響純透明區域，不影響實際繪圖
4. **painter 繪製順序**：painter 的 `paintOverlay()` 在透明背景上繪製，
   線條和填充會正常顯示；未繪製的區域保持透明

---

## 附錄 E：Bitmap 橋接實作除錯記錄（rev.4）

> 日期：2026-03-26
> 狀態：**全部解決**

本附錄記錄 bitmap 橋接從首次可見到完全可用過程中遇到的四個 bug 及修正。

### E.1 GetBitmap() MapMode 雙重轉換

**症狀**：`CopyBits with zero or negative width or height` 警告，bitmap
為空白。

**根因**：`OutputDevice::GetBitmap(Point, Size)` 將參數視為**邏輯座標**，
內部透過 MapMode 轉換為 pixel。但我們傳入的 `aPixelSize` 已經是 pixel
單位（來自 `GetOutputSizePixel()`），造成二次轉換。此外 MapMode origin
為 `(-nLeft, -nTop)`，導致 (0,0) 被轉成負數 pixel 座標。

**修正**：在呼叫 `GetBitmap()` 前關閉 MapMode：
```cpp
pVDev->EnableMapMode(false);
Bitmap aBmp = pVDev->GetBitmap(Point(0, 0), aPixelSize);
pVDev->EnableMapMode(true);
```

**驗證**：`vcl/source/outdev/bitmap.cxx:290-300` 確認 GetBitmap 內部
呼叫 `ImplLogicToDevicePixel()` 轉換座標。

### E.2 白色背景遮蓋文字

**症狀**：紅色邊框可見，但邊框內的文字被白色矩形覆蓋。

**根因**：`DeviceFormat::WITHOUT_ALPHA` + 白色背景 → bitmap 為不透明白底。
`BitmapPrimitive2D` 渲染時覆蓋下方的文件內容。

**修正**（方案 A — Alpha VDev）：
```cpp
// 改用帶 alpha 的 VirtualDevice
ScopedVclPtrInstance<VirtualDevice> pVDev(DeviceFormat::WITH_ALPHA);
pVDev->SetBackground(Wallpaper(COL_TRANSPARENT));
pVDev->Erase();
```

**驗證**：文字在邊框內正常可見。`Bitmap::HasAlpha()` 為 true。
Drawinglayer processor 的 `DrawTransformedBitmapEx()` 正確處理帶 alpha
的 Bitmap。

### E.3 BitmapPrimitive2D Transform Matrix 乘法順序

**症狀**：overlay 位置錯誤。

**根因**：`B2DHomMatrix::scale()` 和 `translate()` 使用**後乘**
（`this = this * op`）。

```cpp
// 錯誤：scale(W,H) → translate(X,Y) 產生 Scale*Translate
// 映射 (0,0) → (W*X, H*Y)，不是 (X, Y)
aTransform.scale(nBoundW, nBoundH);
aTransform.translate(nLeft, nTop);
```

**修正**：直接設定矩陣元素：
```cpp
basegfx::B2DHomMatrix aTransform;
aTransform.set(0, 0, nBoundW);  // scaleX
aTransform.set(1, 1, nBoundH);  // scaleY
aTransform.set(0, 2, nLeft);    // translateX
aTransform.set(1, 2, nTop);     // translateY
```

**驗證**：`basegfx/source/matrix/b2dhommatrix.cxx:203-214` 確認
`translate()` 執行後乘。`svx/source/sdr/overlay/overlaytools.cxx:156-161`
使用直接設定矩陣元素作為 BitmapPrimitive2D 的 reference pattern。

### E.4 Zoom 後 Overlay 位移

**症狀**：頁面縮放後框線整體跑掉（displacement），zoom 前正常。
向量 primitive 和 bitmap primitive 有相同問題，排除 bitmap 本身的問題。

**根因**：兩個獨立問題疊加。

**問題 A — Stale OverlayManager Registration**：

`OverlayManager` 可能在 zoom 時被銷毀重建。銷毀時呼叫
`remove()` 將 OverlayObject 的 `mpOverlayManager` 設為 nullptr，
但 `SwXTextView::m_bOverlayRegistered` 仍為 true。
`ensureOverlayRegistered()` 因 flag 為 true 而跳過重新註冊。

修正：
```cpp
void SwXTextView::ensureOverlayRegistered()
{
    if (!m_pOverlayObj)
        return;
    // 改為檢查 OverlayObject 的實際 manager 狀態
    if (m_bOverlayRegistered && m_pOverlayObj->getOverlayManager())
        return;  // 仍有效
    m_bOverlayRegistered = false;  // 重置 stale flag
    // ... 重新註冊 ...
}
```

**問題 B — Primitive Cache 在 MapMode 變更後未失效**：

OverlayManager 持續存活時（不銷毀重建），OverlayObject 的 cached
primitives 在 zoom 後仍被重用。`getOverlayObjectPrimitive2DSequence()`
檢查 cache 非空即直接回傳，不呼叫 `createOverlayObjectPrimitive2DSequence()`。

修正：覆寫 `getOverlayObjectPrimitive2DSequence()`，在每次呼叫時比較
output device 的 MapMode 與上次建立 primitives 時的 MapMode。不同則
清除 cache：

```cpp
// OverlayExtensionPainter.hxx
virtual Primitive2DContainer
    getOverlayObjectPrimitive2DSequence() const override;
mutable MapMode m_aLastMapMode;

// OverlayExtensionPainter.cxx
Primitive2DContainer
OverlayExtensionPainter::getOverlayObjectPrimitive2DSequence() const
{
    if (getOverlayManager())
    {
        const MapMode& rCur = getOverlayManager()->getOutputDevice().GetMapMode();
        if (rCur != m_aLastMapMode)
        {
            m_aLastMapMode = rCur;
            const_cast<OverlayExtensionPainter*>(this)->resetPrimitive2DSequence();
            const_cast<OverlayExtensionPainter*>(this)->maBaseRange.reset();
        }
    }
    return OverlayObject::getOverlayObjectPrimitive2DSequence();
}
```

此模式遵循 `overlayobject.hxx:63-69` 的註解指引：「Resetting is allowed
in ::getOverlayObjectPrimitive2DSequence() implementations if the
conditions have changed to force a re-creation.」

**驗證**：zoom 前後框線位置正確。`createPrimitives` 在 zoom 後被重新呼叫，
visArea 更新為新的 zoom 級別。

### E.5 右方和下方邊框被裁切

**症狀**：overlay 的四邊框線中，左邊和上邊正常顯示，右邊和下邊被裁切。

**根因**：VirtualDevice 的大小剛好等於 `getOverlayBounds()` 回傳的
bounding rect。Painter 在座標 `(x + width, y + height)` 畫線，但這些
座標落在 VirtualDevice 的邊界像素**外**（pixel 索引 0..N-1，座標 N
超出範圍），線條被裁切。

**修正**：在 bounding rect 四邊各加 40 twips margin（≈ 0.7mm）：
```cpp
constexpr tools::Long nMargin = 40;
nLeft   -= nMargin;
nTop    -= nMargin;
nRight  += nMargin;
nBottom += nMargin;
```

Margin 在 VirtualDevice 的 MapMode origin 和 BitmapPrimitive2D 的
transform matrix 中自動反映（因為 nLeft/nTop/nBoundW/nBoundH 在
margin 加入後才被使用），所以 bitmap 的最終定位仍正確。透明的 margin
區域在渲染時不會遮蓋任何文件內容。

**驗證**：四邊框線均完整顯示。

### E.6 修正摘要

| # | Bug | 根因 | 修正 | 影響的檔案 |
|---|-----|------|------|-----------|
| E.1 | GetBitmap 空白 | MapMode 雙重轉換 | `EnableMapMode(false)` before GetBitmap | `unotxvw.cxx` |
| E.2 | 白色背景遮蓋文字 | 無 alpha 通道 | `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` | `unotxvw.cxx` |
| E.3 | Transform 位置錯誤 | B2DHomMatrix 後乘語意 | 直接設定矩陣元素 | `unotxvw.cxx` |
| E.4 | Zoom 後位移 | Stale registration + Stale primitive cache | 檢查 getOverlayManager() + 覆寫 getOverlayObjectPrimitive2DSequence() | `unotxvw.cxx`, `OverlayExtensionPainter.*` |
| E.5 | 右/下邊框裁切 | VDev 邊界剛好 | Bounding rect 加 40 twips margin | `unotxvw.cxx` |
