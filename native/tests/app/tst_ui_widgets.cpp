// Offscreen tests of the UI library on its own (no AppContext, no pages):
// outside text is shown literally in labels, toasts, job rows and tooltips;
// the wheel leaves unfocused controls alone; Enter presses the focused
// button and a sheet's default button; the list editor, the job time
// estimate, the Library grid's keyboard menu and its cover cache.

#include "app/ui/Components.h"
#include "app/ui/DependencyRow.h"
#include "app/ui/HomeKit.h"
#include "app/ui/Icons.h"
#include "app/ui/JobRows.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Motion.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Style.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include "core/Tr.h"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPaintEngine>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTreeWidget>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace fw;
using namespace fw::ui;

namespace {

const QString kHostile = QStringLiteral("<img src=x><b>t</b>");

// Every label under `root` shows its text as plain text.
bool allLabelsPlain(const QWidget* root, QString* offender)
{
    for (const QLabel* l : root->findChildren<QLabel*>())
        if (!l->text().isEmpty() && l->textFormat() != Qt::PlainText) {
            *offender = l->text();
            return false;
        }
    return true;
}

bool anyLabelShows(const QWidget* root, const QString& text)
{
    for (const QLabel* l : root->findChildren<QLabel*>())
        if (l->text() == text && l->textFormat() == Qt::PlainText)
            return true;
    return false;
}

// Motion is off on the offscreen platform: a test that is about it switches it on.
struct MotionOn {
    MotionOn() { motion::setOverride(true); }
    ~MotionOn() { motion::clearOverride(); }
};

// Popups (menus, tooltips) are in no page screenshot. With FW_UI_GRAB_DIR set,
// the tests that show one save a picture of it there, to look at.
void saveGrab(QWidget* w, const char* name)
{
    const QString dir = qEnvironmentVariable("FW_UI_GRAB_DIR");
    if (!dir.isEmpty())
        w->grab().save(QStringLiteral("%1/%2.png").arg(dir, QLatin1String(name)));
}

// A paint device that keeps the text of every text item painted on it (one
// item per run of one direction and script), and draws nothing.
class TextCapture final : public QPaintDevice {
public:
    explicit TextCapture(const QSize& size) : m_size(size) {}
    QPaintEngine* paintEngine() const override { return &m_engine; }
    const QStringList& texts() const { return m_engine.texts; }

protected:
    int metric(PaintDeviceMetric m) const override
    {
        switch (m) {
        case PdmWidth: return m_size.width();
        case PdmHeight: return m_size.height();
        case PdmWidthMM: return m_size.width() * 254 / 960;
        case PdmHeightMM: return m_size.height() * 254 / 960;
        case PdmNumColors: return std::numeric_limits<int>::max();
        case PdmDepth: return 32;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY: return 96;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(devicePixelRatioFScale());
        default: return 0;
        }
    }

private:
    class Engine final : public QPaintEngine {
    public:
        Engine() : QPaintEngine(QPaintEngine::AllFeatures) {}
        QStringList texts;
        bool begin(QPaintDevice*) override { return true; }
        bool end() override { return true; }
        void updateState(const QPaintEngineState&) override {}
        void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
        void drawImage(const QRectF&, const QImage&, const QRectF&, Qt::ImageConversionFlags) override {}
        void drawTiledPixmap(const QRectF&, const QPixmap&, const QPointF&) override {}
        void drawRects(const QRectF*, int) override {}
        void drawRects(const QRect*, int) override {}
        void drawLines(const QLineF*, int) override {}
        void drawLines(const QLine*, int) override {}
        void drawEllipse(const QRectF&) override {}
        void drawEllipse(const QRect&) override {}
        void drawPath(const QPainterPath&) override {}
        void drawPoints(const QPointF*, int) override {}
        void drawPoints(const QPoint*, int) override {}
        void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
        void drawPolygon(const QPoint*, int, PolygonDrawMode) override {}
        void drawTextItem(const QPointF&, const QTextItem& item) override { texts << item.text(); }
        Type type() const override { return QPaintEngine::User; }
    };
    QSize m_size;
    mutable Engine m_engine;
};

// The texts painted for `view`'s items (and anything else in its viewport).
QStringList paintedTexts(QAbstractItemView* view)
{
    TextCapture capture(view->viewport()->size());
    view->viewport()->render(&capture);
    return capture.texts();
}

void sendWheel(QWidget* w, int delta = -120)
{
    const QPointF pos(w->width() / 2.0, w->height() / 2.0);
    QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &ev);
}

} // namespace

