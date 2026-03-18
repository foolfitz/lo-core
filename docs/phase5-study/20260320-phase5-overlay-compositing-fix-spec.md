# Phase 5 修正：Overlay Buffer Alpha Compositing

> 版本：0.1
> 日期：2026-03-20
> 狀態：Draft
> 前置需求：Phase 5 overlay paint buffer 已實作（commits `708c5bbd3070`, `dbc8f031c52b`）
> 分支：ext-uno-api-26-2-1

---

## 1. 問題描述

### 1.1 現象

Phase 5 引入的 VirtualDevice overlay paint buffer 在 compositing 時，
將整個 buffer（包含 overlay 未繪製的「透明」區域）以不透明方式 blit 到
`SwEditWin` 的 `RenderContext` 上，導致：

- **文件內容完全消失**（被 buffer 的白色/不透明背景覆蓋）
- 只有 overlay 繪製的圖形（邊框、填色矩形）可見
- 文件變成一個白色畫面上只有 overlay 圖形的狀態

### 1.2 重現方式

```bash
PROFILE_URL="file:///tmp/lo-md-dev-profile"

MD_SIDEBAR_OVERLAY_DEMO=1 \
MD_SIDEBAR_SEED_PARAGRAPH_NAV_SAMPLE=1 \
MD_SIDEBAR_VALIDATE_PARAGRAPH_NAV_FULL=1 \
/home/jiajun/LibreOffice/LO-core/instdir/program/soffice \
  -env:UserInstallation="$PROFILE_URL" \
  --writer
```

開啟 sidebar 後，文件編輯區的文字被 overlay buffer 完全遮蓋。

### 1.3 根因分析

問題在 `SwXTextView::CompositOverlayBuffer()` 中：

```cpp
// 目前的實作
void SwXTextView::CompositOverlayBuffer(vcl::RenderContext& rRenderContext)
{
    // ...
    const Size aSize = m_pOverlayBuffer->GetOutputSizePixel();
    const Size aSrcSize = m_pOverlayBuffer->PixelToLogic(aSize);
    rRenderContext.DrawOutDev(
        Point(0, 0), aSize,
        Point(0, 0), aSrcSize,
        *m_pOverlayBuffer);
}
```

`OutputDevice::DrawOutDev()` 執行的是**不透明的像素複製**（opaque blit）。
即使 `m_pOverlayBuffer` 使用 `DeviceFormat::WITH_ALPHA` 建立，且已用
`COL_TRANSPARENT` 背景 `Erase()`，`DrawOutDev()` 不會讀取 source device
的 alpha 通道——它直接將所有像素（包含「透明」像素，在 pixel 層級實際為
白色/黑色）覆蓋到目標上。

這正是 Phase 5 spec §5.7 中標記的風險：

> **風險標記**：這是本 spec 中最需要 prototype 驗證的部分。

---

## 2. 修正方案

### 2.1 方案比較

| 方案 | 做法 | 優點 | 缺點 |
|------|------|------|------|
| A. BitmapEx alpha blit | 從 buffer 取出 `BitmapEx`（含 alpha），用 `DrawBitmap()` 繪製 | alpha 正確混合 | 每次 paint 都要做 bitmap 抽取 |
| B. 逐區域 blit | 維護 overlay bounding rect 列表，只 blit 有 overlay 的矩形 | 不需 alpha；精確 | 需追蹤每個 painter 的繪製範圍 |
| C. 直接繪製（回退 Phase 4） | 放棄 buffer，painter 直接畫到 RenderContext，但 composit 時暫時清除 clip | 最簡單 | 不解決閃爍問題 |

### 2.2 建議方案：B — 逐區域 blit

選擇方案 B，理由：

1. **不依賴平台的 alpha compositing 行為**——Phase 5 spec 已標記
   `DeviceFormat::WITH_ALPHA` + `DrawOutDev` 的跨平台一致性為風險項
2. **精確控制 blit 範圍**——只有 overlay 覆蓋的區域被 buffer 像素替換，
   其他區域完全不受影響
3. **效能可控**——blit 面積與 overlay 數量成正比，不隨文件大小增長
4. **與既有 repaint callback contract 相容**——painter 仍繪製到 buffer，
   只有 compositing 路徑改變

### 2.3 方案 B 的資料流

```
SwEditWin::Paint()
  → document content + internal overlays (正常繪製)
  → EnsureOverlayBuffer()
  → RepaintOverlayBuffer(visibleArea)
      → Erase buffer
      → 對每個 visible overlay 呼叫 paintOverlay()
      → 收集每個 painter 的 bounding rects
  → CompositOverlayBuffer(rRenderContext, boundingRects)
      → 只對 boundingRects 中的每個矩形執行 DrawOutDev()
```

---

## 3. 實作形狀

### 3.1 Painter bounding rect 收集

每個 `OverlayEntry` 需要記錄其 painter 最近一次繪製的 bounding rect。
有兩種方式可以取得：

**方式 B1：painter 自行回報**

在 `XOverlayPainter` IDL 新增方法：

```idl
// 不建議——會改變 published IDL
sequence<com::sun::star::awt::Rectangle> getOverlayBounds();
```

這會改變 published contract，不適合本次修正。

