# Extending LibreOffice UNO API for Custom UI Rendering in Extensions

> Version: 0.1 draft
> Date: 2026-03-27
> Author: Jiajun
> Branch: ext-uno-api-26-2-1

---

## 1. Motivation

### 1.1 Why This Work

I want to build a LibreOffice extension similar to Claude Code or Cline in VSCode — an **AI interaction sidebar**. While the user writes documents in Writer, they can chat with an AI through the sidebar; the AI can annotate, highlight paragraphs, and display suggestions in real time, providing an experience comparable to modern AI-assisted code editors.

After attempting to build this, it became clear that **the existing UNO API has significant gaps for extension UI customization**:

| Capability needed | Current state |
|-------------------|--------------|
| Custom-rendered chat bubbles and code blocks in the sidebar | UNO only provides standard widgets (Button, Edit, etc.); no custom painting |
| Overlaying paragraph highlights and labels on the document view | No overlay API; extensions cannot draw over the document view |
| Text measurement and automatic word-wrapping | XGraphics/XGraphics2 only has single-line `drawText()`; no measurement, no wrapping |
| Querying paragraph positions on screen | No paragraph geometry API |

**These are not quirks of one extension** — they are fundamental limitations that any extension attempting advanced UI will hit. I therefore chose to work on the core codebase and add general-purpose UNO APIs that any extension can use.

### 1.2 Design Strategy

```
LO core (changed once)              Extension (iterated freely)
┌──────────────────────┐           ┌──────────────────────┐
│ New UNO IDL interfaces│           │ Python or C++ .oxt   │
│ New VCL bridge impls  │ ← UNO →  │ Implements callbacks  │
│ in toolkit/ and sw/  │           │ Custom rendering via  │
│                      │           │ new APIs             │
└──────────────────────┘           └──────────────────────┘
```

The core is modified once to produce general-purpose APIs. All subsequent extensions can use them; extensions continue to be distributed as independent .oxt packages.

### 1.3 Scope

This proposal covers API additions at two levels:

- **Toolkit layer** (generic, available to all LO applications): enhanced drawing capability, custom-paint windows, splitter windows
- **Writer layer** (Writer-specific): paragraph navigation and geometry queries, document overlay painting

The work is organized into five phases described below in development order.

---

## 2. Phase 1 + 1.5: XGraphics3 — Text Measurement and Multi-line Drawing

### Problem

Building custom UI (e.g., chat bubbles in a sidebar) requires measuring text dimensions and wrapping text automatically. The existing `XGraphics` / `XGraphics2` interfaces only provide:

- `drawText(x, y, text)` — single-line only
- `getFontMetric()` — returns ascent/descent but cannot measure a specific string

VCL's `OutputDevice` has long had `GetTextWidth()`, `GetTextHeight()`, `GetTextRect()`, and `DrawText(rect, flags)`, but **none of these were ever exposed through the UNO API** to extensions.

A secondary problem is layout consistency: the pre-existing `measureText(text, maxWidth)` and `drawTextInRect(rect, text, flags)` take different inputs, so an extension cannot reliably measure before drawing and get matching results.

### Solution

Add an `XGraphics3` interface inheriting `XGraphics2` (following LO convention), with five new methods:

| New method | Maps to VCL | Purpose |
|------------|-------------|---------|
| `getTextWidth(text)` | `OutputDevice::GetTextWidth()` | Measure string width in pixels |
| `getTextHeight()` | `OutputDevice::GetTextHeight()` | Measure current font line height |
| `measureText(text, maxWidth)` | `OutputDevice::GetTextRect()` | Compute bounding rect after word-wrap within maxWidth |
| `measureTextInRect(rect, text, flags)` | `OutputDevice::GetTextRect()` | Measure with **identical** inputs to `drawTextInRect()`, guaranteeing layout consistency |
| `drawTextInRect(rect, text, flags)` | `OutputDevice::DrawText(rect, flags)` | Draw multi-line wrapped text |

`measureTextInRect()` solves the consistency problem: an extension can measure first and then draw with the same rect and flags; the layout algorithm is identical, so there is no positional drift or unexpected clipping.

