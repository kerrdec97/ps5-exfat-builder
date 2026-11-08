#include "app/pages/ps5/ManagerTab.h"

#include "app/AppContext.h"
#include "app/pages/Page.h"
#include "app/pages/ps5/ConsoleFolderPicker.h"
#include "app/services/ImageTree.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"
#include "jobs/JobQueue.h"

#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QSettings>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

enum GameCol { GNameCol = 0, GStatusCol, GLocalCol, GRemoteCol, GPathCol, GColumnCount };
enum DiffCol { DStatusCol = 0, DPathCol, DLocalCol, DRemoteCol, DColumnCount };

const QString kFoldersKey = QStringLiteral("ps5Manager/consoleFolders");
const QString kLocalKey = QStringLiteral("ps5Manager/localImagesFolder");

Tone statusTone(ps5::GameStatus s)
{
    switch (s) {
    case ps5::GameStatus::OnPs5: return Tone::Ok;
    case ps5::GameStatus::SizeMismatch: return Tone::Warn;
    case ps5::GameStatus::LocalOnly: return Tone::Warn;
    case ps5::GameStatus::Ps5Only: return Tone::Info;
    }
    return Tone::Neutral;
}

Tone diffTone(ps5::DiffKind k)
{
    switch (k) {
    case ps5::DiffKind::MissingOnConsole: return Tone::Err;
    case ps5::DiffKind::OnlyOnConsole: return Tone::Info;
    case ps5::DiffKind::SizeDiffers: return Tone::Warn;
    }
    return Tone::Neutral;
}

QString sizeText(qint64 n)
{
    return n < 0 ? QStringLiteral("—") : formatBytes(n);
}

QWidget* statCell(const QString& title, QLabel*& value, QLabel** captionLabel, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* l = vbox(w, 2);
    l->addWidget(caption(title, w));
    value = label(QStringLiteral("—"), TextStyle::Stat, color::fg_0, w);
    l->addWidget(value);
    if (captionLabel) {
        *captionLabel = caption(QStringLiteral(" "), w); // keeps the cells aligned before a refresh
        l->addWidget(*captionLabel);
    }
    return w;
}

struct Refreshed {
    ps5::ConsoleGameList console;
    std::vector<ps5::LocalImage> local;
};

// The local side of a compare: a dump folder, or the files of an image.
Result<std::vector<ps5::CompareFile>> localSide(const QString& source, const CancellationToken& token)
{
    const QFileInfo fi(source);
    if (fi.isDir())
        return ps5::localFolderFiles(source, token);
    if (!fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(nativePath(source)));
    const QString ext = fi.suffix().toLower();
    if (ext != u"exfat" && ext != u"ffpkg")
        return Error(ErrorCode::Unsupported, QStringLiteral("Only a dump folder, an .exfat or an .ffpkg image can be compared. Extract other images first."));
    const ImageTreeResult tree = readImageTree(source);
    if (!tree.error.isEmpty())
        return Error(ErrorCode::CorruptData, QStringLiteral("Could not read %1: %2").arg(fi.fileName(), tree.error));
    std::vector<ps5::CompareFile> out;
    for (const ImageNode& n : tree.nodes)
        if (!n.isDir && !n.path.isEmpty())
            out.push_back({n.path, n.size});
    return out;
}

} // namespace

