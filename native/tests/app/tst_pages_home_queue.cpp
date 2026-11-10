// Home and Queue pages in the real main window, with real jobs:
//
//  * Home › "Pick up where you left off" stops offering a failed job once
//    Retry was used (the retry is a new job), also after the retry succeeded.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/BackportPage.h"
#include "app/pages/HomePage.h"
#include "app/pages/QueuePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QLabel>
#include <QLayout>
#include <QMimeData>
#include <QPushButton>
#include <QScopeGuard>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

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

    // Backport › Status: the library's badge. The column is as wide as its
    // widest state while the title keeps its room, and a state it has to cut
    // is whole in the row's tooltip, always.
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
        int widest = BackportGameModel::kStatusColMin;
        bool anyBusy = false;
        for (int r = 0; r < int(model->rows().size()); ++r) {
            const BackportRow& row = model->rows()[size_t(r)];
            anyBusy = anyBusy || row.tone == QLatin1String("busy");
            widest = std::max(widest, BackportGameModel::statusWidth(row));
            // Whole in the tooltip of every cell of the row, cut or not.
            for (int c = 0; c < BackportGameModel::ColumnCount; ++c) {
                const QString tip = model->index(r, c).data(Qt::ToolTipRole).toString();
                QVERIFY2(tip.contains(QStringLiteral("Status: %1").arg(BackportGameModel::statusText(row)).toHtmlEscaped()), qPrintable(tip));
            }
        }
        QVERIFY(anyBusy);
        const int column = table->columnWidth(BackportGameModel::StatusCol);
        QVERIFY2(column >= BackportGameModel::kStatusColMin && column <= BackportGameModel::kStatusColMax, qPrintable(QString::number(column)));
        // As wide as the widest state, unless the title would be left with too little.
        const int title = table->columnWidth(BackportGameModel::TitleCol);
        QVERIFY2(column == std::min(widest, int(BackportGameModel::kStatusColMax)) || title <= BackportGameModel::kTitleColMin + 2,
                 qPrintable(QStringLiteral("status %1 of %2, title %3").arg(column).arg(widest).arg(title)));
        QVERIFY2(title >= BackportGameModel::kTitleColMin - 2, qPrintable(QString::number(title)));
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
