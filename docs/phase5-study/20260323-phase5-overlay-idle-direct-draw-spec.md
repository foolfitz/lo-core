# Phase 5 最終方案：Idle Direct Draw（方向 F）

> 版本：1.0
> 日期：2026-03-23
> 狀態：待實作
> 前置文件：`phase5-overlay-direct-paint-spec.md`（v0.5, Direction D 完整失敗記錄）
> 分支：ext-uno-api-26-2-1

---

## 1. 問題總覽

### 1.1 目標

讓 LibreOffice Writer extension 能透過 `XOverlayPainter` callback 在文件視窗上繪製
overlay（例如段落邊框），且滿足三個條件：

1. **overlay 穩定**：打字、操作後不消失
2. **捲動正常**：滾輪、捲軸功能正常
3. **無明顯閃爍**

### 1.2 根本挑戰

VCL 的 `Paint()` callback 中，所有繪圖受 **system clip**（X11 GC clip region）限制，
只能畫在 `paint region`（invalidated area）內。當 partial paint 只涵蓋部分視窗時，
overlay 在 paint region 外的部分被裁切而消失。

---

## 2. 全部失敗方案摘要

### 2.1 Phase 5 Steps 1–2e（Content Buffer 方案）

試圖用 VirtualDevice buffer 保存 overlay，在 Paint() 中 blit 到 window。

| Step | 方案 | 結果 | 失敗原因 |
|------|------|------|----------|
| 1 | `GetBitmap` + `DrawBitmap` alpha blit | 失敗 | VCL `GetBitmap()` 不保留 alpha 通道 |
| 2a | 移除 buffer，直接繪製到 RC | 部分成功 | system clip 限制，overlay 在 partial paint 外消失 |
| 2b | buffer + 逐區域 blit | 部分成功 | buffer 每次 Erase，compositing 被 clip 裁切 |
| 2c | 持久 buffer + 全視窗複製 | 部分成功 | 全複製以不新鮮的 RC 像素覆蓋 buffer 中的 overlay |
| 2d | 持久 buffer + 選擇性複製 + Paint 內 Invalidate | 部分成功 | Bug A: 座標錯誤；Bug B: Invalidate 在 Paint 內不可靠 |
| 2d′ | 2d + 座標修正 + flag race fix | 部分成功 | Invalidate 在 Paint 內仍不可靠 |
| 2e | 2d′ + Idle deferred invalidation | 失敗 | Idle 觸發但 overlay 仍消失——system clip 阻止 blit |

**結論**：在 Paint() 中 blit buffer 到 window 同樣受 system clip 限制，無法繞過。

### 2.2 Direction D（Direct Paint + Deferred Invalidation）

移除 buffer，overlay 直接畫到 RenderContext。Partial paint 後排程
Idle/Timer 觸發 `Invalidate()` 來恢復被裁切的 overlay。

| 迭代 | 方案 | overlay | 捲動 | 閃爍 | 失敗原因 |
|------|------|:---:|:---:|:---:|----------|
| D1 | Idle targeted Invalidate (pixel coords) | ✗ | — | — | `GetPaintRegion()` 回傳 logical coords，與 pixel 比較永遠偵測為 partial |
| D2 | D1 + LogicToPixel 修正 | ✗ | — | — | `m_bOverlayRepaintPending` 不重設，flag 卡死 |
| D3 | D2 + Idle handler 重設 flag | ✗ | — | — | `Invalidate(pixelRect)` 無效——預期 MapMode coords 非 pixel |
| D4 | Idle + 全視窗 `Invalidate()` | ✓ | ✗ | 輕微 | 滾輪不動、捲軸變紅（LINECOLOR leak + 無限循環） |
| D5 | D4 + SetMapMode + targeted Invalidate | ✗→✓ | ✗ | 輕微 | Paint() 外 SetMapMode 不安全 |
| D6 | Paint 內直接 Invalidate | ✗ | ✓ | — | Paint 內 Invalidate 不觸發後續 repaint |
| D7 | Timer 50ms | ✓ | ✓ | **明顯** | 延遲太長 |
| D8 | Timer 16ms | ✓ | ✓ | **明顯** | 延遲仍太長 |
| D9 | Idle POST_PAINT + LINECOLOR fix | ✓ | ✗ | 輕微 | 無限 Idle 循環餓死 input（見下方分析） |
| D10a | D9 + 捲動偵測（visibleArea 比較） | ✓ | ✗ | 輕微 | 同 D9——無限循環是獨立 bug |
| D10b | D10a + full-paint 偵測修正 | ✗ | ✓ | — | 修正循環後 overlay 在打字時消失 |