Ps5ManagerTab::Ps5ManagerTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("manager"), parent)
{
    auto* l = vbox(this, space::lg);

    // ---- where to look -----------------------------------------------------------------
    auto* where = new Card(this);
    m_refresh = button(QStringLiteral("Refresh"), ButtonVariant::Primary, QStringLiteral("refresh"), where);
    where->setTitle(QStringLiteral("Games on the console"), m_refresh);
    auto* form = new QWidget(where);
    auto* fh = hbox(form, space::lg);
    m_folders = textInput(QStringLiteral("/data/etaHEN/games; /mnt/usb0/games"), true, form);
    m_folders->setAccessibleName(QStringLiteral("Console folders"));
    fh->addWidget(field(QStringLiteral("Folders on the console"), m_folders, QStringLiteral("Separate several folders with ;"), form), 1);
    m_localFolder = new PathField(PathField::Mode::Directory, form);
    m_localFolder->setDialogTitle(QStringLiteral("Choose the folder with your built images"));
    fh->addWidget(field(QStringLiteral("Local images folder"), m_localFolder, QStringLiteral("Images directly in it are matched by file name"), form), 1);
    where->body()->addWidget(form);
    auto* stats = new QWidget(where);
    auto* sh = hbox(stats, space::xxl);
    sh->addWidget(statCell(QStringLiteral("Games on the PS5"), m_statRemote, &m_statRemoteCaption, stats), 1);
    sh->addWidget(statCell(QStringLiteral("Local images"), m_statLocal, &m_statLocalCaption, stats), 1);
    sh->addWidget(statCell(QStringLiteral("Images on the PS5"), m_statImages, &m_statImagesCaption, stats), 1);
    sh->addWidget(statCell(QStringLiteral("Free on the PS5"), m_statFree, &m_statFreeCaption, stats), 1);
    where->body()->addWidget(stats);
    l->addWidget(where);

    // ---- game list ------------------------------------------------------------------------
    auto* list = new Card(this, Card::Variant::Table);
    auto* h = list->header();
    m_filter = new Segmented(list);
    for (const QString& s : {QStringLiteral("All"), QStringLiteral("On PS5"), QStringLiteral("Size mismatch"), QStringLiteral("Local only"), QStringLiteral("PS5 only")})
        m_filter->addSegment(s);
    m_filter->setCurrentIndex(0);
    m_filter->setAccessibleName(QStringLiteral("Show"));
    h->addWidget(m_filter);
    m_search = textInput(QStringLiteral("Search"), false, list);
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(110);
    m_search->setMaximumWidth(200);
    h->addWidget(m_search);
    h->addStretch(1);
    m_upload = button(QStringLiteral("Upload"), ButtonVariant::Secondary, QStringLiteral("upload"), list);
    m_compareRow = button(QStringLiteral("Compare…"), ButtonVariant::Secondary, QStringLiteral("file_search"), list);
    m_openLocal = button(QStringLiteral("Show file"), ButtonVariant::Secondary, QStringLiteral("folder_open"), list);
    m_openLocal->setToolTip(QStringLiteral("Show the local image in the file manager"));
    for (auto* b : {m_upload, m_compareRow, m_openLocal})
    {
        setButtonSmall(b, true);
        h->addWidget(b);
    }
    m_games = new QTableWidget(0, GColumnCount, list);
    m_games->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Status"), QStringLiteral("Local"), QStringLiteral("PS5"), QStringLiteral("Path on the PS5")});
    styleToolTable(m_games, 8);
    m_games->setSelectionMode(QAbstractItemView::SingleSelection);
    m_games->horizontalHeader()->setSectionResizeMode(GNameCol, QHeaderView::Stretch);
    m_games->setColumnWidth(GStatusCol, 150);
    m_games->setColumnWidth(GLocalCol, 100);
    m_games->setColumnWidth(GRemoteCol, 100);
    m_games->setColumnWidth(GPathCol, 330);
    m_games->setAccessibleName(QStringLiteral("Games"));
    list->body()->addWidget(m_games);
    m_gamesEmpty = new EmptyState(QStringLiteral("game"), QStringLiteral("Game list not loaded"),
                                  QStringLiteral("Refresh lists the images and game folders in the folders above, with the console's free space."), list);
    auto* load = m_gamesEmpty->addAction(QStringLiteral("Refresh"), ButtonVariant::Primary);
    list->body()->addWidget(m_gamesEmpty);
    m_gamesFooter = caption({}, list);
    list->footer()->addWidget(m_gamesFooter, 1);
    l->addWidget(list);

    // ---- compare --------------------------------------------------------------------------
    auto* cmp = new Card(this, Card::Variant::Table);
    auto* ch = cmp->header();
    ch->addWidget(heading(QStringLiteral("Compare a console folder"), TextStyle::Subtitle, cmp));
    ch->addStretch(1);
    m_cmpFilter = new Segmented(cmp);
    for (const QString& s : {QStringLiteral("All differences"), QStringLiteral("Missing on PS5"), QStringLiteral("Only on PS5"), QStringLiteral("Different size")})
        m_cmpFilter->addSegment(s);
    m_cmpFilter->setCurrentIndex(0);
    ch->addWidget(m_cmpFilter);
    m_cmpCancel = button(QStringLiteral("Cancel"), ButtonVariant::Secondary, QStringLiteral("stop"), cmp);
    m_cmpGo = button(QStringLiteral("Compare"), ButtonVariant::Primary, QStringLiteral("file_search"), cmp);
    ch->addWidget(m_cmpCancel);
    ch->addWidget(m_cmpGo);
    auto* cform = new QWidget(cmp);
    auto* cfl = hbox(cform, space::lg, QMargins(space::lg, space::md, space::lg, space::md));
    auto* rrow = new QWidget(cform);
    auto* rrh = hbox(rrow, space::sm);
    m_cmpRemote = textInput(QStringLiteral("/data/etaHEN/games/PPSA01234"), true, rrow);
    m_cmpRemote->setAccessibleName(QStringLiteral("Console folder to compare"));
    m_cmpBrowse = button(QStringLiteral("Browse…"), ButtonVariant::Secondary, QStringLiteral("folder_open"), rrow);
    rrh->addWidget(m_cmpRemote, 1);
    rrh->addWidget(m_cmpBrowse);
    cfl->addWidget(field(QStringLiteral("Folder on the console"), rrow, {}, cform), 1);
    auto* lrow = new QWidget(cform);
    auto* lrh = hbox(lrow, space::sm);
    m_cmpLocal = new PathField(PathField::Mode::Directory, lrow);
    m_cmpLocal->setDialogTitle(QStringLiteral("Choose the local dump folder"));
    m_cmpImage = button(QStringLiteral("Image…"), ButtonVariant::Secondary, QStringLiteral("disc"), lrow);
    m_cmpImage->setToolTip(QStringLiteral("Compare with the files inside an .exfat or .ffpkg image"));
    lrh->addWidget(m_cmpLocal, 1);
    lrh->addWidget(m_cmpImage, 0, Qt::AlignTop);
    cfl->addWidget(field(QStringLiteral("Local dump folder or image"), lrow, {}, cform), 1);
    cmp->body()->addWidget(cform);
    cmp->body()->addWidget(hairline(cmp));
    m_diffs = new QTableWidget(0, DColumnCount, cmp);
    m_diffs->setHorizontalHeaderLabels({QStringLiteral("Difference"), QStringLiteral("File"), QStringLiteral("Local"), QStringLiteral("PS5")});
    styleToolTable(m_diffs, 6);
    m_diffs->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_diffs->horizontalHeader()->setSectionResizeMode(DPathCol, QHeaderView::Stretch);
    m_diffs->setColumnWidth(DStatusCol, 190);
    m_diffs->setColumnWidth(DLocalCol, 110);
    m_diffs->setColumnWidth(DRemoteCol, 110);
    m_diffs->setAccessibleName(QStringLiteral("Differences"));
    cmp->body()->addWidget(m_diffs);
    m_cmpEmpty = new EmptyState(QStringLiteral("files"), QStringLiteral("No compare yet"),
                                QStringLiteral("Pick a folder on the console and the dump or image it came from. Every file is matched by path "
                                               "and size; nothing is downloaded."), cmp);
    cmp->body()->addWidget(m_cmpEmpty);
    m_cmpFooter = caption({}, cmp);
    cmp->footer()->addWidget(m_cmpFooter, 1);
    l->addWidget(cmp);
    l->addStretch(1);

    // ---- defaults and wiring --------------------------------------------------------------
    QSettings qs;
    const QString games = endpoint().gamesFolder;
    m_folders->setText(qs.value(kFoldersKey, games).toString());
    m_localFolder->setPath(qs.value(kLocalKey, m_ctx.settings().value(general::keys::kDefaultOutputDir).toString()).toString());
    connect(m_folders, &QLineEdit::editingFinished, this, [this] { QSettings().setValue(kFoldersKey, m_folders->text().trimmed()); });
    connect(m_localFolder, &PathField::pathChanged, this, [this](const QString& p) { QSettings().setValue(kLocalKey, p.trimmed()); });
    connect(m_refresh, &QPushButton::clicked, this, &Ps5ManagerTab::refresh);
    connect(load, &QPushButton::clicked, this, &Ps5ManagerTab::refresh);
    connect(m_filter, &Segmented::currentChanged, this, [this](int) { showGames(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { showGames(); });
    connect(m_games, &QTableWidget::itemSelectionChanged, this, &Ps5ManagerTab::updateActions);
    connect(m_games, &QTableWidget::cellDoubleClicked, this, [this](int, int) { compareSelected(); });
    connect(m_upload, &QPushButton::clicked, this, &Ps5ManagerTab::uploadSelected);
    connect(m_compareRow, &QPushButton::clicked, this, &Ps5ManagerTab::compareSelected);
    connect(m_openLocal, &QPushButton::clicked, this, [this] {
        const int i = selectedGameRow();
        if (i >= 0 && !m_rows[size_t(i)].localPath.isEmpty())
            openInFileManager(m_rows[size_t(i)].localPath);
    });
    connect(m_cmpGo, &QPushButton::clicked, this, &Ps5ManagerTab::startCompare);
    connect(m_cmpCancel, &QPushButton::clicked, this, [this] { cancelWork(); });
    connect(m_cmpBrowse, &QPushButton::clicked, this, &Ps5ManagerTab::browseConsole);
    connect(m_cmpImage, &QPushButton::clicked, this, &Ps5ManagerTab::chooseImage);
    connect(m_cmpFilter, &Segmented::currentChanged, this, [this](int) { showReport(); });
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool busy) {
        if (!busy)
            m_comparing = false;
        updateActions();
    });
    m_games->hide();
    m_diffs->hide();
    updateActions();
}