**方式 B2：從 XParagraphNavigator 推算**

目前所有 extension overlay 都是基於段落邊界繪製的。可以在
`CallOverlayPainters()` 內部，利用 `XGraphics` 的繪製呼叫（`drawRect`,
`drawLine` 等）追蹤每次繪製的實際範圍。

但這需要包裝 XGraphics，增加複雜度。

**方式 B3：buffer dirty rect 追蹤**

在 `RepaintOverlayBuffer()` 中，包裝 `XGraphics` 為一個追蹤型 proxy，
記錄所有繪製操作的 bounding box。這不需要改 IDL，也不依賴特定 painter 實作。

```cpp
class TrackingGraphics : public cppu::WeakImplHelper<css::awt::XGraphics>
{
    css::uno::Reference<css::awt::XGraphics> m_xDelegate;
    std::vector<tools::Rectangle> m_aPaintedRects;

    void SAL_CALL drawRect(sal_Int32 x, sal_Int32 y,
                           sal_Int32 w, sal_Int32 h) override
    {
        m_aPaintedRects.emplace_back(Point(x, y), Size(w, h));
        m_xDelegate->drawRect(x, y, w, h);
    }
    // ... 同理包裝 drawLine, drawPolyLine, fillRect 等
};
```

缺點是需要包裝大量 XGraphics 方法。

**方式 B4（建議）：使用 overlay 本身的段落範圍**

`CallOverlayPainters()` 已經透過 `getParagraphBounds()` 取得段落邊界。
可以直接利用 `XParagraphNavigator` 查詢每個 overlay 對應段落的 bounding
rect 作為 compositing 區域。

但這假設所有 painter 都基於段落繪製，限制了未來擴充。

**方式 B5（建議）：整個 visible area 做 alpha blit，但修正 alpha 路徑**

退回方案 A 的精神，但不用 `DrawOutDev()`，改用 `DrawBitmap()` 搭配
從 `VirtualDevice` 取出的含 alpha 的 `Bitmap`：

```cpp
void SwXTextView::CompositOverlayBuffer(vcl::RenderContext& rRenderContext)
{
    if (!m_pOverlayBuffer)
        return;

    auto aScopedPush = rRenderContext.ScopedPush(
        vcl::PushFlags::CLIPREGION | vcl::PushFlags::MAPMODE);
    rRenderContext.SetClipRegion();
    rRenderContext.SetMapMode(MapMode(MapUnit::MapPixel));

    const Size aSize = m_pOverlayBuffer->GetOutputSizePixel();
    Bitmap aBitmap = m_pOverlayBuffer->GetBitmap(Point(0, 0), aSize);
    rRenderContext.DrawBitmap(Point(0, 0), aSize, aBitmap);
}
```

若 `GetBitmap()` 能正確取出含 alpha 的 bitmap，且 `DrawBitmap()` 能正確
做 alpha compositing，這是最簡單的修正。

### 3.2 漸進修正策略與實測結果

由於對 VCL alpha compositing API 的行為尚無把握，原計畫分兩步驗證：

**Step 1：最小修正——驗證 `GetBitmap` + `DrawBitmap` alpha 路徑**

修改 `CompositOverlayBuffer()` 使用 `GetBitmap()` + `DrawBitmap()`
（方案 B5）。

> **Step 1 實測結果（2026-03-20，Linux/X11）：失敗**
>
> - 初次繪製：overlay 邊框可見，文件文字可見 ✓
> - 捲動後：整個可見區域變成白色，文件內容消失 ✗
>
> 結論：`GetBitmap()` 從 `DeviceFormat::WITH_ALPHA` 的 VirtualDevice 取出
> 的 Bitmap **不保留 alpha 通道**，或 `DrawBitmap(Bitmap)` 不做 alpha
> blending。首次繪製正常可能是因為 buffer 剛建立時的初始狀態碰巧正確，
> 但 `Erase()` 後重繪的 buffer 在 blit 時 alpha 資訊丟失。
>
> **Step 1 已排除，應直接進入 Step 2。**

**Step 2a：直接繪製（移除 buffer）**

移除 buffer，painter 直接畫到 RenderContext，清除 clip region 以確保
overlay 在 partial repaint 時仍然可見。

> **Step 2a 實測結果（2026-03-20，Linux/X11）：部分成功**
>
> - 捲動：overlay 邊框持續可見 ✓
> - 游標進入框線內輸入：框線消失 ✗
> - 游標移出後：框線恢復 ✓
>
> 結論：VCL 的 Paint() 機制中，系統級 clip（X11 GC clip 或 VCL 內部
> paint clip）限制了繪製範圍為 invalidated region。`SetClipRegion()` 只
> 清除使用者級 clip，無法清除系統級 clip。因此 painter 只能在 invalidated
> 區域內繪製。當 partial repaint 僅覆蓋段落內文字區域時，overlay 邊框
> 超出此區域的部分無法被重繪，而 document paint 已經覆蓋了舊的 overlay，
> 導致 overlay 消失。
>
> **Step 2a 已排除，需要 buffer 來保持 overlay 內容。**

**Step 2b：逐區域 blit（buffer + bounding rect）**

保留 buffer，但只對 overlay 覆蓋的 bounding rect 區域做 `DrawOutDev()`，
非 overlay 區域不碰。

