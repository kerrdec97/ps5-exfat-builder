#include "app/pages/BackportPage.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/backport/BackportJobs.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/RunReport.h"
#include "engines/backport/SdkFixer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

namespace fw::pages {

namespace {
// backport.run takes one `game` per job; older queue files may hold `gameFolders`.
QStringList gamesOf(const QJsonObject& params)
{
    QStringList out;
    if (params.contains(QStringLiteral("game")))
        out << params.value(QStringLiteral("game")).toString();
    for (const auto& v : params.value(QStringLiteral("gameFolders")).toArray())
        out << v.toString();
    return out;
}

// The game folder a backport, SDK fix or audio report job is about.
QString folderOf(const QJsonObject& params)
{
    const QString g = params.value(QStringLiteral("game")).toString();
    return g.isEmpty() ? params.value(QStringLiteral("folder")).toString() : g;
}

// The run report a finished job names: its outcome, or the error detail of a failed run.
QString reportOf(const JobRecord& rec)
{
    QString html;
    if (rec.outcome)
        html = rec.outcome->details.value(QStringLiteral("report")).toObject().value(QStringLiteral("html")).toString();
    if (html.isEmpty() && rec.error)
        html = backport::reportFromDetail(rec.error->detail);
    return html;
}
} // namespace

using namespace ui;
using namespace theme;

// ---- table ---------------------------------------------------------------------

GameTable::GameTable(bool selectable, QWidget* parent) : QTableWidget(0, 7, parent), m_selectable(selectable)
{
    setHorizontalHeaderLabels({QString(), tr("Title"), tr("Title ID"), tr("Version"),
                               QStringLiteral("SDK"), tr("Size"), tr("Status")});
    horizontalHeader()->setFixedHeight(32);
    horizontalHeader()->setHighlightSections(false);
    horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    horizontalHeader()->setStretchLastSection(false);
    setColumnWidth(0, 36);
    setColumnWidth(2, 110);
    setColumnWidth(3, 100);
    setColumnWidth(4, 70);
    setColumnWidth(5, 90);
    setColumnWidth(6, 190);
    if (!selectable)
        setColumnHidden(0, true);
    verticalHeader()->hide();
    verticalHeader()->setDefaultSectionSize(size::row);
    setShowGrid(false);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setMinimumHeight(6 * size::row + 40);
    setIconSize(QSize(size::coverRow, size::coverRow));
    connect(this, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (it->column() == 0)
            emit checkedChanged();
    });
    connect(this, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        if (auto* it = itemAt(p))
            emit contextRequested(item(it->row(), 1)->data(Qt::UserRole).toString(), viewport()->mapToGlobal(p));
    });
    connect(this, &QTableWidget::cellDoubleClicked, this, [this](int r, int) { emit activatedFolder(item(r, 1)->data(Qt::UserRole).toString()); });
}

QPixmap GameTable::coverPixmap(const QString& coverPath, int px)
{
    if (coverPath.isEmpty() || !QFileInfo::exists(coverPath))
        return {};
    const QString key = QStringLiteral("cover|%1|%2").arg(coverPath).arg(px);
    QPixmap out;
    if (QPixmapCache::find(key, &out))
        return out;
    QPixmap src(coverPath);
    if (src.isNull())
        return {};
    src = src.scaled(px, px, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    out = QPixmap(px, px);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, px, px), radius::sm, radius::sm);
    p.setClipPath(clip);
    p.drawPixmap((px - src.width()) / 2, (px - src.height()) / 2, src);
    QPixmapCache::insert(key, out);
    return out;
}