void Ps5ManagerTab::setConsoleFolders(const QString& folders)
{
    m_folders->setText(folders);
}

void Ps5ManagerTab::setLocalFolder(const QString& folder)
{
    m_localFolder->setPath(folder);
}

QString Ps5ManagerTab::freeSpaceText() const
{
    return m_statFree->text();
}

QString Ps5ManagerTab::compareSummary() const
{
    return m_cmpFooter->text();
}

void Ps5ManagerTab::refresh()
{
    QStringList folders = ps5::splitConsoleFolders(m_folders->text());
    if (folders.isEmpty())
        folders << endpoint().gamesFolder;
    const QString local = m_localFolder->path().trimmed();
    const bool started = runOnConsole<Refreshed>(
        [folders, local, api = endpoint().smpApiPort](ps5::FtpClient& c, const CancellationToken& t) -> Result<Refreshed> {
            Refreshed out;
            FW_TRY_ASSIGN(out.console, ps5::listConsoleGames(c, folders, t, api));
            out.local = ps5::scanLocalImages(local);
            return out;
        },
        [this](const Result<Refreshed>& r) {
            if (!r.ok()) {
                m_gamesFooter->setText(QStringLiteral("Could not reach the console: %1").arg(r.error().message));
                setTextColor(m_gamesFooter, color::danger_hi);
                emit toastRequested(Tone::Err, QStringLiteral("Game list not loaded"), r.error().message);
                return;
            }
            m_rows = ps5::mergeGames(r->console.games, r->local);
            m_folderErrors = r->console.folderErrors;
            setStats(int(r->console.games.size()), int(r->local.size()), r->console.freeBytes, r->console.freeText, r->console.imageBytes);
            showGames();
        });
    if (started) { // the result arrives later, on the UI thread
        m_gamesFooter->setText(QStringLiteral("Listing %1…").arg(folders.join(QStringLiteral(", "))));
        setTextColor(m_gamesFooter, color::fg_3);
    }
}

