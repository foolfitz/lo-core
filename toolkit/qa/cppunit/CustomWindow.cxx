/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <type_traits>

#include <cppuhelper/implbase.hxx>
#include <rtl/ref.hxx>
#include <test/bootstrapfixture.hxx>

#include <com/sun/star/awt/PaintEvent.hpp>
#include <com/sun/star/awt/TextLayoutMetrics.hpp>
#include <com/sun/star/awt/TextMetrics.hpp>
#include <com/sun/star/awt/VclWindowPeerAttribute.hpp>
#include <com/sun/star/awt/WindowAttribute.hpp>
#include <com/sun/star/awt/WindowClass.hpp>
#include <com/sun/star/awt/WindowDescriptor.hpp>
#include <com/sun/star/awt/XCustomPaintHandler.hpp>
#include <com/sun/star/awt/XCustomPaintWindow.hpp>
#include <com/sun/star/awt/XGraphics3.hpp>
#include <com/sun/star/awt/XPaintListener.hpp>
#include <com/sun/star/awt/XSplitterWindow.hpp>
#include <com/sun/star/awt/XToolkit.hpp>
#include <com/sun/star/awt/XWindow.hpp>
#include <com/sun/star/awt/XWindowPeer.hpp>
#include <com/sun/star/lang/EventObject.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>

#include <toolkit/awt/vclxdevice.hxx>
#include <vcl/scheduler.hxx>
#include <vcl/svapp.hxx>
#include <vcl/virdev.hxx>

using namespace css;

namespace
{
uno::Reference<awt::XWindowPeer>
createFloatingWindow( const uno::Reference< lang::XMultiServiceFactory >& xMSF,
                      sal_Int32 nX, sal_Int32 nY, sal_Int32 nWidth, sal_Int32 nHeight )
{
    uno::Reference< awt::XToolkit > xToolkit(
        xMSF->createInstance( u"com.sun.star.awt.Toolkit"_ustr ), uno::UNO_QUERY_THROW );

    awt::WindowDescriptor aDescriptor;
    aDescriptor.Type = awt::WindowClass_TOP;
    aDescriptor.WindowServiceName = u"modelessdialog"_ustr;
    aDescriptor.ParentIndex = -1;
    aDescriptor.Bounds.X = nX;
    aDescriptor.Bounds.Y = nY;
    aDescriptor.Bounds.Width = nWidth;
    aDescriptor.Bounds.Height = nHeight;
    aDescriptor.WindowAttributes
        = awt::WindowAttribute::BORDER + awt::WindowAttribute::MOVEABLE
          + awt::WindowAttribute::SIZEABLE + awt::WindowAttribute::CLOSEABLE
          + awt::VclWindowPeerAttribute::CLIPCHILDREN;

    return xToolkit->createWindow( aDescriptor );
}

uno::Reference< awt::XWindowPeer >
createChildWindow( const uno::Reference< lang::XMultiServiceFactory >& xMSF,
                   const uno::Reference< awt::XWindowPeer >& xParent,
                   const OUString& rServiceName, sal_Int32 nX, sal_Int32 nY,
                   sal_Int32 nWidth, sal_Int32 nHeight )
{
    uno::Reference< awt::XToolkit > xToolkit(
        xMSF->createInstance( u"com.sun.star.awt.Toolkit"_ustr ), uno::UNO_QUERY_THROW );

    awt::WindowDescriptor aDescriptor;
    aDescriptor.Type = awt::WindowClass_SIMPLE;
    aDescriptor.WindowServiceName = rServiceName;
    aDescriptor.Parent = xParent;
    aDescriptor.ParentIndex = -1;
    aDescriptor.Bounds.X = nX;
    aDescriptor.Bounds.Y = nY;
    aDescriptor.Bounds.Width = nWidth;
    aDescriptor.Bounds.Height = nHeight;
    aDescriptor.WindowAttributes = awt::VclWindowPeerAttribute::CLIPCHILDREN;

    return xToolkit->createWindow( aDescriptor );
}

template< typename Predicate >
void processEventsUntil( Predicate&& rPredicate )
{
    for ( int i = 0; i < 20 && !rPredicate(); ++i )
    {
        Scheduler::ProcessEventsToIdle();
        Application::Yield();
    }
}

class PaintProbe final : public cppu::WeakImplHelper< awt::XCustomPaintHandler, awt::XPaintListener >
{
public:
    sal_Int32 mnHandlerCalls = 0;
    sal_Int32 mnPaintListenerCalls = 0;
    sal_Int32 mnMeasuredWidth = -1;
    sal_uInt16 mnMeasuredLines = 0;