void GameTable::setGames(const std::vector<LibraryGame>& games, const QString& filter)
{
    const QStringList checked = checkedFolders();
    blockSignals(true);
    setRowCount(0);
    m_count = 0;
    for (const auto& g : games) {
        const GameSummary& s = g.info;
        if (!filter.isEmpty() && !s.title.contains(filter, Qt::CaseInsensitive) && !s.titleId.contains(filter, Qt::CaseInsensitive)
            && !s.folder.contains(filter, Qt::CaseInsensitive))
            continue;
        const int r = rowCount();
        insertRow(r);
        auto* check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        check->setCheckState(checked.contains(s.folder) ? Qt::Checked : Qt::Unchecked);
        setItem(r, 0, check);
        auto* title = new QTableWidgetItem(s.title.isEmpty() ? QFileInfo(s.folder).fileName() : s.title);
        title->setFont(font::bodyStrong());
        title->setForeground(color::fg_0);
        title->setData(Qt::UserRole, s.folder);
        QStringList tip{nativePath(s.folder)};
        if (!s.platform.isEmpty())
            tip << QStringLiteral("%1 %2").arg(s.platform, s.kind);
        title->setToolTip(tip.join(QStringLiteral(" · ")));
        const QPixmap cover = coverPixmap(s.coverPath, size::coverRow);
        if (!cover.isNull())
            title->setIcon(QIcon(cover));
        setItem(r, 1, title);
        auto mono = [](const QString& t, const QColor& c) {
            auto* it = new QTableWidgetItem(t);
            it->setFont(font::mono());
            it->setForeground(c);
            return it;
        };
        setItem(r, 2, mono(s.titleId, color::fg_1));
        setItem(r, 3, mono(s.version, color::fg_3));
        setItem(r, 4, mono(s.sdk, color::fg_3));
        setItem(r, 5, mono(s.bytes > 0 ? formatBytes(s.bytes) : QString(), color::fg_3));
        auto* statusItem = new QTableWidgetItem;
        statusItem->setData(Qt::UserRole, s.folder);
        setItem(r, 6, statusItem);
        Tone tone = Tone::Neutral;
        QString status = g.status.isEmpty() ? (s.isGame ? tr("Ready") : tr("Not a game dump")) : g.status;
        QString issueTip = g.issues.join(u'\n');
        if (!g.canRun && !g.issues.isEmpty()) {
            status = tr("Not ready: %1").arg(g.issues.first().section(QStringLiteral(": "), 1));
            tone = Tone::Err;
        } else if (!g.issues.isEmpty() && (g.status.isEmpty() || g.status == QLatin1String("Not backported"))) {
            status = tr("Ready · %1 note%2").arg(g.issues.size()).arg(g.issues.size() == 1 ? QString() : tr("s"));
            tone = Tone::Warn;
        }
        if (!s.isGame) tone = Tone::Warn;
        else if (status.startsWith(QLatin1String("Backported")) || status == QLatin1String("Done")) tone = Tone::Ok;
        else if (status.startsWith(QLatin1String("Failed"))) tone = Tone::Err;
        else if (status.startsWith(QLatin1String("Queued")) || status.startsWith(QLatin1String("Running"))) tone = Tone::Info;
        auto* wrap = new QWidget(this);
        auto* h = hbox(wrap, 0, QMargins(8, 0, 8, 0));
        auto* badge = new Badge(status, tone, wrap);
        badge->setObjectName(QStringLiteral("statusBadge"));
        if (!issueTip.isEmpty())
            wrap->setToolTip(issueTip + (g.plan.isEmpty() ? QString() : QStringLiteral("\n\nPlan: ") + g.plan));
        if (tone == Tone::Neutral)
            badge->setIconName({});
        h->addWidget(badge);
        h->addStretch(1);
        setCellWidget(r, 6, wrap);
        ++m_count;
    }
    blockSignals(false);
    emit checkedChanged();
}

void GameTable::setStatusText(const QString& folder, const QString& text, Tone tone)
{
    for (int r = 0; r < rowCount(); ++r)
        if (item(r, 1) && item(r, 1)->data(Qt::UserRole).toString() == folder)
            if (auto* w = cellWidget(r, 6))
                if (auto* b = w->findChild<Badge*>(QStringLiteral("statusBadge"))) {
                    b->set(text, tone);
                    b->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
                }
}

QStringList GameTable::checkedFolders() const
{
    QStringList out;
    for (int r = 0; r < rowCount(); ++r)
        if (item(r, 0) && item(r, 0)->checkState() == Qt::Checked)
            out << item(r, 1)->data(Qt::UserRole).toString();
    return out;
}

QString GameTable::currentFolder() const
{
    const int r = currentRow();
    return r >= 0 && item(r, 1) ? item(r, 1)->data(Qt::UserRole).toString() : QString();
}

void GameTable::setAllChecked(bool on)
{
    blockSignals(true);
    for (int r = 0; r < rowCount(); ++r)
        if (item(r, 0))
            item(r, 0)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    blockSignals(false);
    emit checkedChanged();
}

// ---- page ------------------------------------------------------------------------