### 2.3 Direction D 不可能三角

```
        overlay 穩定
           /    \
          /      \
    捲動正常 —— 無閃爍
```

| 方案 | overlay 穩定 | 捲動正常 | 無閃爍 | 機制 |
|------|:---:|:---:|:---:|------|
| D9（無限 Idle 循環） | ✓ | ✗ | ✓ | 持續 full repaint 維持 overlay，餓死 input |
| D7/D8（Timer） | ✓ | ✓ | ✗ | Timer 延遲恢復，延遲造成閃爍 |
| D10b（單次 Idle） | ✗ | ✓ | ✓ | 循環終止但 overlay 在按鍵間消失 |

**結論**：`Invalidate()` 方案無法同時滿足三個條件。`Invalidate()` 觸發新的 Paint
cycle，新 Paint 受 system clip 限制，本質上是重複同樣的問題。

---

## 3. VCL 深入研究（2026-03-23）

### 3.1 Paint/Invalidate 完整流程

```
Window::Invalidate(rect)
  → ImplInvalidate()
    → 將 rect 合併到 maInvalidateRegion
    → 啟動 maPaintIdle (DEFAULT_IDLE 優先級，延遲觸發)
  [事件循環空閒時]
  → ImplHandlePaintHdl()
    → ImplCallPaint()
      → PushPaintHelper()
        → 複製 maInvalidateRegion → mpPaintRegion
        → mbInitClipRegion = true
      → DoPaint() → Window::Paint()
        → [第一次繪圖時] InitClipRegion()
          → 從 mpPaintRegion 設定 system clip
      → PopPaintHelper()
        → mpPaintRegion = nullptr
        → mbInPaint = false
```

關鍵程式碼：`vcl/source/window/paint.cxx`, `vcl/source/window/clipping.cxx`

### 3.2 System clip 的設定機制

`WindowOutputDevice::InitClipRegion()` (`clipping.cxx:38-66`)：

```cpp
void WindowOutputDevice::InitClipRegion()
{
    vcl::Region aRegion;
    if (mxOwnerWindow->mpWindowImpl->mbInPaint)
        // Paint() 中：使用 paint region（受限區域）
        aRegion = *(mxOwnerWindow->mpWindowImpl->mpPaintRegion);
    else
        // Paint() 外：使用 window child clip region（整個 window）
        aRegion = mxOwnerWindow->ImplGetWinChildClipRegion();

    if (mbClipRegion)
        aRegion.Intersect(ImplPixelToDevicePixel(maRegion));

    SelectClipRegion(aRegion);  // → X11 GC clip
}
```

**關鍵發現**：`mbInPaint` 是分支條件。Paint() **外**繪圖使用整個 window 的
clip region，**不受 paint region 限制**。

### 3.3 VCL 的 OverlayManagerBuffered 機制

VCL 自己的 overlay 系統（用於游標閃爍、選取反白等）使用的策略：

**檔案**：`svx/source/sdr/overlay/overlaymanagerbuffered.cxx`

#### 架構

```
[Paint() 中]
  EndDrawLayers → DrawOverlay → completeRedraw
    1. ImpSaveBackground：從 window/prerender 複製背景到 mpBufferDevice
    2. ImpDrawMembers：在 window 上畫 overlay（受 system clip 限制）

[Paint() 外，Idle POST_PAINT handler]
  ImpBufferTimerHandler：
    1. 從 mpBufferDevice 恢復背景到 mpOutputBufferDevice
    2. 在 mpOutputBufferDevice 上畫 overlay
    3. getOutputDevice().DrawOutDev() 直接畫到 window
       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
       不在 Paint() 中 → InitClipRegion 使用整個 window → 無 system clip 限制！
```

#### 關鍵程式碼摘要

**初始化**（constructor, line 375-386）：
```cpp
OverlayManagerBuffered::OverlayManagerBuffered(OutputDevice& rOutputDevice)
    : OverlayManager(rOutputDevice),
      mpBufferDevice(VclPtr<VirtualDevice>::Create()),
      mpOutputBufferDevice(VclPtr<VirtualDevice>::Create()),
      maBufferIdle("sdr::overlay::OverlayManagerBuffered maBufferIdle")
{
    maBufferIdle.SetPriority(TaskPriority::POST_PAINT);
    maBufferIdle.SetInvokeHandler(
        LINK(this, OverlayManagerBuffered, ImpBufferTimerHandler));
}
```