The C++ bridge lives in `VCLXGraphics`, following the same pattern as existing methods: acquire SolarMutex → sync state → call OutputDevice.

### Status

Complete. IDL: `offapi/com/sun/star/awt/XGraphics3.idl`. Bridge: `toolkit/source/awt/vclxgraphics.cxx`.

---

## 3. Phase 2: XCustomPaintWindow + XSplitterWindow — Custom-paint Sidebar

### Problem

Sidebar panels in extensions are limited to composing standard UNO widgets (Button, Edit, Label, etc.). Rendering chat bubbles, code blocks, or syntax-highlighted content requires a window that accepts a paint callback.

### Solution

Two new UNO components:

**XCustomPaintWindow** — a window with a paint callback
- Extension implements `XCustomPaintHandler`, which is called back during VCL's paint cycle
- Draws using `XGraphics3`
- Forwards mouse and keyboard events to the extension
- Also provides explicit repaint control (`repaint()`, `repaintRect()`) and content-coordinate scroll offset management (`setScrollOffset()`, `getScrollOffsetX()`, `getScrollOffsetY()`)

**XSplitterWindow** — a draggable divider
- Wraps VCL `Splitter`
- User can drag to resize adjacent regions

Target sidebar layout:

```
┌──────────────────────┐
│  ChatView            │ ← XCustomPaintWindow (custom-rendered chat)
│                      │
├══ splitter ══════════┤ ← XSplitterWindow (user-draggable)
│  ChatInput           │ ← existing UnoControlEdit
├──────────────────────┤
│  [Send] [Settings]   │ ← existing UnoControlButton
└──────────────────────┘
```

### Status

Complete. Implementation in `toolkit/source/awt/`. IDL in `offapi/com/sun/star/awt/`.

---

## 4. Phase 3: XParagraphNavigator — Paragraph Navigation and Geometry

### Problem

To annotate specific paragraphs (highlights, labels), an extension needs to know where each paragraph is on screen. The existing UNO API only provides document-model interfaces (`XText`, `XTextRange`, etc.); there is **no view-layer paragraph geometry API**.

### Solution

Add an `XParagraphNavigator` interface on Writer's `TextDocumentView`:

| Method | Purpose |
|--------|---------|
| `getCount()` | Number of paragraphs in the document |
| `getCurrentIndex()` | Index of the paragraph at the cursor |
| `gotoIndex(index)` | Move cursor to the given paragraph |
| `gotoNext(select)` / `gotoPrevious(select)` | Sequential navigation, optionally extending the selection |
| `selectCurrentParagraph()` | Select the current paragraph using the view's normal selection behavior |
| `getParagraphBounds(index)` | Layout fragment rectangles for a paragraph (document twips); returns a `sequence<Rectangle>` because a paragraph may span multiple layout fragments |
| `getParagraphViewBounds(index)` | Same as `getParagraphBounds()` but in screen pixel coordinates |
| `isVisible(index)` | Whether any part of the paragraph is currently in the visible viewport |
| `getParagraphText(index)` | Plain text content of the paragraph |
| `getParagraphStyleName(index)` | Name of the paragraph style applied |

**Scope limitation**: the interface covers only **main body text** paragraphs. Paragraphs inside tables, text frames, headers, footers, footnotes, and other non-body containers are outside the current scope.

Geometry comes from Writer's layout engine (`SwFrame` tree) and is expressed in document twips, consistent with the document coordinate system.

### Status

Complete, CppUnit tests pass. Bridge: `sw/source/uibase/uno/unotxvw.cxx`. IDL: `offapi/com/sun/star/text/XParagraphNavigator.idl`.

---

## 5. Phase 4: XDocumentOverlay — Document Overlay Painting

### Problem

Extensions need to draw on top of the Writer editing area (paragraph highlights, borders, labels, etc.) without modifying document content. This requires a view-bound overlay paint API.

### Solution

Add an `XDocumentOverlay` interface on `TextDocumentView`:

```
Extension                           Writer core
┌─────────────────┐                ┌─────────────────────┐
│ implements       │                │                     │
│ XOverlayPainter  │◄── callback ──│ called during paint │
│ ├ paintOverlay() │                │                     │
│ └ getOverlayBounds()              │                     │
└─────────────────┘                └─────────────────────┘
```

