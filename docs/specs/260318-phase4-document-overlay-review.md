# Phase 4 XDocumentOverlay — Spec-Driven Code Review Record

> Date: 2026-03-18
> Commits reviewed: working tree diff (uncommitted Phase 4 changes)
> Spec: `phase4-document-overlay-spec.md` v0.1
> Reviewer: Codex (spec-driven review skill)

---

## 1. Review Scope

Reviewed the current working tree implementation of `XDocumentOverlay` / `XOverlayPainter` against the Phase 4 spec.

Focus areas requested:

- `Paint()` integration point and MapMode correctness
- `XGraphics` lifetime safety after callback returns
- overlay handle reentrancy risk when callback calls `add/remove`
- print-path exclusion completeness
- whether the published IDL contract is explicit enough for Python extension callers

Files reviewed:

- `offapi/com/sun/star/text/XDocumentOverlay.idl`
- `offapi/com/sun/star/text/XOverlayPainter.idl`
- `offapi/com/sun/star/text/TextDocumentView.idl`
- `offapi/UnoApi_offapi.mk`
- `sw/source/uibase/inc/unotxvw.hxx`
- `sw/source/uibase/uno/unotxvw.cxx`
- `sw/source/uibase/docvw/edtwin2.cxx`
- `sw/qa/uibase/uno/uno.cxx`

---

## 2. Findings

### Finding #1 — Overlay callback reentrancy can invalidate iteration state

**Severity**: High (correctness / stability)
**Location**: `sw/source/uibase/uno/unotxvw.cxx` — `CallOverlayPainters()`, `addOverlay()`, `removeOverlay()`

`CallOverlayPainters()` iterates `m_aOverlays` directly while invoking foreign UNO callbacks. During that callback, extension code can synchronously reenter `addOverlay()` or `removeOverlay()`, both of which mutate the same `std::vector`.

That creates a concrete invalidation risk:

- `addOverlay()` can reallocate and invalidate the current iteration state
- `removeOverlay()` can erase the currently executing entry or a later one
- outcome can range from skipped painters and unstable ordering to iterator/reference invalidation bugs

This conflicts with spec §4.8, which explicitly treats callback-time mutation as a reentrancy hazard that must not be allowed.

**Why it matters**: once this is published UNO API, Python callers may reasonably try to self-remove or toggle overlay state inside `paintOverlay()`. If the implementation cannot tolerate that, the contract must either forbid it in IDL or internally defend against it.

**Recommended fix**:

- snapshot the painter references and stable metadata before iteration; or
- defer add/remove mutations while paint is in progress; and
- document in IDL that `paintOverlay()` must not call overlay registry mutation methods

**Missing validation**: add a focused test where a painter removes itself or adds another painter during callback.

### Finding #2 — MapMode restoration is not anchored to the preserved document paint state

**Severity**: High (public contract mismatch risk)
**Location**: `sw/source/uibase/docvw/edtwin2.cxx` — overlay hook in `SwEditWin::Paint()`

The new hook does:

- `rRenderContext.Push(vcl::PushFlags::ALL);`
- `rRenderContext.SetMapMode(GetMapMode());`

But this does not actually restore the pre-`DLPostPaint2()` document MapMode required by spec §5.5. It simply reapplies the current window MapMode at that point in the paint pipeline.

Writer already preserves the trusted pre/post-paint MapMode in `SwViewShell::maPrePostMapMode`, and internal drawing code uses that preserved state when needed. That is a strong sign the current integration point should also restore from that preserved value rather than assuming the current MapMode is still the correct document-twips state.

**Why it matters**: Phase 4 hardens the promise that overlay painters receive document-twips coordinates aligned with `XParagraphNavigator.getParagraphBounds()`. If this is wrong, Python callers will learn the wrong coordinate contract from behavior even if the IDL says otherwise.

**Recommended fix**:

- restore from the preserved document paint MapMode (`getPrePostMapMode()` or equivalent), not from `GetMapMode()` at hook time
- keep the push/pop restore around the callback
- add a test that proves coordinates are truly in document twips rather than only checking that a callback happened

**Missing validation**: spec §7.2 requires a core test for “overlay drawing MapMode is document twips”, but current tests only prove callback delivery and visible-area non-emptiness.

### Finding #3 — Published IDL still leaves key handle semantics implicit

**Severity**: Medium (contract clarity / compatibility)
**Location**: `offapi/com/sun/star/text/XDocumentOverlay.idl`, `offapi/com/sun/star/text/XOverlayPainter.idl`

The IDL docs are improved, but several compatibility-significant rules from spec §4.3 / §4.10 are still not explicit in published comments:

- handle is opaque
- handle is only unique within one view lifetime
- handle must not be reused across views
- handle becomes invalid after `removeOverlay()`
- callback must not mutate the overlay registry

