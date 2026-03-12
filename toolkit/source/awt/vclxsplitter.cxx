/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <awt/vclxsplitter.hxx>
#include <com/sun/star/awt/SplitEvent.hpp>

#include <vcl/split.hxx>
#include <vcl/svapp.hxx>

namespace toolkit
{


    using namespace ::com::sun::star::uno;
    using namespace ::com::sun::star::awt;
    using namespace ::com::sun::star::lang;


    VCLXSplitter::VCLXSplitter()
        :maSplitListeners( *this )
        ,mnRangeMin( 0 )
        ,mnRangeMax( 0 )
    {
    }


    VCLXSplitter::~VCLXSplitter()
    {
    }


    IMPLEMENT_FORWARD_XINTERFACE2( VCLXSplitter, VCLXWindow, VCLXSplitter_Base )


    IMPLEMENT_FORWARD_XTYPEPROVIDER2( VCLXSplitter, VCLXWindow, VCLXSplitter_Base )


    void SAL_CALL VCLXSplitter::dispose()
    {
        {
            SolarMutexGuard aGuard;

            EventObject aDisposeEvent;
            aDisposeEvent.Source = *this;
            maSplitListeners.disposeAndClear( aDisposeEvent );
        }

        VCLXWindow::dispose();
    }


    void SAL_CALL VCLXSplitter::addSplitListener( const Reference< XSplitListener >& xListener )
    {
        if ( xListener.is() )
            maSplitListeners.addInterface( xListener );
    }


    void SAL_CALL VCLXSplitter::removeSplitListener( const Reference< XSplitListener >& xListener )
    {
        if ( xListener.is() )
            maSplitListeners.removeInterface( xListener );
    }


    void SAL_CALL VCLXSplitter::setSplitPosition( sal_Int32 nPos )
    {
        SolarMutexGuard aGuard;
        VclPtr< Splitter > pSplitter = GetAs< Splitter >();
        if ( pSplitter )
            pSplitter->SetSplitPosPixel( nPos );
    }


    sal_Int32 SAL_CALL VCLXSplitter::getSplitPosition()
    {
        SolarMutexGuard aGuard;
        VclPtr< Splitter > pSplitter = GetAs< Splitter >();
        return pSplitter ? pSplitter->GetSplitPosPixel() : 0;
    }


    void SAL_CALL VCLXSplitter::setHorizontal( sal_Bool bHorizontal )
    {
        SolarMutexGuard aGuard;
        VclPtr< Splitter > pSplitter = GetAs< Splitter >();
        if ( pSplitter )
            pSplitter->SetHorizontal( bHorizontal );
    }


    sal_Bool SAL_CALL VCLXSplitter::getHorizontal()
    {
        SolarMutexGuard aGuard;
        VclPtr< Splitter > pSplitter = GetAs< Splitter >();
        return pSplitter ? pSplitter->IsHorizontal() : false;
    }


    void SAL_CALL VCLXSplitter::setRange( sal_Int32 nMin, sal_Int32 nMax )
    {
        SolarMutexGuard aGuard;
        mnRangeMin = nMin;
        mnRangeMax = nMax;
        VclPtr< Splitter > pSplitter = GetAs< Splitter >();
        if ( pSplitter )
        {
            vcl::Window* pParent = pSplitter->GetParent();
            if ( pParent )
            {
                Size aParentSize = pParent->GetOutputSizePixel();
                tools::Rectangle aDragRect;
                if ( pSplitter->IsHorizontal() )
                    aDragRect = tools::Rectangle(
                        Point( 0, nMin ),
                        Size( aParentSize.Width(), nMax - nMin ) );
                else
                    aDragRect = tools::Rectangle(
                        Point( nMin, 0 ),
                        Size( nMax - nMin, aParentSize.Height() ) );
                pSplitter->SetDragRectPixel( aDragRect, pParent );
            }
        }
    }


    void VCLXSplitter::SetWindow( const VclPtr< vcl::Window > &pWindow )
    {
        // Disconnect old handlers
        {
            SolarMutexGuard aGuard;
            VclPtr< Splitter > pOldSplitter = GetAs< Splitter >();
            if ( pOldSplitter )
            {
                pOldSplitter->SetStartSplitHdl( Link< Splitter*, void >() );
                pOldSplitter->SetSplitHdl( Link< Splitter*, void >() );
                pOldSplitter->SetEndSplitHdl( Link< Splitter*, void >() );
            }
        }

        VCLXWindow::SetWindow( pWindow );

        // Connect handlers to new window
        {
            SolarMutexGuard aGuard;
            VclPtr< Splitter > pSplitter = GetAs< Splitter >();
            if ( pSplitter )
            {
                pSplitter->SetStartSplitHdl( LINK( this, VCLXSplitter, StartSplitHdl ) );
                pSplitter->SetSplitHdl( LINK( this, VCLXSplitter, SplitHdl ) );
                pSplitter->SetEndSplitHdl( LINK( this, VCLXSplitter, EndSplitHdl ) );
            }
        }
    }


    IMPL_LINK_NOARG( VCLXSplitter, StartSplitHdl, Splitter*, void )
    {
        if ( maSplitListeners.getLength() )
        {
            SplitEvent aEvent;
            aEvent.Source = static_cast< cppu::OWeakObject* >( this );
            VclPtr< Splitter > pSplitter = GetAs< Splitter >();
            aEvent.SplitPos = pSplitter ? pSplitter->GetSplitPosPixel() : 0;
            maSplitListeners.splitStarted( aEvent );
        }
    }


    IMPL_LINK_NOARG( VCLXSplitter, SplitHdl, Splitter*, void )
    {
        if ( maSplitListeners.getLength() )
        {
            SplitEvent aEvent;
            aEvent.Source = static_cast< cppu::OWeakObject* >( this );
            VclPtr< Splitter > pSplitter = GetAs< Splitter >();
            aEvent.SplitPos = pSplitter ? pSplitter->GetSplitPosPixel() : 0;
            maSplitListeners.splitting( aEvent );
        }
    }


    IMPL_LINK_NOARG( VCLXSplitter, EndSplitHdl, Splitter*, void )
    {
        if ( maSplitListeners.getLength() )
        {
            SplitEvent aEvent;
            aEvent.Source = static_cast< cppu::OWeakObject* >( this );
            VclPtr< Splitter > pSplitter = GetAs< Splitter >();
            aEvent.SplitPos = pSplitter ? pSplitter->GetSplitPosPixel() : 0;
            maSplitListeners.splitEnded( aEvent );
        }
    }


} // namespace toolkit


/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