    void SAL_CALL paint( const uno::Reference< awt::XGraphics3 >& xGraphics,
                         const awt::Rectangle& /*rUpdateArea*/ ) override
    {
        ++mnHandlerCalls;
        CPPUNIT_ASSERT( xGraphics.is() );

        awt::Rectangle aRect;
        aRect.Width = 80;
        aRect.Height = 40;

        awt::TextMetrics aMetrics = xGraphics->measureText( u"LibreOffice toolkit test"_ustr, 40 );
        mnMeasuredWidth = aMetrics.MaxLineWidth;
        mnMeasuredLines = aMetrics.LineCount;
        xGraphics->drawTextInRect( aRect, u"paint"_ustr, 0x1000 | 0x2000 );
    }

    void SAL_CALL windowPaint( const awt::PaintEvent& /*rEvent*/ ) override
    {
        ++mnPaintListenerCalls;
    }

    void SAL_CALL disposing( const lang::EventObject& /*rEvent*/ ) override {}
};

class ToolkitCustomWindowTest : public test::BootstrapFixture
{
public:
    CPPUNIT_TEST_SUITE(ToolkitCustomWindowTest);
    CPPUNIT_TEST(testCustomPaintWindowPaintNotifications);
    CPPUNIT_TEST(testMeasureTextInRectOnVirtualDevice);
    CPPUNIT_TEST(testSplitterWindowBasicState);
    CPPUNIT_TEST(testTextMetricsLineCountType);
    CPPUNIT_TEST_SUITE_END();

