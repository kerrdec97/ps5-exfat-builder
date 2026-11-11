// Offscreen tests of the right-to-left layout (Arabic, Persian, Hebrew, Urdu):
// the application laid out right to left with the "qps-rtl" pseudo-language.
// The window and every page build mirrored; the sidebar's selection bar is at
// the right edge; a switch is on with its knob at the left (as Windows does
// it); a progress bar fills from the right; a path field's edit stays left to
// right; a toast lies at the left; the job monitor builds.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/models/LogModel.h"
#include "app/pages/Page.h"
#include "app/ui/JobMonitor.h"
#include "app/ui/Motion.h"
#include "app/ui/Shell.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Tr.h"
#include "support/TestSupport.h"
#include "ConsoleTestUtil.h"

#include <QApplication>
#include <QFrame>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

using namespace fw;
using namespace fw::ui;

namespace {

// The x of the brightest pixel on the widget's middle row: a switch's knob.
int brightestX(QWidget& w)
{
    const QImage img = w.grab().toImage();
    const int y = img.height() / 2;
    int best = -1, bestX = -1;
    for (int x = 0; x < img.width(); ++x) {
        const int g = qGray(img.pixel(x, y));
        if (g > best) {
            best = g;
            bestX = x;
        }
    }
    return bestX;
}

bool near(const QColor& a, const QColor& b)
{
    return std::abs(a.red() - b.red()) <= 6 && std::abs(a.green() - b.green()) <= 6 && std::abs(a.blue() - b.blue()) <= 6;
}

} // namespace