void Ps5ManagerTab::setStats(int remote, int local, std::optional<qint64> freeBytes, const QString& freeText, qint64 imageBytes)
{
    m_statRemote->setText(formatCount(remote));
    m_statLocal->setText(formatCount(local));
    m_statImages->setText(formatBytes(imageBytes));
    m_statFree->setText(freeBytes ? formatBytes(*freeBytes) : QStringLiteral("—"));
    m_statRemoteCaption->setText(QStringLiteral("images and game folders"));
    m_statLocalCaption->setText(QStringLiteral("in the local images folder"));
    m_statImagesCaption->setText(QStringLiteral("total size of the images"));
    // ShadowMount+ 1.7 API first; SITE AVAIL is best effort (no current console server has it).
    m_statFreeCaption->setText(freeText.isEmpty() ? QStringLiteral("unknown: needs the ShadowMount+ 1.7 API")
                               : freeText.contains(QLatin1String("ShadowMount+")) ? freeText.simplified()
                                                                                 : QStringLiteral("SITE AVAIL: %1").arg(freeText.simplified()));
    setTextColor(m_statFree, freeBytes && imageBytes > 0 && *freeBytes < (*freeBytes + imageBytes) / 10 ? color::warn_hi : color::fg_0);
}

void Ps5ManagerTab::showGames()
{
    const int filter = m_filter->currentIndex();
    const QString needle = m_search->text().trimmed();
    m_shown.clear();
    m_games->setRowCount(0);
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const ps5::GameRow& g = m_rows[i];
        if (filter == 1 && g.status != ps5::GameStatus::OnPs5)
            continue;
        if (filter == 2 && g.status != ps5::GameStatus::SizeMismatch)
            continue;
        if (filter == 3 && g.status != ps5::GameStatus::LocalOnly)
            continue;
        if (filter == 4 && g.status != ps5::GameStatus::Ps5Only)
            continue;
        if (!needle.isEmpty() && !g.name.contains(needle, Qt::CaseInsensitive) && !g.remotePath.contains(needle, Qt::CaseInsensitive))
            continue;
        const int r = m_games->rowCount();
        m_games->insertRow(r);
        auto* name = toolItem(g.name, false, color::fg_0);
        name->setIcon(icon(g.isDir ? QStringLiteral("folder") : QStringLiteral("disc"), color::fg_3));
        m_games->setItem(r, GNameCol, name);
        setBadgeCell(m_games, r, GStatusCol, g.isDir ? QStringLiteral("Game folder") : ps5::gameStatusLabel(g.status), g.isDir ? Tone::Neutral : statusTone(g.status));
        m_games->setItem(r, GLocalCol, toolItem(sizeText(g.localSize), true, g.localSize < 0 ? color::fg_5 : color::fg_2));
        m_games->setItem(r, GRemoteCol, toolItem(g.isDir ? QStringLiteral("folder") : sizeText(g.remoteSize), true,
                                                g.status == ps5::GameStatus::SizeMismatch ? color::warn_hi : g.remoteSize < 0 ? color::fg_5 : color::fg_2));
        m_games->setItem(r, GPathCol, toolItem(g.remotePath.isEmpty() ? QStringLiteral("— not uploaded —") : g.remotePath, !g.remotePath.isEmpty(),
                                              g.remotePath.isEmpty() ? color::fg_5 : color::fg_3));
        m_shown.push_back(int(i));
    }
    m_games->setVisible(!m_rows.empty());
    m_gamesEmpty->setVisible(m_rows.empty());
    int onPs5 = 0, mismatch = 0, localOnly = 0;
    for (const auto& g : m_rows) {
        onPs5 += g.status == ps5::GameStatus::OnPs5;
        mismatch += g.status == ps5::GameStatus::SizeMismatch;
        localOnly += g.status == ps5::GameStatus::LocalOnly;
    }
    QString text = QStringLiteral("%1 shown · %2 on the PS5 and local · %3 size mismatch · %4 not uploaded")
                       .arg(m_shown.size()).arg(onPs5).arg(mismatch).arg(localOnly);
    if (!m_folderErrors.isEmpty())
        text += QStringLiteral(" · could not list %1").arg(m_folderErrors.join(QStringLiteral("; ")));
    m_gamesFooter->setText(text);
    setTextColor(m_gamesFooter, m_folderErrors.isEmpty() ? color::fg_3 : color::warn_hi);
    updateActions();
}