For C++ readers these rules may be inferred from implementation and spec, but Python / Basic / Java extension callers will mostly learn the API from generated docs and examples.

**Why it matters**: `long` is a very weak surface type. Without stronger prose, callers may assume the handle is document-global, durable, or safe to cache across controller replacement.

**Recommended fix**:

- extend `XDocumentOverlay.idl` to spell out handle lifetime and scope explicitly
- extend `XOverlayPainter.idl` to state that `paintOverlay()` must not call `addOverlay()`, `removeOverlay()`, `setOverlayVisible()`, or `invalidateOverlay()` on the same view during callback unless reentrancy is deliberately supported

### Finding #4 — Print exclusion is plausible, but not explicitly guarded or validated

**Severity**: Medium (compatibility / validation gap)
**Location**: `sw/source/uibase/docvw/edtwin2.cxx`, `sw/qa/uibase/uno/uno.cxx`

The current hook sits in `SwEditWin::Paint()`, so ordinary print code paths do not obviously flow through it. That makes the implementation directionally correct.

However, spec §4.10 treats “overlay does not appear in print output” as a compatibility boundary, and spec §8 lists print-path leakage as a named risk. The patch currently relies on placement in the screen paint path, but does not add:

- an explicit defensive guard tied to output mode / device type
- a regression test
- a recorded manual validation result

**Why it matters**: this is the kind of behavior that tends to stay correct until a future refactor, then silently regresses because nothing encoded the rule close to the hook.

**Recommended fix**:

- add an explicit guard if there is a stable output-mode predicate available
- or, at minimum, record a manual validation result for print/PDF export and keep that as part of Phase 4 acceptance evidence

---

## 3. Findings Not Reproduced as Defects

### XGraphics lifetime after callback looks defensible in current VCL plumbing

**Status**: No concrete defect found in this review
**Area**: `XGraphics` lifecycle

The `XGraphics` object is created from `OutputDevice::CreateUnoGraphics()`, backed by `VCLXGraphics`, and `OutputDevice` disposal actively detaches outstanding wrappers. That means:

- releasing the callback-local `XGraphics` after return is safe
- retaining the UNO reference beyond callback does not obviously UAF immediately; it degrades to a detached wrapper when the underlying `OutputDevice` dies

This aligns with the intended “valid only for the duration of the call” contract, though the safety currently comes more from wrapper detachment behavior than from direct contract tests.

**Residual risk**:

- there is still no dedicated automated test proving post-callback use is rejected or harmless
- the IDL correctly says callers must not store the reference, so this is more of a documentation/validation gap than an implementation bug

---

## 4. Positive Observations

1. **Integration point is conceptually correct**: the overlay hook is placed after document content and internal overlay painting, which matches the spec’s intended stacking model.

2. **Basic lifecycle API shape matches the spec**: `addOverlay()`, `removeOverlay()`, `invalidateOverlay()`, `setOverlayVisible()`, and `isOverlayVisible()` all exist on `SwXTextView` with expected error handling for invalid handles.

3. **Layer ordering implementation matches the documented contract**: `std::stable_sort()` by layer preserves registration order within the same layer.

4. **Dispose cleanup direction is sane**: overlay registry state is cleared when `SwXTextView` is invalidated, and the implementation does not try to dispose extension-owned painter objects.

5. **IDL already documents two crucial caller constraints**: document-twips coordinate space and “do not store `XGraphics` beyond the call” are both present in the published comments.

---

## 5. Validation Summary

| Phase 4 contract area | Status | Evidence |
|:---|:---:|:---|
| `CurrentController` exposes `XDocumentOverlay` | Pass | `UNO_QUERY_THROW` test exists |
| Basic handle lifecycle (`add/remove`, invalid handle throws) | Pass | CppUnit coverage present |
| Visibility state blocks callback | Pass | `testDocumentOverlayVisibilityBlocksCallback` |
| Layer ordering | Pass | `testDocumentOverlayPaintOrder` |
| Callback receives non-null `XGraphics` / non-empty `VisibleArea` | Pass | `testDocumentOverlayCallbackCalled` |
| MapMode is guaranteed document twips | Fail / Unproven | Hook does not restore from preserved pre-post MapMode; no semantic test |
| Reentrancy safety during callback | Fail | Direct vector iteration over mutable registry |
| Overlay excluded from print output | Partial | Integration point suggests yes, but no explicit guard or validation evidence |
| IDL sufficient for Python caller inference | Partial | Core geometry/lifetime notes present, but handle scope and reentrancy rules still implicit |
| View dispose cleanup | Pass (basic) | close document test exists |

---

## 6. Build & Test Verification

No build or test commands were run for this review pass.

This record is based on:

- spec inspection
- working tree diff inspection
- source-path review of relevant VCL / Writer paint and `XGraphics` plumbing