BackportPage::BackportPage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, LibraryStore& library, QWidget* parent)
    : Page(ctx, QStringLiteral("backport"), tr("Backport"),
           tr("Lower the SDK a game needs so it runs on older firmware. Originals are zipped before anything is changed."), parent),
      m_model(queueModel), m_library(library)
{
    m_reader = new GameInfoReader(this);
    connect(m_reader, &GameInfoReader::ready, this, [this](const GameSummary& s) {
        if (s.exists)
            m_library.upsertGame(s, s.isGame ? tr("Not backported") : QString());
        if (!m_pendingAdds.isEmpty())
            m_reader->summarize(m_pendingAdds.takeFirst(), true);
    });

    m_restore = button(tr("Restore originals…"), ButtonVariant::Secondary, QStringLiteral("undo"), this);
    m_backport = button(tr("Backport"), ButtonVariant::Primary, QStringLiteral("backports"), this);
    header()->addAction(m_restore);
    header()->addAction(m_backport);
    connect(m_restore, &QPushButton::clicked, this, &BackportPage::restoreOriginals);
    connect(m_backport, &QPushButton::clicked, this, &BackportPage::primaryAction);

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns);
    layout->setLeftMax(100000);
    auto* left = new QWidget(columns);
    auto* ll = vbox(left, space::lg);

    m_tableCard = new Card(left, Card::Variant::Table);
    auto* h = m_tableCard->header();
    h->addWidget(heading(tr("Game library"), TextStyle::Subtitle, m_tableCard));
    m_counts = caption({}, m_tableCard);
    h->addWidget(m_counts);
    h->addStretch(1);
    m_search = textInput(tr("Search title or ID"), false, m_tableCard);
    m_search->setFixedWidth(220);
    m_search->setClearButtonEnabled(true);
    h->addWidget(m_search);
    auto* add = button(tr("Add folders…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), m_tableCard);
    auto* rescan = button(tr("Rescan"), ButtonVariant::Secondary, QStringLiteral("refresh"), m_tableCard);
    h->addWidget(add);
    h->addWidget(rescan);
    connect(add, &QPushButton::clicked, this, &BackportPage::addFolders);
    connect(rescan, &QPushButton::clicked, this, &BackportPage::scan);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { rebuildTable(); });

    auto* locRow = new QWidget(m_tableCard);
    auto* lh = hbox(locRow, space::md, QMargins(space::lg, 6, space::lg, 6));
    m_locations = caption({}, locRow);
    lh->addWidget(m_locations, 1);
    auto* edit = new Link(tr("Edit…"), locRow);
    edit->setFont(font::caption());
    connect(edit, &Link::activated, this, &BackportPage::addFolders);
    lh->addWidget(edit);
    auto* all = new Link(tr("Select all"), locRow);
    all->setFont(font::caption());
    auto* none = new Link(tr("Select none"), locRow);
    none->setFont(font::caption());
    lh->addWidget(all);
    lh->addWidget(none);
    m_tableCard->body()->addWidget(locRow);
    m_tableCard->body()->addWidget(hairline(m_tableCard));
    m_table = new GameTable(true, m_tableCard);
    m_tableCard->body()->addWidget(m_table);
    connect(all, &Link::activated, this, [this] { m_table->setAllChecked(true); });
    connect(none, &Link::activated, this, [this] { m_table->setAllChecked(false); });
    connect(m_table, &GameTable::checkedChanged, this, &BackportPage::updateCounts);
    connect(m_table, &GameTable::activatedFolder, this, [](const QString& f) { openInFileManager(f); });
    connect(m_table, &GameTable::contextRequested, this, [this](const QString& folder, const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(tr("Backport this game"), this, [this, folder] {
            auto r = jobs::submitBackport(m_ctx, {folder}, m_options->overrides());
            if (r.ok()) {
                m_ctx.queue().holdQueue(false);
                m_table->setStatusText(folder, tr("Queued"), Tone::Info);
            } else
                emit toastRequested(Tone::Err, tr("Backport not started"), r.error().message);
        });
        const bool canFix = jobs::isAvailable(m_ctx, backport::kJobSdkFix);
        menu.addAction(tr("Fix the SDK marker on a copy…"), this, [this, folder] { fixSdk(folder, false); })->setEnabled(canFix);
        menu.addAction(tr("Fix the SDK marker in place…"), this, [this, folder] { fixSdk(folder, true); })->setEnabled(canFix);
        menu.addAction(tr("Undo the last SDK fix…"), this, [this, folder] { undoSdkFix(folder); })
            ->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("backport.restore")));
        menu.addAction(tr("Audio report (read-only)"), this, [this, folder] { audioReport(folder); })
            ->setEnabled(jobs::isAvailable(m_ctx, backport::kJobAudioScan));
        const QString report = m_reports.value(folder);
        menu.addAction(tr("Open the last report"), this, [this, report] { openReport(report); })->setEnabled(!report.isEmpty());
        menu.addSeparator();
        menu.addAction(tr("Build image from this dump"), this, [this, folder] {
            emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), folder}}});
        });
        menu.addAction(tr("Show in folder"), this, [folder] { openInFileManager(folder); });
        menu.addSeparator();
        menu.addAction(tr("Remove from library"), this, [this, folder] { m_library.removeGame(folder); });
        menu.exec(pos);
    });
    m_empty = new EmptyState(QStringLiteral("library"), tr("No games found"),
                             tr("Add a folder that holds game dumps. Each dump is a folder with sce_sys\\param.json or eboot.bin."), m_tableCard);
    auto* emptyAdd = m_empty->addAction(tr("Add folders…"), ButtonVariant::Primary);
    connect(emptyAdd, &QPushButton::clicked, this, &BackportPage::addFolders);
    m_tableCard->body()->addWidget(m_empty);
    m_unavailable = new UnavailableState(tr("Backporting"), QStringLiteral("backport.run"), m_tableCard);
    m_tableCard->body()->addWidget(m_unavailable);
    auto* f = m_tableCard->footer();
    m_footerLeft = caption({}, m_tableCard);
    m_footerRight = caption({}, m_tableCard);
    f->addWidget(m_footerLeft, 1);
    f->addWidget(m_footerRight);
    ll->addWidget(m_tableCard);

    auto* activity = new Card(left);
    auto* activityLinks = new QWidget(activity);
    auto* alh = hbox(activityLinks, space::md);
    m_openReport = new Link(tr("Open report"), activityLinks);
    m_openReport->setVisible(false);
    connect(m_openReport, &Link::activated, this, [this] { openReport(m_lastReport); });
    auto* openLogs = new Link(tr("Open in Logs"), activityLinks);
    connect(openLogs, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("source"), QStringLiteral("backport")}}});
    });
    alh->addWidget(m_openReport);
    alh->addWidget(openLogs);
    activity->setTitle(tr("Activity"), activityLinks);
    activity->body()->setSpacing(space::sm);
    m_activity = new ActivityWell(logModel, activity);
    m_activity->setSource(QStringLiteral("backport"));
    activity->body()->addWidget(m_activity);
    ll->addWidget(activity);
    // Shortcuts to the PS5 connect page (Payloads and Kernel log tabs); no protocol work here.
    auto* console = new InfoBar(Tone::Info, left);
    console->setTitle(tr("Testing on a console?"));
    console->setMessage(tr("Send the kernel-log or debug payload and watch the kernel log on the PS5 connect page while the game starts."));
    connect(console->addLink(tr("Send a payload…")), &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("ps5"), {{QStringLiteral("tab"), QStringLiteral("payloads")}}});
    });
    connect(console->addLink(tr("Open the kernel log")), &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("ps5"), {{QStringLiteral("tab"), QStringLiteral("klog")}}});
    });
    ll->addWidget(console);

    auto* right = new QWidget(columns);
    auto* rl = vbox(right, space::lg);
    m_options = new JobOptionsPanel(ctx, {QStringLiteral("Backport")}, {}, right);
    m_options->setGroupCards(true);
    m_options->build();
    rl->addWidget(m_options);
    auto* tip = new InfoBar(Tone::Info, right);
    tip->setTitle(tr("Black screen on FW 5.00 or newer?"));
    tip->setMessage(tr("Set Target SDK to 5 or higher and backport the unmodified game again. Keep the SDK 4 ZIP for FW 4.xx consoles."));
    rl->addWidget(tip);
    m_providers = new InfoBar(Tone::Info, right);
    m_providers->setTitle(tr("AMPR / PlayGo libraries"));
    connect(m_providers->addLink(tr("Pin them again…")), &Link::activated, this, &BackportPage::pinProviders);
    rl->addWidget(m_providers);
    connect(m_options, &JobOptionsPanel::overridesChanged, this, &BackportPage::refreshProviderBar);
    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == backport::keys::kAmprPlaygoFolder)
            refreshProviderBar();
    });
    rl->addStretch(1);
    layout->setLeft(left);
    layout->setRight(right);
    content()->addWidget(columns);

    connect(&m_library, &LibraryStore::changed, this, &BackportPage::rebuildTable);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, &BackportPage::onJobFinished);
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, [this](const QString& id) {
        auto rec = m_ctx.queue().record(id);
        if (!rec || rec->kind != QLatin1String("backport.run"))
            return;
        for (const QString& game : gamesOf(rec->parameters))
            m_table->setStatusText(game, rec->state == JobState::Running ? tr("Running · %1").arg(thinPercent(rec->progress.overallFraction)) : jobStateName(rec->state),
                                   rec->state == JobState::Running ? Tone::Info : toneForState(rec->state));
    });
    rebuildTable();
    refreshProviderBar();
}