> **Step 2b 實測結果（2026-03-20，Linux/X11）：部分成功**
>
> - 長段落（段落 1，跨多行/多頁）：邊框持續可見 ✓
> - 短段落（段落 0、3，單行）：邊框大部分時候不可見 ✗
> - 滑鼠在段落 0 附近點擊時，邊框偶爾出現
> - 段落 3（末頁）邊框始終不可見
>
> 根因：`CompositOverlayBuffer()` 在 `SwEditWin::Paint()` 內執行，
> 而 `Paint()` 受系統級 clip（VCL paint clip / X11 GC clip）限制，
> 只能繪製到 invalidated region 內。`DrawOutDev()` 雖然指定了 overlay
> 的 bounding rect，但如果該 rect 不在 invalidated region 內，
> blit 結果仍然被 clip 掉。
>
> 同時，`RepaintOverlayBuffer()` 每次 `Paint()` 都 `Erase()` 整個 buffer
> 再重繪，所以 buffer 中的 overlay 內容在每次 paint 後都是完整的。
> 但 compositing 只能輸出到 invalidated region 內的部分——**buffer 有內容
> 但輸出不了**。
>
> **Step 2b 不可行，因為 Paint() 的 system clip 無法繞過。**

**Step 2c：Content buffer + 持久保存 + 全視窗 blit**

Content buffer 方案：buffer 儲存 document content + overlay 的合成畫面。
每次 Paint()：
1. 從 RenderContext 複製 document content 到 buffer
2. 在 buffer 上繪製 overlay
3. 將 buffer blit 回 RenderContext

> **Step 2c 實測結果（2026-03-20，Linux/X11）：部分成功**
>
> - 初始顯示：段落 0 邊框可見 ✓
> - 段落 0 輸入文字時：邊框消失 ✗
> - 段落 2、3：無邊框 ✗
>
> 根因分析：
>
> **問題 1（段落 0 輸入消失）**：Step 1 從 RenderContext 複製**全視窗**
> 像素到 buffer。在 partial paint 時，RC 的 paint clip 外區域像素可能
> 已被 window system（X11 backing store 或 compositing manager）清除
> 或是不新鮮的。Step 1 將這些不新鮮的像素覆蓋了 buffer 中原本正確的
> overlay 內容。Step 2 重畫 overlay，但 Step 3 的 blit 受 paint clip
> 限制，只有 clip 內的區域被輸出到 RC，clip 外的 overlay 就此丟失。
>
> **問題 2（段落 2、3 無邊框）**：buffer 使用 `getPrePostMapMode()`
> 作為 MapMode，這是帶有捲動偏移的 document twips 座標系。不在當前
> 可見區域的段落（如段落 3 在第 2 頁），其 document twips 座標映射到
> buffer 的 pixel 位置超出 buffer 尺寸，overlay 被 buffer 裁切。
> 段落 2 是空段落，其 bounds 高度為 0 或極小，邊框不可見。
>
> **Step 2c 的問題 1 有解（見 Step 2d），問題 2 是預期行為。**

**Step 2d（實作中）：Content buffer + 選擇性複製 + 延遲全視窗重繪**

在 Step 2c 基礎上修正兩個關鍵點：

1. **Step 1 只複製 paint clip 區域**：不複製整個 RC，只複製 paint
   clip（invalidated region）內的像素到 buffer。Buffer 中 clip 外的
   區域保留上次的內容（包含 overlay），不被不新鮮的 RC 像素覆蓋。

2. **Step 4 延遲全視窗重繪**：偵測 partial paint，排程一次全視窗
   `Invalidate()`，確保 overlay 在下一個 paint cycle 被完整輸出。
   用 `m_bOverlayRepaintPending` flag 防止無限迴圈（最多兩次 cycle）。

### 3.3 Step 2d 實作細節

#### 3.3.1 新增成員

```cpp
// unotxvw.hxx — SwXTextView 私有成員
VclPtr<VirtualDevice>  m_pOverlayBuffer;
Size                   m_aOverlayBufferSize;
bool                   m_bOverlayBufferDirty = true;
bool                   m_bOverlayRepaintPending = false;
css::awt::Rectangle    m_aLastOverlayVisibleArea;
```

#### 3.3.2 Dirty flag 設定時機

```cpp
void SwXTextView::addOverlay(...)         { ... m_bOverlayBufferDirty = true; }
void SwXTextView::removeOverlay(...)      { ... m_bOverlayBufferDirty = true; }
void SwXTextView::invalidateOverlay(...)  { m_bOverlayBufferDirty = true; ... }
void SwXTextView::setOverlayVisible(...)  { ... m_bOverlayBufferDirty = true; }
```

#### 3.3.3 PaintOverlays 四步驟

