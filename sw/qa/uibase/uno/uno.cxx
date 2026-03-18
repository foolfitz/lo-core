/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <swmodeltestbase.hxx>

#include <cstdlib>

#include <boost/property_tree/json_parser.hpp>

#include <com/sun/star/frame/XModel2.hpp>
#include <com/sun/star/lang/IndexOutOfBoundsException.hpp>
#include <com/sun/star/text/ControlCharacter.hpp>
#include <com/sun/star/text/XDocumentOverlay.hpp>
#include <com/sun/star/text/XOverlayPainter.hpp>
#include <com/sun/star/text/XParagraphNavigator.hpp>
#include <com/sun/star/text/XTextContent.hpp>
#include <com/sun/star/text/XTextTable.hpp>
#include <com/sun/star/text/XTextViewCursorSupplier.hpp>
#include <com/sun/star/text/XTextViewTextRangeSupplier.hpp>
#include <com/sun/star/util/XCloseable.hpp>
#include <com/sun/star/text/XTextDocument.hpp>
#include <com/sun/star/beans/PropertyAttribute.hpp>
#include <com/sun/star/view/XViewSettingsSupplier.hpp>

#include <toolkit/helper/vclunohelper.hxx>
#include <vcl/scheduler.hxx>
#include <vcl/mapmod.hxx>
#include <vcl/outdev.hxx>
#include <tools/json_writer.hxx>
#include <comphelper/propertyvalue.hxx>
#include <cppuhelper/implbase.hxx>
#include <xmloff/odffields.hxx>

#include <docsh.hxx>
#include <edtwin.hxx>
#include <unotextrange.hxx>
#include <view.hxx>
#include <wrtsh.hxx>
#include <rootfrm.hxx>
#include <sortedobjs.hxx>
#include <anchoredobject.hxx>
#include <frameformats.hxx>
#include <fmtanchr.hxx>
#include <unoprnms.hxx>
#include <unotxdoc.hxx>

namespace
{
uno::Reference<text::XParagraphNavigator>
lcl_GetParagraphNavigator(const uno::Reference<frame::XModel>& xModel)
{
    return uno::Reference<text::XParagraphNavigator>(xModel->getCurrentController(),
                                                     uno::UNO_QUERY_THROW);
}

uno::Reference<text::XDocumentOverlay>
lcl_GetDocumentOverlay(const uno::Reference<frame::XModel>& xModel)
{
    return uno::Reference<text::XDocumentOverlay>(xModel->getCurrentController(),
                                                  uno::UNO_QUERY_THROW);
}

class MockOverlayPainter : public cppu::WeakImplHelper<css::text::XOverlayPainter>
{
public:
    sal_Int32 m_nPaintCount = 0;
    bool m_bReceivedGraphics = false;
    bool m_bCapturedMapMode = false;
    css::awt::Rectangle m_aLastVisibleArea{};
    MapMode m_aLastMapMode;
    OutDevType m_eLastOutDevType = OUTDEV_WINDOW;
    sal_Int32 m_nId = 0;
    static std::vector<sal_Int32>* s_pGlobalOrder;

    MockOverlayPainter(sal_Int32 nId = 0)
        : m_nId(nId)
    {
    }

    void SAL_CALL paintOverlay(const css::uno::Reference<css::awt::XGraphics>& xGraphics,
                               const css::awt::Rectangle& rVisibleArea) override
    {
        ++m_nPaintCount;
        m_bReceivedGraphics = xGraphics.is();
        m_aLastVisibleArea = rVisibleArea;
        if (OutputDevice* pOutDev = VCLUnoHelper::GetOutputDevice(xGraphics))
        {
            m_bCapturedMapMode = true;
            m_aLastMapMode = pOutDev->GetMapMode();
            m_eLastOutDevType = pOutDev->GetOutDevType();
        }
        if (s_pGlobalOrder)
            s_pGlobalOrder->push_back(m_nId);
    }
};
std::vector<sal_Int32>* MockOverlayPainter::s_pGlobalOrder = nullptr;

class ReentrantOverlayPainter : public cppu::WeakImplHelper<css::text::XOverlayPainter>
{
public:
    css::uno::Reference<css::text::XDocumentOverlay> m_xOverlay;
    css::uno::Reference<css::text::XOverlayPainter> m_xPainterToAdd;
    sal_Int32 m_nRemoveHandle = 0;
    sal_Int32 m_nAddedHandle = 0;
    sal_Int32 m_nPaintCount = 0;
    bool m_bDidReenter = false;

    explicit ReentrantOverlayPainter(
        const css::uno::Reference<css::text::XDocumentOverlay>& xOverlay)
        : m_xOverlay(xOverlay)
    {
    }

    void SAL_CALL paintOverlay(const css::uno::Reference<css::awt::XGraphics>&,
                               const css::awt::Rectangle&) override
    {
        ++m_nPaintCount;
        if (m_bDidReenter)
            return;

        m_bDidReenter = true;
        if (m_nRemoveHandle > 0)
            m_xOverlay->removeOverlay(m_nRemoveHandle);
        if (m_xPainterToAdd.is())
            m_nAddedHandle = m_xOverlay->addOverlay(m_xPainterToAdd, 0);
    }
};

uno::Reference<text::XTextViewCursor>
lcl_GetViewCursor(const uno::Reference<frame::XModel>& xModel)
{
    uno::Reference<text::XTextViewCursorSupplier> xSupplier(xModel->getCurrentController(),
                                                            uno::UNO_QUERY_THROW);
    return xSupplier->getViewCursor();
}

void lcl_AppendParagraph(const uno::Reference<text::XText>& xText,
                         const uno::Reference<text::XTextCursor>& xCursor,
                         const OUString& rText, bool bAppendBreak = true)
{
    xText->insertString(xCursor, rText, false);
    if (bAppendBreak)
    {
        xText->insertControlCharacter(xCursor, text::ControlCharacter::PARAGRAPH_BREAK, false);
    }
}

OUString lcl_CreateLongParagraph()
{
    OUStringBuffer aBuffer;
    for (int i = 0; i < 1800; ++i)
    {
        aBuffer.append(u"fragment "_ustr);
    }
    return aBuffer.makeStringAndClear();
}

void lcl_AssertViewRectMatchesDocRect(SwEditWin& rEditWin, const awt::Rectangle& rDocRect,
                                      const awt::Rectangle& rViewRect)
{
    Point aTopLeft = rEditWin.LogicToPixel(Point(rDocRect.X, rDocRect.Y));
    Size aPixelSize = rEditWin.LogicToPixel(Size(rDocRect.Width, rDocRect.Height));
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(aTopLeft.getX()), rViewRect.X);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(aTopLeft.getY()), rViewRect.Y);
    CPPUNIT_ASSERT(std::abs(static_cast<sal_Int32>(aPixelSize.Width()) - rViewRect.Width) <= 1);
    CPPUNIT_ASSERT(std::abs(static_cast<sal_Int32>(aPixelSize.Height()) - rViewRect.Height) <= 1);
}

