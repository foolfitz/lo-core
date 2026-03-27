/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <OverlayExtensionPainter.hxx>
#include <svx/sdr/overlay/overlaymanager.hxx>
#include <vcl/outdev.hxx>
#include <sal/log.hxx>

namespace sw::overlay
{

OverlayExtensionPainter::OverlayExtensionPainter(PaintFunc fnPaint)
    : sdr::overlay::OverlayObject(COL_TRANSPARENT)
    , m_fnPaint(std::move(fnPaint))
{
}

OverlayExtensionPainter::~OverlayExtensionPainter()
{
    if (getOverlayManager())
        getOverlayManager()->remove(*this);
}

void OverlayExtensionPainter::invalidate()
{
    objectChange();
}

drawinglayer::primitive2d::Primitive2DContainer
OverlayExtensionPainter::getOverlayObjectPrimitive2DSequence() const
{
    // Detect zoom/scroll changes: if the output device MapMode has changed
    // since we last created primitives, reset the cache so that
    // createOverlayObjectPrimitive2DSequence() is called again.
    // This handles the case where OverlayManager survives zoom change
    // but our cached primitives are rendered with a stale ViewTransformation.
    if (getOverlayManager())
    {
        const MapMode& rCurMapMode = getOverlayManager()->getOutputDevice().GetMapMode();
        if (rCurMapMode != m_aLastMapMode)
        {
            SAL_INFO("sw.uno", "OverlayExtPainter: MapMode changed, resetting primitive cache");
            m_aLastMapMode = rCurMapMode;
            const_cast<OverlayExtensionPainter*>(this)->resetPrimitive2DSequence();
            const_cast<OverlayExtensionPainter*>(this)->maBaseRange.reset();
        }
    }

    return sdr::overlay::OverlayObject::getOverlayObjectPrimitive2DSequence();
}

drawinglayer::primitive2d::Primitive2DContainer
OverlayExtensionPainter::createOverlayObjectPrimitive2DSequence()
{
    if (!m_fnPaint)
        return drawinglayer::primitive2d::Primitive2DContainer();

    // Compute the visible area from the OverlayManager's output device.
    css::awt::Rectangle aVisibleArea;
    if (getOverlayManager())
    {
        OutputDevice& rDev = getOverlayManager()->getOutputDevice();
        const Size aPixelSize = rDev.GetOutputSizePixel();
        const Point aOrigin = rDev.PixelToLogic(Point(0, 0));
        const Size aLogicSize = rDev.PixelToLogic(aPixelSize);
        aVisibleArea.X = aOrigin.X();
        aVisibleArea.Y = aOrigin.Y();
        aVisibleArea.Width = aLogicSize.Width();
        aVisibleArea.Height = aLogicSize.Height();
    }

    SAL_WARN("sw.uno", "OverlayExtPainter::createPrimitives CALLED"
        " visArea=(" << aVisibleArea.X << "," << aVisibleArea.Y
        << " " << aVisibleArea.Width << "x" << aVisibleArea.Height << ")");

    return m_fnPaint(aVisibleArea);
}

} // namespace sw::overlay

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
