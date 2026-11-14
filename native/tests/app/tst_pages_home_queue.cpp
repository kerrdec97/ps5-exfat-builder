// Home and Queue pages in the real main window, with real jobs:
//
//  * Home › "Pick up where you left off" stops offering a failed job once
//    Retry was used (the retry is a new job), also after the retry succeeded.
//  * The time left the Queue and Home name is the whole job's.
//  * Layout facts of the form pages, Library, DLC and Home (the dock in view,
//    stable header buttons, the Contents list, the setup checklist).

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/BackportPage.h"
#include "app/pages/BuildImagePage.h"
#include "app/pages/CompareResultSheet.h"
#include "app/pages/DumpCheckSheet.h"
#include "app/pages/HomePage.h"
#include "app/pages/ImageToolsPage.h"
#include "app/pages/LibraryPage.h"
#include "app/pages/QueuePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/JobRows.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/FileUtil.h"
#include "core/Progress.h"
#include "jobs/Job.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTreeWidget>

#include <atomic>
#include <chrono>

using namespace fw;

namespace {

bool waitTerminal(JobQueue& q, const QString& id, int ms = 120000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && isTerminal(r->state);
    }, ms);
}

bool makeGame(const QString& dir)
{
    return fwtest::writeTextFile(dir + QStringLiteral("/sce_sys/param.json"),
                                 R"({"titleId":"PPSA00003","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Home Test"}}})")
           && fwtest::writeRandomFile(dir + QStringLiteral("/eboot.bin"), 90000, 5);
}

// A job in its first of two stages on a clock of its own, so its time left is
// known exactly: the stage's (about 200 s) and the whole job's (about 330 s,
// for the verifying still to come). It then waits, its clock standing still,
// until it is cancelled.
std::atomic<qint64> g_etaClockMs{1'000'000};

class TwoStageJob : public Job {
public:
    explicit TwoStageJob(QJsonObject p) : m_p(std::move(p)) {}
    QString kind() const override { return QStringLiteral("test.twostage"); }
    QString title() const override { return QStringLiteral("Two stages"); }
    QString subtitle() const override { return QStringLiteral("test"); }
    QStringList phases() const override { return {QStringLiteral("Writing"), QStringLiteral("Verifying")}; }
    QJsonObject parameters() const override { return m_p; }
    Status validate(const SettingsValues&) const override { return success(); }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        ctx.progress->setClockForTesting([] { return ProgressReporter::Clock::time_point(std::chrono::milliseconds(g_etaClockMs.load())); });
        ctx.progress->setPhases(phases(), {3.0, 1.0});
        ctx.progress->beginPhase(0, 300, 0, false); // 300 steps at one a second
        for (int i = 0; i < 100; ++i) {
            g_etaClockMs += 1000;
            ctx.progress->advance(1);
            (void)ctx.progress->snapshot();
        }
        ctx.progress->flush();
        while (ctx.token.checkpoint())
            QThread::msleep(5);
        return Error::cancelled();
    }

private:
    QJsonObject m_p;
};

// A paint device that keeps the texts painted on it, in the order drawn.
class TextRecorder : public QPaintEngine {
public:
    TextRecorder() : QPaintEngine(QPaintEngine::AllFeatures) {}
    bool begin(QPaintDevice*) override { return true; }
    bool end() override { return true; }
    void updateState(const QPaintEngineState&) override {}
    void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {}
    void drawPolygon(const QPointF*, int, PolygonDrawMode) override {}
    void drawPath(const QPainterPath&) override {}
    void drawTextItem(const QPointF&, const QTextItem& item) override { texts << item.text(); }
    Type type() const override { return QPaintEngine::User; }
    QStringList texts;
};

class TextDevice : public QPaintDevice {
public:
    explicit TextDevice(const QSize& size) : m_size(size) {}
    QPaintEngine* paintEngine() const override { return &recorder; }
    mutable TextRecorder recorder;

protected:
    int metric(PaintDeviceMetric m) const override
    {
        switch (m) {
        case PdmWidth: return m_size.width();
        case PdmHeight: return m_size.height();
        case PdmWidthMM: return m_size.width() * 254 / 960;
        case PdmHeightMM: return m_size.height() * 254 / 960;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY: return 96;
        case PdmDepth: return 32;
        case PdmNumColors: return INT_MAX;
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return int(QPaintDevice::devicePixelRatioFScale());
        default: return 0;
        }
    }

private:
    QSize m_size;
};

// Light pixels (a chevron in fg_3 on a dark row) in the branch area beside
// `index`: the step of the indentation just before the item, mirrored right
// to left.
int chevronPixels(QTreeView* tree, const QModelIndex& index)
{
    const QRect item = tree->visualRect(index);
    const int step = tree->indentation();
    const QRect box = tree->isRightToLeft() ? QRect(item.right() + 1, item.top(), step, item.height())
                                            : QRect(item.left() - step, item.top(), step, item.height());
    const QImage img = tree->viewport()->grab(box).toImage();
    int light = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            light += qGray(img.pixel(x, y)) > 90 ? 1 : 0;
    return light;
}

} // namespace

class TstPagesHomeQueue : public QObject {
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
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        m_ctx->mutableJobFactory().add(QStringLiteral("test.twostage"), QStringLiteral("Two stages"),
                                       [](const QJsonObject& p) -> Result<std::unique_ptr<Job>> { return std::unique_ptr<Job>(new TwoStageJob(p)); });
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void homeStopsOfferingARetriedJob()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build"))); // every engine is linked into the app tests
        auto* home = qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home")));
        QVERIFY(home);
        auto* bar = home->findChild<ui::InfoBar*>(QStringLiteral("home-continue"));
        QVERIFY(bar);
        auto& q = m_ctx->queue();