void BackportPage::rebuildTable()
{
    m_table->setGames(m_library.games(), m_search->text().trimmed());
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("backport.run"));
    const bool any = m_table->gameCount() > 0;
    m_table->setVisible(any);
    m_empty->setVisible(!any && available);
    m_unavailable->setVisible(!available);
    const QStringList folders = m_library.scanFolders();
    QStringList shown;
    for (const QString& f : folders)
        shown << nativePath(f);
    m_locations->setText(folders.isEmpty() ? tr("Scan locations: none yet") : tr("Scan locations: %1").arg(shown.join(QStringLiteral(", "))));
    const QDateTime last = m_library.lastScan();
    m_footerLeft->setText(tr("%1 · %2 game%3 in %4 location%5")
                              .arg(last.isValid() ? tr("Last scan %1").arg(relativeTime(last)) : tr("Not scanned yet"))
                              .arg(m_library.games().size()).arg(m_library.games().size() == 1 ? QString() : tr("s"))
                              .arg(folders.size()).arg(folders.size() == 1 ? QString() : tr("s")));
    updateCounts();
}

void BackportPage::updateCounts()
{
    const QStringList checked = m_table->checkedFolders();
    qint64 bytes = 0;
    for (const auto& g : m_library.games())
        if (checked.contains(g.info.folder))
            bytes += g.info.bytes;
    m_counts->setText(tr("%1 games · %2 selected").arg(m_table->gameCount()).arg(checked.size()));
    m_footerRight->setText(checked.isEmpty() ? QString() : tr("Selected: %1 game%2 · %3").arg(checked.size()).arg(checked.size() == 1 ? QString() : tr("s")).arg(formatBytes(bytes)));
    m_backport->setText(checked.size() == 1 ? tr("Backport 1 game") : checked.isEmpty() ? tr("Backport") : tr("Backport %1 games").arg(checked.size()));
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("backport.run"));
    m_backport->setEnabled(available && !checked.isEmpty());
    m_backport->setToolTip(!available ? tr("Not available in this build") : checked.isEmpty() ? tr("Select at least one game") : QString());
    m_restore->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("backport.restore")));
    emit statusContextChanged();
}