    void testCustomPaintWindowPaintNotifications();
    void testMeasureTextInRectOnVirtualDevice();
    void testSplitterWindowBasicState();
    void testTextMetricsLineCountType();
};

void ToolkitCustomWindowTest::testCustomPaintWindowPaintNotifications()
{
    uno::Reference< awt::XWindowPeer > xParentPeer
        = createFloatingWindow( getMultiServiceFactory(), 50, 50, 240, 160 );
    uno::Reference< awt::XWindow > xParentWindow( xParentPeer, uno::UNO_QUERY_THROW );
    xParentWindow->setVisible( true );

    uno::Reference< awt::XWindowPeer > xChildPeer
        = createChildWindow( getMultiServiceFactory(), xParentPeer, u"custompaintwindow"_ustr,
                             10, 10, 180, 90 );
    uno::Reference< awt::XWindow > xChildWindow( xChildPeer, uno::UNO_QUERY_THROW );
    uno::Reference< awt::XCustomPaintWindow > xCustomPaintWindow( xChildPeer, uno::UNO_QUERY_THROW );

    rtl::Reference< PaintProbe > xProbe = new PaintProbe;
    xChildWindow->addPaintListener( uno::Reference< awt::XPaintListener >( xProbe.get() ) );
    xCustomPaintWindow->setPaintHandler(
        uno::Reference< awt::XCustomPaintHandler >( xProbe.get() ) );
    xChildWindow->setVisible( true );

    xCustomPaintWindow->repaint();
    processEventsUntil( [&xProbe]() {
        return xProbe->mnHandlerCalls > 0 && xProbe->mnPaintListenerCalls > 0;
    } );

    CPPUNIT_ASSERT( xProbe->mnHandlerCalls > 0 );
    CPPUNIT_ASSERT( xProbe->mnPaintListenerCalls > 0 );
    CPPUNIT_ASSERT( xProbe->mnMeasuredWidth >= 0 );
    CPPUNIT_ASSERT( xProbe->mnMeasuredLines >= 1 );

    uno::Reference< lang::XComponent >( xChildPeer, uno::UNO_QUERY_THROW )->dispose();
    uno::Reference< lang::XComponent >( xParentPeer, uno::UNO_QUERY_THROW )->dispose();
}

void ToolkitCustomWindowTest::testMeasureTextInRectOnVirtualDevice()
{
    ScopedVclPtrInstance<VirtualDevice> xDevice;
    xDevice->SetOutputSizePixel( Size( 200, 200 ) );

    rtl::Reference< VCLXDevice > xDeviceWrapper = new VCLXDevice;
    xDeviceWrapper->SetOutputDevice( xDevice );

    uno::Reference< awt::XGraphics3 > xGraphics(
        xDeviceWrapper->createGraphics(), uno::UNO_QUERY_THROW );
    CPPUNIT_ASSERT( xGraphics.is() );

    awt::Rectangle aRect;
    aRect.Width = 40;
    aRect.Height = 200;

    awt::TextMetrics aMetrics = xGraphics->measureText( u"LibreOffice toolkit test"_ustr, 40 );
    awt::TextLayoutMetrics aLayoutMetrics = xGraphics->measureTextInRect(
        aRect, u"LibreOffice toolkit test"_ustr, 0x1000 | 0x2000 );

    CPPUNIT_ASSERT( aLayoutMetrics.Height > 0 );
    CPPUNIT_ASSERT( aLayoutMetrics.LineCount > 1 );
    CPPUNIT_ASSERT_EQUAL( aLayoutMetrics.Width, aLayoutMetrics.UsedRect.Width );
    CPPUNIT_ASSERT_EQUAL( aLayoutMetrics.Height, aLayoutMetrics.UsedRect.Height );
    CPPUNIT_ASSERT_EQUAL( aMetrics.LineCount, aLayoutMetrics.LineCount );
    CPPUNIT_ASSERT_EQUAL( aMetrics.MaxLineWidth, aLayoutMetrics.MaxLineWidth );
}

void ToolkitCustomWindowTest::testSplitterWindowBasicState()
{
    uno::Reference< awt::XWindowPeer > xParentPeer
        = createFloatingWindow( getMultiServiceFactory(), 80, 80, 240, 160 );
    uno::Reference< awt::XWindow > xParentWindow( xParentPeer, uno::UNO_QUERY_THROW );
    xParentWindow->setVisible( true );

    uno::Reference< awt::XWindowPeer > xChildPeer
        = createChildWindow( getMultiServiceFactory(), xParentPeer, u"splitter"_ustr,
                             10, 10, 120, 12 );
    uno::Reference< awt::XWindow > xChildWindow( xChildPeer, uno::UNO_QUERY_THROW );
    uno::Reference< awt::XSplitterWindow > xSplitter( xChildPeer, uno::UNO_QUERY_THROW );

    xChildWindow->setVisible( true );

    xSplitter->setHorizontal( true );
    CPPUNIT_ASSERT( xSplitter->getHorizontal() );

    xSplitter->setSplitPosition( 42 );
    CPPUNIT_ASSERT_EQUAL( sal_Int32( 42 ), xSplitter->getSplitPosition() );

    xSplitter->setRange( 10, 80 );
    xSplitter->setHorizontal( false );
    CPPUNIT_ASSERT( !xSplitter->getHorizontal() );
    xSplitter->setRange( 5, 60 );

    uno::Reference< lang::XComponent >( xChildPeer, uno::UNO_QUERY_THROW )->dispose();
    uno::Reference< lang::XComponent >( xParentPeer, uno::UNO_QUERY_THROW )->dispose();
}

void ToolkitCustomWindowTest::testTextMetricsLineCountType()
{
    static_assert( std::is_same_v< decltype( awt::TextMetrics{}.LineCount ), sal_uInt16 > );
    static_assert( std::is_same_v< decltype( awt::TextLayoutMetrics{}.LineCount ), sal_uInt16 > );

    awt::TextMetrics aMetrics;
    aMetrics.LineCount = 40000;
    CPPUNIT_ASSERT_EQUAL( sal_uInt16( 40000 ), aMetrics.LineCount );

    awt::TextLayoutMetrics aLayoutMetrics;
    aLayoutMetrics.LineCount = 40000;
    CPPUNIT_ASSERT_EQUAL( sal_uInt16( 40000 ), aLayoutMetrics.LineCount );
}

CPPUNIT_TEST_SUITE_REGISTRATION(ToolkitCustomWindowTest);
} // namespace

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
