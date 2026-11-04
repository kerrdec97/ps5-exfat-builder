#include "app/pages/BackportPage.h"

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>

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
} // namespace

using namespace ui;
using namespace theme;

// ---- table ---------------------------------------------------------------------

GameTable::GameTable(bool selectable, QWidget* parent) : QTableWidget(0, 7, parent), m_selectable(selectable)
{
    setHorizontalHeaderLabels({QString(), QStringLiteral("Title"), QStringLiteral("Title ID"), QStringLiteral("Version"),
                               QStringLiteral("SDK"), QStringLiteral("Size"), QStringLiteral("Status")});
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
        QString status = g.status.isEmpty() ? (s.isGame ? QStringLiteral("Ready") : QStringLiteral("Not a game dump")) : g.status;
        QString issueTip = g.issues.join(u'\n');
        if (!g.canRun && !g.issues.isEmpty()) {
            status = QStringLiteral("Not ready: %1").arg(g.issues.first().section(QStringLiteral(": "), 1));
            tone = Tone::Err;
        } else if (!g.issues.isEmpty() && (g.status.isEmpty() || g.status == QLatin1String("Not backported"))) {
            status = QStringLiteral("Ready · %1 note%2").arg(g.issues.size()).arg(g.issues.size() == 1 ? QString() : QStringLiteral("s"));
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
    : Page(ctx, QStringLiteral("backport"), QStringLiteral("Backport"),
           QStringLiteral("Lower the SDK a game needs so it runs on older firmware. Originals are zipped before anything is changed."), parent),
      m_model(queueModel), m_library(library)
{
    m_reader = new GameInfoReader(this);
    connect(m_reader, &GameInfoReader::ready, this, [this](const GameSummary& s) {
        if (s.exists)
            m_library.upsertGame(s, s.isGame ? QStringLiteral("Not backported") : QString());
        if (!m_pendingAdds.isEmpty())
            m_reader->summarize(m_pendingAdds.takeFirst(), true);
    });

    m_restore = button(QStringLiteral("Restore originals…"), ButtonVariant::Secondary, QStringLiteral("undo"), this);
    m_backport = button(QStringLiteral("Backport"), ButtonVariant::Primary, QStringLiteral("backports"), this);
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
    h->addWidget(heading(QStringLiteral("Game library"), TextStyle::Subtitle, m_tableCard));
    m_counts = caption({}, m_tableCard);
    h->addWidget(m_counts);
    h->addStretch(1);
    m_search = textInput(QStringLiteral("Search title or ID"), false, m_tableCard);
    m_search->setFixedWidth(220);
    m_search->setClearButtonEnabled(true);
    h->addWidget(m_search);
    auto* add = button(QStringLiteral("Add folders…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), m_tableCard);
    auto* rescan = button(QStringLiteral("Rescan"), ButtonVariant::Secondary, QStringLiteral("refresh"), m_tableCard);
    h->addWidget(add);
    h->addWidget(rescan);
    connect(add, &QPushButton::clicked, this, &BackportPage::addFolders);
    connect(rescan, &QPushButton::clicked, this, &BackportPage::scan);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { rebuildTable(); });

    auto* locRow = new QWidget(m_tableCard);
    auto* lh = hbox(locRow, space::md, QMargins(space::lg, 6, space::lg, 6));
    m_locations = caption({}, locRow);
    lh->addWidget(m_locations, 1);
    auto* edit = new Link(QStringLiteral("Edit…"), locRow);
    edit->setFont(font::caption());
    connect(edit, &Link::activated, this, &BackportPage::addFolders);
    lh->addWidget(edit);
    auto* all = new Link(QStringLiteral("Select all"), locRow);
    all->setFont(font::caption());
    auto* none = new Link(QStringLiteral("Select none"), locRow);
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
        menu.addAction(QStringLiteral("Backport this game"), this, [this, folder] {
            auto r = jobs::submitBackport(m_ctx, {folder}, m_options->overrides());
            if (r.ok()) {
                m_ctx.queue().holdQueue(false);
                m_table->setStatusText(folder, QStringLiteral("Queued"), Tone::Info);
            } else
                emit toastRequested(Tone::Err, QStringLiteral("Backport not started"), r.error().message);
        });
        menu.addAction(QStringLiteral("Build image from this dump"), this, [this, folder] {
            emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), folder}}});
        });
        menu.addAction(QStringLiteral("Show in folder"), this, [folder] { openInFileManager(folder); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove from library"), this, [this, folder] { m_library.removeGame(folder); });
        menu.exec(pos);
    });
    m_empty = new EmptyState(QStringLiteral("library"), QStringLiteral("No games found"),
                             QStringLiteral("Add a folder that holds game dumps. Each dump is a folder with sce_sys\\param.json or eboot.bin."), m_tableCard);
    auto* emptyAdd = m_empty->addAction(QStringLiteral("Add folders…"), ButtonVariant::Primary);
    connect(emptyAdd, &QPushButton::clicked, this, &BackportPage::addFolders);
    m_tableCard->body()->addWidget(m_empty);
    m_unavailable = new UnavailableState(QStringLiteral("Backporting"), QStringLiteral("backport.run"), m_tableCard);
    m_tableCard->body()->addWidget(m_unavailable);
    auto* f = m_tableCard->footer();
    m_footerLeft = caption({}, m_tableCard);
    m_footerRight = caption({}, m_tableCard);
    f->addWidget(m_footerLeft, 1);
    f->addWidget(m_footerRight);
    ll->addWidget(m_tableCard);

    auto* activity = new Card(left);
    auto* openLogs = new Link(QStringLiteral("Open in Logs"), activity);
    connect(openLogs, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("logs"), {{QStringLiteral("source"), QStringLiteral("backport")}}});
    });
    activity->setTitle(QStringLiteral("Activity"), openLogs);
    activity->body()->setSpacing(space::sm);
    m_activity = new ActivityWell(logModel, activity);
    m_activity->setSource(QStringLiteral("backport"));
    activity->body()->addWidget(m_activity);
    ll->addWidget(activity);

    auto* right = new QWidget(columns);
    auto* rl = vbox(right, space::lg);
    m_options = new JobOptionsPanel(ctx, {QStringLiteral("Backport")}, {}, right);
    m_options->setGroupCards(true);
    m_options->build();
    rl->addWidget(m_options);
    auto* tip = new InfoBar(Tone::Info, right);
    tip->setTitle(QStringLiteral("Black screen on FW 5.00 or newer?"));
    tip->setMessage(QStringLiteral("Set Target SDK to 5 or higher and backport the unmodified game again. Keep the SDK 4 ZIP for FW 4.xx consoles."));
    rl->addWidget(tip);
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
            m_table->setStatusText(game, rec->state == JobState::Running ? QStringLiteral("Running · %1").arg(thinPercent(rec->progress.overallFraction)) : jobStateName(rec->state),
                                   rec->state == JobState::Running ? Tone::Info : toneForState(rec->state));
    });
    rebuildTable();
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
    m_locations->setText(folders.isEmpty() ? QStringLiteral("Scan locations: none yet") : QStringLiteral("Scan locations: %1").arg(shown.join(QStringLiteral(", "))));
    const QDateTime last = m_library.lastScan();
    m_footerLeft->setText(QStringLiteral("%1 · %2 game%3 in %4 location%5")
                              .arg(last.isValid() ? QStringLiteral("Last scan %1").arg(relativeTime(last)) : QStringLiteral("Not scanned yet"))
                              .arg(m_library.games().size()).arg(m_library.games().size() == 1 ? QString() : QStringLiteral("s"))
                              .arg(folders.size()).arg(folders.size() == 1 ? QString() : QStringLiteral("s")));
    updateCounts();
}

