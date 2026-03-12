/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <toolkit/awt/vclxwindow.hxx>
#include <cppuhelper/implbase1.hxx>
#include <comphelper/uno3.hxx>
#include <com/sun/star/awt/XCustomPaintWindow.hpp>
#include <com/sun/star/awt/XCustomPaintHandler.hpp>
#include <vcl/window.hxx>

namespace toolkit
{


    /** VCL Window subclass that delegates Paint() to a UNO handler. */
    class CustomPaintVCLWindow final : public vcl::Window
    {
    private:
        css::uno::Reference< css::awt::XCustomPaintHandler > mxHandler;
        tools::Long mnScrollOffsetX;
        tools::Long mnScrollOffsetY;

    public:
        CustomPaintVCLWindow( vcl::Window* pParent, WinBits nStyle );
        virtual ~CustomPaintVCLWindow() override;
        virtual void dispose() override;

        void SetPaintHandler( const css::uno::Reference< css::awt::XCustomPaintHandler >& xHandler );
        css::uno::Reference< css::awt::XCustomPaintHandler > GetPaintHandler() const { return mxHandler; }

        void SetScrollOffset( tools::Long nX, tools::Long nY );
        tools::Long GetScrollOffsetX() const { return mnScrollOffsetX; }
        tools::Long GetScrollOffsetY() const { return mnScrollOffsetY; }

        virtual void Paint( vcl::RenderContext& rRenderContext,
                            const tools::Rectangle& rRect ) override;
    };


    //= VCLXCustomPaintWindow

    typedef ::cppu::ImplHelper1 <   css::awt::XCustomPaintWindow
                                >   VCLXCustomPaintWindow_Base;

    class VCLXCustomPaintWindow final : public VCLXWindow
                                       ,public VCLXCustomPaintWindow_Base
    {
    public:
        VCLXCustomPaintWindow();

    private:
        virtual ~VCLXCustomPaintWindow() override;

        // XInterface
        DECLARE_XINTERFACE()

        // XTypeProvider
        DECLARE_XTYPEPROVIDER()

        // XCustomPaintWindow
        virtual void SAL_CALL setPaintHandler( const css::uno::Reference< css::awt::XCustomPaintHandler >& xHandler ) override;
        virtual css::uno::Reference< css::awt::XCustomPaintHandler > SAL_CALL getPaintHandler() override;
        virtual void SAL_CALL repaint() override;
        virtual void SAL_CALL repaintRect( const css::awt::Rectangle& Rect ) override;
        virtual void SAL_CALL setScrollOffset( sal_Int32 nX, sal_Int32 nY ) override;
        virtual sal_Int32 SAL_CALL getScrollOffsetX() override;
        virtual sal_Int32 SAL_CALL getScrollOffsetY() override;

        VCLXCustomPaintWindow( const VCLXCustomPaintWindow& ) = delete;
        VCLXCustomPaintWindow& operator=( const VCLXCustomPaintWindow& ) = delete;
    };


} // namespace toolkit


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
