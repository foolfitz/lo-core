/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <awt/vclxcustompaintwindow.hxx>
#include <awt/vclxgraphics.hxx>
#include <com/sun/star/awt/Rectangle.hpp>

#include <vcl/svapp.hxx>
#include <vcl/outdev.hxx>

namespace toolkit
{


    using namespace ::com::sun::star::uno;
    using namespace ::com::sun::star::awt;


    // --- CustomPaintVCLWindow ---

    CustomPaintVCLWindow::CustomPaintVCLWindow( vcl::Window* pParent, WinBits nStyle )
        : vcl::Window( pParent, nStyle | WB_CLIPCHILDREN )
        , mnScrollOffsetX( 0 )
        , mnScrollOffsetY( 0 )
    {
    }


    CustomPaintVCLWindow::~CustomPaintVCLWindow()
    {
        disposeOnce();
    }


    void CustomPaintVCLWindow::dispose()
    {
        mxHandler.clear();
        vcl::Window::dispose();
    }


    void CustomPaintVCLWindow::SetPaintHandler(
        const Reference< XCustomPaintHandler >& xHandler )
    {
        mxHandler = xHandler;
    }


    void CustomPaintVCLWindow::SetScrollOffset( tools::Long nX, tools::Long nY )
    {
        mnScrollOffsetX = nX;
        mnScrollOffsetY = nY;
    }


    void CustomPaintVCLWindow::Paint(
        vcl::RenderContext& rRenderContext,
        const tools::Rectangle& rRect )
    {
        // Do NOT call base Window::Paint() — our handler IS the paint
        // implementation. The base would fire WindowPaint event which
        // we don't need.
        if ( !mxHandler.is() )
            return;

        // Apply scroll offset via MapMode
        rRenderContext.Push( vcl::PushFlags::MAPMODE );
        MapMode aMapMode = rRenderContext.GetMapMode();
        Point aOrigin = aMapMode.GetOrigin();
        aMapMode.SetOrigin( Point(
            aOrigin.X() - mnScrollOffsetX,
            aOrigin.Y() - mnScrollOffsetY ) );
        rRenderContext.SetMapMode( aMapMode );

        // Create a temporary VCLXGraphics for the callback
        rtl::Reference< VCLXGraphics > pGraphics = new VCLXGraphics;
        pGraphics->Init( &rRenderContext );

        // Convert rect to content coordinates (add scroll offset)
        css::awt::Rectangle aUnoRect;
        aUnoRect.X = rRect.Left() + mnScrollOffsetX;
        aUnoRect.Y = rRect.Top() + mnScrollOffsetY;
        aUnoRect.Width = rRect.GetWidth();
        aUnoRect.Height = rRect.GetHeight();

        try
        {
            mxHandler->paint( pGraphics, aUnoRect );
        }
        catch ( const css::uno::Exception& )
        {
            // Swallow exceptions from the handler to prevent
            // crashing the VCL paint loop
        }

        // Disconnect the graphics from the OutputDevice
        pGraphics->SetOutputDevice( nullptr );

        rRenderContext.Pop();
    }


    // --- VCLXCustomPaintWindow ---

    VCLXCustomPaintWindow::VCLXCustomPaintWindow()
    {
    }


    VCLXCustomPaintWindow::~VCLXCustomPaintWindow()
    {
    }


    IMPLEMENT_FORWARD_XINTERFACE2( VCLXCustomPaintWindow, VCLXWindow, VCLXCustomPaintWindow_Base )


    IMPLEMENT_FORWARD_XTYPEPROVIDER2( VCLXCustomPaintWindow, VCLXWindow, VCLXCustomPaintWindow_Base )


    void SAL_CALL VCLXCustomPaintWindow::setPaintHandler(
        const Reference< XCustomPaintHandler >& xHandler )
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        if ( pWin )
            pWin->SetPaintHandler( xHandler );
    }


    Reference< XCustomPaintHandler > SAL_CALL VCLXCustomPaintWindow::getPaintHandler()
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        return pWin ? pWin->GetPaintHandler() : Reference< XCustomPaintHandler >();
    }


    void SAL_CALL VCLXCustomPaintWindow::repaint()
    {
        SolarMutexGuard aGuard;
        VclPtr< vcl::Window > pWin = GetWindow();
        if ( pWin )
            pWin->Invalidate();
    }


    void SAL_CALL VCLXCustomPaintWindow::repaintRect( const css::awt::Rectangle& Rect )
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        if ( pWin )
        {
            // Convert content coordinates to window coordinates
            tools::Rectangle aRect(
                Point( Rect.X - pWin->GetScrollOffsetX(),
                       Rect.Y - pWin->GetScrollOffsetY() ),
                Size( Rect.Width, Rect.Height ) );
            pWin->Invalidate( aRect );
        }
    }


    void SAL_CALL VCLXCustomPaintWindow::setScrollOffset( sal_Int32 nX, sal_Int32 nY )
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        if ( pWin )
        {
            pWin->SetScrollOffset( nX, nY );
            pWin->Invalidate(); // repaint with new offset
        }
    }


    sal_Int32 SAL_CALL VCLXCustomPaintWindow::getScrollOffsetX()
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        return pWin ? pWin->GetScrollOffsetX() : 0;
    }


    sal_Int32 SAL_CALL VCLXCustomPaintWindow::getScrollOffsetY()
    {
        SolarMutexGuard aGuard;
        CustomPaintVCLWindow* pWin
            = dynamic_cast< CustomPaintVCLWindow* >( GetWindow() );
        return pWin ? pWin->GetScrollOffsetY() : 0;
    }


} // namespace toolkit


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