class TstUiWidgets : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<QWidget> m_window;

    // A fresh child of the shown, active test window.
    template <typename T, typename... Args>
    T* add(Args&&... args)
    {
        auto* w = new T(std::forward<Args>(args)..., m_window.get());
        m_window->layout()->addWidget(w);
        w->show();
        return w;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        theme::apply(qApp);
        m_window = std::make_unique<QWidget>();
        new QVBoxLayout(m_window.get());
        m_window->resize(900, 700);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    void cleanupTestCase() { m_window.reset(); }

    void cleanup()
    {
        QToolTip::hideText();
        // Each test starts with an empty window.
        while (QLayoutItem* it = m_window->layout()->takeAt(0)) {
            delete it->widget();
            delete it;
        }
    }

    // ---- outside text is never markup ---------------------------------------------------

    void labelsShowMarkupLiterally()
    {
        QWidget host;
        for (QLabel* l : {label(kHostile, TextStyle::Body, theme::color::fg_1, &host), caption(kHostile, &host), meta(kHostile, &host),
                          monoLabel(kHostile, &host), heading(kHostile, TextStyle::Subtitle, &host)}) {
            QCOMPARE(l->textFormat(), Qt::PlainText);
            QCOMPARE(l->text(), kHostile);
        }
        Link link(kHostile, &host);
        QCOMPARE(link.textFormat(), Qt::PlainText);
        QCOMPARE(link.text(), kHostile);

        // Markup the app writes itself has its own factory.
        QWidget other;
        QLabel* rich = richLabel(QStringLiteral("<b>%1</b>").arg(kHostile.toHtmlEscaped()), TextStyle::Body, theme::color::fg_1, &other);
        QCOMPARE(rich->textFormat(), Qt::RichText);

        MessageRow row(&host);
        row.set(kHostile, Tone::Err);
        QCOMPARE(row.text(), kHostile);
        InfoBar bar(Tone::Err, &host);
        bar.setTitle(kHostile);
        bar.setMessage(kHostile);
        bar.addLink(kHostile);
        ErrorState err(kHostile, kHostile, kHostile, &host);
        StageStepper stepper(&host);
        stepper.setStages({kHostile, QStringLiteral("Two")}, 0);
        QString offender;
        QVERIFY2(allLabelsPlain(&host, &offender), qPrintable(offender));
        QVERIFY(anyLabelShows(&stepper, kHostile));
    }

    void elideLabelShowsMarkupLiterally()
    {
        auto* wide = add<ElideLabel>(kHostile, Qt::ElideRight);
        QCOMPARE(wide->textFormat(), Qt::PlainText);
        wide->resize(600, 20);
        QCOMPARE(wide->text(), kHostile);
        QVERIFY(wide->toolTip().isEmpty());

        // Elided: the full text moves to the tooltip, escaped.
        wide->setFullText(kHostile + QString(200, u'x'));
        wide->resize(80, 20);
        QVERIFY(wide->text() != wide->fullText());
        QVERIFY(wide->text().startsWith(QStringLiteral("<img")));
        QCOMPARE(wide->toolTip(), plainTip(wide->fullText()));
        QVERIFY(wide->toolTip().contains(QStringLiteral("&lt;img src=x&gt;&lt;b&gt;t&lt;/b&gt;")));
        QVERIFY(!wide->toolTip().contains(QStringLiteral("<img")));
        // A tooltip the owner set says more than the label: a new layout keeps it.
        wide->setToolTip(QStringLiteral("the owner's own text"));
        wide->setFullText(wide->fullText() + QStringLiteral(" and more"));
        wide->resize(wide->width() - 10, wide->height());
        QCOMPARE(wide->toolTip(), QStringLiteral("the owner's own text"));
    }

    void toastShowsMarkupLiterally()
    {
        ToastHost toasts(m_window.get());
        toasts.show(Tone::Err, kHostile, kHostile, {{kHostile, {}}}, 0);
        QCOMPARE(toasts.count(), 1);
        QFrame* toast = m_window->findChild<QFrame*>(QStringLiteral("toast"));
        QVERIFY(toast);
        QString offender;
        QVERIFY2(allLabelsPlain(toast, &offender), qPrintable(offender));
        int shown = 0;
        for (const QLabel* l : toast->findChildren<QLabel*>())
            if (l->text() == kHostile)
                ++shown;
        QCOMPARE(shown, 3); // title, body, action
        toasts.clear();
    }

    void jobRowShowsMarkupLiterally()
    {
        JobRecord r;
        r.id = QStringLiteral("j1");
        r.kind = QStringLiteral("test.kind");
        r.title = kHostile;
        r.subtitle = kHostile;
        r.state = JobState::Failed;
        r.error = Error(ErrorCode::IoError, kHostile + QString(80, u'y'));
        for (const JobRow::Variant v : {JobRow::Variant::Queue, JobRow::Variant::Dock}) {
            JobRow row(v);
            row.resize(900, row.sizeHint().height());
            row.setRecord(r, 0, 1, true);
            QString offender;
            QVERIFY2(allLabelsPlain(&row, &offender), qPrintable(offender));
            bool title = false;
            for (const ElideLabel* l : row.findChildren<ElideLabel*>())
                if (l->fullText().contains(kHostile))
                    title = true;
            QVERIFY(title);
            // The note's tooltip carries the whole error message, escaped.
            bool tip = false;
            for (const QLabel* l : row.findChildren<QLabel*>()) {
                QVERIFY2(!l->toolTip().contains(QStringLiteral("<img")), qPrintable(l->toolTip()));
                if (l->toolTip().contains(QStringLiteral("&lt;img src=x&gt;")))
                    tip = true;
            }
            QVERIFY(tip);
        }
    }

    void tooltipsShowMarkupLiterally()
    {
        QCOMPARE(plainTip(QString()), QString());
        QCOMPARE(plainTip(QStringLiteral("a<b")), QStringLiteral("<p style='white-space:pre-wrap'>a&lt;b</p>"));

        // A tooltip set anywhere in the app with text from outside.
        auto* w = add<QLabel>(QStringLiteral("hover me"));
        w->setToolTip(kHostile);
        QHelpEvent help(QEvent::ToolTip, QPoint(3, 3), w->mapToGlobal(QPoint(3, 3)));
        QApplication::sendEvent(w, &help);
        QCOMPARE(QToolTip::text(), plainTip(kHostile));
        QToolTip::hideText();

        // The app's own paragraph tooltips pass as written; plain ones too.
        const QString own = QStringLiteral("<p style='white-space:normal'>Fast &amp; safe</p>");
        w->setToolTip(own);
        QApplication::sendEvent(w, &help);
        QCOMPARE(QToolTip::text(), own);
        QToolTip::hideText();
        w->setToolTip(QStringLiteral("Drag to reorder"));
        QApplication::sendEvent(w, &help);
        QCOMPARE(QToolTip::text(), QStringLiteral("Drag to reorder"));
        QToolTip::hideText();

        // Markup hidden inside the app's wrapper does not pass.
        const QString forged = QStringLiteral("<p style='white-space:normal'><img src=x></p>");
        w->setToolTip(forged);
        QApplication::sendEvent(w, &help);
        QCOMPARE(QToolTip::text(), plainTip(forged));
        QToolTip::hideText();

        // The Library grid builds its tooltip from titles and paths.
        auto* grid = add<LibraryGrid>();
        GridItem it;
        it.key = QStringLiteral("k1");
        it.title = kHostile;
        it.path = QStringLiteral("D:/Dumps/<b>x");
        grid->setItems({it});
        grid->resize(600, 400);
        QHelpEvent gridHelp(QEvent::ToolTip, QPoint(40, 40), grid->mapToGlobal(QPoint(40, 40)));
        QApplication::sendEvent(grid, &gridHelp);
        QVERIFY2(QToolTip::text().contains(QStringLiteral("&lt;img src=x&gt;&lt;b&gt;t&lt;/b&gt;")), qPrintable(QToolTip::text()));
        QVERIFY(!QToolTip::text().contains(QStringLiteral("<img")));
    }

    void sheetTextIsPlainUnlessTheAppMarkedIt()
    {
        auto* s = new Sheet(m_window.get(), kHostile, Sheet::Icon::Warning);
        // Plain by default, also with a "<span" from outside in it.
        QCOMPARE(s->addText(kHostile)->textFormat(), Qt::PlainText);
        const QString forged = QStringLiteral("<span style=\"font-family:'Consolas';font-size:12px\"><img src=x></span>");
        QLabel* plain = s->addText(forged);
        QCOMPARE(plain->textFormat(), Qt::PlainText);
        QCOMPARE(plain->text(), forged);
        // monoSpan() escapes its argument and marks the text as the app's own.
        const QString span = Sheet::monoSpan(QStringLiteral("a<b&c"));
        QVERIFY(span.startsWith(QStringLiteral("<span ")));
        QVERIFY(span.endsWith(QStringLiteral(">a&lt;b&amp;c</span>")));
        QCOMPARE(s->addText(QStringLiteral("Delete %1?").arg(span))->textFormat(), Qt::RichText);
        const QString lines = Sheet::monoLines({QStringLiteral("x<y"), QStringLiteral("b&c")});
        QVERIFY(lines.contains(QStringLiteral("x&lt;y<br>b&amp;c")));
        QCOMPARE(s->addText(lines)->textFormat(), Qt::RichText);
        QCOMPARE(s->addPlainText(span)->textFormat(), Qt::PlainText);
        QCOMPARE(s->addRichText(QStringLiteral("<b>x</b>"))->textFormat(), Qt::RichText);
        // The title is a plain label.
        QVERIFY(anyLabelShows(s, kHostile));
        delete s;
    }

    // ---- token colours -----------------------------------------------------------------

    void labelsAndHairlinesGetTheirTokenColours()
    {
        using namespace theme;
        auto* card = add<QWidget>();
        auto* col = new QVBoxLayout(card);
        struct Sample {
            QLabel* label;
            QColor color;
            const char* name;
        };
        QList<Sample> samples{
            {label(QStringLiteral("MMMM body"), TextStyle::Body, color::fg_1, card), color::fg_1, "body fg_1"},
            {caption(QStringLiteral("MMMM caption"), card), color::fg_3, "caption fg_3"},
            {meta(QStringLiteral("MMMM meta"), card), color::fg_4, "meta fg_4"},
            {heading(QStringLiteral("MMMM heading"), TextStyle::Subtitle, card), color::fg_0, "heading fg_0"},
            {label(QStringLiteral("MMMM overline"), TextStyle::Overline, color::fg_5, card), color::fg_5, "overline fg_5"},
            {new Link(QStringLiteral("MMMM link"), card), color::accent_hi, "link accent_hi"},
            {new ElideLabel(QStringLiteral("MMMM elide"), Qt::ElideRight, card), color::danger_hi, "elide danger_hi"},
        };
        setTextColor(samples.last().label, color::danger_hi);
        for (const Sample& s : samples)
            col->addWidget(s.label);
        QFrame* thin = hairline(card);
        QFrame* strong = hairline(card, true);
        col->addWidget(thin);
        col->addWidget(strong);
        // A colour set after the first show, and a disabled plain label.
        MessageRow* message = new MessageRow(card);
        col->addWidget(message);
        auto* plain = new QLabel(QStringLiteral("MMMM disabled"), card);
        col->addWidget(plain);
        card->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QCoreApplication::processEvents();
        message->set(QStringLiteral("MMMM message"), Tone::Ok);
        plain->setEnabled(false);
        QCoreApplication::processEvents();

        const QImage shot = m_window->grab().toImage();
        // The strongest text pixel of a label: fully covered, so it is the text colour.
        auto textColor = [&](const QWidget* w) {
            const QRect r(w->mapTo(m_window.get(), QPoint(0, 0)), w->size());
            const QColor bg = shot.pixelColor(r.right() - 1, r.top() + 1);
            QColor best = bg;
            int bestDist = -1;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    const int d = qAbs(c.red() - bg.red()) + qAbs(c.green() - bg.green()) + qAbs(c.blue() - bg.blue());
                    if (d > bestDist) {
                        bestDist = d;
                        best = c;
                    }
                }
            return best;
        };
        auto close = [](const QColor& a, const QColor& b) {
            return qAbs(a.red() - b.red()) <= 10 && qAbs(a.green() - b.green()) <= 10 && qAbs(a.blue() - b.blue()) <= 10;
        };
        for (const Sample& s : samples) {
            const QColor got = textColor(s.label);
            QVERIFY2(close(got, s.color), qPrintable(QStringLiteral("%1: %2, expected %3").arg(QLatin1String(s.name), got.name(), s.color.name())));
        }
        QLabel* messageText = nullptr;
        for (QLabel* l : message->findChildren<QLabel*>())
            if (l->text() == QStringLiteral("MMMM message"))
                messageText = l;
        QVERIFY(messageText);
        QVERIFY2(close(textColor(messageText), toneText(Tone::Ok)), qPrintable(textColor(messageText).name()));
        QVERIFY2(close(textColor(plain), color::fg_5), qPrintable(textColor(plain).name()));

        auto pixelAt = [&](const QWidget* w) {
            return shot.pixelColor(w->mapTo(m_window.get(), QPoint(w->width() / 2, 0)));
        };
        QCOMPARE(thin->height(), 1);
        QCOMPARE(pixelAt(thin), color::border_1);
        QCOMPARE(pixelAt(strong), color::border_2);

        // The sidebar's right border.
        auto* side = add<Sidebar>();
        side->addGroup(QStringLiteral("Workflows"));
        side->addItem(QStringLiteral("a"), QStringLiteral("home"), QStringLiteral("Home"));
        side->setFixedHeight(200);
        QCoreApplication::processEvents();
        const QImage sideShot = side->grab().toImage();
        QCOMPARE(sideShot.pixelColor(side->width() - 1, 150), color::border_2);
        QVERIFY(sideShot.pixelColor(side->width() - 3, 150) != color::border_2);
    }

    // ---- the wheel ---------------------------------------------------------------------

    void wheelLeavesUnfocusedControlsAlone()
    {
        auto* other = add<QLineEdit>();
        QComboBox* combo = dropdown(m_window.get());
        m_window->layout()->addWidget(combo);
        combo->addItems({QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")});
        combo->show();
        auto* spin = add<QSpinBox>();
        spin->setRange(0, 10);
        spin->setValue(5);
        QCoreApplication::processEvents();
        QCOMPARE(combo->focusPolicy(), Qt::StrongFocus);
        QCOMPARE(spin->focusPolicy(), Qt::StrongFocus); // a bare Qt control, polished by the style

        other->setFocus();
        QTRY_VERIFY(other->hasFocus());
        sendWheel(combo);
        QCOMPARE(combo->currentIndex(), 0);
        sendWheel(spin);
        QCOMPARE(spin->value(), 5);

        // With keyboard focus the wheel works as before.
        combo->setFocus();
        QTRY_VERIFY(combo->hasFocus());
        sendWheel(combo);
        QCOMPARE(combo->currentIndex(), 1);
        spin->setFocus();
        QTRY_VERIFY(spin->hasFocus());
        sendWheel(spin);
        QCOMPARE(spin->value(), 4);
    }

    // ---- Enter -------------------------------------------------------------------------

    void enterPressesTheFocusedControl()
    {
        QPushButton* b = button(QStringLiteral("Go"), ButtonVariant::Primary, {}, m_window.get());
        m_window->layout()->addWidget(b);
        auto* icon = add<IconButton>(QStringLiteral("close"), QStringLiteral("Close"), IconButton::Size::Regular);
        auto* nav = add<NavItem>(QStringLiteral("k"), QStringLiteral("home"), QStringLiteral("Home"));
        auto* zone = add<DropZone>(QStringLiteral("folder"), QStringLiteral("Choose"), QStringLiteral("or drop"));
        auto* card = add<ActionCard>(QStringLiteral("folder"), QStringLiteral("Open"), QStringLiteral("text"));
        for (QAbstractButton* btn : std::initializer_list<QAbstractButton*>{b, icon, nav, zone, card}) {
            QSignalSpy spy(btn, &QAbstractButton::clicked);
            QTest::keyClick(btn, Qt::Key_Return);
            QCOMPARE(spy.count(), 1);
            QTest::keyClick(btn, Qt::Key_Enter);
            QCOMPARE(spy.count(), 2);
            QTest::keyClick(btn, Qt::Key_Return, Qt::ControlModifier); // a page shortcut, not a press
            QCOMPARE(spy.count(), 2);
        }
        // Toggles keep Space.
        auto* sw = add<Switch>();
        auto* check = add<QCheckBox>(QStringLiteral("Check"));
        QTest::keyClick(sw, Qt::Key_Return);
        QTest::keyClick(check, Qt::Key_Return);
        QVERIFY(!sw->isChecked());
        QVERIFY(!check->isChecked());
    }

    void enterInASheet()
    {
        // The focused button.
        auto* s = new Sheet(m_window.get(), QStringLiteral("Delete it?"), Sheet::Icon::Warning);
        s->addButton(QStringLiteral("Keep"), Sheet::Role::Safe);
        QPushButton* del = s->addButton(QStringLiteral("Delete"), Sheet::Role::Destructive);
        s->open();
        QTRY_VERIFY(QApplication::focusWidget() != nullptr);
        QCOMPARE(QApplication::focusWidget()->property("text").toString(), QStringLiteral("Keep")); // the safe one
        del->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(del->hasFocus());
        QTest::keyClick(del, Qt::Key_Return);
        QCOMPARE(s->result(), 1);
        QVERIFY(!s->isOpen());

        // Elsewhere in the sheet: the default button (primary when nothing is destructive).
        s = new Sheet(m_window.get(), QStringLiteral("Rename"), Sheet::Icon::None);
        QCheckBox* cb = s->addCheckbox(QStringLiteral("Also the folder"));
        auto* edit = new QLineEdit;
        s->body()->addWidget(edit);
        s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
        s->addButton(QStringLiteral("Rename"), Sheet::Role::Primary);
        s->open();
        cb->setFocus();
        QTest::keyClick(cb, Qt::Key_Return);
        QCOMPARE(s->result(), 1);
        QVERIFY(!cb->isChecked());

        // A destructive sheet: Enter in its text field does not press the safe button.
        s = new Sheet(m_window.get(), QStringLiteral("Type the name to delete"), Sheet::Icon::Warning);
        edit = new QLineEdit;
        s->body()->addWidget(edit);
        s->addButton(QStringLiteral("Keep"), Sheet::Role::Safe);
        s->addButton(QStringLiteral("Delete"), Sheet::Role::Destructive);
        s->open();
        edit->setFocus();
        QTRY_VERIFY(edit->hasFocus());
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(s->isOpen());
        QTest::keyClick(edit, Qt::Key_Escape);
        QVERIFY(!s->isOpen());
        QCOMPARE(s->result(), 0);
        QCOMPARE(Sheet::openCount(), 0);
    }

    // ---- a signal handler may delete the sender ---------------------------------------------

    void linkMayBeDeletedByItsSlot()
    {
        InfoBar* bar = add<InfoBar>(Tone::Info);
        bar->setMessage(QStringLiteral("Message"));
        Link* link = bar->addLink(QStringLiteral("Dismiss"));
        QPointer<Link> guard(link);
        int fired = 0;
        connect(link, &Link::activated, bar, [&] {
            ++fired;
            bar->clearLinks();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        });
        QCoreApplication::processEvents();
        QTest::mouseClick(link, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
        QCOMPARE(fired, 1);
        QCoreApplication::processEvents();
        QVERIFY(guard.isNull());
    }

    // ---- numbers ------------------------------------------------------------------------

    // A typed number may use the system's decimal separator; it is stored with a dot.
    void numberFieldsAcceptTheLocalesDecimalSeparator()
    {
        const QLocale german(QLocale::German, QLocale::Germany);
        const QLocale english(QLocale::English, QLocale::UnitedStates);
        QCOMPARE(german.decimalPoint(), QStringLiteral(","));
        QCOMPARE(SettingEditor::numberText(QStringLiteral(" 1,5 "), german), QStringLiteral("1.5"));
        QCOMPARE(SettingEditor::numberText(QStringLiteral("1.5"), german), QStringLiteral("1.5"));
        QCOMPARE(SettingEditor::numberText(QStringLiteral("12"), german), QStringLiteral("12"));
        // Not a number with one separator: left for the setting's own check to refuse.
        QCOMPARE(SettingEditor::numberText(QStringLiteral("1.234,5"), german), QStringLiteral("1.234,5"));
        QCOMPARE(SettingEditor::numberText(QStringLiteral("1,2,3"), german), QStringLiteral("1,2,3"));
        QCOMPARE(SettingEditor::numberText(QStringLiteral("1,5"), english), QStringLiteral("1,5"));

        SettingDef amount;
        amount.key = QStringLiteral("test.amount");
        amount.label = QStringLiteral("Amount");
        amount.type = SettingType::Double;
        SettingEditor ed(amount, m_window.get());
        auto* edit = ed.widget()->findChild<QLineEdit*>();
        QVERIFY(edit);
        QSignalSpy spy(&ed, &SettingEditor::edited);
        const QString sep = QLocale::system().decimalPoint();
        edit->setText(QStringLiteral("2") + sep + QStringLiteral("5"));
        emit edit->editingFinished();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QStringLiteral("2.5"));
        QCOMPARE(ed.value().toString(), QStringLiteral("2.5"));
        delete ed.widget();
    }

    // ---- the list editor ----------------------------------------------------------------

    void stringListEditorKeepsPatternsAsWritten()
    {
        // The definition says which kind of list it is, not the key's name.
        SettingDef patterns;
        patterns.key = QStringLiteral("test.folderPatterns");
        patterns.label = QStringLiteral("Leave out");
        patterns.type = SettingType::StringList;
        SettingEditor ed(patterns, m_window.get());
        auto* edit = ed.widget()->findChild<QLineEdit*>();
        QVERIFY(edit);
        // No folder picker for a list of patterns.
        QVERIFY(ed.widget()->findChildren<QPushButton*>().isEmpty());
        const QStringList stored{QStringLiteral("System Volume Information/"), QStringLiteral("sce_sys/*.bak")};
        ed.setValue(stored);
        QCOMPARE(edit->text(), QStringLiteral("System Volume Information/; sce_sys/*.bak"));
        QCOMPARE(ed.value().toStringList(), stored);

        // Leaving the field untouched is not an edit.
        QSignalSpy spy(&ed, &SettingEditor::edited);
        emit edit->editingFinished();
        QCOMPARE(spy.count(), 0);
        edit->setText(edit->text() + QStringLiteral("; *.tmp"));
        emit edit->editingFinished();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toStringList(), stored + QStringList{QStringLiteral("*.tmp")});
        emit edit->editingFinished();
        QCOMPARE(spy.count(), 1);

        // A list of folders keeps the picker and shows native separators.
        SettingDef folders;
        folders.key = QStringLiteral("general.libraryFolders");
        folders.label = QStringLiteral("Library folders");
        folders.type = SettingType::StringList;
        folders.folderList = true;
        SettingEditor fed(folders, m_window.get());
        QCOMPARE(fed.widget()->findChildren<QPushButton*>().size(), 1);
        fed.setValue(QStringList{QStringLiteral("D:/Dumps/A")});
        QCOMPARE(fed.widget()->findChild<QLineEdit*>()->text(), QDir::toNativeSeparators(QStringLiteral("D:/Dumps/A")));
        QSignalSpy fspy(&fed, &SettingEditor::edited);
        emit fed.widget()->findChild<QLineEdit*>()->editingFinished();
        QCOMPARE(fspy.count(), 0);
        delete ed.widget();
        delete fed.widget();
    }

    // ---- time left -----------------------------------------------------------------------

    void jobTimeLeftIsForTheWholeJob()
    {
        JobRecord r;
        r.id = QStringLiteral("j");
        r.state = JobState::Running;
        r.progress.phase = QStringLiteral("Pre-reading files");
        r.progress.phaseIndex = 0;
        r.progress.phaseCount = 4;
        r.progress.etaMs = 20'000;          // 20 s left in this stage
        r.progress.overallFraction = 0.10;  // a tenth of the job
        r.progress.elapsedMs = 60'000;      // after a minute
        QCOMPARE(jobTimeLeftMs(r), qint64(540'000)); // nine more minutes, not 20 s

        r.progress.overallFraction = 0.01; // too early to tell
        QCOMPARE(jobTimeLeftMs(r), qint64(-1));
        r.progress.overallFraction = 0.98; // never less than the stage needs
        QCOMPARE(jobTimeLeftMs(r), qint64(20'000));
        r.progress.phaseIndex = 3;         // the last stage: its estimate is the job's
        r.progress.overallFraction = 0.01;
        QCOMPARE(jobTimeLeftMs(r), qint64(20'000));
        r.state = JobState::Paused;
        QCOMPARE(jobTimeLeftMs(r), qint64(-1));

        // The row: a running job shows the job's estimate, a paused one no speed or time.
        r.state = JobState::Running;
        r.progress.phaseIndex = 0;
        r.progress.overallFraction = 0.10;
        r.progress.unitIsBytes = true;
        r.progress.ratePerSec = 50.0 * 1024 * 1024;
        JobRow row(JobRow::Variant::Queue);
        row.setRecord(r, 0, 1, true);
        auto note = [&row] {
            for (const QLabel* l : row.findChildren<QLabel*>())
                if (!l->toolTip().isEmpty() && !qobject_cast<const ElideLabel*>(l) && l->toolTip().startsWith(QStringLiteral("<p")))
                    return l->toolTip();
            return QString();
        };
        QVERIFY2(note().contains(QStringLiteral("About 9 min left")), qPrintable(note()));
        QVERIFY2(note().contains(QStringLiteral("Pre-reading files")), qPrintable(note()));
        r.state = JobState::Paused;
        row.setRecord(r, 0, 1, true);
        QVERIFY2(note().contains(QStringLiteral("Paused in Pre-reading files")), qPrintable(note()));
        QVERIFY2(!note().contains(QStringLiteral("left")), qPrintable(note()));
    }

    // A record the reporter made: its smoothed estimate and nothing made up in
    // its place; a row says everything in its tooltip; a stage without a size
    // says how much it did; 100 % only for a job that succeeded.
    void rowTelemetryIsCompleteAndHonest()
    {
        JobRecord r;
        r.id = QStringLiteral("t");
        r.state = JobState::Running;
        r.progress.phase = QStringLiteral("Packing");
        r.progress.phaseNames = {QStringLiteral("Preparing"), QStringLiteral("Packing"), QStringLiteral("Checking packs")};
        r.progress.phaseIndex = 1;
        r.progress.phaseCount = 3;
        r.progress.etaMs = 20'000;
        r.progress.jobEtaMs = -1; // the reporter cannot tell yet
        r.progress.overallFraction = 0.4;
        r.progress.elapsedMs = 60'000;
        QCOMPARE(jobTimeLeftMs(r), qint64(-1)); // no rough guess in its place
        r.progress.jobEtaMs = 95'000;
        QCOMPARE(jobTimeLeftMs(r), qint64(95'000));

        r.progress.unitIsBytes = true;
        r.progress.total = qint64(10) << 30;
        r.progress.done = qint64(4) << 30;
        r.progress.filesTotal = 100;
        r.progress.filesDone = 40;
        r.progress.ratePerSec = 200.0 * 1024 * 1024;
        r.progress.averageRatePerSec = 180.0 * 1024 * 1024;
        const QString d = runningDetailText(r);
        for (const QString& part : {QStringLiteral("Packing"), QStringLiteral("4.0 GB of 10.0 GB"), QStringLiteral("40 of 100 files"),
                                    QStringLiteral("200 MB/s"), QStringLiteral("avg 180 MB/s"), QStringLiteral("About 2 min left")})
            QVERIFY2(d.contains(part), qPrintable(d + QStringLiteral(" lacks ") + part));
        JobRow row(JobRow::Variant::Queue);
        row.setRecord(r, 0, 1, true);
        QString tips;
        for (const QLabel* l : row.findChildren<QLabel*>())
            tips += l->toolTip() + u'|';
        QVERIFY2(tips.contains(QStringLiteral("avg 180 MB/s")) && tips.contains(QStringLiteral("40 of 100 files")), qPrintable(tips));
        // Paused: how far it got, no speed, no time left.
        r.state = JobState::Paused;
        const QString paused = runningDetailText(r);
        QVERIFY2(paused.contains(QStringLiteral("4.0 GB of 10.0 GB")) && !paused.contains(QStringLiteral("MB/s"))
                     && !paused.contains(QStringLiteral("left")),
                 qPrintable(paused));

        // A stage without a size says how much it has done, not a percentage.
        ProgressSnapshot open;
        open.indeterminate = true;
        open.done = qint64(3) << 30;
        open.filesDone = 12;
        QCOMPARE(progressDetailText(open, -1), QStringLiteral("3.0 GB · 12 files"));
        // Never more than the total, whatever a snapshot made by hand says.
        ProgressSnapshot over;
        over.total = 100;
        over.done = 150;
        over.unitIsBytes = false;
        QCOMPARE(progressDetailText(over, -1), QStringLiteral("100 of 100"));

        // 100 % only for a job that succeeded.
        JobRecord f = r;
        f.state = JobState::Failed;
        f.progress.overallFraction = 1.0; // what the queue leaves after a failed verification
        QCOMPARE(thinPercent(shownFraction(f)), thinPercent(0.99));
        f.state = JobState::Succeeded;
        QCOMPARE(shownFraction(f), 1.0);
        f.state = JobState::Running;
        f.progress.overallFraction = std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(shownFraction(f), 0.0);
        // The stages the progress counts in: the reporter's, else the declared ones.
        QCOMPARE(stageNames(r), r.progress.phaseNames);
        JobRecord q;
        q.phases = {QStringLiteral("A")};
        QCOMPARE(stageNames(q), q.phases);
    }

    // ---- Library grid --------------------------------------------------------------------

    void keyboardContextMenuIsForTheCurrentItem()
    {
        auto* grid = add<LibraryGrid>();
        std::vector<GridItem> items;
        for (int i = 0; i < 40; ++i) {
            GridItem it;
            it.key = QStringLiteral("key%1").arg(i);
            it.title = QStringLiteral("Game %1").arg(i);
            items.push_back(it);
        }
        grid->setItems(items);
        grid->resize(800, grid->heightForWidth(800));
        grid->setFocus();
        QTest::keyClick(grid, Qt::Key_Home);
        QTest::keyClick(grid, Qt::Key_Right);
        QCOMPARE(grid->currentKey(), QStringLiteral("key1"));
        grid->setSelectedKeys({QStringLiteral("key0"), QStringLiteral("key1"), QStringLiteral("key2")});

        QSignalSpy spy(grid, &LibraryGrid::contextRequested);
        // Qt reports the middle of the widget for the Menu key.
        QContextMenuEvent menu(QContextMenuEvent::Keyboard, grid->rect().center(), grid->mapToGlobal(grid->rect().center()));
        QApplication::sendEvent(grid, &menu);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QStringLiteral("key1"));
        QCOMPARE(grid->selectedKeys().size(), 3); // the selection it belongs to stays

        // With the mouse it is the item under the pointer, as before.
        spy.clear();
        QContextMenuEvent mouse(QContextMenuEvent::Mouse, QPoint(30, 30), grid->mapToGlobal(QPoint(30, 30)));
        QApplication::sendEvent(grid, &mouse);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QStringLiteral("key0"));
    }

    void coversAreDecodedOncePerFile()
    {
        const QString good = m_root.filePath(QStringLiteral("cover.png"));
        QImage img(900, 600, QImage::Format_RGB32);
        img.fill(Qt::red);
        QVERIFY(img.save(good));
        const QString missing = m_root.filePath(QStringLiteral("gone/cover.png"));

        auto* grid = add<LibraryGrid>();
        GridItem a;
        a.key = QStringLiteral("a");
        a.title = QStringLiteral("With cover");
        a.coverPath = good;
        GridItem b;
        b.key = QStringLiteral("b");
        b.title = QStringLiteral("Cover gone");
        b.coverPath = missing;
        grid->setItems({a, b});
        grid->resize(600, 400);

        const int before = LibraryGrid::coverDecodeCount();
        const QImage first = grid->grab().toImage();
        QCOMPARE(LibraryGrid::coverDecodeCount(), before + 2); // one read each
        // The cover is painted (red), cut to the tile.
        QCOMPARE(first.pixelColor(60, 60), QColor(Qt::red));

        // Repaints, another window size and the list view do not go back to the disk,
        // and the unreadable file is not tried again.
        grid->grab();
        grid->resize(900, 500);
        grid->grab();
        grid->setMode(LibraryGrid::Mode::List);
        grid->grab();
        QCOMPARE(LibraryGrid::coverDecodeCount(), before + 2);

        // Setting the items again (a rescan) gives the missing cover another try.
        grid->setItems({a, b});
        grid->grab();
        QCOMPARE(LibraryGrid::coverDecodeCount(), before + 3);
    }

    void shiftArrowExtendsFromTheKeyboardPosition()
    {
        auto* grid = add<LibraryGrid>();
        std::vector<GridItem> items;
        for (int i = 0; i < 12; ++i) {
            GridItem it;
            it.key = QStringLiteral("key%1").arg(i);
            it.title = QStringLiteral("Game %1").arg(i);
            items.push_back(it);
        }
        grid->setItems(items);
        grid->setMode(LibraryGrid::Mode::List);
        grid->resize(800, grid->heightForWidth(800));
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, QPoint(200, 34 + 56 + 20)); // the second row
        QCOMPARE(grid->selectedKeys(), QStringList{QStringLiteral("key1")});
        for (int i = 0; i < 4; ++i)
            QTest::keyClick(grid, Qt::Key_Down);
        QCOMPARE(grid->currentKey(), QStringLiteral("key5"));
        QTest::keyClick(grid, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(grid->selectedKeys(), (QStringList{QStringLiteral("key5"), QStringLiteral("key6")})); // not key1..key6
    }

    // ---- sheets ------------------------------------------------------------------------------

    void sheetDestroyedWhileOpenGivesNoAnswer()
    {
        // Destroyed with its host while exec() runs: no answer, nothing touched afterwards.
        auto* host = new QWidget(m_window.get());
        host->resize(600, 400);
        host->show();
        bool checked = true;
        QTimer::singleShot(0, [host] { delete host; });
        QVERIFY(!Sheet::confirm(host, QStringLiteral("Delete?"), QStringLiteral("text"), QStringLiteral("Keep"), QStringLiteral("Delete"),
                                true, Sheet::Icon::Warning, QStringLiteral("Do not ask again"), &checked));
        QCOMPARE(checked, true); // left as it was
        QCOMPARE(Sheet::openCount(), 0);

        auto* host2 = new QWidget(m_window.get());
        auto* s = new Sheet(host2, QStringLiteral("Done"), Sheet::Icon::Info);
        s->addButton(QStringLiteral("Open folder"), Sheet::Role::Primary);
        QTimer::singleShot(0, [host2] { delete host2; });
        QCOMPARE(s->exec(), -1);
    }

    void sheetHiddenFromOutsideIsNotCountedAsOpen()
    {
        auto* s = new Sheet(m_window.get(), QStringLiteral("Countdown"), Sheet::Icon::Info);
        s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        s->open();
        QCOMPARE(Sheet::openCount(), 1);
        s->hide(); // not through close()
        QCOMPARE(Sheet::openCount(), 0);
        delete s;

        // An elided label tells assistive tools its whole text and elides again for a new font.
        auto* l = add<ElideLabel>(QStringLiteral("A rather long title that will not fit"), Qt::ElideRight);
        l->setFixedWidth(120);
        QCoreApplication::processEvents();
        QCOMPARE(l->accessibleName(), l->fullText());
        const QString before = l->text();
        QFont big = l->font();
        big.setPixelSize(28);
        l->setFont(big);
        QCoreApplication::processEvents();
        QVERIFY(l->text() != before);
        l->setFullText(QStringLiteral("Another title"));
        QCOMPARE(l->accessibleName(), QStringLiteral("Another title"));

        // A card has one title row however often its title is set.
        Card card;
        QHBoxLayout* row = card.setTitle(QStringLiteral("One"));
        QCOMPARE(card.setTitle(QStringLiteral("Two")), row);
        int titles = 0;
        for (const QLabel* t : card.findChildren<QLabel*>())
            if (t->text() == QStringLiteral("One") || t->text() == QStringLiteral("Two"))
                ++titles;
        QCOMPARE(titles, 1);
    }

    void sheetPanelIsAsTallAsItsWrappedText()
    {
        auto* host = add<QWidget>();
        host->setFixedSize(900, 600);
        QCoreApplication::processEvents();
        auto* s = new Sheet(host, QStringLiteral("A long message"), Sheet::Icon::Info);
        QString text;
        for (int i = 0; i < 14; ++i)
            text += QStringLiteral("the output folder is not there any more ");
        QLabel* body = s->addText(text);
        QPushButton* ok = s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        s->open();
        QCoreApplication::processEvents();
        QWidget* panel = s->findChild<QWidget*>(QStringLiteral("sheetPanel"));
        QVERIFY(panel);
        // The label got the height its text needs at the panel's width, and the
        // button is inside the panel and the window.
        QVERIFY2(body->height() >= body->heightForWidth(body->width()), qPrintable(QString::number(body->height())));
        QVERIFY(panel->rect().contains(ok->geometry()));
        QVERIFY(panel->geometry().bottom() <= host->height());
        s->close();

        // Taller than the window: clamped, the button still on screen.
        s = new Sheet(host, QStringLiteral("An endless message"), Sheet::Icon::Info);
        for (int i = 0; i < 5; ++i)
            text += text;
        s->addText(text);
        ok = s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        s->open();
        QCoreApplication::processEvents();
        panel = s->findChild<QWidget*>(QStringLiteral("sheetPanel"));
        QVERIFY(panel->geometry().bottom() <= host->height());
        QVERIFY(panel->geometry().top() >= 0);
        s->close();
    }

    void toastsStayUnderAnOpenSheet()
    {
        auto* host = add<QWidget>();
        host->setFixedSize(900, 600);
        ToastHost toasts(host);
        auto* s = new Sheet(host, QStringLiteral("Cancel the build?"), Sheet::Icon::Warning);
        s->addButton(QStringLiteral("Keep building"), Sheet::Role::Safe);
        s->open();
        toasts.show(Tone::Ok, QStringLiteral("Image built"), QStringLiteral("body"), {}, 0);
        QFrame* toast = host->findChild<QFrame*>(QStringLiteral("toast"));
        QVERIFY(toast);
        const QObjectList order = host->children();
        QVERIFY(order.indexOf(toast) < order.indexOf(s)); // lower in the stack
        s->close();
        toasts.clear();
    }

    // ---- focus ring -----------------------------------------------------------------------

    void focusRingIsForKeyboardFocusAndStaysAboveASheet()
    {
        auto* ring = new FocusRingOverlay(m_window.get());
        QPushButton* first = button(QStringLiteral("First"), ButtonVariant::Secondary, {}, m_window.get());
        QPushButton* second = button(QStringLiteral("Second"), ButtonVariant::Secondary, {}, m_window.get());
        m_window->layout()->addWidget(first);
        m_window->layout()->addWidget(second);
        auto* edit = add<QLineEdit>();
        QCoreApplication::processEvents();

        // Focus that arrives the keyboard way (Tab, or code that moves it for
        // an arrow key or a sheet): a ring.
        second->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(second->hasFocus());
        QVERIFY(!ring->isVisible());
        QVERIFY(!hasKeyboardFocus(second));
        first->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasFocus());
        QVERIFY(hasKeyboardFocus(first));
        QTRY_VERIFY(ring->isVisible());
        // A click on another button: focus moves, no ring any more.
        QTest::mouseClick(second, Qt::LeftButton);
        QTRY_VERIFY(second->hasFocus());
        QVERIFY(!hasKeyboardFocus(second));
        QTRY_VERIFY(!ring->isVisible());
        QTest::keyClick(second, Qt::Key_Backtab);
        QTRY_VERIFY(first->hasFocus());
        QTRY_VERIFY(ring->isVisible());
        // A text field has one whenever it has the focus.
        QTest::mouseClick(edit, Qt::LeftButton);
        QTRY_VERIFY(edit->hasFocus());
        QTRY_VERIFY(ring->isVisible());

        // A sheet opens while the ring is showing: the ring moves to the
        // sheet's button and is drawn above the sheet, not under it.
        auto* s = new Sheet(m_window.get(), QStringLiteral("Cancel the build?"), Sheet::Icon::Warning);
        s->addButton(QStringLiteral("Keep building"), Sheet::Role::Safe);
        s->addButton(QStringLiteral("Cancel build"), Sheet::Role::Destructive);
        s->open();
        QTRY_VERIFY(ring->isVisible());
        const QObjectList order = m_window->children();
        QVERIFY(order.indexOf(ring) > order.indexOf(s));
        s->close();
        delete ring;
    }

    // ---- small widgets -------------------------------------------------------------------

    void badgeWidthFollowsItsGlyphAndLongTextGetsATooltip()
    {
        Badge b(QStringLiteral("Ready"), Tone::Neutral);
        const int plain = b.sizeHint().width();
        b.setTone(Tone::Ok); // the same text, now with a glyph
        QVERIFY(b.sizeHint().width() > plain);
        QVERIFY(b.testAttribute(Qt::WA_TransparentForMouseEvents));
        QVERIFY(b.toolTip().isEmpty());
        const QString longText = QStringLiteral("Verification failed: the image digest does not match <b>");
        b.setText(longText);
        QCOMPARE(b.toolTip(), plainTip(longText));
        QVERIFY(!b.testAttribute(Qt::WA_TransparentForMouseEvents)); // or the tooltip could never show
    }

    // A button has the font of the variant it has now, not the one it was made
    // with, also after a later restyle (the small size); a value field keeps
    // its mono font when it shows an error.
    void aButtonHasTheFontOfItsVariant()
    {
        const int strong = theme::font::bodyStrong().weight(), plain = theme::font::body().weight();
        QVERIFY(strong != plain);
        auto* host = add<QWidget>();
        auto* h = hbox(host, 8);
        QPushButton* again = button(QStringLiteral("Run again"), ButtonVariant::Primary, QStringLiteral("refresh"), host);
        QPushButton* main = button(QStringLiteral("Connect"), ButtonVariant::Secondary, {}, host);
        QLineEdit* path = textInput(QStringLiteral("Next to the source folder"), true, host);
        // Made without a parent and changed before it is first shown (the
        // polish then must not put back the font it was made with).
        QPushButton* early = button(QStringLiteral("Install"), ButtonVariant::Secondary, {}, nullptr);
        setButtonVariant(early, ButtonVariant::Primary);
        h->addWidget(again);
        h->addWidget(main);
        h->addWidget(path);
        h->addWidget(early);
        QCoreApplication::processEvents();
        QTRY_VERIFY(early->isVisible());
        QCOMPARE(early->font().weight(), strong);
        QCOMPARE(again->font().weight(), strong);
        setButtonVariant(again, ButtonVariant::Secondary);
        QCOMPARE(again->font().weight(), plain);
        setButtonSmall(again, true);
        QCOMPARE(again->font().weight(), plain);
        setButtonVariant(main, ButtonVariant::Primary);
        QCOMPARE(main->font().weight(), strong);
        setButtonSmall(main, true);
        QCOMPARE(main->font().weight(), strong);
        setButtonVariant(main, ButtonVariant::Secondary);
        QCOMPARE(main->font().weight(), plain);

        QCOMPARE(path->font().family(), theme::font::body().family()); // the placeholder is a sentence
        path->setText(QStringLiteral("D:\\Images"));
        QCOMPARE(path->font().family(), theme::font::mono().family());
        setInputError(path, true);
        QCOMPARE(path->font().family(), theme::font::mono().family());
    }

    // A sorted column's chevron follows its label (mirrored right to left), not
    // the far end of a wide column, where it reads as the next column's.
    void sortChevronFollowsTheLabel()
    {
        for (const Qt::LayoutDirection dir : {Qt::LeftToRight, Qt::RightToLeft}) {
            auto* table = add<QTableWidget>(2, 2);
            table->setLayoutDirection(dir);
            table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size")});
            table->setFixedWidth(700);
            QHeaderView* header = table->horizontalHeader();
            header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            header->setStretchLastSection(false);
            header->setSectionResizeMode(0, QHeaderView::Stretch);
            header->resizeSection(1, 120);
            header->setSortIndicatorShown(true);
            header->setSortIndicator(0, Qt::AscendingOrder);
            QCoreApplication::processEvents();
            const QRect section(header->sectionViewportPosition(0), 0, header->sectionSize(0), header->height());
            QVERIFY(section.width() > 400);
            const QImage with = header->grab().toImage();
            header->setSortIndicatorShown(false);
            const QImage without = header->grab().toImage();
            header->setSortIndicatorShown(true);
            QRect chevron, text;
            const QRgb bg = without.pixel(section.center().x(), 3);
            for (int y = 0; y < with.height() - 2; ++y) // the bottom rule is not the label
                for (int x = section.left(); x <= section.right(); ++x) {
                    if (with.pixel(x, y) != without.pixel(x, y))
                        chevron |= QRect(x, y, 1, 1);
                    else if (without.pixel(x, y) != bg)
                        text |= QRect(x, y, 1, 1);
                }
            const QString where = QStringLiteral("%1: text %2..%3, chevron %4..%5")
                                      .arg(dir == Qt::LeftToRight ? QStringLiteral("LTR") : QStringLiteral("RTL"))
                                      .arg(text.left()).arg(text.right()).arg(chevron.left()).arg(chevron.right());
            QVERIFY2(chevron.isValid() && text.isValid(), qPrintable(where));
            QVERIFY2(chevron.width() <= theme::size::iconSm && chevron.height() <= theme::size::iconSm, qPrintable(where));
            const int gap = dir == Qt::LeftToRight ? chevron.left() - text.right() - 1 : text.left() - chevron.right() - 1;
            QVERIFY2(gap >= 2 && gap <= 12, qPrintable(where));
            QVERIFY2(qAbs(chevron.center().y() - text.center().y()) <= 2, qPrintable(where)); // on the label's line
            delete table;
        }
    }

    // The chosen tile's check sits on its icon's centre line, 14 px in from the
    // tile's end (as OptionTiles has it), mirrored right to left.
    void choiceTileCheckSitsOnTheIconLine()
    {
        for (const Qt::LayoutDirection dir : {Qt::LeftToRight, Qt::RightToLeft}) {
            auto* tiles = add<ChoiceTiles>();
            tiles->setLayoutDirection(dir);
            tiles->addChoice(QStringLiteral("exFAT"), QStringLiteral("For ShadowMount+ and PCs."), QStringLiteral("drive"), QStringLiteral("exfat"));
            tiles->addChoice(QStringLiteral("FPKG"), QStringLiteral("An installable package."), QStringLiteral("package"), QStringLiteral("fpkg"));
            tiles->setFixedWidth(600);
            QCoreApplication::processEvents();
            QCOMPARE(tiles->currentIndex(), 0);
            // Two tiles of (600 - 12) / 2 px, the first at the start of the row.
            const int tileW = (600 - 12) / 2;
            const QRect tile = dir == Qt::LeftToRight ? QRect(0, 0, tileW, 104) : QRect(600 - tileW, 0, tileW, 104);
            const QImage img = tiles->grab().toImage();
            // The bubble: bright and coloured, inside the tile's border and its
            // rounded corners, in its trailing half (the icon square is at the
            // leading end).
            const QRect area = dir == Qt::LeftToRight ? QRect(tile.center().x(), 10, tileW / 2 - 8, 84) : QRect(tile.left() + 8, 10, tileW / 2 - 8, 84);
            QRect bubble;
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.valueF() > 0.6 && c.hsvSaturationF() > 0.45)
                        bubble |= QRect(x, y, 1, 1);
                }
            const QString where = QStringLiteral("%1: tile %2..%3, bubble x %4..%5 y %6..%7")
                                      .arg(dir == Qt::LeftToRight ? QStringLiteral("LTR") : QStringLiteral("RTL"))
                                      .arg(tile.left()).arg(tile.right()).arg(bubble.left()).arg(bubble.right())
                                      .arg(bubble.top()).arg(bubble.bottom());
            QVERIFY2(bubble.isValid() && bubble.width() <= 22, qPrintable(where));
            const int iconCentreY = 14 + 34 / 2;
            QVERIFY2(qAbs(bubble.center().y() - iconCentreY) <= 2, qPrintable(where));
            const int inset = dir == Qt::LeftToRight ? tile.right() - bubble.right() : bubble.left() - tile.left();
            QVERIFY2(inset >= 13 && inset <= 16, qPrintable(where));
            delete tiles;
        }
    }

    // The header's status chips are whole: a badge cuts a text after 32
    // characters, but "1 running · 0 queued · then shut down" (or an Arabic
    // "1 running · 0 queued") is longer and the header has the room.
    void headerChipsSayItAll()
    {
        auto* header = add<AppHeader>();
        const QString queue = QStringLiteral("1 running · 0 queued · then shut down");
        const QString console = QStringLiteral("PS5 connected · 192.168.100.200 · FTP 2121");
        QVERIFY(queue.size() > 32 && console.size() > 32);
        header->setQueueChip(queue, Tone::Warn, true);
        header->setConsoleChip(console, Tone::Ok, true);
        QCoreApplication::processEvents();
        QCOMPARE(header->queueChip()->sizeHint().width(), badgeWidth(queue, true));
        QCOMPARE(header->consoleChip()->sizeHint().width(), badgeWidth(console, true));
        QTRY_COMPARE(header->queueChip()->width(), header->queueChip()->sizeHint().width());
        QVERIFY(header->queueChip()->toolTip().isEmpty()); // nothing cut, nothing to explain
        // An ordinary badge still keeps to 32 characters and says the rest as its tooltip.
        Badge plain(QString(), Tone::Warn);
        plain.setText(queue);
        QVERIFY(plain.sizeHint().width() < badgeWidth(queue, true));
        QCOMPARE(plain.toolTip(), plainTip(queue));
    }

    // A badge squeezed below its width (a table column too narrow for a long
    // translation) ends in "…" inside its outline and says it all as a tooltip.
    void aSqueezedBadgeSaysItAllAsATooltip()
    {
        auto* host = add<QWidget>();
        auto* h = hbox(host, 0);
        auto* badge = new Badge(QStringLiteral("Größe abweichend"), Tone::Warn, host);
        h->addWidget(badge);
        h->addStretch(1);
        QCoreApplication::processEvents();
        QTRY_COMPARE(badge->width(), badge->sizeHint().width());
        QVERIFY(badge->toolTip().isEmpty());
        QVERIFY(badge->testAttribute(Qt::WA_TransparentForMouseEvents));
        host->setFixedWidth(badge->sizeHint().width() - 30);
        QTRY_VERIFY(badge->width() < badge->sizeHint().width());
        QCOMPARE(badge->toolTip(), plainTip(QStringLiteral("Größe abweichend")));
        QVERIFY(!badge->testAttribute(Qt::WA_TransparentForMouseEvents));
        // The outline is drawn at the badge's own (narrower) edge, with the text inside it.
        const QImage img = badge->grab().toImage();
        QVERIFY(img.width() == badge->width());
        host->setFixedWidth(badge->sizeHint().width() + 40);
        QTRY_COMPARE(badge->width(), badge->sizeHint().width());
        QVERIFY(badge->toolTip().isEmpty());
    }

    // The search glass (QLineEdit::addAction) is centred in its 32 px field:
    // the 32 px box of tool buttons does not apply to a field's own icon.
    void aFieldsIconIsCentredInIt()
    {
        auto* edit = textInput(QStringLiteral("Search messages"), false, m_window.get());
        m_window->layout()->addWidget(edit);
        edit->addAction(QIcon(iconPixmap(QStringLiteral("search"), 16, theme::color::fg_3, 1.0)), QLineEdit::LeadingPosition);
        edit->show();
        QCoreApplication::processEvents();
        auto* glass = edit->findChild<QToolButton*>();
        QVERIFY(glass);
        QTRY_VERIFY(glass->isVisible());
        QVERIFY2(std::abs(glass->geometry().center().y() - edit->rect().center().y()) <= 1,
                 qPrintable(QStringLiteral("glass %1, field %2").arg(glass->geometry().center().y()).arg(edit->rect().center().y())));
        QVERIFY(glass->height() < edit->height());
    }

    // A field whose text is wider than it: unfocused it shows the start and
    // ends in "…", and its tooltip is the whole text. A setting's text shows
    // its start after a value was set (not its tail).
    void aCutFieldShowsItsStartAndSaysItAll()
    {
        const QString pattern = QStringLiteral("System Volume Information/; $RECYCLE.BIN/; Thumbs.db; desktop.ini; *.bak; *.tmp; ~*");
        SettingDef def;
        def.key = QStringLiteral("test.template");
        def.label = QStringLiteral("Template");
        def.type = SettingType::String;
        SettingEditor ed(def, m_window.get());
        m_window->layout()->addWidget(ed.widget());
        ed.widget()->show();
        auto* edit = ed.widget()->findChild<QLineEdit*>();
        QVERIFY(edit);
        ed.setValue(pattern);
        QCOMPARE(edit->cursorPosition(), 0);
        QCoreApplication::processEvents();
        QVERIFY(isTextCut(edit));
        QHelpEvent tip(QEvent::ToolTip, edit->rect().center(), edit->mapToGlobal(edit->rect().center()));
        QApplication::sendEvent(edit, &tip);
        QTRY_COMPARE(QToolTip::text(), plainTip(pattern));
        QToolTip::hideText();
        ed.setValue(QStringLiteral("OSFIMG"));
        QVERIFY(!isTextCut(edit));
        delete ed.widget();
    }

    // Number fields carry their unit inside, so every control of a settings
    // column ends on one line; a 0 that means "Automatic" or "Off" says so.
    void settingControlsEndOnOneLineAndZeroSaysWhatItMeans()
    {
        auto* host = add<QWidget>();
        host->setFixedWidth(820);
        auto* v = vbox(host, 0);
        SettingDef size;
        size.key = QStringLiteral("exfat.imageSizeGiB");
        size.label = QStringLiteral("Image size");
        size.type = SettingType::Double;
        size.unit = QStringLiteral("GB");
        SettingDef wait;
        wait.key = QStringLiteral("general.autoRetryDelay");
        wait.label = QStringLiteral("Wait before retrying");
        wait.type = SettingType::Int;
        wait.unit = QStringLiteral("connections");
        SettingDef retries;
        retries.key = QStringLiteral("general.autoRetryCount");
        retries.label = QStringLiteral("Retry failed jobs automatically");
        retries.type = SettingType::Int;
        SettingDef replace;
        replace.key = QStringLiteral("test.replace");
        replace.label = QStringLiteral("Replace an existing image");
        replace.type = SettingType::Bool;
        QList<SettingRow*> rows;
        for (const SettingDef* d : {&size, &wait, &retries, &replace}) {
            auto* row = new SettingRow(*d, host);
            v->addWidget(row);
            rows << row;
        }
        rows[0]->editor()->setValue(0);
        rows[1]->editor()->setValue(30);
        rows[2]->editor()->setValue(0);
        QCoreApplication::processEvents();
        auto rightEdge = [host](SettingRow* r) {
            QWidget* w = r->editor()->widget();
            if (auto* e = w->findChild<QLineEdit*>())
                w = e;
            return w->mapTo(host, QPoint(w->width(), 0)).x();
        };
        QTRY_COMPARE(rightEdge(rows[0]), rightEdge(rows[3]));
        QCOMPARE(rightEdge(rows[1]), rightEdge(rows[3]));
        QCOMPARE(rightEdge(rows[2]), rightEdge(rows[3]));
        auto* sizeEdit = rows[0]->editor()->widget()->findChild<QLineEdit*>();
        auto* waitEdit = rows[1]->editor()->widget()->findChild<QLineEdit*>();
        auto* retryEdit = rows[2]->editor()->widget()->findChild<QLineEdit*>();
        QCOMPARE(inputSuffix(sizeEdit), QStringLiteral("GB"));
        QCOMPARE(inputSuffix(waitEdit), QStringLiteral("connections"));
        QVERIFY(sizeEdit->textMargins().right() > QFontMetrics(sizeEdit->font()).horizontalAdvance(QStringLiteral("GB")));
        for (QLabel* l : host->findChildren<QLabel*>())
            QVERIFY2(l->text() != QStringLiteral("GB"), "the unit is inside the field, not a label beside it");
        // What 0 means, beside the number; a real number has no note.
        QCOMPARE(inputNote(sizeEdit), QStringLiteral("Automatic"));
        QCOMPARE(inputNote(retryEdit), QStringLiteral("Off"));
        QCOMPARE(inputNote(waitEdit), QString());
        rows[0]->editor()->setValue(64);
        QCOMPARE(inputNote(sizeEdit), QString());
        QCOMPARE(sizeEdit->text(), QStringLiteral("64"));
        QCOMPARE(SettingRow::displayValue(retries, 0), QStringLiteral("Off"));
        QCOMPARE(SettingRow::displayValue(size, 64), QStringLiteral("64 GB"));
    }

    // A dropdown is as wide as its longest choice, up to 320 px; a choice
    // with a description keeps its name as the short text the closed field
    // shows when the whole text does not fit.
    void dropdownsFitTheirChoices()
    {
        SettingDef def;
        def.key = QStringLiteral("test.naming");
        def.label = QStringLiteral("File name");
        def.type = SettingType::Enum;
        def.choices = {{QStringLiteral("id"), QStringLiteral("Title ID, title and version"), QStringLiteral("PPSA01325 Astro Bot v01.003.000 (example)")},
                       {QStringLiteral("custom"), QStringLiteral("Custom template"), {}}};
        SettingEditor ed(def, m_window.get());
        auto* combo = qobject_cast<QComboBox*>(ed.widget());
        QVERIFY(combo);
        QCOMPARE(combo->maximumWidth(), 320);
        QCOMPARE(combo->minimumWidth(), 160);
        QCOMPARE(combo->itemData(0, kShortTextRole).toString(), QStringLiteral("Title ID, title and version"));
        QVERIFY(!combo->itemData(1, kShortTextRole).isValid());
        QVERIFY(combo->itemText(0).startsWith(QStringLiteral("Title ID, title and version · ")));
        delete ed.widget();
    }

    // The first sentence of a description is the row's help: also in Hindi
    // (the danda ends it), and a long one ends on a whole clause, never inside
    // a bracket or in the middle of a word.
    void rowHelpIsTheFirstSentenceInEveryScript()
    {
        QCOMPARE(SettingRow::conciseHelp(QStringLiteral("Short. Second sentence.")), QStringLiteral("Short."));
        QCOMPARE(SettingRow::conciseHelp(QStringLiteral("पहला वाक्य। दूसरा वाक्य।")), QStringLiteral("पहला वाक्य।"));
        QCOMPARE(SettingRow::conciseHelp(QStringLiteral("هل تريد هذا؟ نعم")), QStringLiteral("هل تريد هذا؟"));
        QCOMPARE(SettingRow::conciseHelp(QStringLiteral("一文目。二文目。")), QStringLiteral("一文目。"));
        const QString clause = QStringLiteral("Seconds between a failure and the automatic retry of the job that failed, for example to let a "
                                              "drive or a network share come back after it went away for a while. More.");
        const QString clauseHelp = SettingRow::conciseHelp(clause);
        QCOMPARE(clauseHelp, QStringLiteral("Seconds between a failure and the automatic retry of the job that failed…"));
        // No clause break before the limit: the whole sentence, which wraps.
        const QString plain = QStringLiteral("A file is stored compressed only if the whole file shrinks by at least this much and the folder "
                                             "image is built with the compressed form of every file that shrinks");
        QVERIFY(plain.size() > 140);
        QCOMPARE(SettingRow::conciseHelp(plain + QStringLiteral(". Then more.")), plain + QStringLiteral("."));
        // A comma inside brackets is not where the row ends: the sentence stays whole.
        const QString bracket = QStringLiteral("Folders whose name matches are left out of every image you build from now on (one level too, so a "
                                               "pattern for a folder also leaves out what is in it, and its files) and the rest goes in.");
        QCOMPARE(SettingRow::conciseHelp(bracket + QStringLiteral(" More.")), bracket);
        // A very long one is cut before the bracket, not inside it.
        const QString huge = bracket.chopped(1) + QStringLiteral(" and then the description goes on for a long time without any stop at all "
                                                                 "so that it is much longer than two lines of a settings row could ever hold");
        QVERIFY(huge.size() > 280);
        QCOMPARE(SettingRow::conciseHelp(huge), QStringLiteral("Folders whose name matches are left out of every image you build from now on…"));
    }

    // In a right-to-left paragraph a sign at the end of a Latin name stays
    // with the name ("ShadowMount+", not "+ShadowMount").
    void namesKeepTheirSignsRightToLeft()
    {
        const QChar lrm(0x200E);
        QCOMPARE(bidiSafe(QStringLiteral("ShadowMount+")), QStringLiteral("ShadowMount+") + lrm);
        QCOMPARE(bidiSafe(QStringLiteral("إعدادات ShadowMount+ للجهاز")), QStringLiteral("إعدادات ShadowMount+") + lrm + QStringLiteral(" للجهاز"));
        QCOMPARE(bidiSafe(QStringLiteral("C++ and C#")), QStringLiteral("C++") + lrm + QStringLiteral(" and C#") + lrm);
        QCOMPARE(bidiSafe(QStringLiteral("a + b")), QStringLiteral("a + b"));
        QCOMPARE(bidiSafe(QStringLiteral("+2")), QStringLiteral("+2"));
        QCOMPARE(bidiSafe(QStringLiteral("x+y")), QStringLiteral("x+y"));
        const QString once = bidiSafe(QStringLiteral("ShadowMount+"));
        QCOMPARE(bidiSafe(once), once);
    }

    // Right to left, the items of a list, a tree, a table and a dropdown read
    // as a label reads them: a Latin sentence keeps its full stop at its end,
    // a name its sign, a path its last backslash, and an Arabic sentence the
    // sign of the name it holds. (The style sheet draws an item's text
    // itself, never through FwStyle: the views' delegate prepares it.) Left
    // to right, nothing is added to the text.
    void itemTextsReadRightToLeftAsLabelsDo()
    {
        struct Case {
            QString text;
            QString whole; // what must be painted in one run, not split by direction
        };
        const QList<Case> cases{{QStringLiteral("Encrypted retail executable."), QStringLiteral("executable.")},
                                {QStringLiteral("ShadowMount+"), QStringLiteral("ShadowMount+")},
                                {QStringLiteral("إعدادات ShadowMount+ للجهاز"), QStringLiteral("ShadowMount+")},
                                {QStringLiteral("D:\\Games\\"), QStringLiteral("Games\\")},
                                {QStringLiteral("36 executables are fake-signed."), QStringLiteral("36 executables are fake-signed.")}};
        auto* list = add<QListWidget>();
        auto* tree = add<QTreeWidget>();
        tree->setHeaderHidden(true);
        auto* table = add<QTableWidget>(int(cases.size()), 1);
        auto* combo = add<QComboBox>();
        for (const Case& c : cases) {
            list->addItem(c.text);
            new QTreeWidgetItem(tree, QStringList{c.text});
            table->setItem(int(list->count() - 1), 0, new QTableWidgetItem(c.text));
            combo->addItem(c.text);
        }
        table->horizontalHeader()->setStretchLastSection(true);
        for (QWidget* w : std::initializer_list<QWidget*>{list, tree, table})
            w->setMinimumHeight(220);
        const auto back = qScopeGuard([combo] { combo->hidePopup(); });
        for (const Qt::LayoutDirection dir : {Qt::RightToLeft, Qt::LeftToRight}) {
            for (QWidget* w : std::initializer_list<QWidget*>{list, tree, table, combo})
                w->setLayoutDirection(dir);
            combo->showPopup();
            QTRY_VERIFY(combo->view()->isVisible());
            QCoreApplication::processEvents();
            for (QAbstractItemView* view : std::initializer_list<QAbstractItemView*>{list, tree, table, combo->view()}) {
                const QStringList painted = paintedTexts(view);
                const QString seen = painted.join(QStringLiteral(" | "));
                const QString where = QLatin1String(view->metaObject()->className());
                if (dir == Qt::LeftToRight) {
                    QVERIFY2(!seen.contains(QChar(0x200E)), qPrintable(where + QStringLiteral(": ") + seen));
                    QVERIFY2(painted.contains(cases.first().text), qPrintable(where + QStringLiteral(": ") + seen));
                    continue;
                }
                for (const Case& c : cases)
                    QVERIFY2(std::any_of(painted.cbegin(), painted.cend(), [&](const QString& t) { return t.contains(c.whole); }),
                             qPrintable(QStringLiteral("%1 in a %2: %3").arg(c.text, where, seen)));
            }
            combo->hidePopup();
        }
    }

    // Item views leave 8 px between an item's icon and its text, as buttons do.
    void itemIconsHaveRoomBeforeTheirText()
    {
        auto* list = add<QListWidget>();
        auto* item = new QListWidgetItem(QIcon(iconPixmap(QStringLiteral("folder"), 16, theme::color::fg_3, 1.0)), QStringLiteral("Places"), list);
        QCoreApplication::processEvents();
        QStyleOptionViewItem opt;
        opt.initFrom(list->viewport());
        opt.widget = list;
        opt.rect = list->visualItemRect(item);
        opt.features = QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay;
        opt.decorationSize = QSize(16, 16);
        opt.icon = item->icon();
        opt.text = item->text();
        opt.font = list->font();
        opt.fontMetrics = QFontMetrics(list->font());
        const QRect deco = list->style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &opt, list);
        const QRect text = list->style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, list);
        QVERIFY2(text.left() - deco.right() - 1 >= theme::space::sm - 1, qPrintable(QStringLiteral("gap %1").arg(text.left() - deco.right() - 1)));
    }

    // A scroll area that keeps the bar's room: its viewport is as wide with
    // nothing to scroll as with a long content, and the idle bar has no handle.
    void scrollAreasKeepTheBarsRoom()
    {
        auto* area = add<QScrollArea>();
        area->setFixedSize(400, 300);
        area->setWidgetResizable(true);
        auto* inner = new QWidget;
        inner->setMinimumHeight(100);
        area->setWidget(inner);
        reserveScrollBarSpace(area);
        QCoreApplication::processEvents();
        const int idleWidth = area->viewport()->width();
        QCOMPARE(idleWidth, 400 - int(theme::size::scrollBar));
        QCOMPARE(area->verticalScrollBar()->property("fwIdle").toBool(), true);
        inner->setMinimumHeight(900);
        QTRY_VERIFY(area->verticalScrollBar()->maximum() > 0);
        QCOMPARE(area->viewport()->width(), idleWidth);
        QCOMPARE(area->verticalScrollBar()->property("fwIdle").toBool(), false);
    }

    // A path or an ID is set in the mono font; the placeholder, a sentence,
    // in the body font while the field is empty.
    void monoOnlyForAValue()
    {
        auto* edit = textInput(QStringLiteral("Choose a folder…"), true, m_window.get());
        QCOMPARE(edit->font(), theme::font::body());
        edit->setText(QStringLiteral("D:\\PS5\\Images"));
        QCOMPARE(edit->font(), theme::font::mono());
        edit->clear();
        QCOMPARE(edit->font(), theme::font::body());
        QCOMPARE(edit->layoutDirection(), Qt::LeftToRight);
        delete edit;
        // A placeholder that shows a value is set like the value.
        auto* address = textInput(QStringLiteral("192.168.1.40"), true, m_window.get());
        QCOMPARE(address->font(), theme::font::mono());
        delete address;
    }

    // An optional tool the user supplies is "Not set" (neutral) until a path
    // is chosen; a chosen one that is missing, or a part of the app, is an error.
    void optionalToolsAreNotSetNotBroken()
    {
        DependencyRow row(QStringLiteral("sony.dds2png"), QStringLiteral("prospero-dds2png"));
        DependencyInfo info;
        info.id = QStringLiteral("sony.dds2png");
        info.name = QStringLiteral("prospero-dds2png");
        info.status = DependencyStatus::Missing;
        info.settingKey = QStringLiteral("fpkg.dds2pngPath");
        row.setInfo(info, false);
        QCOMPARE(row.badge()->text(), QStringLiteral("Not set"));
        QCOMPARE(row.badge()->tone(), Tone::Neutral);
        row.setInfo(info, true);
        QCOMPARE(row.badge()->text(), QStringLiteral("Not found"));
        QCOMPARE(row.badge()->tone(), Tone::Err);
        info.settingKey.clear(); // a part of the app itself
        row.setInfo(info, false);
        QCOMPARE(row.badge()->tone(), Tone::Err);
        info.status = DependencyStatus::Error;
        row.setInfo(info, true);
        QCOMPARE(row.badge()->tone(), Tone::Err);
    }

    // A probe's detail and how-to-fix text are shown in the interface
    // language when the catalogue has them, like the tool's name and purpose;
    // "Set path…" hands on the translated text.
    void dependencyRowSaysHowToFixInTheLanguage()
    {
        const QString res = m_root.filePath(QStringLiteral("dep-lang/res")), user = m_root.filePath(QStringLiteral("dep-lang/user"));
        QVERIFY(QDir().mkpath(res) && QDir().mkpath(user));
        const QString how = QStringLiteral("Check that the whole fpkg-cli folder was copied, then check again.");
        const QString howDe = QStringLiteral("Prüfen Sie, ob der ganze fpkg-cli-Ordner kopiert wurde, und prüfen Sie dann erneut.");
        const QString detail = QStringLiteral("fpkg-cli did not start.");
        const QString detailDe = QStringLiteral("fpkg-cli ist nicht gestartet.");
        const QJsonObject catalogue{{QStringLiteral("code"), QStringLiteral("de")},
                                    {QStringLiteral("name"), QStringLiteral("Deutsch")},
                                    {QStringLiteral("englishName"), QStringLiteral("German")},
                                    {QStringLiteral("strings"), QJsonObject{{how, howDe}, {detail, detailDe}}}};
        QVERIFY(fwtest::writeTextFile(res + QStringLiteral("/de.json"), QJsonDocument(catalogue).toJson()));
        const QString oldRes = i18n::resourceDir(), oldUser = i18n::userDir();
        i18n::configure(res, user);
        const auto restore = qScopeGuard([&] {
            i18n::load(QStringLiteral("en"));
            i18n::configure(oldRes, oldUser);
        });
        QVERIFY(i18n::load(QStringLiteral("de")));

        DependencyRow row(QStringLiteral("dlc.fpkgcli"), QStringLiteral("fpkg-cli"));
        DependencyInfo info;
        info.id = QStringLiteral("dlc.fpkgcli");
        info.name = QStringLiteral("fpkg-cli");
        info.status = DependencyStatus::Error;
        info.detail = detail;
        info.howToFix = how;
        info.settingKey = QStringLiteral("dlc.fpkgCliPath");
        QSignalSpy fix(&row, &DependencyRow::fixRequested);
        row.setInfo(info, true);
        QString tips, texts;
        for (const auto* l : row.findChildren<ElideLabel*>()) {
            tips += l->toolTip() + u'|';
            texts += l->fullText() + u'|';
        }
        QVERIFY2(tips.contains(howDe) && !tips.contains(how), qPrintable(tips));
        QVERIFY2(texts.contains(detailDe), qPrintable(texts));
        Link* setPath = row.findChild<Link*>();
        QVERIFY(setPath && setPath->isVisibleTo(&row));
        emit setPath->activated();
        QCOMPARE(fix.count(), 1);
        QCOMPARE(fix.first().at(1).toString(), howDe);
    }

    // Information sheets: Close looks secondary but is the default button
    // (focused, Enter, Esc); an aside button ("Load more") sits at the start
    // of the row and leaves the sheet open.
    void informationSheetsCloseQuietly()
    {
        auto* host = add<QWidget>();
        host->setFixedSize(900, 600);
        auto* s = new Sheet(host, QStringLiteral("Hex preview"));
        QPushButton* more = s->addButton(QStringLiteral("Load more"), Sheet::Role::Aside);
        QPushButton* close = s->addButton(QStringLiteral("Close"), Sheet::Role::Close);
        QSignalSpy finished(s, &Sheet::finished);
        s->open();
        QCoreApplication::processEvents();
        QTRY_VERIFY(close->hasFocus());
        QVERIFY(close->property("variant").toString() != QLatin1String("primary"));
        QVERIFY(more->mapTo(s, QPoint(0, 0)).x() < close->mapTo(s, QPoint(0, 0)).x());
        more->click();
        QVERIFY(s->isOpen());
        QCOMPARE(finished.count(), 0);
        QTest::keyClick(close, Qt::Key_Escape);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toInt(), 1);
    }

    // The status bar is one font (tabular figures), the free space included.
    void statusBarIsOneFont()
    {
        auto* bar = add<StatusBar>();
        bar->setFreeSpace(QStringLiteral("C: 441.2 GB free"));
        for (QLabel* l : bar->findChildren<QLabel*>())
            if (!l->text().isEmpty())
                QVERIFY2(l->font().family() != theme::monoFamily(), qPrintable(l->text()));
    }

    void progressBlockLabelDoesNotDependOnCallOrder()
    {
        ProgressBlock a, b;
        a.setProgress(0.4, false, QStringLiteral("Packing"));
        a.setState(ProgressBar::State::Paused);
        b.setState(ProgressBar::State::Paused);
        b.setProgress(0.4, false, QStringLiteral("Packing"));
        QCOMPARE(a.percentText(), QStringLiteral("Paused · %1").arg(thinPercent(0.4)));
        QCOMPARE(b.percentText(), a.percentText());
        b.setState(ProgressBar::State::Determinate);
        QCOMPARE(b.percentText(), thinPercent(0.4));

        InfoBar bar(Tone::Info);
        bar.setTitle(QStringLiteral("Failed"));
        bar.setTone(Tone::Err); // after the title
        QLabel* title = nullptr;
        for (QLabel* l : bar.findChildren<QLabel*>())
            if (l->text() == QStringLiteral("Failed"))
                title = l;
        QVERIFY(title);
        QCOMPARE(title->palette().color(QPalette::WindowText), theme::color::danger_hi);
    }

    void pathFieldTakesQuotedPathsAndTheRightKindOfDrop()
    {
        PathField field(PathField::Mode::Directory);
        field.edit()->setText(QStringLiteral("  \"C:\\Dumps\\My Game\"  "));
        QCOMPARE(field.path(), QStringLiteral("C:/Dumps/My Game"));

        const QString file = m_root.filePath(QStringLiteral("drop.bin"));
        QVERIFY(fwtest::writeTextFile(file, "x"));
        auto dropOf = [](PathField& f, const QUrl& url) {
            QMimeData mime;
            mime.setUrls({url});
            QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            enter.ignore();
            QApplication::sendEvent(&f, &enter);
            return enter.isAccepted();
        };
        QVERIFY(dropOf(field, QUrl::fromLocalFile(m_root.path())));  // a folder on a folder field
        QVERIFY(!dropOf(field, QUrl::fromLocalFile(file)));          // a file on a folder field
        QVERIFY(!dropOf(field, QUrl(QStringLiteral("https://example.com/x")))); // not a local file
        PathField fileField(PathField::Mode::OpenFile);
        QVERIFY(dropOf(fileField, QUrl::fromLocalFile(file)));
        QVERIFY(!dropOf(fileField, QUrl::fromLocalFile(m_root.path())));
    }

    void flowLayoutLeavesNoGapForHiddenItems()
    {
        QWidget host;
        auto* flow = new FlowLayout(&host, 8, 8);
        auto* a = new QLabel(QStringLiteral("aaaa"), &host);
        auto* hidden = new QLabel(QStringLiteral("bbbb"), &host);
        auto* c = new QLabel(QStringLiteral("cccc"), &host);
        flow->addWidget(a);
        flow->addWidget(hidden);
        flow->addWidget(c);
        hidden->hide();
        host.resize(600, 40);
        host.show();
        QCoreApplication::processEvents();
        QCOMPARE(c->x(), a->x() + a->width() + 8); // one gap, not two

        // A balanced tile grid with a prime number of tiles is not one column.
        QWidget gridHost;
        auto* grid = new TileGrid(&gridHost, 200, 4);
        grid->setBalanced(true);
        for (int i = 0; i < 5; ++i)
            grid->addWidget(new QLabel(QStringLiteral("tile"), &gridHost));
        QCOMPARE(grid->columnsFor(1200), 3); // 3 + 2
        grid->addWidget(new QLabel(QStringLiteral("tile"), &gridHost));
        QCOMPARE(grid->columnsFor(1200), 3); // 6: 3 + 3
        QCOMPARE(grid->columnsFor(210), 1);
    }

    void disclosureTogglesOncePerPress()
    {
        auto* d = add<Disclosure>(QStringLiteral("Advanced options"), QString());
        QWidget* header = d->findChild<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(header);
        QCoreApplication::processEvents();
        QSignalSpy spy(d, &Disclosure::toggled);
        QTest::mouseClick(header, Qt::RightButton, Qt::NoModifier, QPoint(20, 10));
        QCOMPARE(spy.count(), 0);
        QTest::mouseClick(header, Qt::LeftButton, Qt::NoModifier, QPoint(20, 10));
        QCOMPARE(spy.count(), 1);
        QVERIFY(d->isExpanded());
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "), true);
        QApplication::sendEvent(header, &repeat);
        QCOMPARE(spy.count(), 1); // a held key does not flip it again
        QTest::keyClick(header, Qt::Key_Space);
        QCOMPARE(spy.count(), 2);

        // A right click does not change a choice either.
        auto* seg = add<Segmented>();
        seg->addSegment(QStringLiteral("One"));
        seg->addSegment(QStringLiteral("Two"));
        QCoreApplication::processEvents();
        QTest::mouseClick(seg, Qt::RightButton, Qt::NoModifier, QPoint(seg->width() - 8, 10));
        QCOMPARE(seg->currentIndex(), 0);
        QTest::mouseClick(seg, Qt::LeftButton, Qt::NoModifier, QPoint(seg->width() - 8, 10));
        QCOMPARE(seg->currentIndex(), 1);
    }

    // ---- assistive technology --------------------------------------------------------------

    void selfPaintedControlsSayWhatTheyAre()
    {
        auto iface = [](QObject* o) { return QAccessible::queryAccessibleInterface(o); };

        Badge badge(QStringLiteral("Not found"), Tone::Err);
        QVERIFY(iface(&badge));
        QCOMPARE(iface(&badge)->role(), QAccessible::StaticText);
        QCOMPARE(iface(&badge)->text(QAccessible::Name), QStringLiteral("Not found"));

        ProgressBar bar;
        bar.setFraction(0.63);
        QCOMPARE(iface(&bar)->role(), QAccessible::ProgressBar);
        QVERIFY(iface(&bar)->valueInterface());
        QCOMPARE(iface(&bar)->valueInterface()->currentValue().toInt(), 63);
        bar.setState(ProgressBar::State::Indeterminate);
        QVERIFY(iface(&bar)->state().busy);

        Segmented seg;
        seg.setAccessibleName(QStringLiteral("Verification"));
        seg.addSegment(QStringLiteral("Off"));
        seg.addSegment(QStringLiteral("Quick"));
        seg.setCurrentIndex(1);
        QCOMPARE(iface(&seg)->text(QAccessible::Name), QStringLiteral("Verification"));
        QCOMPARE(iface(&seg)->text(QAccessible::Value), QStringLiteral("Quick"));

        ChoiceTiles tiles;
        tiles.addChoice(QStringLiteral("exFAT"), QStringLiteral("d"), QStringLiteral("drive"), QStringLiteral("exfat"));
        tiles.addChoice(QStringLiteral("FPKG"), QStringLiteral("d"), QStringLiteral("package"), QStringLiteral("fpkg"));
        tiles.setCurrentIndex(1);
        QCOMPARE(iface(&tiles)->text(QAccessible::Value), QStringLiteral("FPKG"));

        Link link(QStringLiteral("How to fix…"));
        QCOMPARE(iface(&link)->role(), QAccessible::Link);
        link.setText(QStringLiteral("Set path…"));
        QCOMPARE(iface(&link)->text(QAccessible::Name), QStringLiteral("Set path…"));

        Disclosure d(QStringLiteral("Advanced options"));
        QWidget* header = d.findChild<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(iface(header)->role(), QAccessible::Button);
        QCOMPARE(iface(header)->text(QAccessible::Name), QStringLiteral("Advanced options"));
        QVERIFY(iface(header)->state().collapsed);
        d.setExpanded(true);
        QVERIFY(iface(header)->state().expanded);

        // A stat tile with a click handler is a button that Tab reaches and Enter presses.
        StatTile tile(QStringLiteral("game"), QStringLiteral("Library"));
        tile.setValue(QStringLiteral("12 games"));
        QCOMPARE(tile.focusPolicy(), Qt::NoFocus);
        QCOMPARE(iface(&tile)->text(QAccessible::Name), QStringLiteral("Library: 12 games"));
        int clicks = 0;
        connect(&tile, &StatTile::clicked, this, [&clicks] { ++clicks; });
        QCOMPARE(tile.focusPolicy(), Qt::TabFocus);
        QCOMPARE(iface(&tile)->role(), QAccessible::Button);
        QTest::keyClick(&tile, Qt::Key_Return);
        QTest::keyClick(&tile, Qt::Key_Space);
        QCOMPARE(clicks, 2);

        // The header chips.
        AppHeader header2;
        QSignalSpy queue(&header2, &AppHeader::queueChipClicked);
        QWidget* chip = header2.queueChip()->parentWidget();
        QCOMPARE(chip->focusPolicy(), Qt::TabFocus);
        QCOMPARE(iface(chip)->role(), QAccessible::Button);
        QTest::keyClick(chip, Qt::Key_Return);
        QCOMPARE(queue.count(), 1);
        QTest::mouseClick(chip, Qt::RightButton, Qt::NoModifier, QPoint(2, 2));
        QCOMPARE(queue.count(), 1);
    }

    // ---- depth, badges, empty states ------------------------------------------------------

    void whiteNeverSitsOnTheBrightGradient()
    {
        using namespace theme;
        // WCAG contrast of white on a colour.
        auto onWhite = [](const QColor& c) {
            auto lin = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            const qreal l = 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
            return 1.05 / (l + 0.05);
        };
        QVERIFY(onWhite(color::accent_fill) >= 4.5);
        QVERIFY(onWhite(color::accent_fill_2) >= 4.5);
        QVERIFY(onWhite(color::accent_fill_pressed) >= 4.5);
        QVERIFY(onWhite(color::accent) < 4.5); // which is why the fill tokens exist
        const QLinearGradient g = accentFillGradient(QRectF(0, 0, 100, 10));
        QCOMPARE(g.stops().first().second, color::accent_fill);
        QCOMPARE(g.stops().last().second, color::accent_fill_2);
        QCOMPARE(withAlpha(color::accent_hi, 40).alpha(), 40);

        // The primary button, the checked switch and a count on accent use them.
        const QString qss = stylesheet();
        const int primary = qss.indexOf(QStringLiteral("QPushButton[variant=\"primary\"] {"));
        QVERIFY(primary >= 0);
        const QString rule = qss.mid(primary, qss.indexOf(u'}', primary) - primary);
        QVERIFY2(rule.contains(color::accent_fill.name()) && rule.contains(color::accent_fill_2.name()), qPrintable(rule));
        QVERIFY(!rule.contains(color::accent.name()));
        auto* sw = add<Switch>();
        sw->setChecked(true);
        QCoreApplication::processEvents();
        const QImage on = sw->grab().toImage();
        const QColor left = on.pixelColor(6, on.height() / 2);
        QVERIFY2(qAbs(left.red() - color::accent_fill.red()) < 24 && qAbs(left.blue() - color::accent_fill.blue()) < 24,
                 qPrintable(left.name()));
    }

    void badgesHaveOneLook()
    {
        using namespace theme;
        Badge neutral(QStringLiteral("Folder"), Tone::Neutral);
        neutral.setIconName(QString());
        QCOMPARE(neutral.sizeHint().height(), size::badge);
        QCOMPARE(neutral.sizeHint().width(), badgeWidth(QStringLiteral("Folder")));
        Badge count(QStringLiteral("3"), Tone::Neutral);
        count.setCount(true);
        QCOMPARE(count.sizeHint().height(), size::badgeCount);
        QVERIFY(count.sizeHint().width() >= size::badgeCount); // never narrower than a circle

        // Painted by hand it is the same pill: neutral is bg_4 with a border_3 outline.
        QImage img(badgeWidth(QStringLiteral("Folder")), size::badge, QImage::Format_ARGB32_Premultiplied);
        img.fill(color::bg_2);
        {
            QPainter p(&img);
            paintBadge(p, QRectF(img.rect()), QStringLiteral("Folder"), Tone::Neutral);
        }
        neutral.resize(neutral.sizeHint());
        QImage widget(neutral.size(), QImage::Format_ARGB32_Premultiplied);
        widget.fill(color::bg_2);
        neutral.render(&widget, QPoint(), QRegion(), QWidget::DrawChildren);
        QCOMPARE(widget, img);
        QCOMPARE(img.pixelColor(img.width() / 2, 0), color::border_3);
        QCOMPARE(img.pixelColor(4, img.height() / 2), color::bg_4);
        // A tone is its colour as a tint: darker than the mark, lighter than the card.
        img.fill(color::bg_2);
        {
            QPainter p(&img);
            paintBadge(p, QRectF(img.rect()), QStringLiteral("Ready"), Tone::Ok);
        }
        const QColor tint = img.pixelColor(4, img.height() / 2);
        QVERIFY2(tint.green() > color::bg_2.green() && tint.green() < color::success.green() / 2, qPrintable(tint.name()));
    }

    void emptyStateIsCompactAndNeverCutsItsText()
    {
        const QString text = QStringLiteral("Jobs you add appear here and run one after another while you keep working.");
        auto* host = add<QWidget>();
        host->setFixedWidth(theme::size::dock);
        auto* col = new QVBoxLayout(host);
        col->setContentsMargins(0, 0, 0, 0);
        auto* empty = new EmptyState(QStringLiteral("queue"), QStringLiteral("No jobs yet"), text, host);
        empty->setCompact(true);
        col->addWidget(empty);
        col->addStretch(1);
        QVERIFY(empty->isCompact());
        QTRY_VERIFY(empty->width() == theme::size::dock);
        QTRY_VERIFY2(empty->height() <= 150, qPrintable(QString::number(empty->height())));
        QLabel* caption = nullptr;
        for (QLabel* l : empty->findChildren<QLabel*>())
            if (l->text() == text)
                caption = l;
        QVERIFY(caption);
        QVERIFY(caption->width() <= theme::size::emptyMax);
        QVERIFY(caption->width() <= empty->width() - 2 * theme::space::xxl); // inside the margins of a 400 px dock
        QVERIFY2(caption->height() >= caption->heightForWidth(caption->width()), "the caption is cut");
        QVERIFY(caption->geometry().bottom() < empty->height());
        // A list makes the state it is given compact.
        auto* loose = new EmptyState(QStringLiteral("filter"), QStringLiteral("No jobs here"), QStringLiteral("Nothing matches."), host);
        QVERIFY(!loose->isCompact());
        const int tall = loose->sizeHint().height();
        loose->setCompact(true);
        QVERIFY(loose->sizeHint().height() < tall);
        // Without a caption it is simply shorter, at any width, and Qt has nothing to warn about.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("Negative sizes")));
        auto* bare = new EmptyState(QStringLiteral("search"), QStringLiteral("No PS5 found"), QString(), host);
        bare->setCompact(true);
        col->insertWidget(1, bare);
        bare->show();
        QCoreApplication::processEvents();
        host->setFixedWidth(220);
        QCoreApplication::processEvents();
        QVERIFY(bare->sizeHint().height() < loose->sizeHint().height());
    }

    void pathFieldCutsALongPlaceholderInTheMiddle()
    {
        const QString longText = QStringLiteral("Not set: click the folder button to choose where it goes");
        auto* field = add<PathField>(PathField::Mode::Directory);
        field->setPlaceholder(longText);
        field->setFixedWidth(260);
        QCoreApplication::processEvents();
        QCOMPARE(field->placeholder(), longText);
        const QString shown = field->edit()->placeholderText();
        QVERIFY2(shown != longText && shown.contains(QChar(0x2026)), qPrintable(shown));
        // Cut in the middle: both ends are still there.
        QVERIFY2(shown.startsWith(QStringLiteral("No")) && shown.endsWith(QStringLiteral("es")), qPrintable(shown));
        QCOMPARE(field->edit()->toolTip(), plainTip(longText));
        QCOMPARE(field->edit()->accessibleDescription(), longText);
        // Room for all of it: nothing is cut and there is no tooltip. (The
        // placeholder is a sentence, set in the body font while the field is
        // empty: the room is measured in that font.)
        QCOMPARE(field->edit()->font(), theme::font::body());
        field->setFixedWidth(QFontMetrics(field->edit()->font()).horizontalAdvance(longText) + field->browseButton()->sizeHint().width() + 120);
        QCoreApplication::processEvents();
        QCOMPARE(field->edit()->placeholderText(), longText);
        QVERIFY(field->edit()->toolTip().isEmpty());
        QVERIFY(field->edit()->accessibleDescription().isEmpty());
    }

    void longTooltipsAreWrappedAt360()
    {
        auto* target = add<QLabel>(QStringLiteral("hover me"));
        QCoreApplication::processEvents();
        auto shownTip = []() -> QLabel* {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->windowType() == Qt::ToolTip && w->isVisible())
                    return qobject_cast<QLabel*>(w);
            return nullptr;
        };
        const QString longTip = QStringLiteral("A sentence that goes on and on. ").repeated(10).trimmed();
        int tall = 0;
        for (const QString& tipText : {longTip, plainTip(longTip)}) { // one plain line, and the app's own wrapped kind
            QToolTip::showText(target->mapToGlobal(QPoint(5, 5)), tipText, target);
            QLabel* tip = nullptr;
            QTRY_VERIFY((tip = shownTip()));
            QTRY_VERIFY2(tip->width() <= theme::size::toolTipMax, qPrintable(QString::number(tip->width())));
            QVERIFY(tip->wordWrap());
            QVERIFY2(tip->height() >= tip->heightForWidth(tip->width()), "the tooltip's text is cut");
            QVERIFY(tip->height() > 3 * tip->fontMetrics().height()); // several lines now
            tall = tip->height();
            saveGrab(tip, tipText == longTip ? "tooltip-plain" : "tooltip-rich");
            QToolTip::hideText();
            QTRY_VERIFY(!shownTip());
        }
        // A short one is left alone: one line, as wide as its text.
        QToolTip::showText(target->mapToGlobal(QPoint(5, 5)), QStringLiteral("Reorder"), target);
        QLabel* tip = nullptr;
        QTRY_VERIFY((tip = shownTip()));
        QVERIFY(tip->width() < theme::size::toolTipMax);
        QVERIFY(!tip->wordWrap());
        QVERIFY2(tip->height() < tall / 3, qPrintable(QStringLiteral("%1 of %2").arg(tip->height()).arg(tall)));
    }

    void menuLeavesRoomForItsShadow()
    {
        const int margin = theme::size::popupShadow;
        QMenu menu(m_window.get());
        menu.addAction(QStringLiteral("Open queue"));
        menu.addAction(QStringLiteral("Clear finished"));
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove"));
        const QPoint at = m_window->mapToGlobal(QPoint(120, 120));
        menu.popup(at);
        QTRY_VERIFY(menu.isVisible());
        // The panel, not the clear margin around it, is where the menu was asked to be.
        QCOMPARE(menu.pos(), at - QPoint(margin, margin));
        const QRect first = menu.actionGeometry(menu.actions().constFirst());
        QVERIFY(first.left() > margin && first.top() > margin);
        QVERIFY(first.right() < menu.width() - margin);
        saveGrab(&menu, "menu");
        // The margin is beside the menu: a click there closes it.
        QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, QPoint(3, 3));
        QTRY_VERIFY(!menu.isVisible());
    }

    // ---- motion ----------------------------------------------------------------------------

    void motionIsOffWhereNobodyWatches()
    {
        motion::clearOverride();
        QVERIFY(!motion::enabled()); // the offscreen platform these tests run on
        {
            MotionOn on;
            QVERIFY(motion::enabled());
            motion::setOverride(false);
            QVERIFY(!motion::enabled());
        }
        QVERIFY(!motion::enabled());

        // Off: a value is at its target at once and nothing runs.
        auto* w = add<QWidget>();
        QCoreApplication::processEvents();
        auto* v = new motion::Value(w, 0.0, motion::base);
        v->animateTo(1.0);
        QCOMPARE(v->value(), 1.0);
        QVERIFY(!v->isRunning());
        QCOMPARE(motion::fast, 110);
        QCOMPARE(motion::base, 160);
        QCOMPARE(motion::slow, 220);
    }

    void motionValueEasesToItsTargetAndStopsWithItsWidget()
    {
        MotionOn on;
        auto* w = add<QWidget>();
        QCoreApplication::processEvents();
        QVERIFY(w->isVisible());
        auto* v = new motion::Value(w, 0.0, motion::slow);
        int changes = 0;
        v->onChange([&changes] { ++changes; });
        v->animateTo(1.0);
        QVERIFY(v->isRunning());
        QVERIFY(v->value() < 1.0);
        QCOMPARE(v->target(), 1.0);
        QTRY_VERIFY(!v->isRunning());
        QCOMPARE(v->value(), 1.0); // exactly the target
        QVERIFY(changes > 1);

        // Hidden half way: at its end state at once, nothing left running.
        v->animateTo(0.0);
        QVERIFY(v->isRunning());
        w->hide();
        QVERIFY(!v->isRunning());
        QCOMPARE(v->value(), 0.0);
        // And nothing starts for a widget nobody sees.
        v->animateTo(1.0);
        QVERIFY(!v->isRunning());
        QCOMPARE(v->value(), 1.0);

        // Deleted half way: no step arrives for a dead widget.
        w->show();
        QCoreApplication::processEvents();
        v->animateTo(0.0);
        QVERIFY(v->isRunning());
        const int before = changes;
        delete w;
        QTest::qWait(motion::slow + 80);
        QCOMPARE(changes, before);

        // Several items painted by one widget: the one left fades out while the next fades in.
        auto* many = add<QWidget>();
        QCoreApplication::processEvents();
        auto* hover = new motion::HoverFade(many);
        hover->setHovered(0);
        QTRY_COMPARE(hover->amount(0), 1.0);
        hover->setHovered(1);
        QCOMPARE(hover->hovered(), 1);
        QVERIFY(hover->amount(0) > 0.0 && hover->amount(1) < 1.0);
        QTRY_VERIFY(!hover->isRunning());
        QCOMPARE(hover->amount(0), 0.0);
        QCOMPARE(hover->amount(1), 1.0);
        QCOMPARE(hover->amount(7), 0.0);
        hover->reset();
        QCOMPARE(hover->amount(1), 0.0);
    }

    void switchSlidesAndEndsAsIfItHadJumped()
    {
        // The reference: toggled with motion off.
        auto* still = add<Switch>();
        QCoreApplication::processEvents();
        still->setChecked(true);
        QCOMPARE(still->position(), 1.0);
        QVERIFY(!still->isAnimating());
        const QImage reference = still->grab().toImage();

        MotionOn on;
        auto* sw = add<Switch>();
        QCoreApplication::processEvents();
        sw->setChecked(true);
        QVERIFY(sw->isChecked()); // the state is immediate; only the picture follows
        QVERIFY(sw->isAnimating());
        QVERIFY(sw->position() < 1.0);
        QTRY_VERIFY(!sw->isAnimating());
        QCOMPARE(sw->position(), 1.0);
        QCOMPARE(sw->grab().toImage(), reference);
        // Off again, and hidden before the knob gets there.
        sw->setChecked(false);
        QVERIFY(sw->isAnimating());
        sw->hide();
        QVERIFY(!sw->isAnimating());
        QCOMPARE(sw->position(), 0.0);
    }

    void segmentedPillSlidesAndEndsOnTheNewSegment()
    {
        auto make = [this] {
            auto* s = add<Segmented>();
            s->addSegment(QStringLiteral("Low impact"));
            s->addSegment(QStringLiteral("Balanced"));
            s->addSegment(QStringLiteral("Maximum"));
            return s;
        };
        Segmented* still = make();
        QCoreApplication::processEvents();
        still->setCurrentIndex(2);
        QVERIFY(!still->isAnimating());
        const QImage reference = still->grab().toImage();

        MotionOn on;
        Segmented* seg = make();
        QCoreApplication::processEvents();
        QSignalSpy spy(seg, &Segmented::currentChanged);
        seg->setCurrentIndex(2);
        QCOMPARE(seg->currentIndex(), 2); // at once
        QCOMPARE(spy.count(), 1);
        QVERIFY(seg->isAnimating());
        seg->setCurrentIndex(1); // sent somewhere else half way, and back
        seg->setCurrentIndex(2);
        QTRY_VERIFY(!seg->isAnimating());
        QCOMPARE(seg->grab().toImage(), reference);
    }

    void progressBarEasesAndRunsNothingWhenIdleOrHidden()
    {
        // Motion off: the fill is at its fraction at once and nothing ticks.
        auto* still = add<ProgressBar>();
        QCoreApplication::processEvents();
        still->setFraction(0.5);
        QCOMPARE(still->shownFraction(), 0.5);
        QVERIFY(!still->isAnimating());
        still->setState(ProgressBar::State::Indeterminate);
        QVERIFY(!still->isAnimating());
        QCOMPARE(still->height(), theme::size::progress);

        MotionOn on;
        auto* bar = add<ProgressBar>();
        QCoreApplication::processEvents();
        QVERIFY(!bar->isAnimating()); // idle at 0
        bar->setFraction(0.5);
        QCOMPARE(bar->fraction(), 0.5);      // the value is immediate
        QVERIFY(bar->shownFraction() < 0.5); // the picture eases
        QVERIFY(bar->isAnimating());
        QTRY_COMPARE(bar->shownFraction(), 0.5);
        QVERIFY(bar->isAnimating()); // the highlight sweep of a running job
        bar->setFraction(0.2);       // another job in the same bar: back at once
        QCOMPARE(bar->shownFraction(), 0.2);
        // Not running: nothing ticks.
        bar->setState(ProgressBar::State::Paused);
        QVERIFY(!bar->isAnimating());
        bar->setState(ProgressBar::State::Determinate);
        QVERIFY(bar->isAnimating());
        bar->setFraction(1.0);
        QTRY_VERIFY(!bar->isAnimating()); // arrived, and a full bar has no sweep
        QCOMPARE(bar->shownFraction(), 1.0);
        // Hidden: nothing ticks; shown again: it goes on.
        bar->setState(ProgressBar::State::Indeterminate);
        QVERIFY(bar->isAnimating());
        bar->hide();
        QVERIFY(!bar->isAnimating());
        bar->show();
        QVERIFY(bar->isAnimating());
        bar->setState(ProgressBar::State::Done);
        QVERIFY(!bar->isAnimating());
    }

    void sheetAndToastFadeButAnswerAtOnce()
    {
        MotionOn on;
        QWidget* host = m_window.get();
        auto* s = new Sheet(host, QStringLiteral("Delete it?"), Sheet::Icon::Warning);
        s->addButton(QStringLiteral("Keep"), Sheet::Role::Safe);
        QPushButton* del = s->addButton(QStringLiteral("Delete"), Sheet::Role::Destructive);
        QPointer<Sheet> alive(s);
        QTimer::singleShot(30, del, [del] { del->click(); }); // while it is still fading in
        QCOMPARE(s->exec(), 1);          // the answer does not wait for the fade
        QCOMPARE(Sheet::openCount(), 0); // and the sheet is not counted as open
        QVERIFY(alive);
        QVERIFY(!alive->isOpen());
        QTRY_VERIFY(!alive); // faded away and deleted

        // Opened and left alone it ends exactly as without motion: no effect left on the panel.
        s = new Sheet(host, QStringLiteral("Done"), Sheet::Icon::Info);
        s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        s->open();
        QFrame* panel = s->findChild<QFrame*>(QStringLiteral("sheetPanel"));
        QVERIFY(panel);
        QVERIFY(panel->graphicsEffect());
        QTRY_VERIFY(!panel->graphicsEffect());
        QCOMPARE(panel->y(), (s->height() - panel->height()) / 2);
        alive = s;
        s->close();
        QCOMPARE(Sheet::openCount(), 0);
        QTRY_VERIFY(!alive);

        // Closed before its first frame (a handler that answers at once): gone, not left fading for ever.
        s = new Sheet(host, QStringLiteral("Done"), Sheet::Icon::Info);
        s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        alive = s;
        s->open();
        s->close();
        QVERIFY(!s->isVisible());
        QCOMPARE(Sheet::openCount(), 0);
        QTRY_VERIFY(!alive);

        ToastHost toasts(host);
        toasts.show(Tone::Ok, QStringLiteral("Done"), QString(), {}, 0);
        QPointer<QFrame> toast(host->findChild<QFrame*>(QStringLiteral("toast")));
        QVERIFY(toast);
        QVERIFY(toast->graphicsEffect());      // sliding in
        QTRY_VERIFY(!toast->graphicsEffect()); // arrived: nothing left on it
        QCOMPARE(toast->x(), host->width() - theme::space::lg - theme::size::toast);
        toasts.clear();
        QCOMPARE(toasts.count(), 0); // out of the stack at once
        QTRY_VERIFY(!toast);         // then faded and deleted
        // Shown and dismissed in the same breath: gone as well.
        toasts.show(Tone::Ok, QStringLiteral("Done"), QString(), {}, 0);
        toast = host->findChild<QFrame*>(QStringLiteral("toast"));
        QVERIFY(toast);
        toasts.clear();
        QTRY_VERIFY(!toast);
    }

    // ---- page pass: covers, tiles, glow, pulse, toasts ----------------------------------------

    // A game without cover art gets one of six gradients, and always the same
    // one for the same title ID: on every run, in every list, whatever the
    // case or the spaces around it.
    void coverPlaceholderIsStableForAnId()
    {
        using namespace theme;
        // Pinned: FNV-1a over the ID. A change of the hash would recolour every library.
        QCOMPARE(coverIndex(QStringLiteral("PPSA01325")), 0);
        QCOMPARE(coverIndex(QStringLiteral("PPSA26344")), 4);
        QCOMPARE(coverIndex(QStringLiteral("PPSA01535")), 3);
        QCOMPARE(coverIndex(QStringLiteral("PPSA01474")), 1);
        QCOMPARE(coverIndex(QStringLiteral("  ppsa01474 ")), 1);
        QCOMPARE(coverIndex(QStringLiteral("Demon's Souls")), 4);
        for (const QString& id : {QStringLiteral("PPSA00001"), QStringLiteral("CUSA12345"), QString(), QStringLiteral("日本語")}) {
            const int i = coverIndex(id);
            QVERIFY(i >= 0 && i < cover::count);
            QCOMPARE(coverIndex(id), i);
        }
        QCOMPARE(coverInitials(QStringLiteral("Demon's Souls")), QStringLiteral("DS"));
        QCOMPARE(coverInitials(QStringLiteral("Ratchet & Clank: Rift Apart")), QStringLiteral("RC"));
        QCOMPARE(coverInitials(QStringLiteral("returnal")), QStringLiteral("R"));
        QCOMPARE(coverInitials(QStringLiteral("  —  ")), QStringLiteral("?"));

        // White initials are readable on every stop of every gradient.
        auto onWhite = [](const QColor& c) {
            auto lin = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            const qreal l = 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
            return 1.05 / (l + 0.05);
        };
        for (int i = 0; i < cover::count; ++i) {
            QVERIFY2(onWhite(cover::stops[i].from) >= 4.5, qPrintable(cover::stops[i].from.name()));
            QVERIFY2(onWhite(cover::stops[i].to) >= 4.5, qPrintable(cover::stops[i].to.name()));
            const QGradientStops stops = cover::gradient(i).stops();
            QCOMPARE(stops.first().second, cover::stops[i].from);
            QCOMPARE(stops.last().second, cover::stops[i].to);
        }
        QCOMPARE(cover::gradient(cover::count + 2).stops(), cover::gradient(2).stops()); // taken modulo
        QVERIFY(onWhite(color::danger_fill) >= 4.5);
        QVERIFY(onWhite(color::danger_fill_pressed) >= 4.5);

        // The same ID paints the same picture; another one, another gradient.
        const auto render = [](const QString& title, const QString& id) {
            QImage img(40, 40, QImage::Format_ARGB32_Premultiplied);
            img.fill(color::bg_2);
            QPainter p(&img);
            paintCoverPlaceholder(p, QRectF(img.rect()), title, id, 6);
            return img;
        };
        const QImage a = render(QStringLiteral("Astro Bot"), QStringLiteral("PPSA01325"));
        QCOMPARE(render(QStringLiteral("Astro Bot"), QStringLiteral("PPSA01325")), a);
        QVERIFY(render(QStringLiteral("Astro Bot"), QStringLiteral("PPSA01535")) != a);
        // Without an ID the title decides.
        QCOMPARE(render(QStringLiteral("Demon's Souls"), QString()), render(QStringLiteral("Demon's Souls"), QStringLiteral("Demon's Souls")));
        // The corner is the card's (rounded), the middle of the left edge the gradient's.
        QCOMPARE(a.pixelColor(0, 0), color::bg_2);
        QVERIFY(a.pixelColor(1, 20) != color::bg_2);

        // The Library grid takes the colour from the item's cover ID, not from its path.
        GridItem one;
        one.key = QStringLiteral("D:/Games/a");
        one.title = QStringLiteral("Astro Bot");
        one.coverId = QStringLiteral("PPSA01325");
        GridItem two = one;
        two.key = QStringLiteral("E:/Other/b");
        auto* grid = add<LibraryGrid>();
        grid->setMode(LibraryGrid::Mode::List);
        grid->setItems({one, two});
        grid->resize(700, grid->heightForWidth(700));
        QCoreApplication::processEvents();
        const QImage rows = grid->grab().toImage();
        // The two rows' covers (40 px, after the check box) are the same picture.
        const int rowH = 56, top = theme::size::tableHeader;
        const QRect first(42, top + (rowH - 40) / 2, 40, 40);
        QCOMPARE(rows.copy(first.adjusted(4, 4, -4, -4)), rows.copy(first.translated(0, rowH).adjusted(4, 4, -4, -4)));
    }

    // The selected tile's glow is drawn by a sibling under the tile widget:
    // it follows the widget, never changes its geometry, and goes with it.
    void selectionGlowFollowsItsWidget()
    {
        auto* host = add<QWidget>();
        host->setFixedSize(400, 200);
        auto* tiles = new QWidget(host);
        tiles->setGeometry(40, 30, 200, 100);
        tiles->show();
        QPointer<SelectionGlow> glow = new SelectionGlow(tiles);
        const int s = SelectionGlow::kSpread;
        QCOMPARE(glow->parentWidget(), host);
        QCOMPARE(glow->geometry(), tiles->geometry().adjusted(-s, -s, s, s));
        QVERIFY(glow->testAttribute(Qt::WA_TransparentForMouseEvents));
        // Under the tiles: the tiles' own child index is higher.
        QVERIFY(host->children().indexOf(glow.data()) < host->children().indexOf(tiles));
        tiles->setGeometry(60, 50, 220, 90);
        QCOMPARE(glow->geometry(), tiles->geometry().adjusted(-s, -s, s, s));
        QCOMPARE(tiles->geometry(), QRect(60, 50, 220, 90));
        tiles->hide();
        QVERIFY(glow->isHidden());
        tiles->show();
        QVERIFY(!glow->isHidden());

        // It paints outside the tile's rectangle only when there is one.
        glow->setItemRect(QRectF(0.5, 0.5, 99, 89), theme::radius::md);
        QCoreApplication::processEvents();
        QImage with(glow->size(), QImage::Format_ARGB32_Premultiplied);
        with.fill(Qt::transparent);
        glow->render(&with, QPoint(), QRegion(), QWidget::RenderFlags());
        QVERIFY(with.pixelColor(s - 2, s + 40).alpha() > 0);     // just left of the tile
        QCOMPARE(with.pixelColor(s + 180, s + 40).alpha(), 0);   // far from it
        glow->setItemRect({}, 0);
        with.fill(Qt::transparent);
        glow->render(&with, QPoint(), QRegion(), QWidget::RenderFlags());
        QCOMPARE(with.pixelColor(s - 2, s + 40).alpha(), 0);

        delete tiles;
        QTRY_VERIFY(!glow);

        // A tile widget that fills its container to the edge (a plain box of
        // a layout): the glow lives one level up, where there is room, under
        // that box, and follows the box as well.
        auto* outer = add<QWidget>();
        outer->setFixedSize(400, 200);
        auto* box = new QWidget(outer);
        box->setGeometry(30, 30, 200, 100);
        box->show();
        auto* flush = new QWidget(box);
        flush->setGeometry(0, 0, 200, 100);
        flush->show();
        QPointer<SelectionGlow> lifted = new SelectionGlow(flush);
        QCOMPARE(lifted->parentWidget(), outer);
        QCOMPARE(lifted->geometry(), QRect(30, 30, 200, 100).adjusted(-s, -s, s, s));
        QVERIFY(outer->children().indexOf(lifted.data()) < outer->children().indexOf(box));
        box->move(50, 40);
        QCOMPARE(lifted->geometry(), QRect(50, 40, 200, 100).adjusted(-s, -s, s, s));
        box->hide();
        QVERIFY(lifted->isHidden());
        box->show();
        QVERIFY(!lifted->isHidden());
        delete box;
        QTRY_VERIFY(!lifted);

        // The tile widget is deleted and its container moves before the glow
        // itself is gone (a page that rebuilds its tiles): nothing is touched.
        auto* box2 = new QWidget(outer);
        box2->setGeometry(30, 30, 200, 100);
        box2->show();
        auto* tiles2 = new QWidget(box2);
        tiles2->setGeometry(0, 0, 200, 100);
        tiles2->show();
        QPointer<SelectionGlow> orphan = new SelectionGlow(tiles2);
        delete tiles2;
        QVERIFY(orphan); // deleted later
        box2->move(60, 50);
        box2->resize(180, 90);
        box2->hide();
        QTRY_VERIFY(!orphan);
        delete box2;
    }

    // The running stage's dot swings only while it is switched on, on screen
    // and motion is enabled; otherwise no timer runs and it rests at 0.
    void pulseRunsOnlyWhileShownAndActive()
    {
        auto* dot = add<QWidget>();
        dot->setFixedSize(14, 14);
        QCoreApplication::processEvents();
        // Motion off (the offscreen platform): never.
        auto* still = new motion::Pulse(dot);
        still->setActive(true);
        QVERIFY(!still->isRunning());
        QCOMPARE(still->value(), 0.0);
        delete still;

        MotionOn on;
        auto* pulse = new motion::Pulse(dot, 200);
        QVERIFY(!pulse->isRunning()); // not switched on
        pulse->setActive(true);
        QVERIFY(pulse->isRunning());
        QTRY_VERIFY(pulse->value() > 0.0);
        QVERIFY(pulse->value() <= 1.0);
        dot->hide();
        QVERIFY(!pulse->isRunning());
        QCOMPARE(pulse->value(), 0.0);
        dot->show();
        QVERIFY(pulse->isRunning());
        pulse->setActive(false);
        QVERIFY(!pulse->isRunning());
        QCOMPARE(pulse->value(), 0.0);
        // Switched on while hidden: it waits for the widget.
        dot->hide();
        pulse->setActive(true);
        QVERIFY(!pulse->isRunning());
        dot->show();
        QVERIFY(pulse->isRunning());
    }

    // A stat tile's mark can carry the state of what it reports, whatever
    // the value's own colour; a page header's tile is the accent one; header
    // buttons keep their own width at the right edge; a disclosure inside a
    // card is a plain row.
    void tilesHeadersAndPlainDisclosures()
    {
        auto* tile = add<StatTile>(QStringLiteral("queue"), QStringLiteral("Queue"));
        tile->setValue(QStringLiteral("Idle"));
        QCOMPARE(tile->iconTone(), Tone::Info); // as before: the accent tile
        tile->setIconTone(Tone::Neutral);
        QCOMPARE(tile->iconTone(), Tone::Neutral);
        tile->setValue(QStringLiteral("2 running"), Tone::Info); // the page's tone stays
        QCOMPARE(tile->iconTone(), Tone::Neutral);
        tile->setIconTone(Tone::Err);
        QCOMPARE(tile->iconTone(), Tone::Err);

        auto* header = add<PageHeader>(QStringLiteral("Build image"), QStringLiteral("Turn a game dump into an image."));
        header->setIcon(QStringLiteral("build"));
        auto* icon = header->findChild<IconTile*>();
        QVERIFY(icon && icon->isAccent());
        auto* action = button(QStringLiteral("New build"), ButtonVariant::Secondary, QStringLiteral("plus"), header);
        header->addAction(action);
        m_window->resize(1300, 700);
        QCoreApplication::processEvents();
        QTRY_COMPARE(action->width(), action->sizeHint().width()); // not stretched over what the caption leaves
        QCOMPARE(action->mapTo(header, QPoint(action->width(), 0)).x(), header->width());
        // A page whose content is narrower than the window: the buttons end where it does.
        header->setContentWidth(900);
        QTRY_COMPARE(action->mapTo(header, QPoint(action->width(), 0)).x(), 900);
        header->setContentWidth(0);
        QTRY_COMPARE(action->mapTo(header, QPoint(action->width(), 0)).x(), header->width());
        // One line: a caption too long for the room ends in "…" and says it all as a tooltip.
        const QString longCaption = QStringLiteral("Turn a game dump into an exFAT image that ShadowMount+ and PCs can mount, "
                                                   "or into a package, a PFS image or a packed folder for a USB drive");
        header->setCaption(longCaption);
        m_window->resize(700, 700);
        QCoreApplication::processEvents();
        QCOMPARE(header->caption(), longCaption);
        auto* captionLabel = header->findChild<ElideLabel*>();
        QVERIFY(captionLabel);
        QTRY_VERIFY(captionLabel->text().endsWith(QChar(0x2026)));
        QCOMPARE(captionLabel->toolTip(), plainTip(longCaption));
        QVERIFY(!captionLabel->wordWrap());
        QVERIFY(captionLabel->height() < 2 * QFontMetrics(captionLabel->font()).height());
        m_window->resize(900, 700);

        auto* card = add<Card>();
        auto* fold = new Disclosure(QStringLiteral("Conversion log"), QString(), card);
        card->body()->addWidget(fold);
        QVERIFY(!fold->isPlain());
        fold->setPlain(true);
        QVERIFY(fold->isPlain());
        QCOMPARE(fold->body()->contentsMargins(), QMargins());
        QCoreApplication::processEvents();
        // No plate of its own: its left edge is the card's fill, with the hairline on top.
        const QImage img = fold->grab().toImage();
        QCOMPARE(img.pixelColor(img.width() / 2, 0), theme::color::border_1);
        fold->setExpanded(true);
        QVERIFY(fold->isExpanded());
    }

    // Toasts have a painted shadow: the host is shown only while there is a
    // toast, lies under them, covers them with room for the shadow, and takes no clicks.
    void toastsCastAPaintedShadow()
    {
        auto* host = add<QWidget>();
        host->setFixedSize(700, 400);
        QCoreApplication::processEvents();
        ToastHost toasts(host);
        QVERIFY(toasts.isHidden());
        QVERIFY(toasts.testAttribute(Qt::WA_TransparentForMouseEvents));
        toasts.show(Tone::Ok, QStringLiteral("Image built"), QStringLiteral("Demon's Souls"), {}, 0);
        auto* toast = host->findChild<QFrame*>(QStringLiteral("toast"));
        QVERIFY(toast);
        QVERIFY(!toasts.isHidden());
        QVERIFY(toasts.geometry().contains(toast->geometry()));
        QVERIFY(toasts.geometry().top() < toast->geometry().top() - 8);
        QVERIFY(host->children().indexOf(&toasts) < host->children().indexOf(toast)); // under it
        // Something dark is painted just above the toast, nothing far away from it.
        QImage img(toasts.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        toasts.render(&img, QPoint(), QRegion(), QWidget::RenderFlags());
        const QPoint above = toast->geometry().topLeft() - toasts.pos() + QPoint(toast->width() / 2, -3);
        QVERIFY(img.pixelColor(above).alpha() > 0);
        QCOMPARE(img.pixelColor(above).red(), 0);
        toasts.clear();
        QVERIFY(toasts.isHidden());
    }

    // Without a fade the sheet deletes itself right after its answer; when
    // events are processed before the nested loop returns (a click followed
    // by more events, as Windows may deliver them) it is gone by then. The
    // answer still comes back, and finished() carries it, once.
    void sheetAnswerSurvivesItsDeletionInsideExec()
    {
        motion::setOverride(false);
        const auto restore = qScopeGuard([] { motion::clearOverride(); });
        auto* s = new Sheet(m_window.get(), QStringLiteral("Delete it?"), Sheet::Icon::Warning);
        s->addButton(QStringLiteral("Keep"), Sheet::Role::Safe);
        QPushButton* del = s->addButton(QStringLiteral("Delete"), Sheet::Role::Destructive);
        QPointer<Sheet> alive(s);
        QList<int> answers;
        connect(s, &Sheet::finished, this, [&answers](int r) { answers << r; });
        QTimer::singleShot(0, this, [del, alive] {
            del->click();
            delete alive.data(); // what its deleteLater() does when events run before the loop returns
        });
        QCOMPARE(s->exec(), 1);
        QVERIFY(!alive); // deleted inside the loop: the answer did not depend on it
        QCOMPARE(answers, QList<int>{1});
        QCOMPARE(Sheet::openCount(), 0);
    }

    // A sheet that keeps on top (the end-of-queue countdown) stays above one
    // opened after it, which gets the keyboard back when the first one closes.
    void aSheetThatKeepsOnTopStaysAboveALaterOne()
    {
        motion::setOverride(false);
        const auto restore = qScopeGuard([] { motion::clearOverride(); });
        auto* top = new Sheet(m_window.get(), QStringLiteral("Sleep in 60 s"), Sheet::Icon::Warning);
        top->setKeepOnTop(true);
        top->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
        top->open();
        auto* later = new Sheet(m_window.get(), QStringLiteral("Report ready"), Sheet::Icon::Info);
        QPushButton* ok = later->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
        later->open();
        QVERIFY(top->isOpen() && later->isOpen());
        // What a click in the middle of the window reaches: the first sheet.
        QWidget* hit = m_window->childAt(m_window->rect().center());
        QVERIFY(hit && (hit == top || top->isAncestorOf(hit)));
        // The keyboard stays with it.
        QVERIFY(top->isAncestorOf(m_window->focusWidget()));
        top->close(0);
        QCOMPARE(m_window->focusWidget(), static_cast<QWidget*>(ok));
        later->close(0);
        QTRY_COMPARE(Sheet::openCount(), 0);
    }

    // ---- item views, dropdowns, text wells ------------------------------------------------

    // A selected tree row is filled across its cells, as a table row is: not
    // only under its branch area (W3AMPR-12).
    void aSelectedTreeRowIsFilledLikeATableRow()
    {
        auto* tree = add<QTreeWidget>();
        tree->setColumnCount(3);
        tree->setRootIsDecorated(true);
        tree->setFixedSize(480, 160);
        auto* first = new QTreeWidgetItem(tree, {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        auto* folder = new QTreeWidgetItem(tree, {QStringLiteral("d"), QStringLiteral("e"), QStringLiteral("f")});
        new QTreeWidgetItem(folder, {QStringLiteral("g"), QStringLiteral("h"), QStringLiteral("i")});
        auto* table = add<QTableWidget>(2, 3);
        table->setFixedSize(480, 160);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                table->setItem(r, c, new QTableWidgetItem(QStringLiteral("x")));
        first->setSelected(true);
        table->selectRow(0);
        QCoreApplication::processEvents();
        saveGrab(tree, "tree-selected-row");
        saveGrab(table, "table-selected-row");

        const QImage t = tree->viewport()->grab().toImage();
        const QImage tb = table->viewport()->grab().toImage();
        const QRect cell = tree->visualRect(tree->model()->index(0, 1));
        const QRect first0 = tree->visualRect(tree->model()->index(0, 0));
        const QRect other = tree->visualRect(tree->model()->index(1, 1));
        const QRect tableCell = table->visualRect(table->model()->index(0, 1));
        QVERIFY(first0.left() >= 8); // the branch area, left of the first cell
        const QColor treeCell = t.pixelColor(cell.right() - 4, cell.center().y());
        const QColor branch = t.pixelColor(first0.left() / 2 + 2, first0.center().y());
        const QColor tableFill = tb.pixelColor(tableCell.right() - 4, tableCell.center().y());
        const QColor unselected = t.pixelColor(other.right() - 4, other.center().y());
        QCOMPARE(treeCell, tableFill);
        QCOMPARE(treeCell, branch);
        QVERIFY(treeCell != unselected);
    }

    // Right to left a dropdown's chevron is at its start, and so is the room
    // the padding keeps for it: the text area is the mirror of the left to
    // right one, whether the dropdown was made in a right-to-left window or
    // turned round later.
    void dropdownPaddingIsMirroredRightToLeft()
    {
        const auto field = [](QComboBox* c) {
            QStyleOptionComboBox opt;
            opt.initFrom(c);
            opt.editable = false;
            opt.frame = true;
            opt.currentText = c->currentText();
            opt.subControls = QStyle::SC_All;
            return c->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, c);
        };
        auto* ltr = add<QComboBox>();
        auto* turned = add<QComboBox>();
        auto* box = add<QWidget>();
        box->setLayoutDirection(Qt::RightToLeft);
        box->setFixedHeight(48);
        auto* born = new QComboBox(box);
        for (QComboBox* c : {ltr, turned, born}) {
            c->addItem(QStringLiteral("An option"));
            c->setFixedWidth(240); // the height is the style sheet's
        }
        born->show();
        QCoreApplication::processEvents();
        turned->setLayoutDirection(Qt::RightToLeft);
        QCoreApplication::processEvents();
        saveGrab(ltr, "dropdown-ltr");
        saveGrab(born, "dropdown-rtl");
        const QRect l = field(ltr);
        QVERIFY(l.left() < 240 - 1 - l.right()); // the chevron's room is at the end
        for (QComboBox* c : {turned, born}) {
            QCOMPARE(c->layoutDirection(), Qt::RightToLeft);
            QCOMPARE(c->size(), ltr->size());
            QCOMPARE(field(c), QStyle::visualRect(Qt::RightToLeft, c->rect(), l));
        }
        // And back.
        turned->setLayoutDirection(Qt::LeftToRight);
        QCoreApplication::processEvents();
        QCOMPARE(field(turned), l);
    }

    // A label of more than one line wraps where it may, and only the last
    // line is cut; the whole text is then its tooltip.
    void elideLabelWrapsToItsLines()
    {
        const QString text = QStringLiteral("Enter the console's address, or let Find my PS5 search the network.");
        auto* l = add<ElideLabel>(QString(), Qt::ElideRight);
        l->setMaxLines(2);
        l->setFullText(text);
        const int full = QFontMetrics(l->font()).horizontalAdvance(text);
        l->setFixedWidth(full * 2 / 3 + 4);
        QCoreApplication::processEvents();
        QCOMPARE(l->text().count(u'\n'), 1);
        QVERIFY(!l->text().contains(QChar(0x2026)));
        QCOMPARE(QString(l->text()).replace(u'\n', u' '), text);
        QVERIFY(l->toolTip().isEmpty());
        l->setFixedWidth(full / 3);
        QCoreApplication::processEvents();
        QCOMPARE(l->text().count(u'\n'), 1);
        QVERIFY2(l->text().endsWith(QChar(0x2026)), qPrintable(l->text()));
        QVERIFY(!l->toolTip().isEmpty());
        l->setMaxLines(1);
        QVERIFY(!l->text().contains(u'\n'));
        QVERIFY(l->text().endsWith(QChar(0x2026)));
    }

    // A log well stays left to right (its lines are English), but its
    // placeholder is a sentence of the interface: right to left in a
    // right-to-left language. Qt paints it in the application's direction
    // (main() sets it for such a language), not the well's.
    void logWellPlaceholderReadsInTheWindowsDirection()
    {
        QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        const auto back = qScopeGuard([] { QGuiApplication::setLayoutDirection(Qt::LeftToRight); });
        auto* box = add<QWidget>();
        QCOMPARE(box->layoutDirection(), Qt::RightToLeft);
        auto* v = new QVBoxLayout(box);
        auto* plain = new QPlainTextEdit(box);
        auto* rich = new QTextEdit(box);
        // The bounding box of what differs from the well's background.
        const auto ink = [](const QImage& img) {
            const QColor bg = img.pixelColor(img.width() / 2, img.height() - 2);
            QRect r;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixelColor(x, y) != bg)
                        r |= QRect(x, y, 1, 1);
            return r;
        };
        for (QAbstractScrollArea* e : std::initializer_list<QAbstractScrollArea*>{plain, rich}) {
            keepLeftToRight(e);
            e->setFixedSize(420, 120);
            v->addWidget(e);
        }
        plain->setPlaceholderText(QStringLiteral("سطر"));
        rich->setPlaceholderText(QStringLiteral("سطر"));
        box->show();
        QCoreApplication::processEvents();
        saveGrab(box, "log-well-placeholder-rtl");
        for (QAbstractScrollArea* e : std::initializer_list<QAbstractScrollArea*>{plain, rich}) {
            QCOMPARE(e->layoutDirection(), Qt::LeftToRight);
            const QImage img = e->viewport()->grab().toImage();
            const QRect r = ink(img);
            QVERIFY2(!r.isEmpty() && r.left() > img.width() / 2,
                     qPrintable(QStringLiteral("%1: %2..%3 of %4").arg(QLatin1String(e->metaObject()->className())).arg(r.left()).arg(r.right()).arg(img.width())));
        }
        // The lines themselves start at the left.
        plain->setPlainText(QStringLiteral("12:00:01 Packing data/level1.bin"));
        rich->setPlainText(QStringLiteral("12:00:01 Packing data/level1.bin"));
        QCoreApplication::processEvents();
        for (QAbstractScrollArea* e : std::initializer_list<QAbstractScrollArea*>{plain, rich}) {
            const QImage img = e->viewport()->grab().toImage();
            const QRect r = ink(img);
            QVERIFY2(!r.isEmpty() && r.left() < img.width() / 4, qPrintable(QString::number(r.left())));
        }
    }

    // The field the user was in before the sheet that keeps on top opened
    // gets the focus back once both sheets have closed, whichever closes
    // first, also after the first one has been deleted, and also when the
    // sheets opened while another window was the active one (the countdown
    // that opens with the app in the background).
    void focusComesBackPastASheetThatKeepsOnTop()
    {
        motion::setOverride(false);
        QWidget other;
        other.resize(200, 100);
        const auto restore = qScopeGuard([this] {
            motion::clearOverride();
            m_window->activateWindow();
        });
        struct Case {
            bool topFirst;
            bool active;
        };
        for (const Case c : {Case{true, true}, Case{false, true}, Case{true, false}, Case{false, false}}) {
            const QString what = QStringLiteral("%1 closes first, window %2").arg(c.topFirst ? QStringLiteral("top") : QStringLiteral("later"),
                                                                                   c.active ? QStringLiteral("active") : QStringLiteral("in the background"));
            QWidget* front = c.active ? m_window.get() : &other;
            front->show();
            front->activateWindow();
            QTRY_COMPARE(QApplication::activeWindow(), front);
            auto* before = add<QLineEdit>();
            auto* field = add<QLineEdit>();
            auto* after = add<QLineEdit>();
            Q_UNUSED(before);
            Q_UNUSED(after);
            field->setFocus(Qt::TabFocusReason);
            QTRY_COMPARE(m_window->focusWidget(), static_cast<QWidget*>(field));
            QPointer<Sheet> top = new Sheet(m_window.get(), QStringLiteral("Sleep in 60 s"), Sheet::Icon::Warning);
            top->setKeepOnTop(true);
            top->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
            top->addButton(QStringLiteral("Sleep now"), Sheet::Role::Destructive);
            top->open();
            QPointer<Sheet> later = new Sheet(m_window.get(), QStringLiteral("Report ready"), Sheet::Icon::Info);
            QPushButton* ok = later->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
            later->open();
            QVERIFY2(top->isAncestorOf(m_window->focusWidget()), qPrintable(what));
            if (c.topFirst) {
                top->close(0);
                QVERIFY2(m_window->focusWidget() == ok, qPrintable(what));
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QVERIFY(!top);
                later->close(0);
            } else {
                later->close(0);
                // The keyboard stays with the sheet on top.
                QVERIFY2(top->isAncestorOf(m_window->focusWidget()), qPrintable(what + QStringLiteral(": the sheet under it took the focus away")));
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QVERIFY(!later);
                top->close(0);
            }
            QVERIFY2(m_window->focusWidget() == field, qPrintable(what));
            QTRY_COMPARE(Sheet::openCount(), 0);
            cleanup();
        }
        other.hide();
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    // Two sheets opened under the one that keeps on top close in any order,
    // the first one first too: while the top one is open the keyboard stays
    // in it; after it, the keyboard is on the later sheet still open; and once
    // all three have closed it is back in the field. Each sheet is deleted
    // before the next one closes; the window is active or in the background.
    void sheetsUnderOneThatKeepsOnTopCloseInAnyOrder()
    {
        motion::setOverride(false);
        QWidget other;
        other.resize(200, 100);
        const auto restore = qScopeGuard([this] {
            for (Sheet* s : m_window->findChildren<Sheet*>())
                if (s->isOpen())
                    s->close(0);
            motion::clearOverride();
            m_window->activateWindow();
        });
        // 0 = the sheet that keeps on top, 1 = the first one under it, 2 = the second one.
        const QList<QList<int>> orders{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        QStringList wrong; // every order and step where the focus is elsewhere
        for (const bool active : {true, false}) {
            QWidget* front = active ? m_window.get() : &other;
            front->show();
            front->activateWindow();
            QTRY_COMPARE(QApplication::activeWindow(), front);
            for (const QList<int>& order : orders) {
                QString what = active ? QStringLiteral("window active, closed") : QStringLiteral("window in the background, closed");
                for (const int k : order)
                    what += QStringLiteral(" %1").arg(k);
                auto* field = add<QLineEdit>();
                field->setFocus(Qt::TabFocusReason);
                QTRY_COMPARE(m_window->focusWidget(), static_cast<QWidget*>(field));
                QList<QPointer<Sheet>> sheets;
                QList<QPointer<QPushButton>> oks;
                auto* top = new Sheet(m_window.get(), QStringLiteral("Sleep in 60 s"), Sheet::Icon::Warning);
                top->setKeepOnTop(true);
                top->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
                top->addButton(QStringLiteral("Sleep now"), Sheet::Role::Destructive);
                top->open();
                sheets << top;
                oks << QPointer<QPushButton>();
                for (const QString& title : {QStringLiteral("Report ready"), QStringLiteral("Copy finished")}) {
                    auto* s = new Sheet(m_window.get(), title, Sheet::Icon::Info);
                    oks << s->addButton(QStringLiteral("OK"), Sheet::Role::Primary);
                    s->open();
                    sheets << s;
                }
                QVERIFY2(top->isAncestorOf(m_window->focusWidget()), qPrintable(what));
                for (const int k : order) {
                    sheets[k]->close(0);
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    QVERIFY2(!sheets[k], qPrintable(what));
                    QWidget* fw = m_window->focusWidget();
                    const bool right = sheets[0] ? sheets[0]->isAncestorOf(fw)
                                     : sheets[2] ? fw == oks[2].data()
                                     : sheets[1] ? fw == oks[1].data()
                                                 : fw == field;
                    if (!right)
                        wrong << what + QStringLiteral(", after %1: the focus is on %2")
                                            .arg(k)
                                            .arg(fw == field ? QStringLiteral("the field")
                                                 : fw        ? QString::fromLatin1(fw->metaObject()->className())
                                                             : QStringLiteral("nothing"));
                }
                QTRY_COMPARE(Sheet::openCount(), 0);
                cleanup();
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
        other.hide();
        m_window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    // ---- file manager -------------------------------------------------------------------

    void openInFileManagerReportsAMissingOutput()
    {
        QVERIFY(!openInFileManager(QString()));
        // Neither the file nor its folder exists: nothing to show, and nothing is started.
        QVERIFY(!openInFileManager(m_root.filePath(QStringLiteral("moved-away/out.exfat"))));
    }

    // Keep this test the last one: QCoreApplication::exit() stops every later
    // event loop of the thread from starting.
    void sheetExecWhenTheApplicationQuits()
    {
        // The nested loop ends without a button: that is the safe button's
        // index, not 0 = "the first button" (here: "Open folder").
        auto* s = new Sheet(m_window.get(), QStringLiteral("Done"), Sheet::Icon::Info);
        s->addButton(QStringLiteral("Open folder"), Sheet::Role::Primary);
        s->addButton(QStringLiteral("Close"), Sheet::Role::Safe);
        QTimer::singleShot(0, [] { QCoreApplication::exit(0); });
        QCOMPARE(s->exec(), 1);
        QCOMPARE(Sheet::openCount(), 0);
    }
};

QTEST_MAIN(TstUiWidgets)
#include "tst_ui_widgets.moc"
