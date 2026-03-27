/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <svx/sdr/overlay/overlayobject.hxx>
#include <drawinglayer/primitive2d/Primitive2DContainer.hxx>
#include <com/sun/star/awt/Rectangle.hpp>
#include <vcl/mapmod.hxx>

#include <functional>

namespace sw::overlay
{
/// Single OverlayObject that bridges extension overlay painters into VCL's
/// OverlayManager system.  The actual painting logic is delegated to a
/// callback (typically a lambda provided by SwXTextView that iterates its
/// sorted, visibility-filtered painter list).
///
/// One instance is shared by all registered extension overlays, so that
/// layer ordering, visibility, and visible-area computation are fully
/// controlled by the SwXTextView side.
///
/// See docs/phase5-study/H2A-overlay-object-bridge-spec.md
class OverlayExtensionPainter final : public sdr::overlay::OverlayObject
{
public:
    /// Callback signature: receives the visible area (document twips)
    /// and the output device for MapMode computation; returns the
    /// Primitive2D container to render.
    using PaintFunc = std::function<drawinglayer::primitive2d::Primitive2DContainer(
        const css::awt::Rectangle& rVisibleArea)>;

    explicit OverlayExtensionPainter(PaintFunc fnPaint);

    virtual ~OverlayExtensionPainter() override;

    /// Public wrapper for objectChange() — clears cached primitives,
    /// triggers re-creation on next paint.
    void invalidate();

    /// Override to detect zoom/MapMode changes and invalidate cached
    /// primitives automatically.  This follows the pattern described
    /// in overlayobject.hxx lines 63-69.
    virtual drawinglayer::primitive2d::Primitive2DContainer
        getOverlayObjectPrimitive2DSequence() const override;

private:
    virtual drawinglayer::primitive2d::Primitive2DContainer
        createOverlayObjectPrimitive2DSequence() override;

    PaintFunc m_fnPaint;
    mutable MapMode m_aLastMapMode;
};

} // namespace sw::overlay

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