int Ps5ManagerTab::selectedGameRow() const
{
    const auto sel = m_games->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return -1;
    const int r = sel.first().row();
    return r < int(m_shown.size()) ? m_shown[size_t(r)] : -1;
}

void Ps5ManagerTab::updateActions()
{
    const bool idle = !isBusy();
    const int i = selectedGameRow();
    const ps5::GameRow* g = i >= 0 ? &m_rows[size_t(i)] : nullptr;
    m_refresh->setEnabled(idle);
    m_upload->setEnabled(g && !g->localPath.isEmpty() && (g->status == ps5::GameStatus::LocalOnly || g->status == ps5::GameStatus::SizeMismatch)
                         && jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.upload")));
    m_upload->setText(g && g->status == ps5::GameStatus::SizeMismatch ? QStringLiteral("Upload again") : QStringLiteral("Upload"));
    m_compareRow->setEnabled(g && g->isDir && idle);
    m_openLocal->setEnabled(g && !g->localPath.isEmpty());
    m_cmpGo->setEnabled(idle);
    m_cmpBrowse->setEnabled(idle);
    m_cmpCancel->setVisible(m_comparing);
    m_cmpCancel->setEnabled(m_comparing);
}

void Ps5ManagerTab::uploadSelected()
{
    const int i = selectedGameRow();
    if (i < 0)
        return;
    const ps5::GameRow g = m_rows[size_t(i)];
    QStringList folders = ps5::splitConsoleFolders(m_folders->text());
    const QString dir = g.status == ps5::GameStatus::SizeMismatch ? ps5::remoteParent(g.remotePath)
                        : folders.isEmpty()                         ? endpoint().gamesFolder
                                                                    : folders.first();
    const QString title = g.status == ps5::GameStatus::SizeMismatch ? QStringLiteral("Upload %1 again?").arg(g.name.toHtmlEscaped())
                                                                    : QStringLiteral("Upload %1?").arg(g.name.toHtmlEscaped());
    const QString text = g.status == ps5::GameStatus::SizeMismatch
                             ? QStringLiteral("The copy on the PS5 (%1) has a different size from the local image (%2). It is overwritten by %3. Unmount the game first.")
                                   .arg(sizeText(g.remoteSize), sizeText(g.localSize), Sheet::monoSpan(nativePath(g.localPath)))
                             : QStringLiteral("%1 (%2) is uploaded to %3.").arg(Sheet::monoSpan(g.name), sizeText(g.localSize), Sheet::monoSpan(dir));
    if (!Sheet::confirm(window(), title, text, QStringLiteral("Cancel"), QStringLiteral("Upload"), g.status == ps5::GameStatus::SizeMismatch, Sheet::Icon::Info))
        return;
    const ps5::ConsoleEndpoint ep = endpoint();
    auto r = jobs::submitFtpUploadMany(m_ctx, ep.host, ep.ftpPort, {g.localPath}, dir, false);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Upload not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, QStringLiteral("Upload queued"), QStringLiteral("%1 → %2").arg(g.name, dir));
}