```cpp
void SwXTextView::PaintOverlays(vcl::RenderContext& rRenderContext,
                                const awt::Rectangle& rVisibleArea)
{
    EnsureOverlayBuffer();

    // Visible area 變化偵測 → 標記 dirty
    if (visibleAreaChanged(rVisibleArea))
        m_bOverlayBufferDirty = true;

    // Step 1: 只複製 paint clip 區域到 buffer
    //   - partial paint: 只複製 invalidated area，buffer 其他區域保留
    //   - full paint: 複製整個 RC
    vcl::Region aPaintRegion = rEditWin.GetPaintRegion();
    if (isPartialPaint(aPaintRegion))
    {
        tools::Rectangle aClipRect = aPaintRegion.GetBoundRect();
        buffer.DrawOutDev(aClipRect → aClipRect, from RC);
    }
    else
    {
        buffer.DrawOutDev(fullSize, from RC);
    }

    // Step 2: 在 buffer 上繪製所有 overlay（始終呼叫，維持 published contract）
    buffer.SetMapMode(documentTwipsMapMode);
    CallOverlayPainters(buffer.CreateUnoGraphics(), rVisibleArea);

    // Step 3: blit buffer → RC（受 system paint clip 限制）
    rRenderContext.SetClipRegion();  // 清除 user clip
    rRenderContext.DrawOutDev(fullSize, from buffer);
    // 注意：system paint clip 仍然限制實際輸出範圍

    // Step 4: 若為 partial paint，排程全視窗 Invalidate
    if (!m_bOverlayRepaintPending && isPartialPaint(aPaintRegion))
    {
        m_bOverlayRepaintPending = true;
        rEditWin.Invalidate();  // 排程下一個 full paint
    }
    else
    {
        m_bOverlayRepaintPending = false;  // full paint 完成，重設
    }
}
```

#### 3.3.4 為什麼這樣能解決問題

```
Keystroke → partial paint (clip = 游標行)
  Step 1: buffer[游標行] ← RC (fresh doc)
          buffer[其他區域] 保留 (上次的 doc + overlay)
  Step 2: overlay 繪製在 buffer 上 (所有位置)
  Step 3: blit buffer → RC (clipped to 游標行)
          → 游標行: doc + overlay ✓
          → 其他區域: RC 保留上次內容 (doc + overlay) ✓
  Step 4: 排程 full Invalidate

Full paint (from Step 4)
  Step 1: buffer[全部] ← RC (fresh doc)
  Step 2: overlay 繪製在 buffer 上
  Step 3: blit buffer → RC (full, no clip)
          → 所有區域: doc + overlay ✓
  Step 4: full paint → 不排程
```

關鍵在 Step 1 的選擇性複製：partial paint 時不覆蓋 buffer 中 clip 外的
已有 overlay，確保那些區域的 overlay 在 Step 3（雖然被 clip 但不影響 RC
上的舊內容）和下一次 full paint 中都能正確顯示。
```

---

## 4. 修改範圍

### 4.1 Step 2c 持久 buffer

| # | 動作 | 檔案 | 說明 |
|:-:|:----:|------|------|
| 1 | 編輯 | `sw/source/uibase/inc/unotxvw.hxx` | 新增 `m_bOverlayBufferDirty`、`m_aLastVisibleArea`、`m_aOverlayBoundingRects` 成員 |
| 2 | 編輯 | `sw/source/uibase/uno/unotxvw.cxx` | `RepaintOverlayBuffer()` 條件化重繪；`CompositOverlayBuffer()` 逐區域 blit；overlay 變動時標記 dirty |

IDL 不變、painter 呼叫不變。

---

## 5. 驗證計畫

### 5.1 CppUnit 測試

修改 `sw/qa/uibase/uno/uno.cxx` 中的
`testDocumentOverlayBufferSurvivesPartialRepaint`：

- 驗證 overlay 像素可見（已有，使用 `CornerOverlayPainter` 和 `lcl_GetOutDevPixelColor`）
- **新增**：驗證非 overlay 區域的像素**不是**全白——應包含文件內容的顏色

```cpp
// 在 overlay 區域之外取樣——應為文件背景色（白色頁面 + 文字的混合），
// 而非 buffer 的全覆蓋白色
// 如果 alpha 正確，文件背景會保留
Point aNonOverlayPoint(10, 10);  // 左上角，遠離 overlay
Color aNonOverlayColor = lcl_GetOutDevPixelColor(*xTarget, aNonOverlayPoint);
// 不應為全透明黑色（alpha 失敗）或 buffer 覆蓋色
CPPUNIT_ASSERT(aNonOverlayColor != COL_BLACK);
```

### 5.2 Extension 側驗證

使用 `markdown-insert-ext` 的 demo overlay：

```bash
PROFILE_URL="file:///tmp/lo-md-dev-profile"

MD_SIDEBAR_OVERLAY_DEMO=1 \
MD_SIDEBAR_SEED_PARAGRAPH_NAV_SAMPLE=1 \
MD_SIDEBAR_VALIDATE_OVERLAY=1 \
MD_SIDEBAR_LOG_OVERLAY=1 \
/home/jiajun/LibreOffice/LO-core/instdir/program/soffice \
  -env:UserInstallation="$PROFILE_URL" \
  --writer