        // A build whose source disappears before it starts: it fails.
        const QString game = m_root.filePath(QStringLiteral("game"));
        QVERIFY(makeGame(game));
        q.holdQueue(true);
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.path(), QStringLiteral("home-test"), {});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        QVERIFY(QDir(game).removeRecursively());
        q.holdQueue(false);
        QVERIFY(waitTerminal(q, r.value()));
        QCOMPARE(q.record(r.value())->state, JobState::Failed);
        home->refresh();
        QVERIFY(!bar->isHidden());

        // The source is back; Retry queues a new job and the bar has done its part.
        QVERIFY(makeGame(game));
        ui::Link* retry = nullptr;
        for (auto* l : bar->findChildren<ui::Link*>())
            if (l->text() == QLatin1String("Retry"))
                retry = l;
        QVERIFY(retry);
        QStringList before;
        for (const JobRecord& rec : q.records())
            before << rec.id;
        emit retry->activated();
        QString second;
        for (const JobRecord& rec : q.records())
            if (!before.contains(rec.id))
                second = rec.id;
        QVERIFY(!second.isEmpty());
        QVERIFY(bar->isHidden());
        QVERIFY(waitTerminal(q, second));
        QVERIFY2(q.record(second)->state == JobState::Succeeded,
                 qPrintable(q.record(second)->error ? q.record(second)->error->message : QString()));
        // The old failed record is still in the queue, but it is not offered again.
        QVERIFY(q.record(r.value()).has_value());
        home->refresh();
        QVERIFY(bar->isHidden());
        q.clearFinished();
    }

    // The Queue's details name a job's parameters in words, and a job the user
    // cancelled is not reported as a failure in the status bar.
    void queueDetailsAndCounts()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.build"))); // every engine is linked into the app tests
        auto* queue = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(queue);
        auto& q = m_ctx->queue();
        const QString game = m_root.filePath(QStringLiteral("queue-game"));
        QVERIFY(makeGame(game));
        q.holdQueue(true);
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.path(), QStringLiteral("queue-test"), {},
                                        QJsonObject{{QStringLiteral("titleId"), QStringLiteral("PPSA00003")}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        m_window->navigate(QStringLiteral("queue"), {{QStringLiteral("jobId"), r.value()}});
        auto* detail = queue->findChild<QWidget*>(QStringLiteral("queue-detail"));
        QVERIFY(detail);
        QStringList labels;
        for (auto* l : detail->findChildren<QLabel*>())
            labels << l->text();
        QVERIFY2(labels.contains(QStringLiteral("Source")), qPrintable(labels.join(u'|')));
        QVERIFY(labels.contains(QStringLiteral("Output")));
        QVERIFY(labels.contains(QStringLiteral("Title ID")));
        QVERIFY(!labels.contains(QStringLiteral("TitleId")));
        QVERIFY(!queue->statusContext().contains(QStringLiteral("cancelled")));

        q.cancel(r.value());
        QVERIFY(waitTerminal(q, r.value()));
        QCOMPARE(q.record(r.value())->state, JobState::Cancelled);
        QVERIFY2(queue->statusContext().contains(QStringLiteral("0 failed · 1 cancelled")), qPrintable(queue->statusContext()));
        // The tab that lists it is named for what it holds (failed and cancelled jobs), with its count.
        auto* tabs = queue->findChild<ui::Segmented*>();
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->segmentText(4), QStringLiteral("Not finished 1"));
        q.holdQueue(false);
        q.clearFinished();
    }

    // The title and the page's main buttons are pinned: they stay where they
    // are, fully visible, however far the content is scrolled; the strip
    // shows its hairline only while content is under it; header buttons end
    // on one line with the content, with or without a scroll bar.
    void pageHeaderStaysPutWhenThePageScrolls()
    {
        int scrolledPages = 0;
        for (const QString& key : m_window->pageKeys()) {
            pages::Page* page = m_window->page(key);
            QVERIFY(page);
            m_window->navigate(key);
            QTest::qWait(30);
            ui::PageHeader* header = page->header();
            QScrollArea* scroll = page->scrollArea();
            // The header is not part of what scrolls.
            QVERIFY2(!scroll->widget()->isAncestorOf(header), qPrintable(key));
            if (header->isHidden()) {
                // A page without a header (Home): no strip, and the content starts 20 px down as before.
                QVERIFY2(page->headerBar()->isHidden(), qPrintable(key));
                QCOMPARE(scroll->mapTo(page, QPoint(0, 0)).y(), 0);
                QCOMPARE(scroll->widget()->layout()->contentsMargins().top(), theme::space::xl);
                continue;
            }
            QVERIFY2(page->headerBar()->isVisible(), qPrintable(key));
            QVERIFY2(!page->isHeaderLined(), qPrintable(key));
            // Header to content: 16 px, as between two cards.
            QCOMPARE(page->headerBar()->layout()->contentsMargins().bottom() + scroll->widget()->layout()->contentsMargins().top(),
                     theme::space::lg);
            auto rightEdges = [&] {
                const int headerRight = header->mapTo(page, QPoint(header->width(), 0)).x();
                const int contentRight = scroll->viewport()->mapTo(page, QPoint(scroll->viewport()->width(), 0)).x() - theme::space::xxl;
                return qMakePair(headerRight, contentRight);
            };
            QTRY_COMPARE(rightEdges().first, rightEdges().second);
            QScrollBar* bar = scroll->verticalScrollBar();
            if (bar->maximum() == 0)
                continue;
            ++scrolledPages;
            const QPoint before = header->mapTo(page, QPoint(0, 0));
            bar->setValue(bar->maximum());
            QTest::qWait(10);
            QVERIFY2(bar->value() > 0, qPrintable(key));
            QCOMPARE(header->mapTo(page, QPoint(0, 0)), before);
            QCOMPARE(header->visibleRegion().boundingRect(), header->rect());
            QVERIFY2(page->isHeaderLined(), qPrintable(key));
            QCOMPARE(rightEdges().first, rightEdges().second);
            bar->setValue(0);
            QVERIFY2(!page->isHeaderLined(), qPrintable(key));
        }
        QVERIFY2(scrolledPages >= 3, "the long pages (Build, Settings, Backport...) scroll in a 1440 x 920 window");
        m_window->navigate(QStringLiteral("home"));
    }

    // Every page keeps the scroll bar's room (nothing moves sideways between
    // pages or when content grows); on a form + dock page in a wide window the
    // header's buttons end where the dock does; a narrow window folds the
    // sidebar to its rail and a wide one opens it again; toasts stay above
    // the status bar.
    void shellKeepsItsLinesAtEveryWidth()
    {
        auto restore = qScopeGuard([this] {
            m_window->resize(1440, 920);
            m_window->setSidebarCompact(false);
            m_window->navigate(QStringLiteral("home"));
        });
        int viewport = -1;
        for (const QString& key : m_window->pageKeys()) {
            m_window->navigate(key);
            QTest::qWait(20);
            QScrollArea* scroll = m_window->page(key)->scrollArea();
            QCOMPARE(scroll->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
            if (viewport < 0)
                viewport = scroll->viewport()->width();
            QVERIFY2(scroll->viewport()->width() == viewport, qPrintable(key));
        }
        // Build image with the sidebar folded: 1440 px leave room past the form and the dock.
        m_window->setSidebarCompact(true);
        auto* build = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build);
        m_window->navigate(QStringLiteral("build"));
        QTest::qWait(30);
        auto rightOf = [&](QWidget* w) { return w->mapTo(build, QPoint(w->width(), 0)).x(); };
        QTRY_COMPARE(rightOf(build->primaryButton()), rightOf(build->dock()));
        QVERIFY(rightOf(build->primaryButton()) < rightOf(build->header()) - theme::space::xxl);
        m_window->setSidebarCompact(false);
        // Narrow: the rail; wide again: the sidebar as the user left it.
        m_window->resize(1100, 760);
        QTRY_VERIFY(m_window->sidebar()->isCompact());
        m_window->resize(1440, 920);
        QTRY_VERIFY(!m_window->sidebar()->isCompact());
        // Toasts: above the status bar.
        m_window->toast(ui::Tone::Ok, QStringLiteral("Done"), QStringLiteral("All good"));
        QFrame* toast = nullptr;
        for (QFrame* f : m_window->findChildren<QFrame*>(QStringLiteral("toast")))
            if (f->isVisible())
                toast = f;
        QVERIFY(toast);
        const int statusTop = m_window->statusBarWidget()->mapTo(m_window.get(), QPoint(0, 0)).y();
        QTRY_VERIFY(toast->geometry().bottom() < statusTop - theme::space::md);
        QVERIFY(toast->geometry().right() < m_window->width() - theme::space::lg - theme::size::scrollBar + 1);
        m_window->toasts()->clear();
    }

    // The start buttons stay enabled and say, as a tooltip, the first thing
    // the form already knows to be missing; the tooltip goes with the reason.
    void startButtonsSayWhatIsMissing()
    {
        auto* build = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build);
        m_window->navigate(QStringLiteral("build"));
        QVERIFY(build->primaryButton()->isEnabled());
        QCOMPARE(build->primaryButton()->toolTip(), QStringLiteral("Choose the game dump first."));
        QCOMPARE(build->queueButton()->toolTip(), build->primaryButton()->toolTip());
        const QString game = m_root.filePath(QStringLiteral("hint-game"));
        QVERIFY(makeGame(game));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("source"), game}});
        QCOMPARE(build->primaryButton()->toolTip(), QString());
        QVERIFY(build->primaryButton()->isEnabled());

        auto* tools = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("imagetools")));
        QVERIFY(tools);
        QCOMPARE(tools->primaryButton()->toolTip(), QStringLiteral("Open an image first."));
        auto* dlc = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("dlc")));
        QVERIFY(dlc);
        QCOMPARE(dlc->primaryButton()->toolTip(), QStringLiteral("Add DLC INIs or DLC packages first."));

        // Backport: nothing is ticked in an empty library.
        pages::Page* backport = m_window->page(QStringLiteral("backport"));
        QVERIFY(backport);
        QPushButton* start = nullptr;
        for (QPushButton* b : backport->header()->findChildren<QPushButton*>())
            if (b->text() == QLatin1String("Start Backport"))
                start = b;
        QVERIFY(start);
        QVERIFY(start->isEnabled());
        QCOMPARE(start->toolTip(), QStringLiteral("Select at least one game in the Game library."));
        m_window->navigate(QStringLiteral("home"));
    }

    // The pinned start button can be pressed with the form scrolled anywhere:
    // what it answers (at the form's top) is brought into view.
    void formErrorIsBroughtIntoView()
    {
        auto* dlc = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("dlc")));
        QVERIFY(dlc);
        m_window->navigate(QStringLiteral("dlc"));
        QTest::qWait(30);
        QScrollBar* bar = dlc->scrollArea()->verticalScrollBar();
        QVERIFY2(bar->maximum() > 0, "the DLC form is longer than a 1440 x 920 window");
        bar->setValue(bar->maximum());
        QVERIFY(dlc->isHeaderLined());
        QVERIFY(dlc->primaryButton()->visibleRegion().boundingRect() == dlc->primaryButton()->rect()); // still there to press
        QTest::mouseClick(dlc->primaryButton(), Qt::LeftButton); // nothing was added: the form says so
        QCOMPARE(bar->value(), 0);
        ui::InfoBar* error = nullptr;
        for (ui::InfoBar* b : dlc->scrollArea()->widget()->findChildren<ui::InfoBar*>())
            if (!b->isHidden())
                for (const QLabel* l : b->findChildren<QLabel*>())
                    if (l->text() == QLatin1String("Add DLC INIs or DLC packages first."))
                        error = b;
        QVERIFY(error);
        QTRY_VERIFY(!error->visibleRegion().isEmpty());
        dlc->clearDemoState(); // takes the error away again
        m_window->navigate(QStringLiteral("home"));
    }

    // A folder dragged over a page that does not take it itself: the sidebar
    // marks the page the drop opens (Build), and the mark goes when the drag
    // leaves. Over the Build page its own drop zone lights up instead.
    void dragOverTheWindowShowsWhereTheDropGoes()
    {
        const QString game = m_root.filePath(QStringLiteral("drag-game"));
        QVERIFY(makeGame(game));
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(game)});
        ui::NavItem* buildItem = m_window->sidebar()->item(QStringLiteral("build"));
        ui::NavItem* toolsItem = m_window->sidebar()->item(QStringLiteral("imagetools"));
        QVERIFY(buildItem && toolsItem);
        // An image goes to Image tools (the name is enough to tell).
        const QString image = m_root.filePath(QStringLiteral("drag.exfat"));
        QVERIFY(fwtest::writeRandomFile(image, 4096, 3));
        QMimeData imageMime;
        imageMime.setUrls({QUrl::fromLocalFile(image)});

        m_window->navigate(QStringLiteral("queue"));
        QDragEnterEvent enter(QPoint(600, 400), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window.get(), &enter);
        QVERIFY(buildItem->isDropTarget());
        QVERIFY(!toolsItem->isDropTarget());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(m_window.get(), &leave);
        QVERIFY(!buildItem->isDropTarget());
        QDragEnterEvent enterImage(QPoint(600, 400), Qt::CopyAction, &imageMime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window.get(), &enterImage);
        QVERIFY(toolsItem->isDropTarget());
        QVERIFY(!buildItem->isDropTarget());
        QCoreApplication::sendEvent(m_window.get(), &leave);
        QVERIFY(!toolsItem->isDropTarget());

        // Something no page opens: no mark anywhere.
        const QString note = m_root.filePath(QStringLiteral("note.txt"));
        QVERIFY(fwtest::writeTextFile(note, "x"));
        QMimeData other;
        other.setUrls({QUrl::fromLocalFile(note)});
        QDragEnterEvent enterOther(QPoint(600, 400), Qt::CopyAction, &other, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window.get(), &enterOther);
        QVERIFY(!buildItem->isDropTarget() && !toolsItem->isDropTarget());
        QCoreApplication::sendEvent(m_window.get(), &leave);

        // The Build page takes a game folder itself.
        auto* build = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build && build->sourceCard());
        m_window->navigate(QStringLiteral("build"));
        QDragEnterEvent enterBuild(QPoint(600, 400), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window.get(), &enterBuild);
        QVERIFY(!buildItem->isDropTarget());
        QVERIFY(build->sourceCard()->dropHighlight());
        QCoreApplication::sendEvent(m_window.get(), &leave);
        QVERIFY(!build->sourceCard()->dropHighlight());
        m_window->navigate(QStringLiteral("home"));
    }

    // Backport › Status: the library's badge. The title and the status share
    // the table's width by that width alone, so no column moves while a run
    // changes the states; a state the column has to cut is whole in the row's
    // tooltip, always.
    void backportStatusColumnNeverCutsAStateWithoutATooltip()
    {
        using pages::BackportGameModel;
        using pages::BackportRow;
        // The badge's text, tone and width.
        BackportRow busy;
        busy.status = QStringLiteral("Patching, signing and packaging");
        busy.tone = QStringLiteral("busy");
        busy.pct = 42;
        QCOMPARE(BackportGameModel::statusText(busy), QStringLiteral("Patching, signing and packaging 42%"));
        QCOMPARE(BackportGameModel::statusTone(busy), ui::Tone::Info);
        // Room for the percentage that is still growing: the column does not twitch.
        QVERIFY(BackportGameModel::statusWidth(busy) >= ui::badgeWidth(QStringLiteral("Patching, signing and packaging 100%"), true));
        BackportRow ready;
        QCOMPARE(BackportGameModel::statusText(ready), QStringLiteral("Ready"));
        QCOMPARE(BackportGameModel::statusTone(ready), ui::Tone::Ok);
        BackportRow muted;
        muted.tone = QStringLiteral("muted");
        QCOMPARE(BackportGameModel::statusTone(muted), ui::Tone::Neutral);
        BackportRow bad;
        bad.tone = QStringLiteral("bad");
        QCOMPARE(BackportGameModel::statusTone(bad), ui::Tone::Err);

        pages::Page* page = m_window->page(QStringLiteral("backport"));
        QVERIFY(page);
        m_window->navigate(QStringLiteral("backport"));
        page->applyDemoState(QStringLiteral("running"));
        const auto leave = qScopeGuard([&] {
            page->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        QTest::qWait(30);
        auto* table = page->findChild<QTableView*>();
        QVERIFY(table);
        auto* proxy = qobject_cast<QSortFilterProxyModel*>(table->model());
        QVERIFY(proxy);
        auto* model = qobject_cast<BackportGameModel*>(proxy->sourceModel());
        QVERIFY(model && !model->rows().empty());
        bool anyBusy = false;
        for (int r = 0; r < int(model->rows().size()); ++r) {
            const BackportRow& row = model->rows()[size_t(r)];
            anyBusy = anyBusy || row.tone == QLatin1String("busy");
            // Whole in the tooltip of every cell of the row, cut or not.
            for (int c = 0; c < BackportGameModel::ColumnCount; ++c) {
                const QString tip = model->index(r, c).data(Qt::ToolTipRole).toString();
                QVERIFY2(tip.contains(QStringLiteral("Status: %1").arg(BackportGameModel::statusText(row)).toHtmlEscaped()), qPrintable(tip));
            }
        }
        QVERIFY(anyBusy);
        auto* header = table->horizontalHeader();
        const int column = table->columnWidth(BackportGameModel::StatusCol);
        QVERIFY2(column >= BackportGameModel::kStatusColMin - 2, qPrintable(QString::number(column)));
        const int title = table->columnWidth(BackportGameModel::TitleCol);
        QVERIFY2(title >= BackportGameModel::kTitleColMin - 2, qPrintable(QString::number(title)));
        // The columns depend on the table's width, not on the states in it: a
        // state that grows during a run moves nothing (PPSA, SDK and FW stay put).
        const auto positions = [&] {
            QList<int> at;
            for (int c = 0; c < BackportGameModel::ColumnCount; ++c)
                at << (table->isColumnHidden(c) ? -1 : header->sectionPosition(c)) << table->columnWidth(c);
            return at;
        };
        const QList<int> before = positions();
        BackportRow* busyRow = nullptr;
        for (int r = 0; r < int(model->rows().size()); ++r)
            if (model->rows()[size_t(r)].tone == QLatin1String("busy"))
                busyRow = &model->rows()[size_t(r)];
        QVERIFY(busyRow);
        busyRow->status = QStringLiteral("Writing the backport files and checking every signature of the game");
        busyRow->pct = 99;
        model->rowChanged(busyRow->game.key);
        QTest::qWait(30);
        QCOMPARE(positions(), before);
    }

    // Home: the mark of each status tile says its state (quiet when there is
    // nothing to say, danger for failed jobs with nothing running).
    void homeTilesCarryTheToneOfTheirState()
    {
        auto* home = qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home")));
        QVERIFY(home);
        m_window->navigate(QStringLiteral("home"));
        auto* console = home->findChild<ui::StatTile*>(QStringLiteral("home-console"));
        auto* queue = home->findChild<ui::StatTile*>(QStringLiteral("home-queue"));
        auto* space = home->findChild<ui::StatTile*>(QStringLiteral("home-space"));
        QVERIFY(console && queue && space);
        QCOMPARE(console->iconTone(), ui::Tone::Neutral); // no console set up
        home->setConsoleStatus(QStringLiteral("PS5 at 192.168.1.40"), ui::Tone::Ok);
        QCOMPARE(console->iconTone(), ui::Tone::Ok);
        home->setConsoleStatus(QString(), ui::Tone::Neutral);
        QCOMPARE(console->iconTone(), ui::Tone::Neutral);

        auto& q = m_ctx->queue();
        q.clearFinished();
        home->refresh();
        QCOMPARE(queue->iconTone(), ui::Tone::Neutral); // idle
        // A build whose source disappears before it starts fails: danger, as nothing runs.
        const QString game = m_root.filePath(QStringLiteral("tone-game"));
        QVERIFY(makeGame(game));
        q.holdQueue(true);
        auto r = jobs::submitImageBuild(*m_ctx, jobs::kFormatExfat, game, m_root.path(), QStringLiteral("tone-test"), {});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        QVERIFY(QDir(game).removeRecursively());
        q.holdQueue(false);
        QVERIFY(waitTerminal(q, r.value()));
        QCOMPARE(q.record(r.value())->state, JobState::Failed);
        home->refresh();
        QCOMPARE(queue->iconTone(), ui::Tone::Err);
        q.clearFinished();
        home->refresh();
        QCOMPARE(queue->iconTone(), ui::Tone::Neutral);
    }

    // "Running job: about … left" (Queue) and the Home queue tile name the
    // whole job's time left, not the time its current stage still needs.
    void timeLeftIsTheWholeJobs()
    {
        auto* queue = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        auto* home = qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home")));
        QVERIFY(queue && home);
        auto* tile = home->findChild<ui::StatTile*>(QStringLiteral("home-queue"));
        QVERIFY(tile);
        auto& q = m_ctx->queue();
        q.clearFinished();
        auto r = q.enqueue(QStringLiteral("test.twostage"), QJsonObject{});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().message));
        const QString id = r.value();
        const auto cleanup = qScopeGuard([&] {
            q.cancel(id);
            (void)waitTerminal(q, id);
            q.clearFinished();
        });
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            const auto rec = q.record(id);
            return rec && rec->state == JobState::Running && rec->progress.done == 100;
        }(), 20000);
        const JobRecord rec = *q.record(id);
        const qint64 job = ui::jobTimeLeftMs(rec);
        QVERIFY2(job > rec.progress.etaMs + 5000, qPrintable(QStringLiteral("job %1, stage %2").arg(job).arg(rec.progress.etaMs)));
        const QString jobText = formatTimeLeft(job).toLower();
        QVERIFY2(jobText != formatTimeLeft(rec.progress.etaMs).toLower(), qPrintable(jobText));

        m_window->navigate(QStringLiteral("queue"));
        QString caption;
        QTRY_VERIFY([&] {
            for (const QLabel* l : queue->findChildren<QLabel*>())
                if (l->text().startsWith(QLatin1String("Running job: ")))
                    caption = l->text();
            return !caption.isEmpty();
        }());
        QCOMPARE(caption, QStringLiteral("Running job: %1").arg(jobText));

        m_window->navigate(QStringLiteral("home"));
        home->refresh();
        QStringList texts;
        for (const QLabel* l : tile->findChildren<QLabel*>())
            texts << l->text();
        QVERIFY2(texts.contains(QStringLiteral("Writing · %1").arg(jobText)), qPrintable(texts.join(u'|')));
    }

    // A long form keeps the Jobs dock in view: however far it is scrolled,
    // the dock column stays where it was at the top, beside the form and
    // never below its end; narrow (wrapped under the form) it scrolls along.
    void jobsDockStaysInViewOnLongForms()
    {
        auto* build = qobject_cast<pages::BuildImagePage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(build);
        m_window->navigate(QStringLiteral("build"));
        QScrollBar* bar = build->scrollArea()->verticalScrollBar();
        const auto restore = qScopeGuard([&] {
            bar->setValue(0);
            m_window->navigate(QStringLiteral("home"));
        });
        QTRY_VERIFY2(bar->maximum() > 100, "the Build form is longer than a 1440 x 920 window");
        QTest::qWait(30);
        QWidget* aside = build->asideBody();
        QVERIFY(aside && aside->isVisible());
        QVERIFY(aside->isAncestorOf(build->dock()));
        auto* columns = dynamic_cast<ui::TwoColumnLayout*>(aside->parentWidget()->layout());
        QVERIFY(columns);
        // Wide enough for the two columns side by side (the offscreen platform's
        // fallback fonts make some rows wider than on Windows).
        m_window->resize(1800, 920);
        const auto wide = qScopeGuard([&] { m_window->resize(1440, 920); });
        QTRY_VERIFY2(!columns->isStacked(), qPrintable(QStringLiteral("the dock beside the form: %1 wide, the form at least %2")
                                                           .arg(aside->parentWidget()->width())
                                                           .arg(columns->itemAt(0)->minimumSize().width())));
        QTest::qWait(30);
        const QPoint rest = aside->mapTo(build, QPoint(0, 0));
        QWidget* viewport = build->scrollArea()->viewport();
        const auto where = [&] { return QRect(aside->mapTo(viewport, QPoint(0, 0)), aside->size()); };
        const auto inView = [&] { return viewport->rect().contains(where()); };
        const auto said = [&] {
            const QRect r = where();
            return QStringLiteral("dock %1,%2 %3x%4 in a %5x%6 view, scrolled %7")
                .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(viewport->width()).arg(viewport->height()).arg(bar->value());
        };
        QVERIFY2(inView(), qPrintable(said()));
        for (const int v : {bar->maximum() / 3, bar->maximum() / 2, bar->maximum()}) {
            bar->setValue(v);
            QTRY_COMPARE(aside->mapTo(build, QPoint(0, 0)), rest);
            QVERIFY2(inView(), qPrintable(said()));
            QVERIFY(aside->geometry().bottom() <= aside->parentWidget()->rect().bottom()); // never past the form
        }
        bar->setValue(0);
        QTRY_COMPARE(aside->mapTo(build, QPoint(0, 0)), rest);
        // Wrapped under the form (a narrow window), it stays in its place in the content.
        m_window->resize(1100, 920);
        QTRY_VERIFY(columns->isStacked());
        // Under 1200 px the sidebar folds to its rail, which widens the content
        // and moves the dock up while it happens: wait until it has settled.
        int settled = -1;
        QTRY_VERIFY([&] {
            const int y = aside->y();
            const bool same = y > 0 && y == settled;
            settled = y;
            return same;
        }());
        const int below = aside->y();
        bar->setValue(bar->maximum());
        QTest::qWait(10);
        QCOMPARE(aside->y(), below);
    }

    // Image tools: "Add to queue" stays in the header for every action (an
    // edit and a comparison start at once: there, unavailable, saying why);
    // the Contents list has 28 px rows and is as tall as its rows between
    // 240 and 520 px; Convert offers the Build page's format tiles; the
    // lone icon buttons of the image strip have their own glyphs.
    void imageToolsKeepsItsButtonsAndFitsItsList()
    {
        auto* tools = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        QVERIFY(tools);
        m_window->navigate(QStringLiteral("imagetools"));
        const auto restore = qScopeGuard([&] {
            tools->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        tools->applyDemoState(QStringLiteral("extract"));
        QVERIFY(tools->queueButton()->isVisibleTo(tools));
        QVERIFY(tools->queueButton()->isEnabled());
        bool show = false, change = false;
        for (ui::IconButton* b : tools->sourceCard()->findChildren<ui::IconButton*>())
            show = show || (b->accessibleName() == QLatin1String("Show in folder") && b->iconName() == QLatin1String("external"));
        for (QPushButton* b : tools->sourceCard()->findChildren<QPushButton*>())
            change = change || b->text() == QStringLiteral("Change…");
        QVERIFY2(show && change, "Show in folder has a glyph of its own beside Change…");

        tools->applyDemoState(QStringLiteral("edit"));
        QVERIFY(tools->queueButton()->isVisibleTo(tools));
        QVERIFY(!tools->queueButton()->isEnabled());
        QCOMPARE(tools->queueButton()->toolTip(), QStringLiteral("This action starts at once and is not added to the queue."));
        // The count beside the tool buttons wraps (German) only between its
        // parts, never between a number and its unit.
        QStringList captions;
        for (QLabel* l : tools->findChildren<QLabel*>())
            if (l->isVisibleTo(tools) && l->text().contains(QStringLiteral("files")))
                captions << l->text();
        QVERIFY2(captions.contains(QStringLiteral("18,442 files · 45.2 GB")), qPrintable(captions.join(u'|')));
        QTreeWidget* tree = tools->tree();
        QVERIFY(tree->topLevelItemCount() >= 4);
        QVERIFY(tree->rootIsDecorated());
        QVERIFY2(tree->sizeHintForRow(0) >= theme::size::rowCompact, qPrintable(QString::number(tree->sizeHintForRow(0))));
        const int few = tree->height();
        QVERIFY2(few >= 240 && few <= 520, qPrintable(QString::number(few)));
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            tree->topLevelItem(i)->setExpanded(true); // more rows: a taller list, still within its bounds
        QVERIFY2(tree->height() > few && tree->height() <= 520, qPrintable(QStringLiteral("%1 → %2").arg(few).arg(tree->height())));
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            tree->topLevelItem(i)->setExpanded(false);
        QCOMPARE(tree->height(), 240);

        tools->applyDemoState(QStringLiteral("compare"));
        QVERIFY(tools->queueButton()->isVisibleTo(tools) && !tools->queueButton()->isEnabled());
        tools->applyDemoState(QStringLiteral("convert"));
        QVERIFY(tools->queueButton()->isEnabled());
        pages::OptionTiles* tiles = nullptr;
        for (QWidget* w : tools->findChildren<QWidget*>())
            if (w->accessibleName() == QLatin1String("Target format") && w->isVisibleTo(tools))
                tiles = dynamic_cast<pages::OptionTiles*>(w);
        QVERIFY(tiles);
        QCOMPARE(tiles->count(), 3);
        QCOMPARE(tools->convertTarget(), jobs::kFormatFfpfsc);
    }

    // Library: Rescan and Add folder… only on the tabs they act on (Games,
    // Images), the overflow menu last; an empty library makes Add folder…
    // the main button and Rescan rests while there is nowhere to look.
    void libraryHeaderFollowsTheTab()
    {
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(lib);
        m_window->navigate(QStringLiteral("library"));
        const auto restore = qScopeGuard([&] {
            lib->setTab(pages::LibraryPage::GamesTab);
            m_window->navigate(QStringLiteral("home"));
        });
        lib->setTab(pages::LibraryPage::GamesTab);
        QPushButton *rescan = nullptr, *add = nullptr;
        for (QPushButton* b : lib->header()->findChildren<QPushButton*>()) {
            if (b->text() == QLatin1String("Rescan"))
                rescan = b;
            else if (b->text() == QStringLiteral("Add folder…"))
                add = b;
        }
        ui::IconButton* more = nullptr;
        for (ui::IconButton* b : lib->header()->findChildren<ui::IconButton*>())
            if (b->accessibleName() == QLatin1String("More"))
                more = b;
        QVERIFY(rescan && add && more);
        QVERIFY(rescan->isVisibleTo(lib) && add->isVisibleTo(lib) && more->isVisibleTo(lib));
        QTRY_VERIFY(rescan->x() < add->x() && add->x() < more->x()); // same parent: the menu is last
        QCOMPARE(rescan->parentWidget(), more->parentWidget());
        QCOMPARE(lib->gamesView()->count(), 0); // nothing was added in this program
        QCOMPARE(add->property("variant").toString(), QStringLiteral("primary"));
        QCOMPARE(rescan->isEnabled(), !lib->imageFolders().isEmpty());
        lib->setTab(pages::LibraryPage::ImagesTab);
        QVERIFY(rescan->isVisibleTo(lib) && add->isVisibleTo(lib) && more->isVisibleTo(lib));
        lib->setTab(pages::LibraryPage::LanguagesTab);
        QVERIFY(!rescan->isVisibleTo(lib) && !add->isVisibleTo(lib) && !more->isVisibleTo(lib));
        lib->setTab(pages::LibraryPage::DlcTab);
        QVERIFY(!rescan->isVisibleTo(lib) && !add->isVisibleTo(lib) && !more->isVisibleTo(lib));
        // The DLC emulator's empty list counts "0 of 32 KiB", not "0.0".
        bool counted = false;
        for (QLabel* l : lib->findChildren<QLabel*>())
            counted = counted || (l->isVisibleTo(lib) && l->text() == QStringLiteral("0 entries · 0 of 32 KiB"));
        QVERIFY(counted);
    }

    // Queue: with nothing in it, its header buttons stay where they are and
    // rest; a held queue can always be let go.
    void queueButtonsRestOnAnEmptyQueue()
    {
        auto* queue = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(queue);
        auto& q = m_ctx->queue();
        q.clearFinished();
        m_window->navigate(QStringLiteral("queue"));
        const auto restore = qScopeGuard([&] {
            q.holdQueue(false);
            m_window->navigate(QStringLiteral("home"));
        });
        QVERIFY(!q.isHeld());
        QPushButton* sw = queue->queueSwitchButton();
        QPushButton* clear = nullptr;
        for (QPushButton* b : queue->header()->findChildren<QPushButton*>())
            if (b->text() == QLatin1String("Clear finished"))
                clear = b;
        QVERIFY(clear);
        QTRY_VERIFY(sw->isVisibleTo(queue) && !sw->isEnabled());
        QCOMPARE(sw->toolTip(), QStringLiteral("Nothing in the queue"));
        QVERIFY(clear->isVisibleTo(queue) && !clear->isEnabled());
        q.holdQueue(true);
        QTRY_VERIFY(sw->isEnabled());
        q.holdQueue(false);
        QTRY_VERIFY(!sw->isEnabled());
    }

    // Home: a setup step's help is whole (two lines if it needs them) with
    // its link under it, and Recent jobs ends on one line with the setup card.
    void homeChecklistAndCardsLineUp()
    {
        auto* home = qobject_cast<pages::HomePage*>(m_window->page(QStringLiteral("home")));
        QVERIFY(home);
        m_window->navigate(QStringLiteral("home"));
        home->refresh();
        QTest::qWait(30); // the checklist rows are made again by refresh(): laid out
        QWidget* setup = home->findChild<QWidget*>(QStringLiteral("home-setup"));
        QWidget* recent = home->findChild<QWidget*>(QStringLiteral("home-recent"));
        QVERIFY(setup && recent);
        QVERIFY2(setup->isVisible(), "no console is set up in this program: the checklist shows");
        QTRY_COMPARE(recent->mapTo(home, QPoint(0, 0)).y(), setup->mapTo(home, QPoint(0, 0)).y());
        QTRY_VERIFY2(recent->height() >= setup->height(), qPrintable(QStringLiteral("%1 / %2").arg(recent->height()).arg(setup->height())));
        int withLinks = 0;
        for (const ui::ChecklistItem& item : home->checklist()) {
            if (item.fixText.isEmpty())
                continue;
            QWidget* row = home->findChild<QWidget*>(QStringLiteral("checklist-") + item.id);
            QVERIFY(row);
            QLabel* detail = nullptr;
            ui::Link* fix = nullptr;
            for (QLabel* l : row->findChildren<QLabel*>()) {
                if (auto* link = qobject_cast<ui::Link*>(l); link && link->text() == item.fixText)
                    fix = link;
                else if (l->text() == item.detail)
                    detail = l;
            }
            QVERIFY2(detail && fix, qPrintable(item.id));
            QTRY_VERIFY2(fix->mapTo(row, QPoint(0, 0)).y() >= detail->mapTo(row, QPoint(0, detail->height())).y(),
                     qPrintable(QStringLiteral("%1: link at %2, help %3 + %4, row %5 high, visible %6")
                                    .arg(item.id)
                                    .arg(fix->mapTo(row, QPoint(0, 0)).y())
                                    .arg(detail->mapTo(row, QPoint(0, 0)).y())
                                    .arg(detail->height())
                                    .arg(row->height())
                                    .arg(row->isVisible())));
            QVERIFY(detail->wordWrap());
            ++withLinks;
        }
        QVERIFY(withLinks >= 1);
    }

    // DLC: the list is as tall as its rows (no empty band under a short
    // list), the picture column has no word over it (its name on hover),
    // titles and results wrap rather than end in "…" at once.
    void dlcListFitsItsRows()
    {
        auto* dlc = qobject_cast<pages::JobFormPage*>(m_window->page(QStringLiteral("dlc")));
        QVERIFY(dlc);
        m_window->navigate(QStringLiteral("dlc"));
        const auto restore = qScopeGuard([&] {
            dlc->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        QTableWidget* table = nullptr;
        for (QTableWidget* t : dlc->findChildren<QTableWidget*>())
            if (t->accessibleName() == QLatin1String("DLC entries"))
                table = t;
        QVERIFY(table);
        dlc->applyDemoState(QStringLiteral("list"));
        QTRY_VERIFY(table->isVisible() && table->rowCount() == 3);
        QVERIFY(table->wordWrap());
        QCOMPARE(table->horizontalHeaderItem(0)->text(), QString());
        QCOMPARE(table->horizontalHeaderItem(0)->toolTip(), QStringLiteral("Icon"));
        const int rowsHeight = table->horizontalHeader()->sizeHint().height() + 3 * table->verticalHeader()->defaultSectionSize();
        QVERIFY2(table->height() >= rowsHeight && table->height() <= rowsHeight + 8,
                 qPrintable(QStringLiteral("%1 for %2").arg(table->height()).arg(rowsHeight)));
    }

    // An icon button among labelled buttons has the secondary button's plate
    // from the application style sheet (no style sheet of its own), and its
    // resting look when disabled.
    void outlinedIconButtonsTakeTheThemesPlate()
    {
        QWidget host;
        host.resize(80, 60);
        auto* b = new ui::IconButton(QStringLiteral("refresh"), QStringLiteral("Refresh"), ui::IconButton::Size::Regular, &host);
        b->move(10, 10);
        pages::outlineIconButton(b);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QVERIFY2(b->styleSheet().isEmpty(), qPrintable(b->styleSheet()));
        QCOMPARE(b->property("outline").toBool(), true);
        // Inside the 1 px outline, clear of the 18 px glyph in the middle.
        const auto plate = [b] { return b->grab().toImage().pixelColor(4, b->height() / 2); };
        const auto near = [](const QColor& a, const QColor& c) {
            return std::abs(a.red() - c.red()) + std::abs(a.green() - c.green()) + std::abs(a.blue() - c.blue()) <= 12;
        };
        QVERIFY2(near(plate(), theme::color::bg_4), qPrintable(plate().name()));
        b->setEnabled(false);
        QVERIFY2(near(plate(), theme::color::bg_2), qPrintable(plate().name()));
    }

    // Every tree of the app shows a folder's chevron in its branch area, drawn
    // by the style: Image tools › Contents, the dump check's groups and the
    // comparison's groups.
    void treesShowTheirFolderChevrons()
    {
        auto* tools = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(tools && lib);
        const auto restore = qScopeGuard([&] {
            tools->clearDemoState();
            lib->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        const auto check = [](QTreeView* tree, const char* what) {
            QVERIFY2(tree && tree->isVisible(), what);
            QModelIndex folder;
            for (int r = 0; r < tree->model()->rowCount() && !folder.isValid(); ++r)
                if (tree->model()->hasChildren(tree->model()->index(r, 0)))
                    folder = tree->model()->index(r, 0);
            QVERIFY2(folder.isValid(), what);
            QModelIndex leafRow;
            for (int r = 0; r < tree->model()->rowCount() && !leafRow.isValid(); ++r)
                if (!tree->model()->hasChildren(tree->model()->index(r, 0)))
                    leafRow = tree->model()->index(r, 0);
            QTRY_VERIFY2(chevronPixels(tree, folder) >= 6, qPrintable(QStringLiteral("%1: %2 light pixels beside a folder")
                                                                             .arg(QLatin1String(what)).arg(chevronPixels(tree, folder))));
            if (leafRow.isValid()) // nothing beside a row without children
                QCOMPARE(chevronPixels(tree, leafRow), 0);
        };
        m_window->navigate(QStringLiteral("imagetools"));
        tools->applyDemoState(QStringLiteral("edit"));
        QTRY_VERIFY(tools->tree()->isVisible() && tools->tree()->topLevelItemCount() > 0);
        check(tools->tree(), "Image tools contents");
        {
            // A selected row keeps the selection fill under its branch area
            // (the style draws it there with or without a branch rule).
            QTreeWidget* tree = tools->tree();
            QTreeWidgetItem* file = nullptr;
            for (int i = 0; i < tree->topLevelItemCount() && !file; ++i)
                if (tree->topLevelItem(i)->childCount() == 0)
                    file = tree->topLevelItem(i);
            QVERIFY(file);
            tree->setCurrentItem(file);
            QTRY_VERIFY(file->isSelected());
            const QRect item = tree->visualRect(tree->model()->index(tree->indexOfTopLevelItem(file), 0));
            const QColor branch = tree->viewport()->grab().toImage().pixelColor(item.left() - tree->indentation() / 2, item.top() + 3);
            QCOMPARE(branch.name(), theme::color::accent_lo.name());
            tree->clearSelection();
        }

        tools->applyDemoState(QStringLiteral("compare-result"));
        pages::CompareResultSheet* compare = nullptr;
        QTRY_VERIFY([&] {
            for (auto* s : m_window->findChildren<pages::CompareResultSheet*>())
                if (s->isOpen())
                    compare = s;
            return compare && compare->list() && compare->list()->isVisible();
        }());
        check(compare->list(), "comparison groups");
        tools->clearDemoState();

        m_window->navigate(QStringLiteral("library"));
        lib->applyDemoState(QStringLiteral("games-check"));
        pages::DumpCheckSheet* dump = nullptr;
        QTRY_VERIFY([&] {
            for (auto* s : m_window->findChildren<pages::DumpCheckSheet*>())
                if (s->isOpen())
                    dump = s;
            return dump && dump->list()->isVisible();
        }());
        check(dump->list(), "dump check groups");
    }

    // Screenshot demos of a job count it where the real job would be counted:
    // the Jobs dock beside the Build monitor (its count, its footer, the
    // finished job's row) and the Queue's list; the Build header names the
    // FPKG format of the demo job.
    void demoJobsShowWhereTheRealOnesWould()
    {
        auto* build = qobject_cast<pages::BuildImagePage*>(m_window->page(QStringLiteral("build")));
        auto* queue = qobject_cast<pages::QueuePage*>(m_window->page(QStringLiteral("queue")));
        QVERIFY(build && queue);
        m_ctx->queue().clearFinished();
        const auto restore = qScopeGuard([&] {
            build->clearDemoState();
            queue->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        m_window->navigate(QStringLiteral("build"));
        ui::JobsDock* dock = build->dock();
        QTRY_VERIFY(!dock->footerText().isEmpty());
        const QString idle = dock->footerText();
        const QString running = ui::JobMonitor::demoRecord(QStringLiteral("running"), nullptr).id;
        const QString done = ui::JobMonitor::demoRecord(QStringLiteral("done"), nullptr).id;

        build->applyDemoState(QStringLiteral("building"));
        QVERIFY(build->isMonitorShown());
        QVERIFY2(dock->footerText() != idle, qPrintable(dock->footerText()));
        QCOMPARE(dock->runningJob(), running);
        QCOMPARE(build->header()->caption(), QStringLiteral("Package a game folder, an image or a .gp5 project as an installable .pkg."));
        build->clearDemoState();
        QCOMPARE(dock->footerText(), idle);
        QVERIFY(dock->runningJob().isEmpty());

        build->applyDemoState(QStringLiteral("built"));
        QTRY_VERIFY(dock->list()->visibleIds().contains(done)); // a finished job stays listed
        build->clearDemoState();
        QTRY_VERIFY(!dock->list()->visibleIds().contains(done));
        QCOMPARE(build->header()->caption(), QStringLiteral("Turn a game dump into an exFAT image that ShadowMount+ and PCs can mount."));

        m_window->navigate(QStringLiteral("queue"));
        queue->applyDemoState(QStringLiteral("job-running"));
        QTRY_VERIFY(queue->list()->visibleIds().contains(running));
        QCOMPARE(queue->list()->selectedId(), running);
        QVERIFY2(queue->statusContext().startsWith(QStringLiteral("Queue: 1 running")), qPrintable(queue->statusContext()));
        queue->clearDemoState();
        QTRY_VERIFY(!queue->list()->visibleIds().contains(running));
        QVERIFY(queue->statusContext().startsWith(QStringLiteral("Queue: 0 running")));
    }

    // A Queue row as wide as the list beside the details column (about
    // 660 px): the note column gives way so the title is not cut to about
    // 100 px ("Demon's Souls (…"); a wide row keeps its 220 px note column.
    void queueRowsKeepRoomForTheTitle()
    {
        const JobRecord rec = ui::JobMonitor::demoRecord(QStringLiteral("running"), nullptr);
        QWidget host;
        host.resize(1100, 80);
        auto* row = new ui::JobRow(ui::JobRow::Variant::Queue, &host);
        row->setRecord(rec, 0, 1, false);
        row->setGeometry(0, 0, 664, row->sizeHint().height());
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        ui::ElideLabel* title = nullptr;
        for (ui::ElideLabel* l : row->findChildren<ui::ElideLabel*>())
            if (l->fullText() == rec.title)
                title = l;
        auto* bar = row->findChild<ui::ProgressBar*>();
        QVERIFY(title && bar);
        QWidget* middle = bar->parentWidget();
        // The title and the note column share what the fixed columns leave:
        // the note column gives up room, down to 150 px, until the title has 200.
        const auto expected = [&] { return std::clamp(title->width() + middle->width() - 200, 150, 220); };
        const auto said = [&] { return QStringLiteral("title %1 px, note column %2 px").arg(title->width()).arg(middle->width()); };
        QTRY_VERIFY2(middle->width() == expected(), qPrintable(said()));
        QVERIFY2(middle->width() < 220, qPrintable(said())); // 664 px is narrow for this row
        row->setGeometry(0, 0, 1060, row->sizeHint().height());
        QTRY_COMPARE(middle->width(), 220);
        QVERIFY2(title->width() >= 200, qPrintable(said()));
    }

    // The Library demo shows only the backport states the app stores:
    // "Backported", or nothing (drawn as "—"); never an English "Not backported".
    void libraryDemoShowsTheStoredBackportStates()
    {
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(lib);
        m_window->navigate(QStringLiteral("library"));
        const auto restore = qScopeGuard([&] {
            lib->clearDemoState();
            m_window->navigate(QStringLiteral("home"));
        });
        lib->applyDemoState(QStringLiteral("games-list"));
        int backported = 0, unknown = 0;
        for (const auto& item : lib->gamesView()->items()) {
            QVERIFY2(item.badge.isEmpty() || item.badge == i18n::translate(QStringLiteral("Backported")), qPrintable(item.badge));
            ++(item.badge.isEmpty() ? unknown : backported);
        }
        QVERIFY(backported > 0 && unknown > 0);
    }

    // Right to left, the format tiles keep a sign at the end of a Latin name
    // ("ShadowMount+" in a description), as every label does (ui::bidiSafe).
    void formatTilesKeepTheSignsOfNamesRightToLeft()
    {
        // The texts the tiles hand to the painter (the test platform's glyphs
        // are all alike, so a picture would not show the order).
        const auto painted = [](Qt::LayoutDirection dir) {
            pages::OptionTiles tiles;
            tiles.setLayoutDirection(dir);
            tiles.addTile(QStringLiteral("PS5+"), QStringLiteral("Mounts with ShadowMount+."), QStringLiteral("package"), QStringLiteral("a"));
            tiles.resize(260, tiles.heightForWidth(260));
            TextDevice device(tiles.size());
            tiles.render(&device);
            return device.recorder.texts.join(QString());
        };
        const QChar lrm(0x200E);
        const QString rtl = painted(Qt::RightToLeft);
        QVERIFY2(rtl.contains(QStringLiteral("ShadowMount+") + lrm) && rtl.contains(QStringLiteral("PS5+") + lrm), qPrintable(rtl));
        const QString ltr = painted(Qt::LeftToRight);
        QVERIFY2(ltr.contains(QStringLiteral("ShadowMount+.")) && !ltr.contains(lrm), qPrintable(ltr)); // left to right as it is
    }

    // "1 day ago", not "1 days ago".
    void relativeTimes()
    {
        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(pages::relativeTime(now.addSecs(-30 * 3600)), QStringLiteral("1 day ago"));
        QCOMPARE(pages::relativeTime(now.addSecs(-50 * 3600)), QStringLiteral("2 days ago"));
        QCOMPARE(pages::relativeTime(now.addSecs(-2 * 3600 - 5)), QStringLiteral("2 h ago"));
    }
};

QTEST_MAIN(TstPagesHomeQueue)
#include "tst_pages_home_queue.moc"
