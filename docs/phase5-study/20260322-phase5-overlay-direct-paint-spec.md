# Phase 5 策略重設：Direct Paint + Targeted Invalidation

> 版本：0.5
> 日期：2026-03-22
> 狀態：Direction D 已到極限——不可能三角（overlay穩定 vs 捲動正常 vs 無閃爍）。需換策略。
> 前置文件：`phase5-overlay-compositing-fix-spec.md`（Steps 1–2e 全部失敗記錄）
> 分支：ext-uno-api-26-2-1

---

## 1. 問題回顧

### 1.1 Steps 1–2e 失敗總結（Content Buffer 方案）

| Step | 方案 | 結果 | 失敗原因 |
|------|------|------|----------|
| 1 | `GetBitmap` + `DrawBitmap` alpha blit | 失敗 | VCL `GetBitmap()` 不保留 alpha 通道 |
| 2a | 移除 buffer，直接繪製到 RC | 部分成功 | system clip 限制，overlay 在 partial paint 外消失 |
| 2b | buffer + 逐區域 blit | 部分成功 | buffer 每次 Erase，compositing 被 clip 裁切 |
| 2c | 持久 buffer + 全視窗複製 | 部分成功 | Step 1 全複製以不新鮮的 RC 像素覆蓋 buffer 中的 overlay |
| 2d | 持久 buffer + 選擇性複製 + Paint 內 Invalidate | 部分成功 | Bug A: 座標錯誤；Bug B: Invalidate 在 Paint 內不可靠 |
| 2d′ | 2d + 座標修正 + flag race fix | 部分成功 | Invalidate 在 Paint 內仍不可靠 |
| 2e | 2d′ + Idle deferred invalidation | 失敗 | Idle 觸發但 overlay 仍消失——system clip 阻止 blit |

### 1.2 Direction D 迭代記錄（Direct Paint + Targeted Invalidation）

| 迭代 | 方案 | overlay 穩定 | 滾輪/捲軸 | 閃爍 | 失敗原因 |
|------|------|:---:|:---:|:---:|----------|
| D1 | Direct paint + Idle targeted Invalidate（pixel coords） | ✗ | — | — | `GetPaintRegion()` 回傳 logical coords，與 `GetOutputSizePixel()` (pixel) 比較，永遠偵測為 partial paint |
| D2 | D1 + LogicToPixel 修正 partial paint 偵測 | ✗ | — | — | `m_bOverlayRepaintPending` 不在 Idle handler 重設，flag 卡死在 true |
| D3 | D2 + Idle handler 重設 flag | ✗ | — | — | Idle handler 中 `Invalidate(pixelRect)` 無效——`Window::Invalidate(rect)` 預期 editwin 當前 MapMode 座標，非 pixel |
| D4 | Idle + 全視窗 `Invalidate()` | ✓ | ✗ | 輕微 | 滾輪不動、捲軸變紅。當時未修正 LINECOLOR leak |
| D5 | D4 + Idle handler 中 SetMapMode + targeted Invalidate | ✗→✓ | ✗ | 輕微 | 同 D4 副作用。在 Paint() 外 SetMapMode 不安全 |
| D6 | 移除 Idle，Paint 內直接 Invalidate + LINECOLOR ScopedPush | ✗ | ✓ | — | Paint 內的 Invalidate 不觸發後續 repaint |
| D7 | Timer 50ms + LINECOLOR fix | ✓ | ✓ | **明顯** | Timer 延遲 50ms 造成可見閃爍 |
| D8 | Timer 16ms + LINECOLOR fix | ✓ | ✓ | **明顯** | 16ms 仍有可見閃爍 |
| **D9** | **Idle POST_PAINT + LINECOLOR fix** | **✓** | **✗** | **輕微可接受** | 滾輪不動、捲軸異常。**確認：LINECOLOR leak 非捲動問題根因** |
| D10a | D9 + 捲動偵測（visibleArea 比較） | ✓ | ✗ | 輕微 | overlay bounds `Contains()` 比較：段落 bounds 超出可視區 → `bNeedRepaint` 永遠 true → **無限 Idle→Invalidate→Paint 循環** 餓死 input events（同 D9 根因） |
| **D10b** | **D10a + full-paint 偵測（pixel window-size 比較）** | **✗** | **✓** | — | 修正無限循環後，滾輪/捲軸恢復正常，但 overlay 在打字時完全消失。D9 的 overlay 穩定其實靠無限循環的「持續 full repaint」維持 |