**Idle handler**（line 190-373）—— 直接畫到 window：
```cpp
IMPL_LINK_NOARG(OverlayManagerBuffered, ImpBufferTimerHandler, Timer*, void)
{
    maBufferIdle.Stop();
    if (maBufferRememberedRangePixel.isEmpty())
        return;

    // 恢復上次 completeRedraw 的 MapMode
    const MapMode aPrevMapMode(getOutputDevice().GetMapMode());
    if (bPatchMapMode)
        getOutputDevice().SetMapMode(maMapModeLastCompleteRedraw);

    // 從 buffer 恢復背景到 output buffer
    mpOutputBufferDevice->DrawOutDev(aTopLeft, aSize, aTopLeft, aSize,
                                     *mpBufferDevice);

    // 在 output buffer 上畫 overlay
    mpOutputBufferDevice->EnableMapMode();
    OverlayManager::ImpDrawMembers(aBufferRememberedRangeLogic,
                                   *mpOutputBufferDevice);

    // ★ 直接從 output buffer 複製到 window ★
    // 不呼叫 Invalidate()！不觸發 Paint()！
    getOutputDevice().EnableMapMode(false);
    getOutputDevice().DrawOutDev(aTopLeft, aSize, aTopLeft, aSize,
                                 *mpOutputBufferDevice);

    maBufferRememberedRangePixel.reset();

    if (bPatchMapMode)
        getOutputDevice().SetMapMode(aPrevMapMode);
}
```

**invalidateRange**（line 423-481）—— 不呼叫 Window::Invalidate：
```cpp
void OverlayManagerBuffered::invalidateRange(const basegfx::B2DRange& rRange)
{
    // 不呼叫 Window::Invalidate()！
    // 只啟動 Idle 並記錄需要更新的 pixel 區域
    maBufferIdle.Start();
    maBufferRememberedRangePixel.expand(aTopLeft);
    maBufferRememberedRangePixel.expand(aBottomRight);
}
```

### 3.4 為什麼 OverlayManagerBuffered 不破壞捲動

1. **不呼叫 `Invalidate()`**：不觸發新的 Paint cycle，不排程 maPaintIdle
2. **不產生 partial paint**：直接用 `DrawOutDev()` 寫入 window pixel
3. **Idle POST_PAINT 只在 overlay 變化時觸發**：游標閃爍、選取改變時才排程，
   不像我們的 D9 在每次 partial paint 後都排程
4. **背景保存/恢復機制**：`mpBufferDevice` 記錄了 overlay 底下的原始背景，
   更新 overlay 時先恢復背景再重繪，避免殘影

### 3.5 Paint() 外直接繪圖的先例

VCL 中已有多處在 Paint() 外直接繪圖的案例：

1. **OverlayManagerBuffered** (`ImpBufferTimerHandler`)：如上
2. **InvertTracking** (`window2.cxx:162-220`)：拖曳/選取的 rubber-band
3. **PaintBufferGuard** (`paint.cxx:45-152`)：double-buffer 完成後複製到 window

直接繪圖在下次 Paint() 時會被覆蓋，但這對 overlay 沒問題：
下次 Paint() 會經過 PaintOverlays 重新繪製 overlay。

---

## 4. 方向 F 設計：Idle Direct Draw

### 4.1 核心思路

照搬 `OverlayManagerBuffered` 的策略：

1. **Paint() 中**：照常呼叫 `CallOverlayPainters()`（受 system clip 限制）
2. **Paint() 後**：如果是 partial paint，排程 Idle (POST_PAINT)
3. **Idle handler 中**：直接在 editwin 的 OutputDevice 上重繪 overlay
   （不呼叫 `Invalidate()`）

與 D9-D10 的關鍵差異：**Idle handler 不呼叫 `Invalidate()`，而是直接繪製**。

### 4.2 需要修改的檔案

| 檔案 | 動作 |
|------|------|
| `sw/source/uibase/inc/unotxvw.hxx` | 新增 `m_aLastOverlayVisibleArea`（保留），可能移除不需要的成員 |
| `sw/source/uibase/uno/unotxvw.cxx` | 重寫 `PaintOverlays()` Step 2 + 重寫 `OverlayRepaintIdleHdl` |

IDL、test、extension Python 不需要修改。

### 4.3 實作詳細設計

#### PaintOverlays() — Step 1 不變