```

| # | 驗證項目 | 預期結果 |
|:-:|----------|----------|
| 1 | 文件文字可見 | 所有段落文字正常顯示 |
| 2 | overlay 邊框可見 | 段落 0、中間、末尾的紅色邊框可見 |
| 3 | overlay 不遮蓋文字 | 邊框與文字共存，不互相覆蓋 |
| 4 | 游標移動不閃爍 | overlay 在游標移動時不消失（Phase 5 核心成果） |
| 5 | 文字輸入不閃爍 | 其他段落的 overlay 在輸入時持續可見 |
| 6 | 捲動正確 | overlay 隨捲動正確移動 |
| 7 | 縮放正確 | zoom 後 overlay 大小和位置正確 |

### 5.3 最小 build / test 指令

```bash
make sw
make CppunitTest_sw_uibase_uno
```

---

## 6. 風險

| 風險 | 說明 | 緩解 |
|------|------|------|
| Buffer 與文件 z-order 衝突 | overlay blit 覆蓋文件內容（不透明 blit）| 只 blit overlay bounding rect，rect 外不碰 |
| Bounding rect 與繪製範圍不匹配 | painter 繪製超出 bounding rect 時被截斷 | bounding rect 加 padding；或用 tracking proxy |
| 多次 Paint() 才完整顯示 | 首次 Paint() 的 clip 可能不覆蓋所有 overlay | 可接受——overlay 不需要即時完整可見；或在 `addOverlay()` 時主動 `Invalidate()` overlay 區域 |
| 捲動/縮放時 buffer 座標過時 | visible area 改變但 buffer 未重繪 | visible area 比較觸發 dirty 標記 |

---

## 7. 實測結果總覽

| Step | 方案 | 結果 | 問題 |
|------|------|------|------|
| 1 | `GetBitmap` + `DrawBitmap` alpha blit | 失敗 | alpha 不保留，捲動後全白 |
| 2a | 移除 buffer，直接繪製 | 部分成功 | system clip 限制，部分 repaint 時 overlay 消失 |
| 2b | buffer + 逐區域 blit | 部分成功 | buffer 每次 Erase 後 compositing 被 clip，短段落 overlay 不可見 |
| 2c | 持久 buffer + 條件重繪 | **待驗證** | — |

核心發現：VCL 的 `Paint()` 有兩層 clip——使用者級（`SetClipRegion`，可清除）
和系統級（invalidated region，不可清除）。任何在 `Paint()` 內的繪製操作
都受系統級 clip 限制。因此 overlay 方案必須：
1. 使用 buffer 保持 overlay 內容（Step 2a 證明直接繪製不可行）
2. 不在每次 `Paint()` 時清空 buffer（Step 2b 證明每次 Erase 會丟失 clip 外的內容）
3. 只在 overlay 內容真正改變時才重繪 buffer（Step 2c 的核心策略）

---

## 8. Step 2d 實測結果與根因深入分析

### 8.1 Step 2d 初版實測（2026-03-22，Linux/X11）

> **結果：部分成功——與 Step 2c 相同症狀**
>
> - 初始顯示：三個段落的框線皆可見 ✓
> - 捲動：框線正確跟隨 ✓
> - 在段落內輸入文字：該段落及所有其他段落的框線消失 ✗
> - 捲動後：框線恢復 ✓
>
> 捲動能恢復框線，但打字後框線持續消失——與先前所有 Step 的症狀一致。

### 8.2 Step 2d 深入根因分析

對 `PaintOverlays()` 四步驟進行完整生命週期追蹤，發現**兩個具體 bug**：

#### Bug A：Step 3 DrawOutDev 座標錯誤

Step 2 將 buffer MapMode 設為 `getPrePostMapMode()`（文件 twips 座標系，
含捲動偏移）。Step 3 的 blit 使用此 MapMode 下的座標：

```cpp
// Bug：source Point(0,0) 不是 buffer 的像素原點
const Size aSrcSize = m_pOverlayBuffer->PixelToLogic(aPixelSize);
rRenderContext.DrawOutDev(
    Point(0, 0), aPixelSize,     // dest: pixel (0,0)
    Point(0, 0), aSrcSize,       // src: logical (0,0) in getPrePostMapMode
    *m_pOverlayBuffer);
```

`Point(0, 0)` 在 `getPrePostMapMode()` 下是**文件原點**（文件最左上角），
不是 buffer 的像素原點。VCL 的 `DrawOutDev` 內部轉換：

```
LogicToPixel(Point(0,0), getPrePostMapMode)
  = (0 - visArea.Left) * scale
  = 負值（當文件已捲動時）
```

導致 DrawOutDev 從 buffer 的負像素座標讀取，blit 結果被裁切或偏移。

- **未捲動時**：visArea 起始於 (0,0)，座標碰巧正確
- **捲動後**：座標錯誤，blit 完全失敗

**這解釋了為什麼捲動後框線會「恢復」**：捲動觸發自然的 full repaint，
而 Step 3 在未捲動位置重新開始時座標正確。

**修正**：Step 3 blit 前將 buffer 重設為 `MapPixel`：

```cpp
auto aBufPush = m_pOverlayBuffer->ScopedPush(vcl::PushFlags::MAPMODE);
m_pOverlayBuffer->SetMapMode(MapMode(MapUnit::MapPixel));
rRenderContext.DrawOutDev(
    Point(0, 0), aPixelSize,
    Point(0, 0), aPixelSize,    // 兩側都是 pixel 座標
    *m_pOverlayBuffer);