**XDocumentOverlay** methods:

| Method | Purpose |
|--------|---------|
| `addOverlay(painter, layer)` | Register an overlay painter; returns a handle |
| `removeOverlay(handle)` | Unregister and remove the overlay |
| `invalidateOverlay(area)` | Request a repaint of the overlay. The `area` parameter is present in the IDL for future area-specific invalidation; the current implementation invalidates the entire overlay object regardless of the given area. |
| `setOverlayVisible(handle, visible)` | Show or hide the overlay |
| `isOverlayVisible(handle)` | Query the current visibility state |

**XOverlayPainter** (implemented by the extension):

| Method | Purpose |
|--------|---------|
| `paintOverlay(graphics, visibleArea)` | Draw the overlay on the given XGraphics |
| `getOverlayBounds()` | Report the rectangles the overlay occupies (document twips). Returning an empty sequence is permitted; the view falls back to the entire visible area and still calls `paintOverlay()`. |

### Status

IDL published, CppUnit tests pass. Phase 4 validated the overlay paint lifecycle.

---

## 6. Phase 5: OverlayObject Bridge — Fixing Overlay Persistence

### Problem

The Phase 4 implementation was erased by VCL's `OverlayManagerBuffered` on every cursor blink and partial repaint. The root cause: VCL maintains a background save/restore loop, and **only `OverlayObject` instances registered with the OverlayManager** are redrawn within that loop. Any pixels painted directly to the OutputDevice outside this system are wiped on the next restore.

Seven alternative approaches were attempted (directions F1–F4, G1–G3), exhausting all strategies that draw directly into the paint pipeline. All failed. The only viable path is proper OverlayObject registration.

### Solution

A new `OverlayExtensionPainter` class inherits `sdr::overlay::OverlayObject`, following the same pattern as Writer's internal `OverlayRanges`, `ShadowOverlayObject`, and `AnchorOverlayObject`:

1. **OverlayObject registration** — `OverlayManager::add()` makes VCL aware of the object so it is redrawn in the background restore loop
2. **Bitmap bridge** — `createOverlayObjectPrimitive2DSequence()` creates a VirtualDevice, calls `XOverlayPainter::paintOverlay()`, captures the result as a `Bitmap`, and wraps it in a `BitmapPrimitive2D`
3. **Transparency** — `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` background; unpainted regions remain transparent
4. **MapMode tracking** — overrides `getOverlayObjectPrimitive2DSequence()` to detect MapMode changes on zoom/scroll, automatically invalidating the primitive cache so it is rebuilt at the new scale

### Key Bugs Encountered and Fixed

| Bug | Root cause | Fix |
|-----|-----------|-----|
| Overlay displaced after zoom | OverlayManager destroyed/recreated; `m_bOverlayRegistered` stayed `true` | Check actual OverlayManager pointer, not just the flag; reset primitive cache on MapMode change |
| `GetBitmap()` returned blank | `GetBitmap()` interprets arguments as logical coordinates under the current MapMode | Call `EnableMapMode(false)` before `GetBitmap()`, pass pixel coordinates |
| White box covering document text | `DeviceFormat::WITHOUT_ALPHA` has no alpha channel | Switch to `DeviceFormat::WITH_ALPHA` + `COL_TRANSPARENT` + `Erase()` |
| Wrong transform matrix | `B2DHomMatrix::scale().translate()` post-multiplies, giving wrong result | Use `set()` to write matrix elements directly |
| Right/bottom borders clipped | Pixels drawn at exact VirtualDevice boundary fall outside valid pixel range | Add 40 twips margin to bounding rect |

### Status

**Complete and verified.** Overlay remains visible and correctly positioned through cursor blinks, typing, scrolling, and zoom changes, with a transparent background.

Full technical specification: `docs/phase5-study/H2A-overlay-object-bridge-spec.md` (rev.4).

---

## 7. Changed Files

### Toolkit layer (Phases 1, 2)