void Ps5ManagerTab::compareSelected()
{
    const int i = selectedGameRow();
    if (i < 0 || !m_rows[size_t(i)].isDir)
        return;
    m_cmpRemote->setText(m_rows[size_t(i)].remotePath);
    m_cmpLocal->edit()->setFocus();
}

void Ps5ManagerTab::browseConsole()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address at the top of the page first."));
        return;
    }
    const QString start = m_cmpRemote->text().trimmed().startsWith(u'/') ? m_cmpRemote->text().trimmed() : ep.gamesFolder;
    const QString chosen = pickConsoleFolder(window(), ep, start, QStringLiteral("Choose the console folder to compare"), QStringLiteral("Use this folder"));
    if (!chosen.isEmpty())
        m_cmpRemote->setText(chosen);
}

void Ps5ManagerTab::chooseImage()
{
    const QString image = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the image to compare with"), QString(),
                                                       QStringLiteral("Images (*.exfat *.ffpkg)"));
    if (!image.isEmpty())
        m_cmpLocal->setPath(image);
}

void Ps5ManagerTab::startCompare()
{
    const QString remote = m_cmpRemote->text().trimmed();
    const QString local = m_cmpLocal->path().trimmed();
    bool bad = false;
    if (!remote.startsWith(u'/')) {
        setInputError(m_cmpRemote, true);
        bad = true;
    } else {
        setInputError(m_cmpRemote, false);
    }
    if (local.isEmpty() || !QFileInfo::exists(local)) {
        m_cmpLocal->setError(local.isEmpty() ? QStringLiteral("Choose the local dump folder or image.") : QStringLiteral("It does not exist."));
        bad = true;
    } else {
        m_cmpLocal->setError({});
    }
    if (bad)
        return;
    compare(ps5::normalizeRemotePath(remote), local);
}

