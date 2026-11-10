// Offscreen tests of the UI library on its own (no AppContext, no pages):
// outside text is shown literally in labels, toasts, job rows and tooltips;
// the wheel leaves unfocused controls alone; Enter presses the focused
// button and a sheet's default button; the list editor, the job time
// estimate, the Library grid's keyboard menu and its cover cache.

#include "app/ui/Components.h"
#include "app/ui/HomeKit.h"
#include "app/ui/JobRows.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Sheet.h"
#include "app/ui/Shell.h"
#include "app/ui/Style.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QImage>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

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
        QCOMPARE(sideShot.pixelColor(side->width() - 1, 150), color::border_1);
        QVERIFY(sideShot.pixelColor(side->width() - 3, 150) != color::border_1);
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