```cpp
void SwXTextView::PaintOverlays(vcl::RenderContext& rRenderContext,
                                const css::awt::Rectangle& rVisibleArea)
{
    if (!m_pView) return;
    SwEditWin& rEditWin = m_pView->GetEditWin();
    const MapMode& rDocMapMode = m_pView->GetWrtShell().getPrePostMapMode();

    // Step 1: 直接在 RC 上繪製 overlay（受 system clip 限制）
    {
        auto aScopedPush = rRenderContext.ScopedPush(
            vcl::PushFlags::MAPMODE | vcl::PushFlags::LINECOLOR
            | vcl::PushFlags::FILLCOLOR);
        rRenderContext.SetMapMode(rDocMapMode);
        auto xGraphics = rRenderContext.CreateUnoGraphics();
        if (xGraphics.is())
            CallOverlayPainters(xGraphics, rVisibleArea);
    }

    // Step 2: Partial paint 偵測 + 排程 Idle
    // （見下方）
}
```

#### PaintOverlays() — Step 2：排程 Idle

```cpp
    // Step 2: 如果是 partial paint，排程 Idle 直接重繪 overlay
    if (m_aOverlayRepaintIdle.IsActive())
        return;

    vcl::Region aPaintRegion = rEditWin.GetPaintRegion();
    if (aPaintRegion.IsNull() || aPaintRegion.IsEmpty())
        return;  // full paint — 所有 overlay 已在 Step 1 中完整繪製

    // 比較 paint region vs window size（pixel 座標）
    tools::Rectangle aPaintBoundPixel = rEditWin.LogicToPixel(
        aPaintRegion.GetBoundRect());
    Size aWinSize = rEditWin.GetOutputSizePixel();
    tools::Rectangle aWinRect(Point(0, 0), aWinSize);
    aPaintBoundPixel.expand(2);  // rounding tolerance

    if (aPaintBoundPixel.Contains(aWinRect))
        return;  // full paint

    // Partial paint — overlay 部分可能被 clip，排程直接重繪
    m_aOverlayRepaintIdle.Start();
}
```

#### OverlayRepaintIdleHdl — 直接繪製（核心變更）

```cpp
IMPL_LINK_NOARG(SwXTextView, OverlayRepaintIdleHdl, Timer*, void)
{
    if (!m_pView)
        return;

    SwEditWin& rEditWin = m_pView->GetEditWin();
    OutputDevice* pOutDev = rEditWin.GetOutDev();
    if (!pOutDev)
        return;

    const MapMode& rDocMapMode = m_pView->GetWrtShell().getPrePostMapMode();

    // 取得 visible area（與 edtwin2.cxx 中的 PaintOverlays 呼叫相同邏輯）
    const tools::Rectangle& rVisArea = m_pView->GetVisArea();
    css::awt::Rectangle aVisArea(
        rVisArea.Left(), rVisArea.Top(),
        rVisArea.GetWidth(), rVisArea.GetHeight());

    // 直接在 editwin 的 OutputDevice 上繪製 overlay
    // 不在 Paint() 中 → InitClipRegion 使用整個 window → 無 system clip 限制
    {
        auto aScopedPush = pOutDev->ScopedPush(
            vcl::PushFlags::MAPMODE | vcl::PushFlags::LINECOLOR
            | vcl::PushFlags::FILLCOLOR);
        pOutDev->SetMapMode(rDocMapMode);

        css::uno::Reference<css::awt::XGraphics> xGraphics
            = pOutDev->CreateUnoGraphics();
        if (xGraphics.is())
            CallOverlayPainters(xGraphics, aVisArea);
    }

    // 確保繪製立即顯示在螢幕上
    pOutDev->Flush();
}
```

### 4.4 為什麼這會成功

| 問題 | D9-D10 的做法 | 方向 F 的做法 | 結果 |
|------|--------------|-------------|------|
| overlay 恢復 | `Invalidate()` → 新 Paint → 受 system clip | 直接 `DrawOutDev`/`CreateUnoGraphics` 畫到 window | ✓ 不受 system clip |
| 捲動 | Invalidate 排程 maPaintIdle → 餓死 input | 不呼叫 Invalidate → 不排程 maPaintIdle | ✓ 不影響 input |
| 閃爍 | Invalidate → Paint cycle 延遲 | Idle POST_PAINT → 即時直接繪製 | ✓ 幾乎無閃爍 |
| 下次 Paint 覆蓋 | — | 被覆蓋沒關係，PaintOverlays Step 1 會重繪 | ✓ 自動恢復 |

### 4.5 風險和注意事項

1. **MapMode 安全**：D5 證明在 Paint() 外 SetMapMode 不安全。但這裡 SetMapMode
   在 ScopedPush 中，且是設在 OutputDevice 而非 Window 本身。
   OverlayManagerBuffered 也在 Idle handler 中做 `getOutputDevice().SetMapMode()`，
   所以這個模式經過 VCL 驗證。