void Ps5ManagerTab::compare(const QString& remoteFolder, const QString& localSource)
{
    const bool started = runOnConsole<ps5::CompareReport>(
        [this, remoteFolder, localSource](ps5::FtpClient& c, const CancellationToken& t) -> Result<ps5::CompareReport> {
            FW_TRY_ASSIGN(std::vector<ps5::CompareFile> local, localSide(localSource, t));
            QElapsedTimer tick;
            tick.start();
            FW_TRY_ASSIGN(ps5::CompareReport rep, ps5::compareConsoleFolder(c, remoteFolder, local, t, [&](int folders, int files) {
                              if (tick.elapsed() < 150)
                                  return;
                              tick.restart();
                              postToUi([this, folders, files] {
                                  m_cmpFooter->setText(QStringLiteral("Reading the console folder… %1 folder(s), %2 file(s)").arg(folders).arg(files));
                              });
                          }));
            rep.localSource = localSource;
            return rep;
        },
        [this](const Result<ps5::CompareReport>& r) {
            if (!r.ok()) {
                m_cmpFooter->setText(r.error().isCancelled() ? QStringLiteral("Compare cancelled.") : QStringLiteral("Compare failed: %1").arg(r.error().message));
                setTextColor(m_cmpFooter, r.error().isCancelled() ? color::fg_3 : color::danger_hi);
                if (!r.error().isCancelled())
                    emit toastRequested(Tone::Err, QStringLiteral("Compare failed"), r.error().message);
                return;
            }
            m_report = r.value();
            showReport();
        });
    if (started) {
        m_cmpFooter->setText(QStringLiteral("Reading %1 on the console…").arg(remoteFolder));
        setTextColor(m_cmpFooter, color::fg_3);
        m_comparing = true;
        updateActions();
    }
}

void Ps5ManagerTab::showReport()
{
    m_diffs->setRowCount(0);
    if (!m_report) {
        m_diffs->hide();
        m_cmpEmpty->show();
        return;
    }
    const int filter = m_cmpFilter->currentIndex();
    for (const ps5::FileDiff& d : m_report->diffs) {
        if (filter == 1 && d.kind != ps5::DiffKind::MissingOnConsole)
            continue;
        if (filter == 2 && d.kind != ps5::DiffKind::OnlyOnConsole)
            continue;
        if (filter == 3 && d.kind != ps5::DiffKind::SizeDiffers)
            continue;
        const int r = m_diffs->rowCount();
        m_diffs->insertRow(r);
        setBadgeCell(m_diffs, r, DStatusCol, ps5::diffKindLabel(d.kind), diffTone(d.kind));
        m_diffs->setItem(r, DPathCol, toolItem(d.path, true, color::fg_1));
        m_diffs->setItem(r, DLocalCol, toolItem(sizeText(d.localSize), true, color::fg_3));
        m_diffs->setItem(r, DRemoteCol, toolItem(sizeText(d.remoteSize), true, d.kind == ps5::DiffKind::SizeDiffers ? color::warn_hi : color::fg_3));
    }
    const bool any = !m_report->diffs.empty();
    m_diffs->setVisible(any);
    m_cmpEmpty->setVisible(!any);
    QString text = QStringLiteral("%1 — %2 local file(s) (%3), %4 on the PS5 (%5)")
                       .arg(m_report->summary())
                       .arg(formatCount(m_report->localFiles), formatBytes(m_report->localBytes))
                       .arg(formatCount(m_report->remoteFiles), formatBytes(m_report->remoteBytes));
    if (!m_report->warnings.isEmpty())
        text += QStringLiteral(" · ") + m_report->warnings.join(QStringLiteral(" · "));
    m_cmpFooter->setText(text);
    m_cmpFooter->setToolTip(m_report->warnings.join(u'\n'));
    setTextColor(m_cmpFooter, m_report->identical() ? color::success_hi : !m_report->warnings.isEmpty() ? color::warn_hi : color::fg_2);
}

void Ps5ManagerTab::setStatusFilter(int index)
{
    m_filter->setCurrentIndex(index);
    showGames();
}