void BackportPage::addFolders()
{
    const QString dir = QFileDialog::getExistingDirectory(window(), tr("Add a folder of game dumps"));
    if (dir.isEmpty())
        return;
    if (readGameSummary(dir, false).isGame) {
        addGameFolder(dir);
        return;
    }
    m_library.addScanFolder(dir);
    scan();
}

void BackportPage::addGameFolder(const QString& folder)
{
    if (m_reader->isBusy())
        m_pendingAdds << folder;
    else
        m_reader->summarize(folder, true);
}

void BackportPage::scan()
{
    const QStringList folders = m_library.scanFolders();
    if (folders.isEmpty()) {
        emit toastRequested(Tone::Warn, tr("Nothing to scan"), tr("Add a folder of game dumps first."));
        return;
    }
    Result<QString> r = jobs::isAvailable(m_ctx, QStringLiteral("backport.scan"))
                            ? m_ctx.queue().enqueue(QStringLiteral("backport.scan"), QJsonObject{{QStringLiteral("folders"), QJsonArray::fromStringList(folders)}})
                            : jobs::scanLibrary(m_ctx, folders);
    if (!r.ok()) {
        // No scanner in this build: list the direct children that look like dumps.
        int found = 0;
        for (const QString& f : folders)
            for (const QFileInfo& fi : QDir(f).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (QFile::exists(fi.filePath() + QStringLiteral("/sce_sys/param.json")) || QFile::exists(fi.filePath() + QStringLiteral("/eboot.bin"))) {
                    addGameFolder(fi.filePath());
                    ++found;
                }
        m_library.setLastScan(QDateTime::currentDateTime());
        emit toastRequested(Tone::Info, tr("Scanned without the engine"), tr("%1 game folder%2 found one level below the scan locations.").arg(found).arg(found == 1 ? QString() : tr("s")));
        return;
    }
    m_scanJobId = r.value();
    m_ctx.queue().holdQueue(false);
}

void BackportPage::onJobFinished(const QString& id)
{
    auto rec = m_ctx.queue().record(id);
    if (!rec)
        return;
    if (id == m_scanJobId && rec->outcome) {
        std::vector<LibraryGame> games;
        for (const auto& v : rec->outcome->details.value(QStringLiteral("games")).toArray()) {
            LibraryGame g;
            const QJsonObject o = v.toObject();
            g.info = GameSummary::fromJson(o);
            g.status = o.value(QStringLiteral("status")).toString();
            g.lastSeen = QDateTime::currentDateTime();
            for (const auto& iv : o.value(QStringLiteral("issues")).toArray()) {
                const QJsonObject io = iv.toObject();
                g.issues << QStringLiteral("%1: %2").arg(io.value(QStringLiteral("severity")).toString(), io.value(QStringLiteral("message")).toString());
            }
            g.canRun = o.value(QStringLiteral("canRun")).toBool(true);
            g.plan = o.value(QStringLiteral("plan")).toString();
            games.push_back(g);
        }
        if (!games.empty())
            m_library.setGames(std::move(games));
        m_library.setLastScan(QDateTime::currentDateTime());
        m_scanJobId.clear();
    }
    if (rec->kind == backport::kJobRun || rec->kind == backport::kJobSdkFix || rec->kind == backport::kJobAudioScan) {
        const QString html = reportOf(*rec);
        if (!html.isEmpty()) {
            m_reports.insert(folderOf(rec->parameters), QDir::fromNativeSeparators(html));
            m_lastReport = QDir::fromNativeSeparators(html);
            m_openReport->setVisible(true);
            m_openReport->setToolTip(nativePath(m_lastReport));
        }
    }
    const bool undo = rec->kind == QLatin1String("backport.restore") && rec->parameters.value(QStringLiteral("undoSdkFix")).toBool();
    if (rec->kind == backport::kJobSdkFix || rec->kind == backport::kJobAudioScan || rec->kind == backport::kJobPinProviders || undo) {
        const bool ok = rec->state == JobState::Succeeded && rec->outcome;
        const QString title = rec->kind == backport::kJobSdkFix  ? tr("SDK fix")
            : rec->kind == backport::kJobAudioScan               ? tr("Audio report")
            : rec->kind == backport::kJobPinProviders            ? tr("AMPR / PlayGo pins")
                                                                 : tr("Undo the SDK fix");
        if (rec->state != JobState::Cancelled)
            emit toastRequested(ok ? Tone::Ok : Tone::Err, ok ? title : tr("%1 failed").arg(title),
                                ok ? rec->outcome->summary : (rec->error ? rec->error->message : jobStateName(rec->state)));
        const QString folder = folderOf(rec->parameters);
        if (rec->kind == backport::kJobSdkFix)
            m_table->setStatusText(folder, ok ? tr("SDK fixed") : tr("SDK fix failed"), ok ? Tone::Ok : Tone::Err);
        // A copy appears in the library next to its source.
        if (ok && rec->kind == backport::kJobSdkFix && rec->parameters.value(QStringLiteral("mode")).toString() != QLatin1String("inPlace")) {
            const QString out = QDir::fromNativeSeparators(rec->outcome->details.value(QStringLiteral("output")).toString());
            if (!out.isEmpty() && out != folder)
                addGameFolder(out);
        }
    }
    if (rec->kind == QLatin1String("backport.run")) {
        for (const QString& folder : gamesOf(rec->parameters)) {
            QString status = rec->state == JobState::Succeeded ? (rec->outcome ? rec->outcome->summary : tr("Backported")) : tr("Failed: %1").arg(rec->error ? rec->error->message : jobStateName(rec->state));
            for (const auto& g : m_library.games())
                if (g.info.folder == folder)
                    m_library.upsertGame(g.info, status);
        }
    }
}

void BackportPage::primaryAction()
{
    const QStringList checked = m_table->checkedFolders();
    if (checked.isEmpty() || !m_backport->isEnabled())
        return;
    auto r = jobs::submitBackport(m_ctx, checked, m_options->overrides());
    if (!r.ok()) {
        emit toastRequested(Tone::Err, tr("Backport not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    for (const QString& f : checked)
        m_table->setStatusText(f, tr("Queued"), Tone::Info);
    emit toastRequested(Tone::Info, tr("Backport queued"), tr("%1 game%2").arg(checked.size()).arg(checked.size() == 1 ? QString() : tr("s")));
}

void BackportPage::restoreOriginals()
{
    const QString zip = QFileDialog::getOpenFileName(window(), tr("Choose the originals ZIP"), {}, tr("Originals ZIP (*.zip)"));
    if (zip.isEmpty())
        return;
    const QString game = m_table->currentFolder();
    if (!Sheet::confirm(window(), tr("Restore the original files?"),
                        tr("The files inside %1 replace the backported ones in the game folder. The backport ZIP is kept.").arg(Sheet::monoSpan(nativePath(zip))),
                        tr("Keep backport"), tr("Restore originals"), false, Sheet::Icon::Info))
        return;
    auto r = jobs::submitBackportRestore(m_ctx, zip, game);
    if (!r.ok())
        emit toastRequested(Tone::Err, tr("Restore not started"), r.error().message);
    else
        m_ctx.queue().holdQueue(false);
}

Result<QString> BackportPage::enqueueBackportJob(const QString& kind, const QJsonObject& params, const QString& notStarted)
{
    if (!jobs::isAvailable(m_ctx, kind)) {
        emit toastRequested(Tone::Err, notStarted, jobs::unavailable(kind).message);
        return jobs::unavailable(kind);
    }
    auto r = m_ctx.queue().enqueue(kind, params, m_options->overrides());
    if (!r.ok()) {
        emit toastRequested(Tone::Err, notStarted, r.error().message);
        return r;
    }
    m_ctx.queue().holdQueue(false);
    return r;
}

void BackportPage::fixSdk(const QString& folder, bool inPlace)
{
    const QString sdk = m_options->effectiveValue(backport::keys::kTargetSdk).toString();
    const QString text = inPlace
        ? tr("Every executable in %1 gets the SDK %2 marker (loads on FW %2.00 and later). The files that change are "
                         "saved to a backup ZIP in the originals ZIP folder first, then replaced through a journal that rolls back "
                         "on any error. No fakelib is added and param.json is not changed. Undo it later with Undo the last SDK fix.")
              .arg(Sheet::monoSpan(nativePath(folder)), sdk)
        : tr("A copy named %1 is made next to the folder, with every executable at the SDK %2 marker (loads on FW %2.00 "
                         "and later). No fakelib is added and param.json is not changed; the folder itself is only read.")
              .arg(Sheet::monoSpan(backport::sdkFixCopyName(QFileInfo(folder).fileName(), sdk.toInt())), sdk);
    if (!Sheet::confirm(window(), inPlace ? tr("Change the SDK marker in place?") : tr("Fix the SDK marker on a copy?"),
                        text, tr("Cancel"), inPlace ? tr("Change in place") : tr("Make the copy"), false,
                        inPlace ? Sheet::Icon::Warning : Sheet::Icon::Info))
        return;
    const QJsonObject p{{QStringLiteral("folder"), folder}, {QStringLiteral("mode"), inPlace ? QStringLiteral("inPlace") : QStringLiteral("copy")}};
    if (enqueueBackportJob(backport::kJobSdkFix, p, tr("SDK fix not started")).ok())
        m_table->setStatusText(folder, tr("Queued · SDK fix"), Tone::Info);
}

void BackportPage::undoSdkFix(const QString& folder)
{
    if (!Sheet::confirm(window(), tr("Undo the last SDK fix?"),
                        tr("The files the last in-place SDK fix of %1 replaced are put back from its backup. Files "
                                       "that changed since are left as they are and reported.")
                            .arg(Sheet::monoSpan(nativePath(folder))),
                        tr("Keep the fix"), tr("Undo it"), false, Sheet::Icon::Info))
        return;
    (void)enqueueBackportJob(QStringLiteral("backport.restore"),
                             QJsonObject{{QStringLiteral("game"), folder}, {QStringLiteral("undoSdkFix"), true}},
                             tr("Undo not started"));
}

void BackportPage::audioReport(const QString& folder)
{
    if (enqueueBackportJob(backport::kJobAudioScan, QJsonObject{{QStringLiteral("game"), folder}}, tr("Audio report not started")).ok())
        emit toastRequested(Tone::Info, tr("Audio report queued"),
                            tr("Read-only: nothing in the game is changed. Open report shows it when it is done."));
}

void BackportPage::pinProviders()
{
    const QString folder = m_options->effectiveValue(backport::keys::kAmprPlaygoFolder).toString().trimmed();
    if (folder.isEmpty())
        return;
    if (!Sheet::confirm(window(), tr("Pin the AMPR / PlayGo libraries again?"),
                        tr("The SHA-256 of libSceAmpr and libScePlayGo in %1 is recorded now. Later backports use "
                                       "exactly these files and refuse them if they change, until you pin them again.")
                            .arg(Sheet::monoSpan(nativePath(folder))),
                        tr("Cancel"), tr("Pin them"), false, Sheet::Icon::Info))
        return;
    (void)enqueueBackportJob(backport::kJobPinProviders, QJsonObject{{QStringLiteral("folder"), folder}},
                             tr("Pinning not started"));
}

void BackportPage::openReport(const QString& htmlPath)
{
    if (htmlPath.isEmpty())
        return;
    if (!QFileInfo(htmlPath).isFile()) {
        emit toastRequested(Tone::Warn, tr("Report not found"), nativePath(htmlPath));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(htmlPath)))
        openInFileManager(htmlPath);
}

void BackportPage::refreshProviderBar()
{
    const QString folder = m_options->effectiveValue(backport::keys::kAmprPlaygoFolder).toString().trimmed();
    m_providers->setVisible(!folder.isEmpty() && jobs::isAvailable(m_ctx, backport::kJobPinProviders));
    m_providers->setMessage(tr("From %1. They are pinned (SHA-256) at the first backport. After you replace one on "
                                           "purpose, pin them again, or the backport is refused.")
                                .arg(nativePath(folder)));
}

void BackportPage::refresh() { scan(); }

bool BackportPage::acceptsDrop(const QList<QUrl>& urls) const
{
    for (const auto& u : urls)
        if (QFileInfo(u.toLocalFile()).isDir())
            return true;
    return false;
}

void BackportPage::dropUrls(const QList<QUrl>& urls)
{
    for (const auto& u : urls) {
        const QString f = u.toLocalFile();
        if (!QFileInfo(f).isDir())
            continue;
        if (readGameSummary(f, false).isGame)
            addGameFolder(f);
        else
            m_library.addScanFolder(f);
    }
}

void BackportPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("folder"))) {
        const QString f = args.value(QStringLiteral("folder")).toString();
        addGameFolder(f);
    }
}

QString BackportPage::statusContext() const
{
    const int n = m_table->checkedFolders().size();
    return n > 0 ? tr("%1 game%2 selected").arg(n).arg(n == 1 ? QString() : tr("s")) : QString();
}

void BackportPage::setDragHighlight(bool on) { m_tableCard->setDropHighlight(on); }

void BackportPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("report")) {
        m_openReport->setVisible(true);
        return;
    }
    if (name != QLatin1String("pins"))
        return;
    m_providers->setVisible(true);
    m_providers->setMessage(tr("From %1. They are pinned (SHA-256) at the first backport. After you replace one on "
                                           "purpose, pin them again, or the backport is refused.")
                                .arg(nativePath(QStringLiteral("D:/Backport/ampr_emu"))));
    QTimer::singleShot(0, this, [this] { scrollArea()->verticalScrollBar()->setValue(scrollArea()->verticalScrollBar()->maximum()); });
}

void BackportPage::clearDemoState()
{
    m_openReport->setVisible(!m_lastReport.isEmpty());
    refreshProviderBar();
    scrollArea()->verticalScrollBar()->setValue(0);
}

} // namespace fw::pages