2. **Graphics context 取得**：`CreateUnoGraphics()` 內部呼叫 `AcquireGraphics()`。
   在 Paint() 外應該也能取得（InvertTracking 就是這樣做的）。如果失敗，
   fallback 到 `Invalidate()` 方案。

3. **Flush 必要性**：直接繪製後需要 `Flush()` 才能顯示在螢幕上。
   OverlayManagerBuffered 不需要顯式 Flush（因為 DrawOutDev 到 window
   會自動 flush），但可能因平台而異，保險起見加上。

4. **Double buffering 相容性**：如果 SwEditWin 啟用了 double buffering，
   直接繪製可能寫到 buffer 而非螢幕。需要確認
   `SwEditWin::SupportsDoubleBuffering()` 的狀態。

5. **無限循環防護**：方向 F 的 Idle handler 不呼叫 Invalidate，所以不會
   觸發新的 Paint → 不會重新進入 PaintOverlays → Idle 不會被重新排程。
   循環自然終止。mock painters（`getOverlayBounds()` 回傳空）不觸發 Idle，
   `ProcessEventsToIdle()` 安全。

### 4.6 不需要捲動偵測

方向 F 不需要 D10 的 visibleArea 比較捲動偵測：
- Idle handler 不呼叫 Invalidate → 不餓死 input events → 捲動自然正常
- 捲動時的每個 full paint 都會經過 PaintOverlays Step 1 重繪 overlay
- 可以移除 `m_aLastOverlayVisibleArea` 成員

---

## 5. 座標空間速查表

| API | 回傳座標空間 | 備註 |
|-----|-------------|------|
| `GetPaintRegion().GetBoundRect()` | editwin 當前 MapMode（logical） | **非 pixel** |
| `GetOutputSizePixel()` | pixel | |
| `LogicToPixel(rect)` | pixel（from editwin's MapMode） | |
| `LogicToPixel(rect, mapMode)` | pixel（from 指定 MapMode） | |
| `PixelToLogic(rect, mapMode)` | 指定 MapMode 的 logical | |
| `getPrePostMapMode()` | document twips（含 scroll/zoom offset） | |
| `getParagraphBounds()` | document twips | |
| `getOverlayBounds()` | document twips | |
| `Window::Invalidate(rect)` | editwin 當前 MapMode（logical） | **非 pixel** |
| `GetOutDev()` | WindowOutputDevice | Paint() 外可用 |

---

## 6. Build & 驗證

```bash
# Build
cd /home/jiajun/LibreOffice/LO-core
make sw

# CppUnit tests
make CppunitTest_sw_uibase_uno

# 手動測試
PROFILE_URL="file:///tmp/lo-md-dev-profile"
MD_SIDEBAR_OVERLAY_DEMO=1 \
MD_SIDEBAR_SEED_PARAGRAPH_NAV_SAMPLE=1 \
MD_SIDEBAR_VALIDATE_OVERLAY=1 \
/home/jiajun/LibreOffice/LO-core/instdir/program/soffice \
  -env:UserInstallation="$PROFILE_URL" --writer
```

驗證項目：
1. CppUnit 全部通過（`ProcessEventsToIdle` 不掛住）
2. 初始顯示：三個段落邊框可見
3. 打字：邊框穩定不消失（或 <16ms 恢復，不可見閃爍）
4. 捲動/縮放：邊框正確，滾輪/捲軸正常
5. overlay add/remove/visibility：即時反映

---

## 7. 目前代碼狀態

目前 `unotxvw.cxx` 中的代碼為 **D10b 版本**（Idle + 捲動偵測 + full-paint 偵測）。
需要修改的部分：

1. **移除** `m_aLastOverlayVisibleArea` 成員和捲動偵測邏輯
2. **保留** `m_aOverlayRepaintIdle` 和 partial paint 偵測
3. **重寫** `OverlayRepaintIdleHdl`：從 `Invalidate()` 改為直接繪製

Header (`unotxvw.hxx`) 中可能需要移除 `m_aLastOverlayVisibleArea`，其餘不變。

---

## 8. 與前份 Spec 的關係

- `phase5-overlay-compositing-fix-spec.md`：Steps 1–2e 失敗記錄（buffer 方案）
- `phase5-overlay-direct-paint-spec.md` v0.5：Direction D 完整迭代記錄 + VCL 研究初步結果
- **本文件**：最終方案設計，包含 VCL 深入研究結果和方向 F 的完整實作規格