void lcl_AssertParagraphNavigatorOutOfScope(
    const uno::Reference<text::XParagraphNavigator>& xParagraphNavigator,
    const uno::Reference<text::XTextViewCursor>& xViewCursor,
    const uno::Reference<text::XTextRange>& xRange)
{
    xViewCursor->gotoRange(xRange, false);
    Scheduler::ProcessEventsToIdle();

    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(-1), xParagraphNavigator->getCurrentIndex());
    CPPUNIT_ASSERT(!xParagraphNavigator->gotoNext(false));
    CPPUNIT_ASSERT(!xParagraphNavigator->gotoPrevious(false));
}

}

/// Covers sw/source/uibase/uno/ fixes.
class SwUibaseUnoTest : public SwModelTestBase
{
public:
    SwUibaseUnoTest()
        : SwModelTestBase(u"/sw/qa/uibase/uno/data/"_ustr)
    {
    }
};

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testLockControllers)
{
    createSwDoc();
    {
        uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
        xModel->lockControllers();
    }
    {
        uno::Reference<util::XCloseable> xCloseable(mxComponent, uno::UNO_QUERY_THROW);
        xCloseable->close(false);
    }
    // Without the accompanying fix in place, this test would have crashed.
    mxComponent.clear();
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testCondFieldCachedValue)
{
    createSwDoc("cond-field-cached-value.docx");
    Scheduler::ProcessEventsToIdle();

    // Without the accompanying fix in place, this test would have failed with:
    // - Expected: 1
    // - Actual  :
    // i.e. the conditional field lost its cached content.
    getParagraph(2, u"1"_ustr);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testCreateTextRangeByPixelPosition)
{
    // Given a document with 2 characters, and the pixel position of the point between them:
    createSwDoc();
    SwDoc* pDoc = getSwDoc();
    SwDocShell* pDocShell = getSwDocShell();
    SwWrtShell* pWrtShell = pDocShell->GetWrtShell();
    pWrtShell->Insert2(u"AZ"_ustr);
    pWrtShell->Left(SwCursorSkipMode::Chars, /*bSelect=*/false, 1, /*bBasicCall=*/false);
    Point aLogic = pWrtShell->GetCharRect().Center();
    SwView* pView = pDocShell->GetView();
    SwEditWin& rEditWin = pView->GetEditWin();
    Point aPixel = rEditWin.LogicToPixel(aLogic);

    // When converting that pixel position to a document model position (text range):
    uno::Reference<frame::XModel2> xModel(mxComponent, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xControllers = xModel->getControllers();
    uno::Reference<text::XTextViewTextRangeSupplier> xController(xControllers->nextElement(),
                                                                 uno::UNO_QUERY);
    awt::Point aPoint(aPixel.getX(), aPixel.getY());
    uno::Reference<text::XTextRange> xTextRange
        = xController->createTextRangeByPixelPosition(aPoint);

    // Then make sure that text range points after the first character:
    auto pTextRange = dynamic_cast<SwXTextRange*>(xTextRange.get());
    SwPaM aPaM(pDoc->GetNodes());
    pTextRange->GetPositions(aPaM);
    sal_Int32 nActual = aPaM.GetPoint()->GetContentIndex();
    // Without the needed PixelToLogic() call in place, this test would have failed with:
    // - Expected: 1
    // - Actual  : 0
    // i.e. the returned text range pointed before the first character, not between the first and
    // the second character.
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), nActual);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testCreateTextRangeByPixelPositionGraphic)
{
    // Given a document with an as-char image and the center of that image in pixels:
    createSwDoc();
    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xTextGraphic(
        xFactory->createInstance(u"com.sun.star.text.TextGraphicObject"_ustr), uno::UNO_QUERY);
    xTextGraphic->setPropertyValue(u"AnchorType"_ustr,
                                   uno::Any(text::TextContentAnchorType_AS_CHARACTER));
    xTextGraphic->setPropertyValue(u"Width"_ustr, uno::Any(static_cast<sal_Int32>(10000)));
    xTextGraphic->setPropertyValue(u"Height"_ustr, uno::Any(static_cast<sal_Int32>(10000)));
    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor(xBodyText->createTextCursor());
    uno::Reference<text::XTextContent> xTextContent(xTextGraphic, uno::UNO_QUERY);
    xBodyText->insertTextContent(xCursor, xTextContent, false);
    SwDoc* pDoc = getSwDoc();
    SwDocShell* pDocShell = getSwDocShell();
    SwWrtShell* pWrtShell = pDocShell->GetWrtShell();
    SwRootFrame* pLayout = pWrtShell->GetLayout();
    SwFrame* pPage = pLayout->GetLower();
    SwFrame* pBody = pPage->GetLower();
    SwFrame* pText = pBody->GetLower();
    SwSortedObjs& rDrawObjs = *pText->GetDrawObjs();
    SwAnchoredObject* pAnchored = rDrawObjs[0];
    Point aLogic = pAnchored->GetObjRect().Center();
    SwView* pView = pDocShell->GetView();
    SwEditWin& rEditWin = pView->GetEditWin();
    Point aPixel = rEditWin.LogicToPixel(aLogic);

    // When converting that pixel position to a document model position (text range):
    uno::Reference<frame::XModel2> xModel(mxComponent, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xControllers = xModel->getControllers();
    uno::Reference<text::XTextViewTextRangeSupplier> xController(xControllers->nextElement(),
                                                                 uno::UNO_QUERY);
    awt::Point aPoint(aPixel.getX(), aPixel.getY());
    // Without the accompanying fix in place, this test would have crashed, because an XTextRange
    // can't point to a graphic node.
    uno::Reference<text::XTextRange> xTextRange
        = xController->createTextRangeByPixelPosition(aPoint);

    // Then make sure that the anchor of the image is returned:
    const auto& rFormats = *pDoc->GetSpzFrameFormats();
    const auto pFormat = rFormats[0];
    SwPosition aAnchorPos(*pFormat->GetAnchor().GetContentAnchor());
    auto pTextRange = dynamic_cast<SwXTextRange*>(xTextRange.get());
    SwPaM aPaM(pDoc->GetNodes());
    pTextRange->GetPositions(aPaM);
    CPPUNIT_ASSERT_EQUAL(aAnchorPos, *aPaM.GetPoint());
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testCreateTextRangeByPixelPositionAtPageGraphic)
{
    // Given a document with an at-page anchored image:
    createSwDoc();
    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xTextGraphic(
        xFactory->createInstance(u"com.sun.star.text.TextGraphicObject"_ustr), uno::UNO_QUERY);
    xTextGraphic->setPropertyValue(u"AnchorType"_ustr,
                                   uno::Any(text::TextContentAnchorType_AT_PAGE));
    xTextGraphic->setPropertyValue(u"AnchorPageNo"_ustr, uno::Any(static_cast<sal_Int16>(1)));
    xTextGraphic->setPropertyValue(u"Width"_ustr, uno::Any(static_cast<sal_Int32>(10000)));
    xTextGraphic->setPropertyValue(u"Height"_ustr, uno::Any(static_cast<sal_Int32>(10000)));
    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor(xBodyText->createTextCursor());
    uno::Reference<text::XTextContent> xTextContent(xTextGraphic, uno::UNO_QUERY);
    xBodyText->insertTextContent(xCursor, xTextContent, false);
    SwDocShell* pDocShell = getSwDocShell();
    SwWrtShell* pWrtShell = pDocShell->GetWrtShell();
    SwRootFrame* pLayout = pWrtShell->GetLayout();
    SwFrame* pPage = pLayout->GetLower();
    SwSortedObjs& rDrawObjs = *pPage->GetDrawObjs();
    SwAnchoredObject* pAnchored = rDrawObjs[0];
    Point aLogic = pAnchored->GetObjRect().Center();
    SwView* pView = pDocShell->GetView();
    SwEditWin& rEditWin = pView->GetEditWin();
    Point aPixel = rEditWin.LogicToPixel(aLogic);

    // When asking for the doc model pos of the image's anchor by pixel position:
    uno::Reference<frame::XModel2> xModel(mxComponent, uno::UNO_QUERY);
    uno::Reference<container::XEnumeration> xControllers = xModel->getControllers();
    uno::Reference<text::XTextViewTextRangeSupplier> xController(xControllers->nextElement(),
                                                                 uno::UNO_QUERY);
    awt::Point aPoint(aPixel.getX(), aPixel.getY());
    // Without the accompanying fix in place, this test would have crashed.
    uno::Reference<text::XTextRange> xTextRange
        = xController->createTextRangeByPixelPosition(aPoint);

    // Then make sure that the result is empty, since the image is at-page anchored:
    CPPUNIT_ASSERT(!xTextRange.is());
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorBasic)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor = xBodyText->createTextCursor();
    lcl_AppendParagraph(xBodyText, xCursor, u"alpha"_ustr);
    lcl_AppendParagraph(xBodyText, xCursor, u"beta"_ustr);
    lcl_AppendParagraph(xBodyText, xCursor, u"gamma"_ustr, false);
    uno::Reference<text::XTextRange> xAlphaParagraph = getParagraph(1, u"alpha"_ustr);
    uno::Reference<text::XTextRange> xBetaParagraph = getParagraph(2, u"beta"_ustr);
    uno::Reference<beans::XPropertySet> xBetaProperties(xBetaParagraph, uno::UNO_QUERY_THROW);
    xBetaProperties->setPropertyValue(UNO_NAME_PARA_STYLE_NAME, uno::Any(u"Heading 1"_ustr));
    Scheduler::ProcessEventsToIdle();

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);

    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(3), xParagraphNavigator->getCount());
    CPPUNIT_ASSERT_EQUAL(OUString(u"beta"_ustr), xParagraphNavigator->getParagraphText(1));
    CPPUNIT_ASSERT_EQUAL(getProperty<OUString>(xBetaProperties, UNO_NAME_PARA_STYLE_NAME),
                         xParagraphNavigator->getParagraphStyleName(1));

    xParagraphNavigator->gotoIndex(0, false);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(0), xParagraphNavigator->getCurrentIndex());
    CPPUNIT_ASSERT(!xParagraphNavigator->gotoPrevious(false));

    xParagraphNavigator->gotoIndex(1, false);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), xParagraphNavigator->getCurrentIndex());

    uno::Reference<text::XTextViewCursor> xViewCursor = lcl_GetViewCursor(xModel);
    xViewCursor->gotoRange(xAlphaParagraph->getStart(), false);
    CPPUNIT_ASSERT(xViewCursor->goRight(1, false));
    xViewCursor->gotoRange(xBetaParagraph->getStart(), true);
    OUString aExpectedExpandedSelection = xViewCursor->getString();

    xParagraphNavigator->gotoIndex(0, false);
    CPPUNIT_ASSERT(xViewCursor->goRight(1, false));
    CPPUNIT_ASSERT(xParagraphNavigator->gotoNext(true));
    CPPUNIT_ASSERT_EQUAL(aExpectedExpandedSelection, xViewCursor->getString());
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), xParagraphNavigator->getCurrentIndex());

    xParagraphNavigator->gotoIndex(1, false);
    CPPUNIT_ASSERT(xParagraphNavigator->gotoNext(false));
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(2), xParagraphNavigator->getCurrentIndex());
    CPPUNIT_ASSERT(!xParagraphNavigator->gotoNext(false));

    CPPUNIT_ASSERT(xParagraphNavigator->gotoPrevious(false));
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), xParagraphNavigator->getCurrentIndex());
    CPPUNIT_ASSERT(xParagraphNavigator->isVisible(1));

    uno::Sequence<awt::Rectangle> aDocBounds = xParagraphNavigator->getParagraphBounds(1);
    CPPUNIT_ASSERT(aDocBounds.getLength() > 0);
    CPPUNIT_ASSERT(aDocBounds[0].Width > 0);
    CPPUNIT_ASSERT(aDocBounds[0].Height > 0);

    uno::Sequence<awt::Rectangle> aViewBounds = xParagraphNavigator->getParagraphViewBounds(1);
    CPPUNIT_ASSERT(aViewBounds.getLength() > 0);
    CPPUNIT_ASSERT(aViewBounds[0].Width > 0);
    CPPUNIT_ASSERT(aViewBounds[0].Height > 0);
    CPPUNIT_ASSERT_EQUAL(aDocBounds.getLength(), aViewBounds.getLength());

    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();
    for (sal_Int32 i = 0; i < aDocBounds.getLength(); ++i)
        lcl_AssertViewRectMatchesDocRect(rEditWin, aDocBounds[i], aViewBounds[i]);

    CPPUNIT_ASSERT(xViewCursor->goRight(1, false));
    xParagraphNavigator->selectCurrentParagraph();
    CPPUNIT_ASSERT_EQUAL(u"beta"_ustr, xViewCursor->getString());
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorMainBodyEnumeration)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor = xBodyText->createTextCursor();
    lcl_AppendParagraph(xBodyText, xCursor, u"alpha"_ustr);

    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XTextTable> xTable(
        xFactory->createInstance(u"com.sun.star.text.TextTable"_ustr), uno::UNO_QUERY_THROW);
    xTable->initialize(1, 1);
    xBodyText->insertTextContent(xCursor, xTable, /*bAbsorb=*/false);
    uno::Reference<text::XText> xCellText(xTable->getCellByName(u"A1"_ustr), uno::UNO_QUERY_THROW);
    xCellText->setString(u"inside table"_ustr);

    uno::Reference<text::XTextCursor> xEndCursor(
        xBodyText->createTextCursorByRange(xBodyText->getEnd()));
    lcl_AppendParagraph(xBodyText, xEndCursor, u"beta"_ustr, false);
    Scheduler::ProcessEventsToIdle();

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);

    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(2), xParagraphNavigator->getCount());
    CPPUNIT_ASSERT_EQUAL(u"alpha"_ustr, xParagraphNavigator->getParagraphText(0));
    CPPUNIT_ASSERT_EQUAL(u"beta"_ustr, xParagraphNavigator->getParagraphText(1));

    xParagraphNavigator->gotoIndex(1, false);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), xParagraphNavigator->getCurrentIndex());
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorInvalidIndex)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor = xBodyText->createTextCursor();
    lcl_AppendParagraph(xBodyText, xCursor, u"alpha"_ustr);
    lcl_AppendParagraph(xBodyText, xCursor, u"beta"_ustr, false);
    Scheduler::ProcessEventsToIdle();

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);
    const sal_Int32 nOutOfRange = xParagraphNavigator->getCount();

    // positive out-of-range
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->gotoIndex(nOutOfRange, false),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphBounds(nOutOfRange),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphViewBounds(nOutOfRange),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->isVisible(nOutOfRange),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphText(nOutOfRange),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphStyleName(nOutOfRange),
                         lang::IndexOutOfBoundsException);

    // negative index
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->gotoIndex(-1, false),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphBounds(-1),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphViewBounds(-1),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->isVisible(-1),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphText(-1),
                         lang::IndexOutOfBoundsException);
    CPPUNIT_ASSERT_THROW(xParagraphNavigator->getParagraphStyleName(-1),
                         lang::IndexOutOfBoundsException);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorCurrentIndexOutOfScope)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<container::XNameAccess> xPageStyles = getStyles(u"PageStyles"_ustr);
    uno::Reference<beans::XPropertySet> xPageStyle(
        xPageStyles->getByName(u"Standard"_ustr), uno::UNO_QUERY_THROW);
    xPageStyle->setPropertyValue(u"HeaderIsOn"_ustr, uno::Any(true));
    xPageStyle->setPropertyValue(u"FooterIsOn"_ustr, uno::Any(true));
    uno::Reference<text::XText> xHeaderText(
        xPageStyle->getPropertyValue(u"HeaderText"_ustr), uno::UNO_QUERY_THROW);
    xHeaderText->setString(u"inside header"_ustr);
    uno::Reference<text::XText> xFooterText(
        xPageStyle->getPropertyValue(u"FooterText"_ustr), uno::UNO_QUERY_THROW);
    xFooterText->setString(u"inside footer"_ustr);

    uno::Reference<text::XTextTable> xTable(
        xFactory->createInstance(u"com.sun.star.text.TextTable"_ustr), uno::UNO_QUERY_THROW);
    xTable->initialize(1, 1);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xBodyCursor = xBodyText->createTextCursor();
    lcl_AppendParagraph(xBodyText, xBodyCursor, u"alpha"_ustr, false);
    xBodyText->insertTextContent(xBodyCursor, xTable, /*bAbsorb=*/false);
    uno::Reference<text::XText> xCellText(xTable->getCellByName(u"A1"_ustr), uno::UNO_QUERY_THROW);
    xCellText->setString(u"inside table"_ustr);

    uno::Reference<text::XTextContent> xFootnote(
        xFactory->createInstance(u"com.sun.star.text.Footnote"_ustr), uno::UNO_QUERY_THROW);
    xBodyText->insertTextContent(xBodyCursor, xFootnote, /*bAbsorb=*/false);
    uno::Reference<text::XText> xFootnoteText(xFootnote, uno::UNO_QUERY_THROW);
    xFootnoteText->setString(u"inside footnote"_ustr);

    uno::Reference<text::XTextContent> xTextFrame(
        xFactory->createInstance(u"com.sun.star.text.TextFrame"_ustr), uno::UNO_QUERY_THROW);
    xBodyText->insertTextContent(xBodyCursor, xTextFrame, /*bAbsorb=*/false);
    uno::Reference<text::XText> xFrameText(xTextFrame, uno::UNO_QUERY_THROW);
    xFrameText->setString(u"inside frame"_ustr);
    Scheduler::ProcessEventsToIdle();

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);
    uno::Reference<text::XTextViewCursor> xViewCursor = lcl_GetViewCursor(xModel);

    lcl_AssertParagraphNavigatorOutOfScope(xParagraphNavigator, xViewCursor, xCellText->getStart());
    lcl_AssertParagraphNavigatorOutOfScope(xParagraphNavigator, xViewCursor,
                                           xFootnoteText->getStart());
    lcl_AssertParagraphNavigatorOutOfScope(xParagraphNavigator, xViewCursor, xHeaderText->getStart());
    lcl_AssertParagraphNavigatorOutOfScope(xParagraphNavigator, xViewCursor, xFooterText->getStart());
    lcl_AssertParagraphNavigatorOutOfScope(xParagraphNavigator, xViewCursor, xFrameText->getStart());
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorMultiFragment)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<container::XNameAccess> xPageStyles = getStyles(u"PageStyles"_ustr);
    uno::Reference<beans::XPropertySet> xPageStyle(
        xPageStyles->getByName(u"Standard"_ustr), uno::UNO_QUERY_THROW);
    xPageStyle->setPropertyValue(u"Height"_ustr, uno::Any(static_cast<sal_Int32>(2500)));
    xPageStyle->setPropertyValue(u"TopMargin"_ustr, uno::Any(static_cast<sal_Int32>(200)));
    xPageStyle->setPropertyValue(u"BottomMargin"_ustr, uno::Any(static_cast<sal_Int32>(200)));

    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor = xBodyText->createTextCursor();
    xBodyText->insertString(xCursor, lcl_CreateLongParagraph(), false);
    Scheduler::ProcessEventsToIdle();

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);
    uno::Sequence<awt::Rectangle> aDocBounds = xParagraphNavigator->getParagraphBounds(0);
    uno::Sequence<awt::Rectangle> aViewBounds = xParagraphNavigator->getParagraphViewBounds(0);

    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(1), xParagraphNavigator->getCount());
    CPPUNIT_ASSERT_MESSAGE("expected paragraph to span multiple fragments",
                           aDocBounds.getLength() > 1);
    CPPUNIT_ASSERT_EQUAL(aDocBounds.getLength(), aViewBounds.getLength());

    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();
    for (sal_Int32 i = 0; i < aDocBounds.getLength(); ++i)
        lcl_AssertViewRectMatchesDocRect(rEditWin, aDocBounds[i], aViewBounds[i]);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testParagraphNavigatorHiddenParagraph)
{
    createSwDoc();

    uno::Reference<text::XTextDocument> xTextDocument(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<lang::XMultiServiceFactory> xFactory(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XText> xBodyText = xTextDocument->getText();
    uno::Reference<text::XTextCursor> xCursor = xBodyText->createTextCursor();
    lcl_AppendParagraph(xBodyText, xCursor, u"alpha"_ustr);
    lcl_AppendParagraph(xBodyText, xCursor, u"hidden beta"_ustr);
    lcl_AppendParagraph(xBodyText, xCursor, u"gamma"_ustr, false);

    uno::Reference<text::XTextRange> xHiddenParagraph = getParagraph(2, u"hidden beta"_ustr);
    uno::Reference<text::XTextCursor> xHiddenCursor(
        xBodyText->createTextCursorByRange(xHiddenParagraph->getStart()));
    uno::Reference<text::XTextContent> xHiddenField(
        xFactory->createInstance(u"com.sun.star.text.TextField.HiddenParagraph"_ustr),
        uno::UNO_QUERY_THROW);
    uno::Reference<beans::XPropertySet> xHiddenProps(xHiddenField, uno::UNO_QUERY_THROW);
    xHiddenProps->setPropertyValue(UNO_NAME_IS_HIDDEN, uno::Any(true));
    xBodyText->insertTextContent(xHiddenCursor, xHiddenField, false);

    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<view::XViewSettingsSupplier> xViewSettingsSupplier(
        xModel->getCurrentController(), uno::UNO_QUERY_THROW);
    uno::Reference<beans::XPropertySet> xViewSettings = xViewSettingsSupplier->getViewSettings();
    xViewSettings->setPropertyValue(UNO_NAME_SHOW_HIDDEN_PARAGRAPHS, uno::Any(false));
    Scheduler::ProcessEventsToIdle();

    uno::Reference<text::XParagraphNavigator> xParagraphNavigator = lcl_GetParagraphNavigator(xModel);
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(3), xParagraphNavigator->getCount());
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(0),
                         xParagraphNavigator->getParagraphBounds(1).getLength());
    CPPUNIT_ASSERT_EQUAL(static_cast<sal_Int32>(0),
                         xParagraphNavigator->getParagraphViewBounds(1).getLength());
    CPPUNIT_ASSERT(!xParagraphNavigator->isVisible(1));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetTextFormFields)
{
    // Given a document with 3 fieldmarks: 2 zotero items and a zotero
    // bibliography:
    createSwDoc();
    for (int i = 0; i < 2; ++i)
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"FieldType"_ustr, uno::Any(ODF_UNHANDLED)),
            comphelper::makePropertyValue(u"FieldCommand"_ustr,
                                          uno::Any(u"ADDIN ZOTERO_ITEM foo bar"_ustr)),
            comphelper::makePropertyValue(u"FieldResult"_ustr, uno::Any(u"result"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:TextFormField"_ustr, aArgs);
    }
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"FieldType"_ustr, uno::Any(ODF_UNHANDLED)),
            comphelper::makePropertyValue(u"FieldCommand"_ustr,
                                          uno::Any(u"ADDIN ZOTERO_BIBL foo bar"_ustr)),
            comphelper::makePropertyValue(u"FieldResult"_ustr,
                                          uno::Any(u"<p>aaa</p><p>bbb</p>"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:TextFormField"_ustr, aArgs);
    }

    // When getting the zotero items:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:TextFormFields?type=vnd.oasis.opendocument.field.UNHANDLED&"
                              "commandPrefix=ADDIN%20ZOTERO_ITEM");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we find the 2 items and ignore the bibliography:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (fields)
    // i.e. the returned JSON was just empty.
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), aTree.get_child("fields").count(""));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetDocumentProperties)
{
    // Given a document with 3 custom properties: 2 Zotero ones and one other:
    createSwDoc();
    SwDocShell* pDocShell = getSwDocShell();
    uno::Reference<document::XDocumentPropertiesSupplier> xDPS(pDocShell->GetModel(),
                                                               uno::UNO_QUERY);
    uno::Reference<document::XDocumentProperties> xDP = xDPS->getDocumentProperties();
    uno::Reference<beans::XPropertyContainer> xUDP = xDP->getUserDefinedProperties();
    xUDP->addProperty(u"ZOTERO_PREF_1"_ustr, beans::PropertyAttribute::REMOVABLE,
                      uno::Any(u"foo"_ustr));
    xUDP->addProperty(u"ZOTERO_PREF_2"_ustr, beans::PropertyAttribute::REMOVABLE,
                      uno::Any(u"bar"_ustr));
    xUDP->addProperty(u"OTHER"_ustr, beans::PropertyAttribute::REMOVABLE, uno::Any(u"baz"_ustr));

    // When getting the zotero properties:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:SetDocumentProperties?namePrefix=ZOTERO_PREF_");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we find the 2 properties and ignore the other one:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (userDefinedProperties)
    // i.e. the returned JSON was just empty.
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2),
                         aTree.get_child("userDefinedProperties").count(""));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetBookmarks)
{
    // Given a document with 3 bookmarks: 2 zotero references and a zotero bibliography:
    createSwDoc();
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"Bookmark"_ustr, uno::Any(u"ZOTERO_BREF_1"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:InsertBookmark"_ustr, aArgs);
    }
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"Bookmark"_ustr, uno::Any(u"ZOTERO_BREF_2"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:InsertBookmark"_ustr, aArgs);
    }
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"Bookmark"_ustr, uno::Any(u"ZOTERO_BIBL"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:InsertBookmark"_ustr, aArgs);
    }

    // When getting the reference bookmarks:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Bookmarks?namePrefix=ZOTERO_BREF_");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we get the 2 references but not the bibliography:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (bookmarks)
    // i.e. the returned JSON was just empty.
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), aTree.get_child("bookmarks").count(""));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetFields)
{
    // Given a document with a refmark:
    createSwDoc();
    SwWrtShell* pWrtShell = getSwDocShell()->GetWrtShell();
    OUString aName(u"ZOTERO_ITEM CSL_CITATION {} "_ustr);
    for (int i = 0; i < 5; ++i)
    {
        uno::Sequence<css::beans::PropertyValue> aArgs = {
            comphelper::makePropertyValue(u"TypeName"_ustr, uno::Any(u"SetRef"_ustr)),
            comphelper::makePropertyValue(u"Name"_ustr, uno::Any(aName + OUString::number(i + 1))),
            comphelper::makePropertyValue(u"Content"_ustr, uno::Any(u"mycontent"_ustr)),
        };
        dispatchCommand(mxComponent, u".uno:InsertField"_ustr, aArgs);
        pWrtShell->SttEndDoc(/*bStt=*/false);
        pWrtShell->SplitNode();
        pWrtShell->SttEndDoc(/*bStt=*/false);
    }

    // When getting the refmarks:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Fields?typeName=SetRef&namePrefix=ZOTERO_ITEM%20CSL_CITATION");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we get the 1 refmark:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (setRefs)
    // i.e. the returned JSON was just empty.
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(5), aTree.get_child("setRefs").count(""));
    auto it = aTree.get_child("setRefs").begin();
    boost::property_tree::ptree aRef = (it++)->second;
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} 1"),
                         aRef.get<std::string>("name"));
    aRef = (it++)->second;
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} 2"),
                         aRef.get<std::string>("name"));
    aRef = (it++)->second;
    // Without the accompanying fix in place, this test would have failed with:
    // - Expected: ZOTERO_ITEM CSL_CITATION {} 3
    // - Actual  : ZOTERO_ITEM CSL_CITATION {} 4
    // i.e. the output was unsorted.
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} 3"),
                         aRef.get<std::string>("name"));
    aRef = (it++)->second;
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} 4"),
                         aRef.get<std::string>("name"));
    aRef = (it++)->second;
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} 5"),
                         aRef.get<std::string>("name"));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetLayout)
{
    // Given a document with 2 pages:
    createSwDoc();
    SwDoc* pDoc = getSwDoc();
    SwWrtShell* pWrtShell = pDoc->GetDocShell()->GetWrtShell();
    pWrtShell->InsertPageBreak();

    // When getting info about the layout:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Layout");
    auto pXTextDocument = dynamic_cast<SwXTextDocument*>(mxComponent.get());
    pXTextDocument->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we get the 2 pages:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    auto aPages = aTree.get_child("commandValues").get_child("pages");
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(2), aPages.count(""));
    for (const auto& rPage : aPages)
    {
        CPPUNIT_ASSERT(!rPage.second.get<bool>("isInvalidContent"));
    }
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetTextFormField)
{
    // Given a document with a fieldmark:
    createSwDoc();
    uno::Sequence<css::beans::PropertyValue> aArgs = {
        comphelper::makePropertyValue(u"FieldType"_ustr, uno::Any(ODF_UNHANDLED)),
        comphelper::makePropertyValue(u"FieldCommand"_ustr,
                                      uno::Any(u"ADDIN ZOTERO_ITEM foo bar"_ustr)),
        comphelper::makePropertyValue(u"FieldResult"_ustr, uno::Any(u"result"_ustr)),
    };
    dispatchCommand(mxComponent, u".uno:TextFormField"_ustr, aArgs);

    // When stepping into the fieldmark with the cursor and getting the command value for
    // uno:TextFormField:
    SwWrtShell* pWrtShell = getSwDocShell()->GetWrtShell();
    pWrtShell->SttEndDoc(/*bStt=*/false);
    pWrtShell->Left(SwCursorSkipMode::Chars, /*bSelect=*/false, 1, /*bBasicCall=*/false);
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:TextFormField?type=vnd.oasis.opendocument.field.UNHANDLED&"
                              "commandPrefix=ADDIN%20ZOTERO_ITEM");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we find the inserted fieldmark:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (type)
    // i.e. the returned JSON was just an empty object.
    auto field = aTree.get_child("field");
    CPPUNIT_ASSERT_EQUAL(std::string("vnd.oasis.opendocument.field.UNHANDLED"),
                         field.get<std::string>("type"));
    CPPUNIT_ASSERT_EQUAL(std::string("ADDIN ZOTERO_ITEM foo bar"),
                         field.get<std::string>("command"));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetSections)
{
    // Given a document with a section:
    createSwDoc();
    uno::Sequence<css::beans::PropertyValue> aArgs = {
        comphelper::makePropertyValue(
            u"RegionName"_ustr, uno::Any(u"ZOTERO_BIBL {} CSL_BIBLIOGRAPHY RNDRfiit6mXBc"_ustr)),
        comphelper::makePropertyValue(u"Content"_ustr, uno::Any(u"<p>aaa</p><p>bbb</p>"_ustr)),
    };
    dispatchCommand(mxComponent, u".uno:InsertSection"_ustr, aArgs);

    // When asking for a list of section names:
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Sections?namePrefix=ZOTERO_BIBL");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Make sure we find our just inserted section:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (sections)
    // i.e. the returned JSON was an empty object.
    CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), aTree.get_child("sections").count(""));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetBookmark)
{
    // Given a document with a bookmark:
    createSwDoc();
    uno::Sequence<css::beans::PropertyValue> aArgs = {
        comphelper::makePropertyValue(u"Bookmark"_ustr, uno::Any(u"ZOTERO_BREF_1"_ustr)),
        comphelper::makePropertyValue(u"BookmarkText"_ustr, uno::Any(u"<p>aaa</p><p>bbb</p>"_ustr)),
    };
    dispatchCommand(mxComponent, u".uno:InsertBookmark"_ustr, aArgs);

    // When stepping into the bookmark with the cursor and getting the command value for
    // .uno:Bookmark:
    SwWrtShell* pWrtShell = getSwDocShell()->GetWrtShell();
    pWrtShell->SttEndDoc(/*bStt=*/false);
    pWrtShell->Left(SwCursorSkipMode::Chars, /*bSelect=*/false, 1, /*bBasicCall=*/false);
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Bookmark?namePrefix=ZOTERO_BREF_");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we find the inserted bookmark:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    boost::property_tree::ptree aBookmark = aTree.get_child("bookmark");
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (bookmark)
    // i.e. the returned JSON was an empty object.
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_BREF_1"), aBookmark.get<std::string>("name"));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testGetField)
{
    // Given a document with a refmark:
    createSwDoc();
    uno::Sequence<css::beans::PropertyValue> aArgs = {
        comphelper::makePropertyValue(u"TypeName"_ustr, uno::Any(u"SetRef"_ustr)),
        comphelper::makePropertyValue(u"Name"_ustr,
                                      uno::Any(u"ZOTERO_ITEM CSL_CITATION {} refmark"_ustr)),
        comphelper::makePropertyValue(u"Content"_ustr, uno::Any(u"content"_ustr)),
    };
    dispatchCommand(mxComponent, u".uno:InsertField"_ustr, aArgs);

    // When in the refmark with the cursor and getting the command value for .uno:Field:
    SwWrtShell* pWrtShell = getSwDocShell()->GetWrtShell();
    pWrtShell->SttEndDoc(/*bStt=*/false);
    pWrtShell->Left(SwCursorSkipMode::Chars, /*bSelect=*/false, 1, /*bBasicCall=*/false);
    tools::JsonWriter aJsonWriter;
    std::string_view aCommand(".uno:Field?typeName=SetRef&namePrefix=ZOTERO_ITEM%20CSL_CITATION");
    getSwTextDoc()->getCommandValues(aJsonWriter, aCommand);

    // Then make sure we find the inserted refmark:
    OString pJSON(aJsonWriter.finishAndGetAsOString());
    std::stringstream aStream((std::string(pJSON)));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    boost::property_tree::ptree aBookmark = aTree.get_child("setRef");
    // Without the accompanying fix in place, this test would have failed with:
    // - No such node (setRef)
    // i.e. the returned JSON was an empty object.
    CPPUNIT_ASSERT_EQUAL(std::string("ZOTERO_ITEM CSL_CITATION {} refmark"),
                         aBookmark.get<std::string>("name"));
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDoNotBreakWrappedTables)
{
    // Given an empty document:
    createSwDoc();

    // When checking the state of the DoNotBreakWrappedTables compat flag:
    uno::Reference<lang::XMultiServiceFactory> xDocument(mxComponent, uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xSettings(
        xDocument->createInstance(u"com.sun.star.document.Settings"_ustr), uno::UNO_QUERY);
    bool bDoNotBreakWrappedTables{};
    // Without the accompanying fix in place, this test would have failed with:
    // An uncaught exception of type com.sun.star.beans.UnknownPropertyException
    // i.e. the compat flag was not recognized.
    xSettings->getPropertyValue(u"DoNotBreakWrappedTables"_ustr) >>= bDoNotBreakWrappedTables;
    // Then make sure it's false by default:
    CPPUNIT_ASSERT(!bDoNotBreakWrappedTables);

    // And when setting DoNotBreakWrappedTables=true:
    xSettings->setPropertyValue(u"DoNotBreakWrappedTables"_ustr, uno::Any(true));
    // Then make sure it gets enabled:
    xSettings->getPropertyValue(u"DoNotBreakWrappedTables"_ustr) >>= bDoNotBreakWrappedTables;
    CPPUNIT_ASSERT(bDoNotBreakWrappedTables);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testAllowTextAfterFloatingTableBreak)
{
    // Given an empty document:
    createSwDoc();

    // When checking the state of the AllowTextAfterFloatingTableBreak compat flag:
    uno::Reference<lang::XMultiServiceFactory> xDocument(mxComponent, uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xSettings(
        xDocument->createInstance(u"com.sun.star.document.Settings"_ustr), uno::UNO_QUERY);
    bool bAllowTextAfterFloatingTableBreak{};
    // Without the accompanying fix in place, this test would have failed with:
    // An uncaught exception of type com.sun.star.beans.UnknownPropertyException
    // i.e. the compat flag was not recognized.
    xSettings->getPropertyValue(u"AllowTextAfterFloatingTableBreak"_ustr)
        >>= bAllowTextAfterFloatingTableBreak;
    // Then make sure it's false by default:
    CPPUNIT_ASSERT(!bAllowTextAfterFloatingTableBreak);

    // And when setting AllowTextAfterFloatingTableBreak=true:
    xSettings->setPropertyValue(u"AllowTextAfterFloatingTableBreak"_ustr, uno::Any(true));
    // Then make sure it gets enabled:
    xSettings->getPropertyValue(u"AllowTextAfterFloatingTableBreak"_ustr)
        >>= bAllowTextAfterFloatingTableBreak;
    CPPUNIT_ASSERT(bAllowTextAfterFloatingTableBreak);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDoNotMirrorRtlDrawObjs)
{
    // Given an empty document:
    createSwDoc();

    // When checking the state of the DoNotMirrorRtlDrawObjs compat flag:
    uno::Reference<lang::XMultiServiceFactory> xDocument(mxComponent, uno::UNO_QUERY);
    uno::Reference<beans::XPropertySet> xSettings(
        xDocument->createInstance(u"com.sun.star.document.Settings"_ustr), uno::UNO_QUERY);
    bool bDoNotMirrorRtlDrawObjs{};
    // Without the accompanying fix in place, this test would have failed with:
    // An uncaught exception of type com.sun.star.beans.UnknownPropertyException
    // i.e. the compat flag was not recognized.
    xSettings->getPropertyValue(u"DoNotMirrorRtlDrawObjs"_ustr) >>= bDoNotMirrorRtlDrawObjs;
    // Then make sure it's false by default:
    CPPUNIT_ASSERT(!bDoNotMirrorRtlDrawObjs);

    // And when setting DoNotMirrorRtlDrawObjs=true:
    xSettings->setPropertyValue(u"DoNotMirrorRtlDrawObjs"_ustr, uno::Any(true));
    // Then make sure it gets enabled:
    xSettings->getPropertyValue(u"DoNotMirrorRtlDrawObjs"_ustr) >>= bDoNotMirrorRtlDrawObjs;
    CPPUNIT_ASSERT(bDoNotMirrorRtlDrawObjs);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testRedlineRenderModePartInfo)
{
    // Given a document with redline render mode set to "omit deletes":
    createSwDoc();
    SwWrtShell* pWrtShell = getSwDocShell()->GetWrtShell();
    SwViewOption aOpt(*pWrtShell->GetViewOptions());
    aOpt.SetRedlineRenderMode(SwRedlineRenderMode::OmitDeletes);
    pWrtShell->ApplyViewOptions(aOpt);

    // When getting the LOK part info:
    OUString aPartInfo = getSwTextDoc()->getPartInfo(0);

    // Then make sure we get the correct value:
    std::stringstream aStream((std::string(aPartInfo.toUtf8())));
    boost::property_tree::ptree aTree;
    boost::property_tree::read_json(aStream, aTree);
    // Without the accompanying fix in place, this test would have failed with:
    // - <unspecified file>(1): expected value
    // i.e. the json "mode" key was missing.
    CPPUNIT_ASSERT_EQUAL(std::string("2"), aTree.get<std::string>("mode"));
}

// XDocumentOverlay tests

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayBasicLifecycle)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);
    CPPUNIT_ASSERT(xOverlay.is());

    // add / remove
    rtl::Reference<MockOverlayPainter> xPainter(new MockOverlayPainter);
    sal_Int32 nHandle = xOverlay->addOverlay(xPainter, 0);
    CPPUNIT_ASSERT(nHandle > 0);

    // visibility toggle
    CPPUNIT_ASSERT(xOverlay->isOverlayVisible(nHandle));
    xOverlay->setOverlayVisible(nHandle, false);
    CPPUNIT_ASSERT(!xOverlay->isOverlayVisible(nHandle));
    xOverlay->setOverlayVisible(nHandle, true);
    CPPUNIT_ASSERT(xOverlay->isOverlayVisible(nHandle));

    // invalidateOverlay must not throw
    xOverlay->invalidateOverlay(awt::Rectangle(0, 0, 0, 0));
    xOverlay->invalidateOverlay(awt::Rectangle(100, 100, 500, 500));

    // remove succeeds; double remove throws
    xOverlay->removeOverlay(nHandle);
    CPPUNIT_ASSERT_THROW(xOverlay->removeOverlay(nHandle),
                         lang::IllegalArgumentException);

    // invalid handle throws
    CPPUNIT_ASSERT_THROW(xOverlay->removeOverlay(999),
                         lang::IllegalArgumentException);
    CPPUNIT_ASSERT_THROW(xOverlay->setOverlayVisible(999, false),
                         lang::IllegalArgumentException);
    CPPUNIT_ASSERT_THROW(xOverlay->isOverlayVisible(999),
                         lang::IllegalArgumentException);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayPaintCallback)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);

    rtl::Reference<MockOverlayPainter> xPainter(new MockOverlayPainter);
    sal_Int32 nHandle = xOverlay->addOverlay(xPainter, 0);

    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();
    rEditWin.Invalidate();
    rEditWin.PaintImmediately();

    // callback was invoked with valid XGraphics and non-empty VisibleArea
    CPPUNIT_ASSERT(xPainter->m_nPaintCount >= 1);
    CPPUNIT_ASSERT(xPainter->m_bReceivedGraphics);
    CPPUNIT_ASSERT(xPainter->m_aLastVisibleArea.Width > 0);
    CPPUNIT_ASSERT(xPainter->m_aLastVisibleArea.Height > 0);

    // MapMode is document twips matching getPrePostMapMode()
    CPPUNIT_ASSERT(xPainter->m_bCapturedMapMode);
    CPPUNIT_ASSERT_EQUAL(MapUnit::MapTwip, xPainter->m_aLastMapMode.GetMapUnit());
    const MapMode& rExpectedMapMode = getSwDocShell()->GetWrtShell()->getPrePostMapMode();
    CPPUNIT_ASSERT(rExpectedMapMode == xPainter->m_aLastMapMode);

    xOverlay->removeOverlay(nHandle);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayVisibilityBlocksCallback)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);

    rtl::Reference<MockOverlayPainter> xPainter(new MockOverlayPainter);
    sal_Int32 nHandle = xOverlay->addOverlay(xPainter, 0);
    xOverlay->setOverlayVisible(nHandle, false);

    // Trigger repaint
    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();
    rEditWin.Invalidate();
    rEditWin.PaintImmediately();

    CPPUNIT_ASSERT_EQUAL(sal_Int32(0), xPainter->m_nPaintCount);

    xOverlay->removeOverlay(nHandle);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayPaintOrder)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);

    std::vector<sal_Int32> aOrder;
    MockOverlayPainter::s_pGlobalOrder = &aOrder;

    // Register painter 2 at layer 100 first, then painter 1 at layer 0
    rtl::Reference<MockOverlayPainter> xPainter2(new MockOverlayPainter(2));
    rtl::Reference<MockOverlayPainter> xPainter1(new MockOverlayPainter(1));
    sal_Int32 nHandle2 = xOverlay->addOverlay(xPainter2, 100);
    sal_Int32 nHandle1 = xOverlay->addOverlay(xPainter1, 0);

    // Trigger repaint
    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();
    rEditWin.Invalidate();
    rEditWin.PaintImmediately();

    MockOverlayPainter::s_pGlobalOrder = nullptr;

    // Layer 0 painter called before layer 100 painter
    CPPUNIT_ASSERT_EQUAL(size_t(2), aOrder.size());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(1), aOrder[0]);
    CPPUNIT_ASSERT_EQUAL(sal_Int32(2), aOrder[1]);

    xOverlay->removeOverlay(nHandle1);
    xOverlay->removeOverlay(nHandle2);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayCallbackReentrancy)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);

    SwEditWin& rEditWin = getSwDocShell()->GetView()->GetEditWin();

    rtl::Reference<MockOverlayPainter> xAddedPainter(new MockOverlayPainter);
    rtl::Reference<ReentrantOverlayPainter> xReentrantPainter(
        new ReentrantOverlayPainter(xOverlay));
    xReentrantPainter->m_xPainterToAdd = xAddedPainter;

    sal_Int32 nHandle = xOverlay->addOverlay(xReentrantPainter, 0);
    xReentrantPainter->m_nRemoveHandle = nHandle;

    rEditWin.Invalidate();
    rEditWin.PaintImmediately();

    const sal_Int32 nFirstPaintCount = xReentrantPainter->m_nPaintCount;
    CPPUNIT_ASSERT(nFirstPaintCount >= 1);
    CPPUNIT_ASSERT(xReentrantPainter->m_nAddedHandle > 0);

    rEditWin.Invalidate();
    rEditWin.PaintImmediately();

    CPPUNIT_ASSERT_EQUAL(nFirstPaintCount, xReentrantPainter->m_nPaintCount);
    CPPUNIT_ASSERT(xAddedPainter->m_nPaintCount >= 1);

    xOverlay->removeOverlay(xReentrantPainter->m_nAddedHandle);
}

CPPUNIT_TEST_FIXTURE(SwUibaseUnoTest, testDocumentOverlayDisposeCleanup)
{
    createSwDoc();
    uno::Reference<frame::XModel> xModel(mxComponent, uno::UNO_QUERY_THROW);
    uno::Reference<text::XDocumentOverlay> xOverlay = lcl_GetDocumentOverlay(xModel);

    rtl::Reference<MockOverlayPainter> xPainter(new MockOverlayPainter);
    xOverlay->addOverlay(xPainter, 0);

    // Close the document — the overlay should be cleaned up without crash
    uno::Reference<util::XCloseable> xCloseable(mxComponent, uno::UNO_QUERY_THROW);
    xCloseable->close(true);
    mxComponent.clear();
}

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