void BackportPage::updateCounts()
{
    const QStringList checked = m_table->checkedFolders();
    qint64 bytes = 0;
    for (const auto& g : m_library.games())
        if (checked.contains(g.info.folder))
            bytes += g.info.bytes;
    m_counts->setText(QStringLiteral("%1 games · %2 selected").arg(m_table->gameCount()).arg(checked.size()));
    m_footerRight->setText(checked.isEmpty() ? QString() : QStringLiteral("Selected: %1 game%2 · %3").arg(checked.size()).arg(checked.size() == 1 ? QString() : QStringLiteral("s")).arg(formatBytes(bytes)));
    m_backport->setText(checked.size() == 1 ? QStringLiteral("Backport 1 game") : checked.isEmpty() ? QStringLiteral("Backport") : QStringLiteral("Backport %1 games").arg(checked.size()));
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("backport.run"));
    m_backport->setEnabled(available && !checked.isEmpty());
    m_backport->setToolTip(!available ? QStringLiteral("Not available in this build") : checked.isEmpty() ? QStringLiteral("Select at least one game") : QString());
    m_restore->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("backport.restore")));
    emit statusContextChanged();
}

void BackportPage::addFolders()
{
    const QString dir = QFileDialog::getExistingDirectory(window(), QStringLiteral("Add a folder of game dumps"));
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
        emit toastRequested(Tone::Warn, QStringLiteral("Nothing to scan"), QStringLiteral("Add a folder of game dumps first."));
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
        emit toastRequested(Tone::Info, QStringLiteral("Scanned without the engine"), QStringLiteral("%1 game folder%2 found one level below the scan locations.").arg(found).arg(found == 1 ? QString() : QStringLiteral("s")));
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
    if (rec->kind == QLatin1String("backport.run")) {
        for (const QString& folder : gamesOf(rec->parameters)) {
            QString status = rec->state == JobState::Succeeded ? (rec->outcome ? rec->outcome->summary : QStringLiteral("Backported")) : QStringLiteral("Failed: %1").arg(rec->error ? rec->error->message : jobStateName(rec->state));
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
        emit toastRequested(Tone::Err, QStringLiteral("Backport not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    for (const QString& f : checked)
        m_table->setStatusText(f, QStringLiteral("Queued"), Tone::Info);
    emit toastRequested(Tone::Info, QStringLiteral("Backport queued"), QStringLiteral("%1 game%2").arg(checked.size()).arg(checked.size() == 1 ? QString() : QStringLiteral("s")));
}

void BackportPage::restoreOriginals()
{
    const QString zip = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the originals ZIP"), {}, QStringLiteral("Originals ZIP (*.zip)"));
    if (zip.isEmpty())
        return;
    const QString game = m_table->currentFolder();
    if (!Sheet::confirm(window(), QStringLiteral("Restore the original files?"),
                        QStringLiteral("The files inside %1 replace the backported ones in the game folder. The backport ZIP is kept.").arg(Sheet::monoSpan(nativePath(zip))),
                        QStringLiteral("Keep backport"), QStringLiteral("Restore originals"), false, Sheet::Icon::Info))
        return;
    auto r = jobs::submitBackportRestore(m_ctx, zip, game);
    if (!r.ok())
        emit toastRequested(Tone::Err, QStringLiteral("Restore not started"), r.error().message);
    else
        m_ctx.queue().holdQueue(false);
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
    return n > 0 ? QStringLiteral("%1 game%2 selected").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")) : QString();
}

void BackportPage::setDragHighlight(bool on) { m_tableCard->setDropHighlight(on); }

} // namespace fw::pages