```

#### Bug B：Step 4 `Invalidate()` 在 `Paint()` 內不可靠

Step 4 在 `Paint()` handler 內部直接呼叫 `rEditWin.Invalidate()` 來排程
full repaint。實測證明**此 Invalidate 在 Linux/X11 上被靜默丟棄**：

- 打字後框線消失且不恢復（Step 4 的 full repaint 從未發生）
- 捲動後框線恢復（捲動觸發的是 VCL 內部的 invalidation，不走 Paint() 內部路徑）

此外，原始 flag 邏輯有 race condition：

```cpp
// 原始邏輯
if (!m_bOverlayRepaintPending) {
    m_bOverlayRepaintPending = true;
    rEditWin.Invalidate();
} else {
    // 假設這是排程的 full repaint → 重設 flag
    m_bOverlayRepaintPending = false;  // BUG: 可能是另一個 partial paint!
}
```

快速打字時，下一個 partial paint 可能在 full repaint 之前到達，
進入 `else` 分支「吃掉」flag，導致 full repaint 永遠不被正確識別。

### 8.3 Step 2d Bug A 修正後實測（2026-03-22）

只修正 Bug A（Step 3 座標）+ 修正 flag race condition（只在觀察到實際
full paint 時才重設 flag），但仍在 Paint() 內呼叫 Invalidate()：

> **結果：改善但未完全解決**
>
> - 初始顯示：三個段落框線可見 ✓
> - 捲動：框線正確跟隨 ✓
> - 打字：框線消失 ✗
> - 捲動後恢復 ✓
>
> **確認 Bug B**：`Invalidate()` 在 `Paint()` 內不可靠是根本問題。
> 座標修正解決了捲動場景，但打字場景需要可靠的 deferred invalidation。

---

## 9. Step 2e：Content buffer + Idle deferred invalidation

### 9.1 方案

在 Step 2d 基礎上，將 Step 4 的 `Invalidate()` 從 `Paint()` 內部移到
VCL `Idle` callback（`TaskPriority::POST_PAINT`），確保 invalidation 在
Paint() handler 返回後才被事件循環處理。

### 9.2 關鍵設計：防止 `ProcessEventsToIdle` 無限迴圈

CppUnit 測試使用 `Scheduler::ProcessEventsToIdle()` 處理所有待處理事件。
如果 Idle handler 呼叫 `Invalidate()` → paint → schedule Idle → Idle fires →
Invalidate() → ... 會造成無限迴圈。

**解法**：Idle handler **不重設** `m_bOverlayRepaintPending` flag。
Flag 只被實際的 full paint 重設。

```
生命週期（正常運行時）：
1. partial paint → flag=false → flag=true, start Idle
2. Idle fires → Invalidate()（flag 保持 true）
3. full paint → flag=true → else branch → flag=false
4. 下次 partial paint → 回到 1

生命週期（ProcessEventsToIdle）：
1. partial paint → flag=false → flag=true, start Idle
2. Idle fires → Invalidate()（flag 保持 true）
3. paint from Invalidate:
   - 若 full → flag=false → 不排程 → 結束 ✓
   - 若 partial → flag=true → 不排程（已 pending）→ 結束 ✓
     （最多一次未修復的 partial paint，下次自然 full paint 修復）
```

### 9.3 新增成員

```cpp
// unotxvw.hxx
#include <vcl/idle.hxx>

// SwXTextView 私有成員（新增）
Idle  m_aOverlayRepaintIdle;
DECL_LINK(OverlayRepaintIdleHdl, Timer*, void);
```

### 9.4 Idle handler 實作

```cpp
// 建構子
m_aOverlayRepaintIdle.SetPriority(TaskPriority::POST_PAINT);
m_aOverlayRepaintIdle.SetInvokeHandler(
    LINK(this, SwXTextView, OverlayRepaintIdleHdl));

// Handler：不重設 flag，只呼叫 Invalidate
IMPL_LINK_NOARG(SwXTextView, OverlayRepaintIdleHdl, Timer*, void)
{
    // 注意：不重設 m_bOverlayRepaintPending。
    // Flag 由 PaintOverlays Step 4 在偵測到 full paint 時重設。
    // 這防止 ProcessEventsToIdle 觸發的無限 Idle → Invalidate → Paint 迴圈。
    if (m_pView)
        m_pView->GetEditWin().Invalidate();
}
```

### 9.5 PaintOverlays Step 4 修正

```cpp
// Step 4: Idle deferred invalidation
{
    SwEditWin& rEditWin = m_pView->GetEditWin();
    vcl::Region aPaintRegion = rEditWin.GetPaintRegion();
    bool bIsPartialPaint = false;
    if (!aPaintRegion.IsNull() && !aPaintRegion.IsEmpty())
    {
        tools::Rectangle aPaintBound = aPaintRegion.GetBoundRect();
        tools::Rectangle aWinRect(Point(0, 0), rEditWin.GetOutputSizePixel());
        bIsPartialPaint = (aPaintBound != aWinRect);
    }

    if (bIsPartialPaint)
    {
        if (!m_bOverlayRepaintPending)
        {
            m_bOverlayRepaintPending = true;
            m_aOverlayRepaintIdle.Start();  // deferred, 不在 Paint() 內
        }
    }
    else
    {
        // 實際的 full paint → 重設 flag
        m_bOverlayRepaintPending = false;
    }
}
```

### 9.6 生命週期管理

```cpp
// 解構子
m_aOverlayRepaintIdle.Stop();

