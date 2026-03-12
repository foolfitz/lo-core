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
#include <toolkit/helper/listenermultiplexer.hxx>
#include <cppuhelper/implbase1.hxx>
#include <comphelper/uno3.hxx>
#include <com/sun/star/awt/XSplitterWindow.hpp>

class Splitter;

namespace toolkit
{


    //= VCLXSplitter

    typedef ::cppu::ImplHelper1 <   css::awt::XSplitterWindow
                                >   VCLXSplitter_Base;

    class VCLXSplitter final : public VCLXWindow
                              ,public VCLXSplitter_Base
    {
    private:
        SplitListenerMultiplexer maSplitListeners;
        tools::Long mnRangeMin;
        tools::Long mnRangeMax;

    public:
        VCLXSplitter();

    private:
        virtual ~VCLXSplitter() override;

        // XInterface
        DECLARE_XINTERFACE()

        // XTypeProvider
        DECLARE_XTYPEPROVIDER()

        // XComponent
        void SAL_CALL dispose() override;

        // XSplitterWindow
        virtual void SAL_CALL addSplitListener( const css::uno::Reference< css::awt::XSplitListener >& xListener ) override;
        virtual void SAL_CALL removeSplitListener( const css::uno::Reference< css::awt::XSplitListener >& xListener ) override;
        virtual void SAL_CALL setSplitPosition( sal_Int32 nPos ) override;
        virtual sal_Int32 SAL_CALL getSplitPosition() override;
        virtual void SAL_CALL setHorizontal( sal_Bool bHorizontal ) override;
        virtual sal_Bool SAL_CALL getHorizontal() override;
        virtual void SAL_CALL setRange( sal_Int32 nMin, sal_Int32 nMax ) override;

        // VCLXWindow
        void SetWindow( const VclPtr< vcl::Window > &pWindow ) override;

        DECL_LINK( StartSplitHdl, Splitter*, void );
        DECL_LINK( SplitHdl, Splitter*, void );
        DECL_LINK( EndSplitHdl, Splitter*, void );

        VCLXSplitter( const VCLXSplitter& ) = delete;
        VCLXSplitter& operator=( const VCLXSplitter& ) = delete;
    };


} // namespace toolkit


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