### 1.3 關鍵發現

#### 已確認的事實

1. **System clip 不可清除**：X11 GC clip 在 `Paint()` 中不可繞過
2. **Direct paint + Idle + 全視窗 Invalidate 可讓 overlay 穩定**（D4/D9 證明）
3. **LINECOLOR/FILLCOLOR 必須在 ScopedPush 中保存/恢復**，否則洩漏到後續 VCL 繪圖
4. **Paint 內的 `Invalidate(rect)` 不會觸發後續 repaint**（D6 證明），與 Spec 原本假設相反
5. **`Window::Invalidate(rect)` 預期 editwin 當前 MapMode 的座標**，非 pixel（D3 證明）
6. **`GetPaintRegion()` 回傳 editwin 當前 MapMode 的邏輯座標**，非 pixel（D1 證明）
7. **Timer 延遲 ≥16ms 會造成可見閃爍**（D7/D8 證明）
8. **LINECOLOR leak 不是捲動問題的根因**（D9 證明——修正 LINECOLOR 後滾輪仍不動）
9. **D9 的 overlay 穩定靠無限 Idle 循環維持**（D10 證明）：overlay bounds（`getParagraphBounds` 回傳整個段落的 doc-twips）可超出可視區，`Contains()` 即使在 full paint 也失敗 → `bNeedRepaint` 永真 → Idle→Invalidate→Paint 無限循環。循環副作用：持續 full repaint 讓 overlay 穩定，但餓死所有 input events
10. **修正無限循環後（pixel window-size 比較），滾輪/捲軸恢復**（D10b 證明）：確認捲動問題根因就是無限 Idle 循環餓死 input events
11. **單次 Idle recovery 不足以讓 overlay 在快速打字時穩定**（D10b 證明）：partial paint → Idle → full paint（overlay 恢復）→ 下一個按鍵 → partial paint（overlay 再次被 clip）→ 恢復/消失交替太快，使用者感知為「消失」

#### 核心矛盾（已釐清）

Direction D 的所有方案都面臨同一個不可能三角：

```
        overlay 穩定
           /    \
          /      \
    捲動正常 —— 無閃爍
```

| 方案 | overlay 穩定 | 捲動正常 | 無閃爍 | 機制 |
|------|:---:|:---:|:---:|------|
| D9（無限 Idle 循環） | ✓ | ✗ | ✓ | 持續 full repaint 維持 overlay，餓死 input |
| D7/D8（Timer） | ✓ | ✓ | ✗ | Timer 延遲恢復 overlay，但延遲造成閃爍 |
| D10b（單次 Idle + 捲動偵測） | ✗ | ✓ | ✓ | 循環終止後 overlay 在按鍵間消失 |

**根因**：在 Paint() callback 中，system clip 限制所有繪圖在 paint region 內。任何 overlay 區域超出 partial paint region 的部分都會被裁切。恢復需要額外的 repaint，但額外 repaint 本身就與「正常繪圖」和「無閃爍」衝突。

**結論**：Direction D（Direct Paint + deferred Invalidation）無法同時滿足三個條件。需要換策略——在 Paint() callback 之外繪製 overlay，或使用不受 system clip 影響的繪圖層。

#### 滾輪/捲軸問題的根因（已確認）

D10 證實：**根因是無限 Idle→Invalidate→Paint 循環餓死 input events**。

D9 中 overlay bounds 比較邏輯有 bug（`Contains()` 對超出可視區的段落 bounds 永遠失敗），導致每次 Idle-triggered full paint 後仍排程新 Idle → 無限循環。修正為 pixel window-size 比較後，循環正確終止，滾輪/捲軸恢復正常。