// Invalidate()（view dispose 時）
m_aOverlayRepaintIdle.Stop();
```

---

## 10. 實測結果總覽（更新）

| Step | 方案 | 結果 | 問題 |
|------|------|------|------|
| 1 | `GetBitmap` + `DrawBitmap` alpha blit | 失敗 | alpha 不保留，捲動後全白 |
| 2a | 移除 buffer，直接繪製 | 部分成功 | system clip 限制，partial repaint 時 overlay 消失 |
| 2b | buffer + 逐區域 blit | 部分成功 | buffer 每次 Erase，compositing 被 clip |
| 2c | 持久 buffer + 全視窗複製 | 部分成功 | Step 1 全複製覆蓋 buffer 中的 overlay |
| 2d | 持久 buffer + 選擇性複製 + Paint 內 Invalidate | 部分成功 | Bug A: Step 3 座標錯誤；Bug B: Invalidate 在 Paint 內不可靠 |
| 2d′ | 2d + Step 3 座標修正 + flag race fix | 部分成功 | Bug B 未解：Invalidate 在 Paint 內仍不可靠 |
| 2e | 2d′ + Idle deferred invalidation | **失敗** | 與 2d/2d′ 完全相同的現象——Idle 確實觸發但 overlay 仍消失 |

### 核心發現追加

4. `Invalidate()` 在 `Paint()` handler 內部呼叫，在 Linux/X11 上被靜默丟棄
   （可能是 VCL 的 paint-in-progress 保護機制或 X11 事件合併）
5. `getPrePostMapMode()` 的 origin 含捲動偏移，buffer 的 MapMode 在 blit
   前必須重設為 `MapPixel`，否則 `DrawOutDev` 的 source 座標錯誤
6. **Idle deferred Invalidate 與直接 Invalidate 結果完全相同**——問題不在
   Invalidate 的時機，而在於 VCL system clip 阻止了 Paint() 中繪製超出
   paint region 的內容。content buffer blit 策略的根本假設（Step 3 可以
   覆蓋整個視窗）不成立。
7. **Steps 1–2e 共同失敗模式**：打字 → partial paint → overlay 不在 clip
   內 → 消失 → 後續 repaint 無法恢復。需要根本改變策略。

---

## 11. Step 2e 實測結果與再分析

### 11.1 測試結果

**日期**：2026-03-22
**測試環境**：Linux/X11, ext-uno-api-26-2-1 分支

**CppUnit 測試**：33 個全部通過，無掛住（infinite loop 防護有效）

**手動測試結果**（與 Step 2d/2d′ 完全相同）：
- 初始畫面：三個框線正確顯示（第一段、中間段、最後段）
- 捲動：框線保持正確，隨捲動正常更新
- **打字**：在有框線的段落輸入文字時，**該段落的框線消失**，且**其他段落的框線也全部消失**
- 捲動後：所有框線恢復顯示

### 11.2 關鍵結論：問題不在 Invalidate 的時機

Step 2e 的 Idle 機制確實讓 `Invalidate()` 在 Paint() 返回後才被呼叫，
CppUnit 測試也證實了 Idle 正確觸發且無無限迴圈。但手動測試的結果與
直接在 Paint() 內呼叫 `Invalidate()` 完全一致——**說明 Bug B 的根因
不是 Invalidate 在 Paint 內被靜默丟棄**。

真正的問題更深層：

### 11.3 重新定位根因

**觀察到的行為模式**：
1. 打字觸發的是**極小區域的 partial paint**（只包含游標位置附近的幾行文字）
2. Step 1（buffer 複製 RC 內容）只複製了 partial paint 區域的文件內容
3. Step 2（overlay 繪製）在 buffer 上繪製了所有 overlay
4. Step 3（buffer blit 回 RC）**受到 VCL system clip 的限制**，
   只有 partial paint 區域被實際輸出到螢幕
5. Step 4 觸發 full repaint（無論是直接 Invalidate 還是 Idle），
   但 **full repaint 中 Step 1 會用 RC 的「新」document content 覆蓋
   buffer 中 Step 2 繪製的 overlay**

**核心矛盾**：content buffer 策略依賴 buffer 保留上一次的 overlay 繪製結果，
並且只在 partial paint 區域用新的 document content 覆蓋。但是：

- **打字導致的 repaint 可能連續觸發多次 partial paint**，每次覆蓋不同區域
- 快速打字時，多次 partial paint 的累積效果可能覆蓋掉 buffer 中所有 overlay
- 即使 full repaint 到達，Step 1 會複製整個 RC（此時 RC 上沒有 overlay），
  等於清除了 buffer 中所有舊的 overlay 繪製
- Step 2 會重新繪製 overlay，Step 3 blit 回 RC，看起來應該正確
- **但如果 full repaint 的 Step 3 仍受到某種 clip 限制**，
  或者 full repaint 實際上不是真正的 full（GetPaintRegion 偵測有誤），
  就會導致 overlay 無法顯示

### 11.4 需要驗證的假設

1. **Idle 觸發的 full repaint 是否真的是 full paint？**
   - 需要在 PaintOverlays 中加入 SAL_INFO 日誌，記錄每次呼叫的
     paint region 大小 vs 視窗大小，以確認 Idle 後的 repaint
     是否真的是 full paint

2. **full paint 時 Step 3 的 blit 是否成功覆蓋了整個視窗？**
   - 需要確認 `SetClipRegion()`（清除 user clip）是否足夠，
     或者 system clip 仍然限制了輸出範圍

3. **打字時是否觸發了多次連續 partial paint，導致 buffer 中
   overlay 被逐步覆蓋？**
   - 需要日誌追蹤 PaintOverlays 的呼叫序列

4. **根本架構問題：content buffer 方案是否可行？**
   - 如果 VCL 的 system clip 無法從 user code 清除，那麼任何
     「在 Paint() 中 blit 超過 paint region 的內容」都不可能成功
   - 這意味著 content buffer 方案需要根本性的重新思考

### 11.5 可能的替代方向

| 方向 | 說明 | 風險 |
|------|------|------|
| **A. 放棄 buffer，直接繪製 overlay** | 不用 VirtualDevice，在 Paint() 中直接呼叫 overlay painters 繪製到 RC 上。overlay 只在 partial paint 區域內繪製（其他區域本來就不需要重繪）。需要 painters 能自行裁剪到 paint region。 | painters 需要知道 clip 區域；partial paint 外的 overlay 仍然可能消失 |
| **B. 使用 VCL Overlay（sdr overlay）** | 利用 VCL/svx 的 overlay 機制（`sdr::overlay::OverlayManager`），這是 LibreOffice 自己用來繪製選取框、拖曳把手等的機制。這些 overlay 有獨立的 invalidation 路徑，不受 document paint 的 clip 限制。 | 需要深入了解 sdr overlay 架構；可能需要不同的 painter API |
| **C. overlay 作為獨立的透明視窗** | 在 SwEditWin 上疊加一個透明子視窗，overlay 繪製在該視窗上。與 document paint 完全解耦。 | 跨平台透明視窗支援不一致；效能和事件處理問題 |
| **D. 接受 partial paint 限制，改進 repaint 策略** | 不嘗試在 partial paint 中繪製完整 overlay，而是在每次 partial paint 後 invalidate overlay 所在的矩形區域（而非整個視窗）。 | 仍然依賴 Invalidate 的可靠性；但因為 Idle 已證實有效，只需精確 invalidate overlay 矩形 |

### 11.6 建議優先嘗試方向

**方向 D**（精確 invalidate overlay 矩形）值得優先嘗試：

Step 2e 已證明 Idle 機制可以可靠地觸發 Invalidate。問題可能在於
`rEditWin.Invalidate()`（全視窗）與 `rEditWin.Invalidate(aRect)`
（特定區域）的行為不同。全視窗 Invalidate 可能被 VCL 合併或優化掉，
而特定區域的 Invalidate 可能更可靠。

具體做法：
1. 在 Step 4 偵測到 partial paint 時，計算哪些 overlay 矩形在 paint region 外
2. Idle handler 中只 invalidate 那些特定矩形，而非整個視窗
3. 這些矩形的 repaint 仍然是 partial paint，但 clip 區域包含了 overlay

**方向 A**（直接繪製，放棄 buffer）也值得考慮作為 fallback：

重新審視 Step 2a 的思路。Step 2a 的問題是「overlay 在 partial paint
區域外消失」，但這其實是**預期行為**——partial paint 本來就只更新一部分
螢幕。真正的問題是：document repaint 清除了 overlay 所在區域，但沒有
重新繪製 overlay。如果 overlay painters 在每次 Paint() 中都被呼叫，
且只繪製 paint region 內的部分，那麼 overlay 應該能隨著 document
content 一起被正確更新。

---

## 12. Steps 1–2e 全部失敗的共同模式

回顧所有嘗試，有一個共同的失敗模式：

**打字 → partial paint → document content 更新 → overlay 不在 partial
paint 區域內 → overlay 從螢幕消失 → 後續 repaint 未能恢復 overlay**

每個 Step 嘗試用不同方式解決「後續 repaint 未能恢復 overlay」，
但全部失敗。這強烈暗示問題不在「如何觸發後續 repaint」，而在於
**VCL paint 架構本身不允許在 Paint() callback 中繪製超出 system clip
的區域**。任何基於「在 Paint() 中 blit 整個 buffer」的方案都會
受到這個限制。

下一步應該從根本上改變策略，不再嘗試在 Paint() 中繪製超出
paint region 的內容，而是確保每次 Paint() 中 overlay 在
paint region 內的部分都被正確繪製。

---

## 13. 與 Phase 5 spec 的關係

本修正保留 Phase 5 的 VirtualDevice buffer 架構，但將 buffer 從「每次 Paint
都重建」改為「持久保存、條件化重繪」。這是 Phase 5 spec §5.7 預見的風險
的解決方案，同時也改善了效能——大部分 Paint() 呼叫不需要重繪 buffer。

但 Steps 1–2e 的全部失敗表明，content buffer 方案可能需要根本性的
架構變更，或者完全放棄 buffer 改用直接繪製策略。
