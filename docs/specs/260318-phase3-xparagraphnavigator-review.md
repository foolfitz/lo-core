# Phase 3 XParagraphNavigator — Spec-Driven Code Review Record

> Date: 2026-03-18
> Commits reviewed: `d5892a688105`, `bd9f422e2c8f`
> Spec: `phase3-paragraph-navigator-spec.md` v0.2
> Reviewer: Claude (spec-driven review skill)

---

## 1. Review Scope

Reviewed two commits implementing `XParagraphNavigator` published UNO interface against Phase 3 spec:

- **d5892a688105**: Initial IDL, bridge implementation in `SwXTextView`, and CppUnit tests
- **bd9f422e2c8f**: Tighten IDL docs, fix `getParagraphStyleName` to return programmatic names, expand test coverage

Files reviewed:
- `offapi/com/sun/star/text/XParagraphNavigator.idl`
- `offapi/com/sun/star/text/TextDocumentView.idl`
- `offapi/UnoApi_offapi.mk`
- `sw/source/uibase/inc/unotxvw.hxx`
- `sw/source/uibase/uno/unotxvw.cxx`
- `sw/qa/uibase/uno/uno.cxx`

---

## 2. Findings and Fixes Applied

### Finding #1 — Dead code in `lcl_GotoParagraphNavigatorNode` [Fixed]

**Severity**: Low (code quality)
**Location**: `sw/source/uibase/uno/unotxvw.cxx` — `lcl_GotoParagraphNavigatorNode`

`EnterStdMode()` clears the shell cursor's selection before the `HasMark()` check. The `if (pShellCursor->HasMark())` branch was unreachable dead code.

**Fix**: Removed the dead branch; directly use `rSh.GetCursor()->GetPoint()` as selection anchor with an explanatory comment.

### Finding #2 — `selectCurrentParagraph()` out-of-scope behavior undocumented [Fixed]

**Severity**: Medium (contract gap)
**Location**: `offapi/com/sun/star/text/XParagraphNavigator.idl`

The implementation silently returns when the view cursor is outside the supported scope, but the IDL doc didn't mention this behavior. Other methods (`getCurrentIndex`, `gotoNext`, `gotoPrevious`) all explicitly document their out-of-scope behavior.

**Fix**: Added `<p>If the current view cursor is outside the supported paragraph scope, this method has no effect.</p>` to the IDL doc.

### Finding #7 — `getCurrentIndex()` missing 0-based documentation [Fixed]

**Severity**: Low (documentation)
**Location**: `offapi/com/sun/star/text/XParagraphNavigator.idl`

Spec §4.3 requires "IDL 必須明寫 0 起算". The `getCurrentIndex()` doc only mentioned the `-1` sentinel but didn't state the index is zero-based or its valid range.

**Fix**: Changed doc to: "returns the zero-based current paragraph index, in the range `0 .. getCount()-1`, or `-1` if ..."

### Finding #6 — Negative index tests missing [Fixed]

**Severity**: Low (validation gap)
**Location**: `sw/qa/uibase/uno/uno.cxx` — `testParagraphNavigatorInvalidIndex`

Only `gotoIndex(-1)` was tested for negative index. Other throwing methods (`getParagraphBounds`, `getParagraphViewBounds`, `isVisible`, `getParagraphText`, `getParagraphStyleName`) only had positive out-of-range tests.

**Fix**: Added `-1` negative index assertions for all six throwing methods.

---

## 3. Findings Not Fixed (Noted for Future)

### Finding #3 — Missing `isVisible()` test for scrolled-out-of-view paragraph

**Severity**: Low-Medium (validation gap)

`isVisible()` checks `Overlaps(VisArea)` but the only test covers hidden paragraphs. A paragraph with layout frames that is scrolled off-screen should also return `false`, but this path has no automated test. May be unstable in headless svp — consider manual verification or a carefully constructed test in a follow-up.

### Finding #4 — `getParagraphBounds`/`getParagraphViewBounds` no shared snapshot

**Severity**: Low (design note)

Each method independently calls `lcl_GetParagraphNavigatorFragments`. In practice this is safe because both are SolarMutex-guarded and the spec declares snapshot semantics. No change needed now; if a combined query becomes useful for Phase 4, a helper can be added then.

### Finding #5 — O(n) linear scan on every call

**Severity**: Low (performance note)

All index mapping helpers do full node array scans. A single `gotoNext()` triggers two full scans. Acceptable for initial implementation under snapshot semantics. Consider caching or building an index vector if performance becomes a concern with large documents.

---

## 4. Positive Observations

1. **SolarMutexGuard + null check consistent**: All 11 public methods follow the same `SolarMutexGuard` + `GetView()` null check pattern.

2. **Style name identity correct**: `getParagraphStyleName` uses `SwStyleNameMapper::FillProgName` to return UNO programmatic names, matching `ParaStyleName` property. Test directly compares against the property value.

3. **Multi-fragment contract verified**: Test forces cross-page paragraph by shrinking page height, asserts `>1` fragments, and checks doc/view bounds count match with per-fragment LogicToPixel verification.

4. **Out-of-scope coverage comprehensive**: Tests cover table cell, header, footer, footnote, and text frame — all returning `getCurrentIndex() == -1` and `gotoNext()/gotoPrevious() == false`.

5. **Hidden paragraph test uses correct mechanism**: Uses `TextField.HiddenParagraph` + `ShowHiddenParagraphs` view setting (not `CharHidden`), which is the proper paragraph-level hiding path.

6. **IDL documentation quality**: Second commit significantly improved published contract documentation for scope, fragment order, empty-sequence semantics, and selection behavior.

---

## 5. Validation Summary

| Spec §7 acceptance criteria | Status | Evidence |
|:---|:---:|:---|
| #1 `CurrentController` → `XParagraphNavigator` | Pass | `UNO_QUERY_THROW` in all tests |
| #2 `getCount()` matches main body order | Pass | `testParagraphNavigatorMainBodyEnumeration` (table excluded) |
| #3 Navigation faithful to Writer semantics | Pass | `testParagraphNavigatorBasic` (boundary + selection) |
| #4 `getCurrentIndex() == -1` out of scope | Pass | `testParagraphNavigatorCurrentIndexOutOfScope` (5 scope types) |
| #5 `getParagraphBounds()` in document twips | Pass | IDL doc + CppUnit LogicToPixel cross-check |
| #6 `getParagraphViewBounds()` in view client pixel | Pass | `lcl_AssertViewRectMatchesDocRect` helper |
| #7 Multi-fragment not union rect | Pass | `testParagraphNavigatorMultiFragment` asserts `>1` rects |
| #8 Empty-sequence + `isVisible() == false` | Pass | `testParagraphNavigatorHiddenParagraph` |
| #9 Capability probe / fallback | Pending | Extension-side validation (out of LO-core scope) |
| #10 Phase 3/4 boundary clean | Pass | No overlay/highlight contract in IDL |

---

## 6. Build & Test Verification

```
make offapi         — OK
make sw             — OK
CppunitTest_sw_uibase_uno — OK (24/24 tests passed)
```

All ParagraphNavigator tests passed:
- `testParagraphNavigatorBasic`
- `testParagraphNavigatorMainBodyEnumeration`
- `testParagraphNavigatorInvalidIndex`
- `testParagraphNavigatorCurrentIndexOutOfScope`
- `testParagraphNavigatorMultiFragment`
- `testParagraphNavigatorHiddenParagraph`