---

## 2. 目前代碼狀態

當前代碼為 **D9 版本**（Idle POST_PAINT + LINECOLOR fix）。

### 2.1 已完成的修改

| 檔案 | 狀態 | 說明 |
|------|------|------|
| `offapi/com/sun/star/text/XOverlayPainter.idl` | ✅ | 新增 `getOverlayBounds()` |
| `sw/source/uibase/inc/unotxvw.hxx` | ✅ | 移除 buffer 成員，Idle + handler 宣告 |
| `sw/source/uibase/uno/unotxvw.cxx` | ⚠️ D9 | overlay 穩定但捲動異常 |
| `sw/qa/uibase/uno/uno.cxx` | ✅ | Mock painters 實作 `getOverlayBounds()` |
| `markdown-insert-ext/pythonpath/document_overlay_validation.py` | ✅ | `ValidationOverlayPainter` 實作 `getOverlayBounds()` |

### 2.2 D9 核心代碼摘要

```cpp
// PaintOverlays Step 1: 直接繪製（LINECOLOR/FILLCOLOR 已保護）
{
    auto aScopedPush = rRenderContext.ScopedPush(
        vcl::PushFlags::MAPMODE | vcl::PushFlags::LINECOLOR
        | vcl::PushFlags::FILLCOLOR);
    rRenderContext.SetMapMode(rDocMapMode);
    auto xGraphics = rRenderContext.CreateUnoGraphics();
    if (xGraphics.is())
        CallOverlayPainters(xGraphics, rVisibleArea);
}

// PaintOverlays Step 2: 檢查 overlay bounds 是否被 clip，排程 Idle
if (m_aOverlayRepaintIdle.IsActive())
    return;
// ... 轉換 paint region 到 doc-twips，比較 overlay bounds ...
if (bNeedRepaint)
    m_aOverlayRepaintIdle.Start();

// Idle handler: 全視窗 Invalidate（零 MapMode 改變）
IMPL_LINK_NOARG(SwXTextView, OverlayRepaintIdleHdl, Timer*, void)
{
    if (!m_pView) return;
    m_pView->GetEditWin().Invalidate();
}
```

### 2.3 CppUnit 測試

全部通過。mock painters 回傳空 `getOverlayBounds()` → 永不排程 Idle → `ProcessEventsToIdle()` 安全。

### 2.4 手動測試

| 項目 | D9 (Idle) | D10b (Idle+捲動偵測) | D8 (Timer 16ms) |
|------|:---------:|:-------------------:|:---------------:|
| overlay 穩定 | ✓（靠無限循環） | ✗（打字時消失） | ✓ |
| 閃爍程度 | 輕微可接受 | — | 明顯 |
| 滾輪捲動 | ✗ | ✓ | ✓ |
| 捲軸正常 | ✗ | ✓ | ✓ |
| CppUnit | ✓ | ✓ | ✓ |

---

## 3. 尚未嘗試的方向

### 已嘗試並排除的方向

- ~~**方向 A：捲動偵測 + Idle**~~ → D10a/D10b 已實作。捲動偵測有效但無限循環是獨立 bug。修正循環後 overlay 在打字時消失。
- ~~**方向 E：調查 Idle + Invalidate 為何破壞捲動**~~ → D10 已確認根因：overlay bounds `Contains()` bug 導致無限循環餓死 input。

### 3.1 方向 F：Idle handler 直接繪製（跳過 Paint cycle）

Idle handler 不呼叫 `Invalidate()`，而是直接取得 editwin 的 OutputDevice 並繪製 overlay：

```cpp
IMPL_LINK_NOARG(SwXTextView, OverlayRepaintIdleHdl, Timer*, void)
{
    if (!m_pView) return;
    SwEditWin& rEditWin = m_pView->GetEditWin();
    vcl::RenderContext* pRC = rEditWin.GetOutDev();
    // 直接在 window 上繪製 overlay（不經過 Paint() → 無 system clip）
    auto aScopedPush = pRC->ScopedPush(...);
    pRC->SetMapMode(docMapMode);
    auto xGraphics = pRC->CreateUnoGraphics();
    CallOverlayPainters(xGraphics, visibleArea);
}
```