| Kind | File | Notes |
|------|------|-------|
| IDL | `offapi/com/sun/star/awt/XGraphics3.idl` | Text measurement and multi-line drawing |
| IDL | `offapi/com/sun/star/awt/TextLayoutMetrics.idl` | Return type for `measureTextInRect()` |
| IDL | `offapi/com/sun/star/awt/XCustomPaintWindow.idl` | Custom-paint window |
| IDL | `offapi/com/sun/star/awt/XCustomPaintHandler.idl` | Paint callback interface |
| IDL | `offapi/com/sun/star/awt/XSplitterWindow.idl` | Splitter window |
| C++ | `toolkit/inc/awt/vclxgraphics.hxx` | XGraphics3 bridge header |
| C++ | `toolkit/source/awt/vclxgraphics.cxx` | XGraphics3 bridge |
| C++ | `toolkit/source/awt/` | XCustomPaintWindow / XSplitterWindow bridge |
| Test | `toolkit/qa/cppunit/CustomWindow.cxx` | Toolkit CppUnit tests |

### Writer layer (Phases 3, 4, 5)

| Kind | File | Notes |
|------|------|-------|
| IDL | `offapi/com/sun/star/text/XParagraphNavigator.idl` | Paragraph navigation and geometry |
| IDL | `offapi/com/sun/star/text/XDocumentOverlay.idl` | Overlay management |
| IDL | `offapi/com/sun/star/text/XOverlayPainter.idl` | Overlay paint callback |
| IDL | `offapi/com/sun/star/text/TextDocumentView.idl` | Service declaration for new interfaces |
| C++ | `sw/source/uibase/uno/unotxvw.cxx` | XParagraphNavigator / XDocumentOverlay bridge |
| C++ | `sw/source/uibase/docvw/OverlayExtensionPainter.cxx` | OverlayObject bridge subclass |
| C++ | `sw/source/uibase/inc/OverlayExtensionPainter.hxx` | Header for above |
| C++ | `sw/source/uibase/inc/unotxvw.hxx` | SwXTextView additions |
| C++ | `sw/Library_sw.mk` | Add OverlayExtensionPainter to build |
| C++ | `sw/CppunitTest_sw_uibase_uno.mk` | Test build configuration |
| Test | `sw/qa/uibase/uno/uno.cxx` | CppUnit tests |

---

## 8. Current Status and Future Directions

### Current status

- Phases 1–4 IDL and C++ bridges are complete
- Phase 5 OverlayObject bridge is complete and verified (persistence, transparency, zoom tracking)
- CppUnit tests cover add/remove/invalidate/paint callback/partial-repaint scenarios, and robustness against reentrant misuse (note: the published contract forbids calling `addOverlay`/`removeOverlay`/`setOverlayVisible` from inside `paintOverlay()`; the tests verify the implementation does not crash on such misuse, not that it is a supported pattern)
- Builds and runs on branch `ext-uno-api-26-2-1`

### Possible future improvements

1. **Bitmap → vector primitives** (medium-term): the bitmap bridge may show resolution artifacts at high DPI or large zoom factors. Replacing it with GDIMetaFile recording + conversion to vector `Primitive2D` would be resolution-independent and natively transparent.

2. **Overlay hit-testing**: overlays are currently display-only. Adding hit-test support would enable click and hover interactions.

3. **Other application modules**: the Writer-layer APIs (Phases 3–5) are implemented only in `sw/`. Similar overlay and navigation APIs could be extended to Calc and Impress.

4. **Reference extension**: a Python .oxt demonstrating all the new APIs together — AI sidebar, paragraph annotation, and document overlay — to validate the full stack end-to-end.

---

## 9. A Note on Development Approach

This work relied heavily on AI tooling (Claude Code). As a non-traditional community contributor — self-taught and working with AI assistance — I am not as familiar with LibreOffice internals as experienced core developers. That said, AI-assisted code reading and iterative development made it possible to understand VCL, OverlayManager, and the drawinglayer pipeline deeply enough to produce a working implementation.

Each phase followed the same workflow: research → write a spec → confirm approach → implement → record results. All spec documents are in `docs/`.

Feedback on API design, implementation choices, or community process is very welcome.