void Ps5ManagerTab::applyDemo()
{
    const auto at = [](const QString& n, ps5::GameStatus s, qint64 local, qint64 remote, const QString& path, bool dir = false) {
        ps5::GameRow g;
        g.name = n;
        g.status = s;
        g.localSize = local;
        g.remoteSize = remote;
        g.remotePath = path;
        g.isDir = dir;
        if (local >= 0)
            g.localPath = QStringLiteral("D:/Images/") + n;
        return g;
    };
    const qint64 gb = 1024LL * 1024 * 1024;
    m_rows = {at(QStringLiteral("PPSA01325 Astro Bot v01.003.exfat"), ps5::GameStatus::OnPs5, 41 * gb, 41 * gb, QStringLiteral("/data/etaHEN/games/PPSA01325 Astro Bot v01.003.exfat")),
              at(QStringLiteral("PPSA01341 Demon's Souls v01.004.ffpkg"), ps5::GameStatus::SizeMismatch, 33 * gb, 21 * gb, QStringLiteral("/mnt/usb0/games/PPSA01341 Demon's Souls v01.004.ffpkg")),
              at(QStringLiteral("PPSA01476 Returnal v01.006.ffpfsc"), ps5::GameStatus::LocalOnly, 52 * gb, -1, {}),
              at(QStringLiteral("PPSA02929 Ratchet v01.001.exfat"), ps5::GameStatus::Ps5Only, -1, 37 * gb, QStringLiteral("/data/etaHEN/games/PPSA02929 Ratchet v01.001.exfat")),
              at(QStringLiteral("PPSA05678-app0"), ps5::GameStatus::Ps5Only, -1, -1, QStringLiteral("/data/etaHEN/games/PPSA05678-app0"), true)};
    m_folderErrors.clear();
    m_folders->setText(QStringLiteral("/data/etaHEN/games; /mnt/usb0/games"));
    setStats(4, 3, 612 * gb, QStringLiteral("657129103360"), 99 * gb);
    showGames();
    m_games->selectRow(1);
    ps5::CompareReport rep;
    rep.remoteFolder = QStringLiteral("/data/etaHEN/games/PPSA05678-app0");
    rep.localFiles = 1842;
    rep.remoteFiles = 1840;
    rep.matching = 1838;
    rep.localBytes = 23 * gb;
    rep.remoteBytes = 23 * gb - 812 * 1024 * 1024;
    rep.diffs = {{QStringLiteral("Data/Audio/music_04.bnk"), ps5::DiffKind::MissingOnConsole, 412 * 1024 * 1024, -1},
                 {QStringLiteral("Data/Audio/music_05.bnk"), ps5::DiffKind::MissingOnConsole, 398 * 1024 * 1024, -1},
                 {QStringLiteral("Data/Levels/level_12.pak"), ps5::DiffKind::SizeDiffers, 1288490188, 1288486912},
                 {QStringLiteral("ampr_emu.index"), ps5::DiffKind::OnlyOnConsole, -1, 1468006}};
    m_cmpRemote->setText(rep.remoteFolder);
    m_cmpLocal->setPath(QStringLiteral("D:/Dumps/PPSA05678-app0"));
    m_report = rep;
    showReport();
}

void Ps5ManagerTab::clearDemo()
{
    m_rows.clear();
    m_shown.clear();
    m_folderErrors.clear();
    m_report.reset();
    m_games->setRowCount(0);
    m_games->hide();
    m_gamesEmpty->show();
    m_gamesFooter->clear();
    for (QLabel* v : {m_statRemote, m_statLocal, m_statImages, m_statFree})
        v->setText(QStringLiteral("—"));
    for (QLabel* c : {m_statRemoteCaption, m_statLocalCaption, m_statImagesCaption, m_statFreeCaption})
        c->setText(QStringLiteral(" "));
    m_cmpRemote->clear();
    m_cmpLocal->setPath({});
    m_cmpFooter->clear();
    showReport();
    QSettings qs;
    m_folders->setText(qs.value(kFoldersKey, endpoint().gamesFolder).toString());
    m_localFolder->setPath(qs.value(kLocalKey, m_ctx.settings().value(general::keys::kDefaultOutputDir).toString()).toString());
    updateActions();
}

} // namespace fw::pages