**優點**：不觸發 Paint cycle → 不產生 Invalidate → 不餓死 input events → 不閃爍
**缺點**：Paint() 外直接繪製到 window 可能在下次 repaint 時被覆蓋（但那次 repaint 本身會經過 PaintOverlays 重繪 overlay）；可能在某些平台上不安全
**風險**：中。需要確認 VCL 是否允許在 Paint() 外直接繪製到 window
**推薦度**：★★★★ — 最有可能一步到位解決不可能三角

### 3.2 方向 G：VCL Overlay 平面（sdr::overlay）

利用 VCL 原生的 overlay mechanism（`sdr::overlay::OverlayManager`），在獨立繪製平面上疊加 overlay。游標和選取反白都是用這個機制——它們在 partial paint 中不會消失。

```cpp
// SwEditWin 已經有 OverlayManager
sdr::overlay::OverlayManager* pOverlayMgr = ...;
// 建立 OverlayObject，加到 manager
auto pObj = new sdr::overlay::OverlayObjectPrimitive2D(...);
pOverlayMgr->add(*pObj);
```

**優點**：從根本解決 system clip 問題；不需要任何 Invalidate hack；VCL 已驗證的機制
**缺點**：需要深入理解 sdr::overlay 架構；可能需要大幅修改 XOverlayPainter IDL API 設計（目前 callback-based，可能需要改為提供 primitive 的方式）；學習曲線陡峭
**風險**：高。API 設計可能需要根本性改變
**推薦度**：★★★ — 正確但困難的方案

### 3.3 方向 H：混合 Idle + Timer（自適應延遲）

結合 D10b 的捲動偵測和 Timer debounce。打字時用 Idle（快速），但加入「連續 partial paint 計數器」：

```cpp
// 若連續 N 次 partial paint（每次 Idle 恢復後又被 clip），
// 切換到 Timer（例如 200ms）等 input burst 結束再恢復
if (m_nConsecutivePartialPaints > 3)
    m_aOverlayRepaintTimer.Start();  // 200ms debounce
else
    m_aOverlayRepaintIdle.Start();   // immediate
```

**優點**：在低頻操作（單次 click）時快速恢復，高頻操作（快速打字）時等 burst 結束再恢復
**缺點**：overlay 在快速打字期間仍會消失（直到打字停頓 200ms）；邏輯複雜
**風險**：低
**推薦度**：★★ — 妥協方案，行為與 D7/D8 在快速打字時相同

### 3.4 評估總結

| 方向 | overlay 穩定 | 捲動 | 閃爍 | 實作難度 | 推薦 |
|------|:---:|:---:|:---:|:---:|:---:|
| F（Idle 直接繪製） | 可能 ✓ | ✓ | ✓ | 中 | ★★★★ |
| G（sdr::overlay） | ✓ | ✓ | ✓ | 高 | ★★★ |
| H（自適應延遲） | ✗（burst 中消失） | ✓ | 可接受 | 低 | ★★ |

---

## 4. 座標空間速查表

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

---

## 5. 附錄：段落索引 vs 段落身份

手動測試發現：按 Enter 新增段落後，overlay 框線跟著段落索引而非段落內容移動。

**這是預期行為**。demo 的 `ValidationOverlayPainter` 用固定 `para_index` 建立。真正的 extension 需要追蹤段落身份（例如透過 text content 比對或 cursor position）來動態更新 painter 的目標段落。

---

## 6. 與前一份 Spec 的關係

本 spec 完全取代 `phase5-overlay-compositing-fix-spec.md` 中 §2–§9 的修正方案。

前一份 spec 的以下內容仍然有效：
- §1（問題描述）的初始觀察
- §7（實測結果總覽）的歷史記錄
- §10–§12（Steps 2d/2e 的深入分析和核心發現）