class TstRtlLayout : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        // As main() does it for a right-to-left language, before the first window.
        QVERIFY(i18n::load(i18n::pseudoRtlCode()));
        QVERIFY(i18n::isRightToLeft());
        QApplication::setLayoutDirection(Qt::RightToLeft);
        motion::setOverride(false); // the knob, the fill and the toasts are where they end up at once
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        QVERIFY(fwtest::closeConsoleServices(m_ctx->settings()));
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
        motion::clearOverride();
        QApplication::setLayoutDirection(Qt::LeftToRight);
        i18n::load(QStringLiteral("en"));
    }

    void windowIsMirrored()
    {
        QVERIFY(m_window->isRightToLeft());
        // The sidebar at the right, the page at its left.
        const int sidebarX = m_window->sidebar()->mapTo(m_window.get(), QPoint(0, 0)).x();
        QVERIFY2(sidebarX > m_window->width() / 2, qPrintable(QString::number(sidebarX)));
        QVERIFY(m_window->sidebar()->x() + m_window->sidebar()->width() >= m_window->width() - 1);
    }

    void everyPageBuilds()
    {
        const QStringList keys = m_window->pageKeys();
        QVERIFY(keys.size() >= 10);
        for (const QString& key : keys) {
            m_window->navigate(key);
            QCOMPARE(m_window->currentPage(), key);
            pages::Page* page = m_window->page(key);
            QVERIFY(page->isVisible());
            QVERIFY2(page->isRightToLeft(), qPrintable(key));
            QVERIFY(page->x() + page->width() <= m_window->sidebar()->mapTo(m_window.get(), QPoint(0, 0)).x() + 1);
        }
        m_window->navigate(QStringLiteral("home"));
    }

    // The selected item's bar, and the one bar that slides between the items, at the right edge.
    void navSelectionBarAtTheRightEdge()
    {
        m_window->navigate(QStringLiteral("settings"));
        NavItem* item = m_window->sidebar()->item(QStringLiteral("settings"));
        QVERIFY(item);
        QVERIFY(item->isChecked());
        const QRect bar = item->barRect();
        QVERIFY2(bar.left() > item->width() - 8, qPrintable(QStringLiteral("%1 of %2").arg(bar.left()).arg(item->width())));
        const QRect inSidebar = bar.translated(item->mapTo(m_window->sidebar(), QPoint(0, 0)));
        QWidget* sliding = nullptr;
        for (QWidget* w : m_window->sidebar()->findChildren<QWidget*>())
            if (w->isVisible() && w->size() == bar.size() && w->testAttribute(Qt::WA_TransparentForMouseEvents) && !qobject_cast<NavItem*>(w))
                sliding = w;
        QVERIFY(sliding);
        QTRY_COMPARE(QRect(sliding->mapTo(m_window->sidebar(), QPoint(0, 0)), sliding->size()), inSidebar);
        m_window->navigate(QStringLiteral("home"));
    }

    // Windows' way right to left: on is the knob at the left, off at the right.
    void switchKnob()
    {
        QWidget host;
        auto* sw = new Switch(&host);
        sw->move(10, 10);
        host.resize(100, 50);
        QVERIFY(sw->isRightToLeft());
        sw->setChecked(false);
        QTRY_COMPARE(sw->position(), 0.0);
        QVERIFY2(brightestX(*sw) > sw->width() / 2, "off: the knob is at the right");
        sw->setChecked(true);
        QTRY_COMPARE(sw->position(), 1.0);
        QVERIFY2(brightestX(*sw) < sw->width() / 2, "on: the knob is at the left");
    }

    void progressFillStartsAtTheRight()
    {
        QWidget host;
        auto* bar = new ProgressBar(&host);
        bar->setGeometry(0, 0, 200, bar->height());
        host.resize(220, 40);
        bar->setFraction(0.3);
        QTRY_COMPARE(bar->shownFraction(), 0.3);
        const QImage img = bar->grab().toImage();
        const int y = img.height() / 2;
        const QColor track = theme::color::bg_5;
        QVERIFY2(near(img.pixelColor(20, y), track), "the left end is the empty track");
        QVERIFY2(!near(img.pixelColor(180, y), track), "the right end is filled");
    }

    void pathFieldEditIsLeftToRight()
    {
        QWidget host;
        auto* l = new QVBoxLayout(&host);
        auto* field = new PathField(PathField::Mode::Directory, &host);
        l->addWidget(field);
        host.resize(500, 80);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QVERIFY(field->isRightToLeft());
        QCOMPARE(field->edit()->layoutDirection(), Qt::LeftToRight);
        // The row follows the layout: right to left Browse is at the left of the field.
        QVERIFY(field->browseButton()->geometry().right() < field->edit()->geometry().left());
    }

    void toastLiesAtTheLeft()
    {
        m_window->toast(Tone::Ok, QStringLiteral("Done"), QStringLiteral("All good"));
        QFrame* toast = nullptr;
        for (QFrame* f : m_window->findChildren<QFrame*>(QStringLiteral("toast")))
            if (f->isVisible())
                toast = f;
        QVERIFY(toast);
        QTRY_VERIFY(toast->geometry().right() < m_window->width() / 2);
        QCOMPARE(toast->x(), int(theme::space::lg));
        m_window->toasts()->clear();
    }

    void jobMonitorBuilds()
    {
        JobMonitor m(*m_ctx, m_window->logModel(), JobMonitor::Variant::Wide, m_window.get());
        QStringList lines;
        m.setDemoRecord(JobMonitor::demoRecord(QStringLiteral("running"), &lines), lines);
        m.resize(900, 600);
        m.show();
        QVERIFY(m.isRightToLeft());
        QVERIFY(!m.percentText().isEmpty());
        ProgressBar* bar = m.findChild<ProgressBar*>();
        QVERIFY(bar);
        QVERIFY(bar->isRightToLeft());
        // The live log reads left to right in any language.
        bool ltrLog = false;
        for (QWidget* w : m.findChildren<QWidget*>())
            if (w->objectName() == QLatin1String("fwConsole"))
                ltrLog = w->layoutDirection() == Qt::LeftToRight;
        QVERIFY(ltrLog);
    }
};

QTEST_MAIN(TstRtlLayout)
#include "tst_rtl_layout.moc"
