#include "app/pages/DlcPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/dlc/DlcIdentity.h"
#include "engines/dlc/DlcSettings.h"
#include "engines/dlc/FpkgCli.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QBuffer>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QScrollBar>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

enum Col { IconCol = 0, TitleCol, ContentIdCol, TitleIdCol, RegionCol, VersionCol, SourceCol, ResultCol, ColCount };

bool isInput(const QString& p)
{
    return p.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive) || p.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive);
}

// An entry without an icon0.png: the generated cover the Library uses, in the
// colour of the game the add-on belongs to (its title ID).
QPixmap placeholderIcon(const QString& title, const QString& titleId)
{
    QPixmap pm(40, 40);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintCoverPlaceholder(p, QRectF(0, 0, 40, 40), title, titleId, 6);
    return pm;
}

// The Companion's Resize512: 512x512, no alpha, stretched, high quality.
QByteArray resize512Png(const QString& path, QString* error)
{
    QImage img(path);
    if (img.isNull()) {
        if (error)
            *error = FW_TR("The image could not be read.");
        return {};
    }
    QImage out = img.convertToFormat(QImage::Format_RGB888)
                     .scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    out.save(&buf, "PNG");
    return png;
}

QString regionLabel(const QString& code)
{
    // The same names as the Region column.
    if (code == u"UP") return FW_TR("Americas");
    if (code == u"EP") return FW_TR("Europe");
    if (code == u"JP") return FW_TR("Japan");
    if (code == u"HP") return FW_TR("Asia");
    if (code == u"KP") return FW_TR("Korea");
    return code;
}

} // namespace

// ---- construction -------------------------------------------------------------------------------

DlcPage::DlcPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("dlc"), QStringLiteral("DLC"),
                  FW_TR("Turn DLC lists (.ini) and DLC packages into installable PS5 DLC packages."),
                  FW_TR("Convert DLC"), FW_TR("DLC conversion"), parent)
{
    setNewJobText(FW_TR("Back to the list"));
    setRequiredKinds({dlc::kJobConvert});
    m_cacheDir = QDir(Paths::cacheDir()).filePath(QStringLiteral("dlc-lookup"));
    m_iconDir = QDir(Paths::tempRoot()).filePath(QStringLiteral("dlc-icons"));
    buildListCard();
    buildSaveCard();
    buildOptionsCard();
    addJobOptionsCard(FW_TR("Engine and verification"), {QStringLiteral("DLC")},
                      {dlc::keys::kOutputFolder, dlc::keys::kEntitlementKey, dlc::keys::kSourcePasscode},
                      FW_TR("All DLC settings"));
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == dlc::keys::kFpkgCliPath)
            m_ctx.checkDependenciesAsync();
        if (key == dlc::keys::kOutputFolder)
            updateState();
    });
    connect(&ctx, &AppContext::dependenciesChecked, this, [this] { updateState(); });
    updateState();
}

DlcPage::~DlcPage()
{
    m_work.cancel(); // a package read or lookup still running stops at its next check
}

void DlcPage::buildListCard()
{
    auto* card = new Card(this);
    m_listTools = new QWidget(card);
    auto* th = hbox(m_listTools, space::sm);
    auto* add = button(FW_TR("Add files\u2026"), ButtonVariant::Secondary, QStringLiteral("file_plus"), m_listTools);
    add->setToolTip(FW_TR("Add DLC INIs (.ini) or DLC packages (.pkg). You can also drop them onto this page."));
    connect(add, &QPushButton::clicked, this, &DlcPage::browseInputs);
    th->addWidget(add);
    auto* titleRow = card->setTitle(FW_TR("DLC to convert"), m_listTools);
    m_countBadge = new Badge(FW_TR("Nothing added"), Tone::Neutral, card);
    m_countBadge->setIconName(QString());
    titleRow->insertWidget(1, m_countBadge);

    m_drop = new DropZone(QStringLiteral("package_plus"), FW_TR("Drop DLC INIs or DLC packages here"),
                          FW_TR("dlc_emu.ini and other DLC lists (.ini), or PS5 DLC .pkg files. Click to browse."), card);
    connect(m_drop, &QAbstractButton::clicked, this, &DlcPage::browseInputs);
    card->body()->addWidget(m_drop);

    m_table = new QTableWidget(0, ColCount, card);
    m_table->setHorizontalHeaderLabels({QString(), FW_TR("Title"), FW_TR("Content ID"),
                                        FW_TR("Title ID"), FW_TR("Region"),
                                        FW_TR("Version"), FW_TR("Source file"), FW_TR("Result")});
    // The picture column needs no word over it; its name is still there for
    // a screen reader and on hover.
    if (QTableWidgetItem* head = m_table->horizontalHeaderItem(IconCol)) {
        head->setToolTip(FW_TR("Icon"));
        head->setData(Qt::AccessibleTextRole, FW_TR("Icon"));
    }
    for (const int c : {int(ContentIdCol), int(TitleIdCol), int(VersionCol), int(SourceCol)})
        keepColumnLeftToRight(m_table, c); // sizes, dates, paths, IDs: left to right in any language
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setShowGrid(false);
    // Titles and results are sentences: two lines of the 52 px row before a
    // "…" (the whole text is the cell's tooltip). IDs have their width.
    m_table->setWordWrap(true);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(52);
    m_table->setIconSize(QSize(40, 40));
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->setAccessibleName(FW_TR("DLC entries"));
    auto* hh = m_table->horizontalHeader();
    hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->resizeSection(IconCol, 48);
    hh->resizeSection(TitleCol, 132);
    // As wide as a content ID (36 characters of mono) and its padding, no wider:
    // the room left goes to the Result, the last column, which takes the rest.
    hh->resizeSection(ContentIdCol, QFontMetrics(font::mono()).horizontalAdvance(QStringLiteral("UP9000-PPSA01341_00-DSDIGITALDELUXE0")) + 24);
    hh->resizeSection(RegionCol, 104);
    hh->setMinimumSectionSize(40);
    hh->setStretchLastSection(true);
    // The title ID follows the Content ID; the version and source file are in the title's tooltip.
    m_table->setColumnHidden(TitleIdCol, true);
    m_table->setColumnHidden(VersionCol, true);
    m_table->setColumnHidden(SourceCol, true);
    connect(m_table, &QTableWidget::cellChanged, this, &DlcPage::syncFromTable);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col == IconCol)
            chooseIcon(row);
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        updateRegionLine();
        updateState();
    });
    m_table->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* removeAct = new QAction(FW_TR("Remove from the list"), m_table);
    removeAct->setShortcut(QKeySequence::Delete);
    removeAct->setShortcutContext(Qt::WidgetShortcut);
    connect(removeAct, &QAction::triggered, this, [this] {
        QList<int> rows = selectedRows();
        std::sort(rows.begin(), rows.end(), std::greater<>());
        {
            // removeRow() reports the selection change before the row has left
            // the table: keep the slots out until m_rows matches again.
            const QSignalBlocker blocker(m_table);
            for (int r : rows) {
                if (r < 0 || r >= int(m_rows.size()))
                    continue;
                m_table->removeRow(r);
                m_rows.erase(m_rows.begin() + r);
            }
        }
        updateRegionLine();
        updateState();
    });
    m_table->addAction(removeAct);
    auto* iconAct = new QAction(FW_TR("Choose icon0.png…"), m_table);
    connect(iconAct, &QAction::triggered, this, [this] {
        const QList<int> rows = selectedRows();
        if (!rows.isEmpty())
            chooseIcon(rows.first());
    });
    m_table->addAction(iconAct);
    card->body()->addWidget(m_table, 1);

    auto* actions = new QWidget(card);
    auto* ah = hbox(actions, space::sm);
    m_regionBtn = button(FW_TR("Change region\u2026"), ButtonVariant::Secondary, QStringLiteral("globe"), actions);
    m_regionBtn->setToolTip(FW_TR("Retarget the selected DLC to another region's game: the publisher prefix and title "
                                           "ID change, each DLC keeps its own suffix."));
    connect(m_regionBtn, &QPushButton::clicked, this, &DlcPage::changeRegion);
    m_previewBtn = button(FW_TR("Preview changes"), ButtonVariant::Secondary, QStringLiteral("eye"), actions);
    connect(m_previewBtn, &QPushButton::clicked, this, &DlcPage::showPreview);
    m_saveIniBtn = button(FW_TR("Save updated INIs"), ButtonVariant::Secondary, QStringLiteral("save"), actions);
    m_saveIniBtn->setToolTip(FW_TR("Write \"<input>-<TitleId>-updated.ini\" files with the edited identifiers; "
                                            "comments, unknown keys and the encoding are kept."));
    connect(m_saveIniBtn, &QPushButton::clicked, this, &DlcPage::saveInisInteractive);
    ah->addWidget(m_regionBtn);
    ah->addWidget(m_previewBtn);
    ah->addWidget(m_saveIniBtn);
    ah->addStretch(1);
    m_clearLink = new Link(FW_TR("Clear list"), actions);
    connect(m_clearLink, &Link::activated, this, &DlcPage::clearList);
    ah->addWidget(m_clearLink);
    card->body()->addWidget(actions);

    m_regionLine = caption(QString(), card);
    m_regionLine->setTextFormat(Qt::PlainText); // carries names from the Store lookup
    m_regionLine->setWordWrap(true);
    m_regionLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    card->body()->addWidget(m_regionLine);
    m_loadRow = new MessageRow(card);
    card->body()->addWidget(m_loadRow);

    auto* logTitle = new Disclosure(FW_TR("Conversion log"), QString(), card);
    logTitle->setPlain(true); // a row of this card, not a plate in it
    m_log = new QPlainTextEdit(logTitle);
    keepLeftToRight(m_log); // log, code or hex: left to right in any language
    m_log->setReadOnly(true);
    m_log->setPlaceholderText(FW_TR("New lines appear here as the app works.")); // not a blank well before the first line
    m_log->setMinimumHeight(140);
    m_log->setFont(font::mono());
    m_log->setMaximumBlockCount(5000);
    logTitle->body()->addWidget(m_log);
    card->body()->addWidget(logTitle);
    form()->addWidget(card);
    setSourceCard(card);
}

void DlcPage::buildSaveCard()
{
    auto* card = new Card(this);
    card->setTitle(FW_TR("Save to"));
    m_output = new PathField(PathField::Mode::Directory, card);
    m_output->setDialogTitle(FW_TR("Choose the output folder"));
    m_output->edit()->setAccessibleName(FW_TR("Output folder"));
    m_output->setPath(m_ctx.settings().value(dlc::keys::kOutputFolder).toString());
    connect(m_output, &PathField::pathChanged, this, [this](const QString& p) {
        if (!m_demo && !qApp->property("fwScreenshotMode").toBool())
            (void)m_ctx.settings().setValue(dlc::keys::kOutputFolder, p.trimmed());
        updateState();
    });
    card->body()->addWidget(field(FW_TR("Folder"), m_output,
                                  FW_TR("Packages are named <ContentId>.pkg; an existing file is never replaced (-2, "
                                                 "-3 \u2026)."),
                                  card));
    form()->addWidget(card);
}

void DlcPage::buildOptionsCard()
{
    auto* card = new Card(this);
    card->setTitle(FW_TR("Options"));
    auto* row = new QWidget(card);
    auto* h = hbox(row, space::md);
    h->setAlignment(Qt::AlignTop);
    m_key = textInput(FW_TR("32 hexadecimal characters"), true, row);
    m_key->setMaxLength(32);
    m_key->setText(m_ctx.settings().value(dlc::keys::kEntitlementKey).toString());
    m_key->setAccessibleName(FW_TR("Entitlement key"));
    m_key->setToolTip(FW_TR("Development entitlement records are generated for the target Content ID with this key. "
                                     "Default: 32 zeroes. The DLC engine builds additional content (entitlement DRM)."));
    h->addWidget(field(FW_TR("Entitlement key"), m_key, {}, row), 1);
    m_passcode = textInput(FW_TR("32 hexadecimal characters"), true, row);
    m_passcode->setMaxLength(32);
    m_passcode->setText(m_ctx.settings().value(dlc::keys::kSourcePasscode).toString());
    m_passcode->setAccessibleName(FW_TR("PKG source passcode"));
    m_passcode->setToolTip(FW_TR("Used to extract imported DLC packages and for the rebuilt package. Default: 32 zeroes."));
    h->addWidget(field(FW_TR("PKG source passcode"), m_passcode, {}, row), 1);
    card->body()->addWidget(row);
    for (QLineEdit* e : {m_key, m_passcode})
        connect(e, &QLineEdit::editingFinished, this, [this, e] {
            const QString v = e->text().trimmed();
            setInputError(e, !dlc::hex32Problem(v).isEmpty());
            if (dlc::hex32Problem(v).isEmpty() && !m_demo && !qApp->property("fwScreenshotMode").toBool())
                (void)m_ctx.settings().setValue(e == m_key ? dlc::keys::kEntitlementKey : dlc::keys::kSourcePasscode, v);
        });

    auto* eng = new QWidget(card);
    auto* eh = hbox(eng, space::sm);
    eh->addWidget(label(FW_TR("DLC engine"), TextStyle::Body, color::fg_1, eng));
    m_engineBadge = new Badge(FW_TR("Checking\u2026"), Tone::Neutral, eng);
    eh->addWidget(m_engineBadge);
    m_engineDetail = caption(QString(), eng);
    m_engineDetail->setWordWrap(true);
    eh->addWidget(m_engineDetail, 1);
    auto* setup = new Link(FW_TR("Settings\u2026"), eng);
    connect(setup, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), QStringLiteral("Tools")},
                                                             {QStringLiteral("key"), dlc::keys::kFpkgCliPath}}});
    });
    eh->addWidget(setup);
    card->body()->addWidget(eng);
    form()->addWidget(card);
}

// ---- inputs --------------------------------------------------------------------------------------------

bool DlcPage::acceptsDrop(const QList<QUrl>& urls) const
{
    for (const QUrl& u : urls)
        if (u.isLocalFile() && isInput(u.toLocalFile()))
            return true;
    return false;
}

void DlcPage::dropUrls(const QList<QUrl>& urls)
{
    QStringList files;
    for (const QUrl& u : urls)
        if (u.isLocalFile() && isInput(u.toLocalFile()))
            files << u.toLocalFile();
    addInputs(files);
}

void DlcPage::setDragHighlight(bool on)
{
    JobFormPage::setDragHighlight(on);
    m_drop->setHighlighted(on);
}

void DlcPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("files")))
        addInputs(args.value(QStringLiteral("files")).toStringList());
}

void DlcPage::browseInputs()
{
    const QStringList files = QFileDialog::getOpenFileNames(window(), FW_TR("Add DLC INIs or PKGs"), QString(),
                                                            FW_TR("DLC inputs") + QStringLiteral(" (*.ini *.pkg);;INI (*.ini);;PKG (*.pkg)"));
    addInputs(files);
}

void DlcPage::appendLog(const QString& line)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line));
}

void DlcPage::addInputs(const QStringList& paths)
{
    int added = 0, failed = 0;
    for (const QString& raw : paths) {
        const QString path = QFileInfo(raw).absoluteFilePath();
        const QFileInfo fi(path);
        if (!fi.isFile())
            continue;
        if (path.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)) {
            auto doc = dlc::DlcIniDocument::load(path);
            if (!doc.ok()) {
                appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 %2").arg(nativePath(path), doc.error().message));
                ++failed;
                continue;
            }
            if (doc->entries().empty())
                appendLog(QStringLiteral("No content_id entries: %1").arg(nativePath(path)));
            for (const dlc::DlcIniEntry& e : doc->entries()) {
                bool dup = false;
                for (const Row& r : m_rows)
                    if (!r.isPackage && r.item.source.compare(path, Qt::CaseInsensitive) == 0 && r.item.entry == e.index)
                        dup = true;
                if (dup)
                    continue;
                Row r;
                r.item.source = path;
                r.item.entry = e.index;
                r.item.detectedContentId = e.contentId;
                r.item.detectedTitleId = e.titleId;
                r.item.contentId = e.contentId.trimmed().toUpper();
                r.item.titleId = e.titleId.trimmed().toUpper();
                r.item.title = e.title;
                r.item.sourceSize = fi.size();
                r.item.sourceModifiedMs = fi.lastModified().toMSecsSinceEpoch();
                r.warnings = e.warnings;
                r.result = FW_TR("Compatibility unverified");
                appendLog(QStringLiteral("Loaded %1: %2 / %3").arg(nativePath(path), e.titleId, e.contentId));
                for (const QString& w : e.warnings)
                    appendLog(QStringLiteral("  %1").arg(w));
                addRow(std::move(r));
                ++added;
            }
        } else if (path.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive)) {
            bool dup = m_loadingPaths.contains(path.toLower());
            for (const Row& r : m_rows)
                if (r.isPackage && r.item.source.compare(path, Qt::CaseInsensitive) == 0)
                    dup = true;
            if (dup)
                continue;
            m_loadingPaths.insert(path.toLower());
            ++m_loading;
            updateState();
            const QString cliPath = m_ctx.settings().value(dlc::keys::kFpkgCliPath).toString();
            const QString passcode = m_passcode->text().trimmed().isEmpty() ? dlc::kZeroKey : m_passcode->text().trimmed();
            const QString tempDir = QDir(Paths::tempRoot()).filePath(QStringLiteral("dlc-inspect"));
            struct Loaded {
                Result<dlc::DlcPackageInfo> info = Error(ErrorCode::Internal, QString());
                QString shownError; // the error in the interface language ("" = shown as the engine wrote it)
                QByteArray icon;
            };
            auto* watcher = new QFutureWatcher<std::shared_ptr<Loaded>>(this);
            connect(watcher, &QFutureWatcher<std::shared_ptr<Loaded>>::finished, this, [this, watcher, path, gen = m_listGen] {
                watcher->deleteLater();
                --m_loading;
                m_loadingPaths.remove(path.toLower());
                if (gen != m_listGen) {
                    updateState(); // the list was cleared while this package was read
                    return;
                }
                const auto loaded = watcher->result();
                if (!loaded->info.ok()) {
                    appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 %2").arg(nativePath(path), loaded->info.error().message));
                    const QString why = loaded->shownError.isEmpty() ? loaded->info.error().message : loaded->shownError;
                    m_loadRow->set(FW_TR("%1 could not be read: %2").arg(QFileInfo(path).fileName(), why), Tone::Err);
                    updateState();
                    return;
                }
                const dlc::DlcPackageInfo& info = loaded->info.value();
                if (!info.isDlc) {
                    const QString why = FW_TR_NOOP("This PKG is a game or patch, not DLC. Use Build image \u203a FPKG for games and "
                                                   "updates; the DLC page accepts additional-content PKGs only.");
                    appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 %2").arg(nativePath(path), why));
                    m_loadRow->set(i18n::translate(why), Tone::Err);
                    updateState();
                    return;
                }
                Row r;
                r.isPackage = true;
                r.package = info;
                r.item.source = path;
                r.item.entry = -1;
                r.item.detectedContentId = info.contentId;
                r.item.detectedTitleId = info.titleId;
                r.item.contentId = info.contentId;
                r.item.titleId = info.titleId;
                r.item.title = info.title;
                r.item.version = info.version;
                r.item.sourceSize = info.size;
                r.item.sourceModifiedMs = info.modifiedUtc.toMSecsSinceEpoch();
                r.item.metadataLine = QStringLiteral("%1; version %2; firmware %3; DRM %4")
                                          .arg(info.type, info.version.isEmpty() ? QStringLiteral("-") : info.version,
                                               info.firmware.isEmpty() ? QStringLiteral("-") : info.firmware, info.drm);
                r.warnings = info.warnings;
                r.result = info.canRebuild ? FW_TR("Compatibility unverified") : info.rebuildLimitation;
                if (!loaded->icon.isEmpty())
                    r.icon.loadFromData(loaded->icon);
                appendLog(QStringLiteral("Loaded %1: %2 / %3").arg(nativePath(path), info.titleId, info.contentId));
                appendLog(QStringLiteral("  %1").arg(r.item.metadataLine));
                for (const QString& w : info.warnings)
                    appendLog(QStringLiteral("  %1").arg(w));
                addRow(std::move(r));
                updateState();
            });
            watcher->setFuture(QtConcurrent::run([cliPath, path, passcode, tempDir, cancel = m_work.token()] {
                auto loaded = std::make_shared<Loaded>();
                const dlc::FpkgCli cli = dlc::locateFpkgCli(cliPath);
                if (!cli.ok() && !dlc::looksLikePs4Package(path)) {
                    loaded->info = Error(ErrorCode::DependencyMissing, cli.problems.join(u' '));
                    loaded->shownError = cli.shownProblems.join(u' '); // the log keeps the English text
                    return loaded;
                }
                loaded->info = dlc::inspectDlcPackage(cli, path, passcode, cancel);
                if (loaded->info.ok() && loaded->info->isDlc) {
                    QDir().mkpath(tempDir);
                    loaded->icon = dlc::extractDlcIcon(cli, path, passcode, tempDir, cancel);
                }
                return loaded;
            }));
        } else {
            appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 Choose a DLC .ini or .pkg file.").arg(nativePath(path)));
            ++failed;
        }
    }
    if (added > 0 || failed > 0)
        m_loadRow->set(added == 1 ? (failed == 1 ? FW_TR("1 entry added; 1 file failed") : FW_TR("1 entry added; %1 files failed").arg(failed))
                                  : (failed == 1 ? FW_TR("%1 entries added; 1 file failed").arg(added)
                                                 : FW_TR("%1 entries added; %2 files failed").arg(added).arg(failed)),
                       failed ? Tone::Warn : Tone::Ok);
    updateState();
}

void DlcPage::addRow(Row row)
{
    m_rows.push_back(std::move(row));
    const int r = int(m_rows.size()) - 1;
    m_syncing = true;
    m_table->insertRow(r);
    for (int c = 0; c < ColCount; ++c) {
        auto* item = new QTableWidgetItem;
        if (c != TitleCol && c != ContentIdCol && c != TitleIdCol)
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(r, c, item);
    }
    m_syncing = false;
    refreshRow(r);
    discoverRegions(r);
}

void DlcPage::refreshRow(int r)
{
    if (r < 0 || r >= int(m_rows.size()))
        return;
    const Row& row = m_rows[size_t(r)];
    m_syncing = true;
    m_table->item(r, IconCol)->setIcon(QIcon(row.icon.isNull() ? placeholderIcon(row.item.title, row.item.titleId) : row.icon));
    m_table->item(r, IconCol)->setToolTip(FW_TR("Double-click to choose icon0.png"));
    m_table->item(r, TitleCol)->setText(row.item.title);
    const QString version = row.item.version.isEmpty() ? QStringLiteral("-") : row.item.version;
    m_table->item(r, TitleCol)->setToolTip(plainTip(row.isPackage
        ? FW_TR("%1\nTitle ID: %2\nVersion: %3\nSource: %4").arg(row.item.title, row.item.titleId, version, nativePath(row.item.source))
        : FW_TR("%1\nTitle ID: %2\nVersion: %3\nSource: %4 [entry %5]")
              .arg(row.item.title, row.item.titleId, version, nativePath(row.item.source), QString::number(row.item.entry + 1))));
    m_table->item(r, ContentIdCol)->setText(row.item.contentId);
    m_table->item(r, ContentIdCol)->setFont(font::mono());
    m_table->item(r, TitleIdCol)->setText(row.item.titleId);
    m_table->item(r, TitleIdCol)->setFont(font::mono());
    {
        // Short label in the cell; the Companion's full text in the tooltip.
        const QString code = dlc::regionCode(row.item.contentId);
        const QString shortName = code == u"UP" ? FW_TR("Americas") : code == u"EP" ? FW_TR("Europe")
                                : code == u"JP" ? FW_TR("Japan") : code == u"HP" ? FW_TR("Asia")
                                : code == u"KP" ? FW_TR("Korea") : FW_TR("Unknown");
        m_table->item(r, RegionCol)->setText(row.item.targetMarket.isEmpty() ? QStringLiteral("%1 · %2").arg(shortName, code)
                                                                              : FW_TR("%1 Store").arg(row.item.targetMarket.section(u'(', 1).remove(u')')));
        m_table->item(r, RegionCol)->setToolTip(plainTip(row.item.targetMarket.isEmpty() ? dlc::regionText(row.item.contentId)
                                                                                          : FW_TR("%1 (Store)").arg(row.item.targetMarket)));
    }
    m_table->item(r, VersionCol)->setText(row.item.version);
    m_table->item(r, SourceCol)->setText(QFileInfo(row.item.source).fileName()
                                         + (row.isPackage ? QString() : QStringLiteral(" [%1]").arg(row.item.entry + 1)));
    m_table->item(r, SourceCol)->setToolTip(plainTip(nativePath(row.item.source)));
    m_table->item(r, ResultCol)->setText(row.result);
    m_table->item(r, ResultCol)->setToolTip(plainTip(row.warnings.isEmpty() ? row.result : row.result + QStringLiteral("\n") + row.warnings.join(u'\n')));
    const bool problem = !dlc::validateIdentity(row.item.titleId, row.item.contentId).ok()
                      || (row.package && !row.package->canRebuild);
    m_table->item(r, ResultCol)->setForeground(problem ? toneText(Tone::Warn) : color::fg_2);
    m_syncing = false;
}

void DlcPage::syncFromTable(int r, int c)
{
    if (m_syncing || r < 0 || r >= int(m_rows.size()))
        return;
    Row& row = m_rows[size_t(r)];
    const QString text = m_table->item(r, c)->text();
    if (c == TitleCol) {
        row.item.title = text.trimmed();
        if (row.item.title.isEmpty())
            row.item.title = dlc::suffixOf(row.item.contentId);
    } else if (c == ContentIdCol) {
        row.item.contentId = text.trimmed().toUpper();
        row.item.targetMarket.clear();
        if (dlc::isContentId(row.item.contentId))
            row.item.titleId = dlc::titleIdOf(row.item.contentId);
    } else if (c == TitleIdCol) {
        row.item.titleId = text.trimmed().toUpper();
    }
    if (c == ContentIdCol || c == TitleIdCol) {
        const Status st = dlc::validateIdentity(row.item.titleId, row.item.contentId);
        row.result = st.ok() ? FW_TR("Manual target; compatibility unverified") : st.error().message;
    }
    refreshRow(r);
    updateState();
}

void DlcPage::chooseIcon(int r)
{
    if (r < 0 || r >= int(m_rows.size()))
        return;
    const QString file = QFileDialog::getOpenFileName(window(), FW_TR("Select the icon0.png"), QString(),
                                                      QStringLiteral("PNG (*.png)"));
    if (file.isEmpty())
        return;
    if (r >= int(m_rows.size()))
        return; // the list changed under the file dialog
    QString why;
    const QByteArray png = resize512Png(file, &why);
    if (png.isEmpty()) {
        m_loadRow->set(why, Tone::Err);
        return;
    }
    QDir().mkpath(m_iconDir);
    Row& row = m_rows[size_t(r)];
    row.iconFile = QDir(m_iconDir).filePath(QStringLiteral("icon0-%1-%2.png").arg(QDateTime::currentMSecsSinceEpoch()).arg(r));
    if (!writeFileAtomic(row.iconFile, png).ok()) {
        m_loadRow->set(FW_TR("The icon could not be prepared."), Tone::Err);
        return;
    }
    row.icon.loadFromData(png);
    row.iconChosen = true;
    row.item.iconPath = row.iconFile;
    refreshRow(r);
}

void DlcPage::clearList()
{
    // Packages still being read and lookups still running belong to the old
    // list: they are told to stop, and what they still deliver is dropped.
    ++m_listGen;
    m_work.cancel();
    m_work = CancellationSource();
    m_rows.clear();
    m_table->setRowCount(0);
    m_lookupQueue.clear();
    m_loadRow->clear();
    m_regionLine->clear();
    updateState();
}

QList<int> DlcPage::selectedRows() const
{
    QList<int> rows;
    for (const QModelIndex& i : m_table->selectionModel()->selectedRows())
        rows << i.row();
    if (rows.isEmpty() && m_table->currentRow() >= 0)
        rows << m_table->currentRow();
    std::sort(rows.begin(), rows.end());
    return rows;
}

std::vector<dlc::DlcItem> DlcPage::items() const
{
    std::vector<dlc::DlcItem> v;
    for (const Row& r : m_rows)
        v.push_back(r.item);
    return v;
}

QString DlcPage::outputFolder() const
{
    const QString o = m_output->path().trimmed();
    if (!o.isEmpty())
        return o;
    if (m_rows.empty())
        return {};
    return QDir(QFileInfo(m_rows.front().item.source).absolutePath()).filePath(QStringLiteral("DLC packages"));
}

void DlcPage::setOutputFolder(const QString& dir) { m_output->setPath(dir); }

void DlcPage::updateState()
{
    const bool any = !m_rows.empty();
    m_drop->setVisible(!any);
    m_table->setVisible(any);
    // As tall as its rows (three at least, ten at most, then it scrolls): no
    // empty band under a short list.
    const int shownRows = std::clamp(int(m_rows.size()), 3, 10);
    m_table->setFixedHeight(m_table->horizontalHeader()->sizeHint().height() + shownRows * m_table->verticalHeader()->defaultSectionSize()
                            + 2 * m_table->frameWidth() + 2);
    m_regionBtn->setEnabled(any);
    m_previewBtn->setEnabled(any);
    m_saveIniBtn->setEnabled(any);
    m_clearLink->setVisible(any);
    // The regional line speaks about the listed DLC: with none, it would be an
    // empty line of space under the buttons.
    m_regionLine->setVisible(any && !m_regionLine->text().isEmpty());
    if (m_loading > 0)
        m_countBadge->set((m_loading == 1 ? FW_TR("Reading %1 package\u2026") : FW_TR("Reading %1 packages\u2026")).arg(m_loading),
                          Tone::Info);
    else if (any)
        m_countBadge->set((m_rows.size() == 1 ? FW_TR("%1 entry") : FW_TR("%1 entries")).arg(m_rows.size()),
                          Tone::Info);
    else
        m_countBadge->set(FW_TR("Nothing added"), Tone::Neutral);
    m_output->setPlaceholder(m_rows.empty() ? FW_TR("A \"DLC packages\" folder next to the first input")
                                            : nativePath(outputFolder()));
    // What is known to be missing, in validateForm()'s words.
    setStartHint(!any ? FW_TR("Add DLC INIs or DLC packages first.")
                      : m_loading > 0 ? FW_TR("Packages are still being read. Try again in a moment.") : QString());
    const DependencyInfo info = m_ctx.dependencies().last(QStringLiteral("dlc.fpkgcli"));
    if (info.id.isEmpty()) {
        m_engineBadge->set(FW_TR("Checking\u2026"), Tone::Neutral);
        m_engineDetail->setText(FW_TR("fpkg-cli (LibProsperoPkg) builds the DLC packages."));
    } else if (info.status == DependencyStatus::Ready) {
        m_engineBadge->set(info.version.isEmpty() ? FW_TR("Ready") : info.version, Tone::Ok);
        m_engineDetail->setText(nativePath(info.path));
    } else {
        m_engineBadge->set(info.status == DependencyStatus::Missing ? FW_TR("Not set") : i18n::translate(dependencyStatusName(info.status)),
                           Tone::Warn);
        m_engineDetail->setText(info.detail.isEmpty() ? info.howToFix : info.detail);
    }
    emit statusContextChanged();
}

QString DlcPage::statusContext() const
{
    return m_rows.empty() ? QString() : (m_rows.size() == 1 ? FW_TR("%1 DLC entry") : FW_TR("%1 DLC entries")).arg(m_rows.size());
}

// ---- region lookup ----------------------------------------------------------------------------------

bool DlcPage::onlineLookupAllowed() const
{
    if (qEnvironmentVariableIsSet("FW_OFFLINE") || m_demo)
        return false;
    const QVariant v = m_ctx.settings().value(dlc::keys::kOnlineLookup);
    return !v.isValid() || v.toBool();
}

QString DlcPage::lookupKey(const Row& r) const
{
    if (!dlc::isContentId(r.item.detectedContentId))
        return {};
    return QStringLiteral("%1|%2").arg(dlc::titleIdOf(r.item.detectedContentId), r.item.detectedContentId.left(6));
}

void DlcPage::discoverRegions(int row)
{
    if (!onlineLookupAllowed() || row < 0 || row >= int(m_rows.size()))
        return;
    const QString key = lookupKey(m_rows[size_t(row)]);
    if (key.isEmpty() || m_lookups.contains(key) || m_lookupRunning.contains(key) || m_lookupQueue.contains(key))
        return;
    m_lookupQueue << key;
    pumpLookups();
}

// Two lookups at a time.
void DlcPage::pumpLookups()
{
    while (m_lookupRunning.size() < 2 && !m_lookupQueue.isEmpty()) {
        const QString k = m_lookupQueue.takeFirst();
        QString cid, title;
        for (const Row& r : m_rows)
            if (lookupKey(r) == k) {
                cid = r.item.detectedContentId;
                title = r.item.title;
                break;
            }
        if (cid.isEmpty())
            continue;
        m_lookupRunning.insert(k);
        auto* w = new QFutureWatcher<std::shared_ptr<dlc::DlcLookupResult>>(this);
        connect(w, &QFutureWatcher<std::shared_ptr<dlc::DlcLookupResult>>::finished, this, [this, w, k, gen = m_listGen] {
            w->deleteLater();
            m_lookupRunning.remove(k);
            if (gen != m_listGen) {
                // Started for a list that was cleared (and cancelled with it):
                // not a result. A row of the new list with this key asks again.
                for (const Row& r : m_rows)
                    if (lookupKey(r) == k && !m_lookupQueue.contains(k)) {
                        m_lookupQueue << k;
                        break;
                    }
                updateRegionLine();
                pumpLookups();
                return;
            }
            m_lookups.insert(k, w->result());
            const auto& res = *w->result();
            int observed = 0;
            for (const auto& c : res.candidates)
                if (c.productKind == u"DLC")
                    ++observed;
            appendLog(QStringLiteral("Regional discovery %1: %2; %3 game identifier observations; %4 observed DLC products. DLC "
                                     "compatibility unverified.")
                          .arg(k.section(u'|', 0, 0),
                               res.resolvedName.isEmpty() ? QStringLiteral("game name not resolved") : res.resolvedName)
                          .arg(res.candidates.size() - size_t(observed))
                          .arg(observed));
            for (int i = 0; i < res.warnings.size() && i < 6; ++i)
                appendLog(QStringLiteral("  %1").arg(res.warnings[i]));
            updateRegionLine();
            pumpLookups();
        });
        const QString cacheDir = m_cacheDir;
        w->setFuture(QtConcurrent::run([cacheDir, cid, title, cancel = m_work.token()] {
            dlc::DlcLookupService svc(cacheDir);
            return std::make_shared<dlc::DlcLookupResult>(svc.resolveRegions(dlc::titleIdOf(cid), cid, title, cancel));
        }));
    }
    updateRegionLine();
}

void DlcPage::updateRegionLine()
{
    // Whatever is said below: the line shows only with DLC listed (as updateState() keeps it).
    const auto shown = qScopeGuard([this] { m_regionLine->setVisible(!m_rows.empty() && !m_regionLine->text().isEmpty()); });
    const QList<int> rows = selectedRows();
    const int r = rows.isEmpty() ? (m_rows.empty() ? -1 : 0) : rows.first();
    if (r < 0 || r >= int(m_rows.size())) {
        m_regionLine->setText(FW_TR("Import DLC to find the game's regional releases. Change region\u2026 applies a reviewed target."));
        return;
    }
    const QString key = lookupKey(m_rows[size_t(r)]);
    if (key.isEmpty() || !onlineLookupAllowed()) {
        m_regionLine->setText(FW_TR("Regional lookup: use Change region\u2026 to find the game or enter manual identifiers."));
        return;
    }
    if (m_lookupRunning.contains(key) || m_lookupQueue.contains(key)) {
        m_regionLine->setText(FW_TR("Finding regional releases for %1\u2026 Local work remains available.").arg(key.section(u'|', 0, 0)));
        return;
    }
    const auto res = m_lookups.value(key);
    if (!res) {
        m_regionLine->setText(FW_TR("Regional lookup stopped. Use Change region\u2026 to retry or enter manual identifiers."));
        return;
    }
    const QString cid = m_rows[size_t(r)].item.detectedContentId;
    const auto maps = dlc::gameMappings(res->candidates, QString(), cid);
    if (maps.empty()) {
        m_regionLine->setText(FW_TR("No regional mapping found for %1. Change region\u2026 lets you enter the game name or "
                                             "manual identifiers.")
                                  .arg(res->resolvedName.isEmpty() ? key.section(u'|', 0, 0) : res->resolvedName));
        return;
    }
    QStringList parts;
    for (const QString& code : {QStringLiteral("UP"), QStringLiteral("EP"), QStringLiteral("JP"), QStringLiteral("HP"), QStringLiteral("KP")}) {
        QStringList ids;
        for (const auto& c : dlc::gameMappings(res->candidates, code, cid)) {
            const QString id = QStringLiteral("%1 / %2").arg(c.titleId, c.contentId.left(6));
            if (!ids.contains(id))
                ids << id;
        }
        if (!ids.isEmpty())
            parts << QStringLiteral("%1: %2").arg(regionLabel(code), ids.join(QStringLiteral(", ")));
    }
    int observed = 0;
    for (const auto& c : res->candidates)
        if (c.productKind == u"DLC")
            ++observed;
    QString name = res->resolvedName;
    if (name.size() > 120)
        name = name.left(117) + QStringLiteral("\u2026");
    m_regionLine->setText((res->isStale ? FW_TR("%1 \u2014 Regional game IDs: %2 (stale cache). Observed DLC products: %3. DLC compatibility unverified.")
                           : res->fromCache ? FW_TR("%1 \u2014 Regional game IDs: %2 (cached). Observed DLC products: %3. DLC compatibility unverified.")
                                            : FW_TR("%1 \u2014 Regional game IDs: %2. Observed DLC products: %3. DLC compatibility unverified."))
                              .arg(name, parts.join(QStringLiteral("; ")), QString::number(observed)));
    m_regionLine->setToolTip(FW_TR("Regional game IDs identify releases; observed DLC products are listed separately. "
                                            "Identifier mappings do not confirm in-game DLC compatibility."));
}

QString DlcPage::applyTarget(const QList<int>& rows, const QString& titleId, const QString& contentId, const QString& evidence,
                             const QString& market, bool addCopies)
{
    if (rows.isEmpty())
        return FW_TR("Select the DLC entries to retarget.");
    const QString tid = titleId.trimmed().toUpper();
    const QString cid = contentId.trimmed().toUpper();
    // One row: the dialog's Content ID; several: each keeps its own _NN and suffix.
    std::vector<std::pair<int, QString>> targets;
    for (int r : rows) {
        if (r < 0 || r >= int(m_rows.size()))
            continue;
        QString rowCid = cid;
        if (rows.size() > 1) {
            auto re = dlc::retargetContentId(m_rows[size_t(r)].item.contentId, tid, cid.left(6));
            if (!re.ok())
                return re.error().message;
            rowCid = re.value();
        }
        if (Status st = dlc::validateIdentity(tid, rowCid); !st.ok())
            return st.error().message;
        if (dlc::isContentId(m_rows[size_t(r)].item.detectedContentId)
            && m_rows[size_t(r)].item.detectedContentId.mid(7, 4) != tid.left(4))
            return FW_TR("A PS4 DLC cannot be converted into PS5 DLC, or vice versa.");
        targets.emplace_back(r, rowCid);
    }
    for (const auto& [r, rowCid] : targets) {
        if (addCopies) {
            Row copy = m_rows[size_t(r)];
            copy.item.titleId = tid;
            copy.item.contentId = rowCid;
            copy.item.targetMarket = market;
            copy.result = evidence;
            addRow(std::move(copy));
        } else {
            Row& row = m_rows[size_t(r)];
            row.item.titleId = tid;
            row.item.contentId = rowCid;
            row.item.targetMarket = market;
            row.result = evidence;
            refreshRow(r);
        }
    }
    updateState();
    return {};
}

void DlcPage::changeRegion()
{
    const QList<int> rows = selectedRows();
    if (rows.isEmpty())
        return;
    const Row& first = m_rows[size_t(rows.first())];
    const QString sourceCid = first.item.contentId;
    const QString sourceTid = first.item.titleId;

    auto* s = new Sheet(window(), FW_TR("DLC regions / targets"), Sheet::Icon::None, 1000);
    s->addText(rows.size() == 1
                   ? FW_TR("Selected: 1 DLC entry. Original: %1 / %2<br>Find the game, choose a region, then apply. Each DLC "
                           "item keeps its own suffix; an exact observed DLC SKU is used when available. Game identifiers can "
                           "be shared across regions.")
                         .arg(Sheet::monoSpan(sourceTid), Sheet::monoSpan(sourceCid))
                   : FW_TR("Selected: %1 DLC entries. Original: %2 / %3<br>Find the game, choose a region, then apply. Each DLC "
                           "item keeps its own suffix; an exact observed DLC SKU is used when available. Game identifiers can "
                           "be shared across regions.")
                         .arg(QString::number(rows.size()), Sheet::monoSpan(sourceTid), Sheet::monoSpan(sourceCid)));
    // Query row
    auto* qrow = new QWidget(s);
    auto* qh = hbox(qrow, space::sm);
    auto* query = textInput(FW_TR("Title ID, Content ID, game name or Store product URL"), false, qrow);
    query->setText(sourceTid);
    auto* find = button(FW_TR("Find regions"), ButtonVariant::Secondary, QStringLiteral("search"), qrow);
    auto* manual = new QCheckBox(FW_TR("Manual identifiers"), qrow);
    qh->addWidget(field(FW_TR("Game name / ID"), query, {}, qrow), 1);
    qh->addWidget(find, 0, Qt::AlignBottom);
    qh->addWidget(manual, 0, Qt::AlignBottom);
    s->body()->addWidget(qrow);
    // Region + mapping
    auto* rrow = new QWidget(s);
    auto* rh = hbox(rrow, space::md);
    auto* region = dropdown(rrow);
    region->addItem(FW_TR("Choose target region\u2026"), QString());
    region->addItem(FW_TR("Americas / US"), QStringLiteral("UP"));
    region->addItem(FW_TR("Europe / UK"), QStringLiteral("EP"));
    region->addItem(FW_TR("Japan"), QStringLiteral("JP"));
    region->addItem(FW_TR("Asia / Hong Kong"), QStringLiteral("HP"));
    region->addItem(FW_TR("Korea"), QStringLiteral("KP"));
    rh->addWidget(field(FW_TR("Target region"), region, {}, rrow), 0);
    auto* mapping = dropdown(rrow);
    mapping->setToolTip(FW_TR("Base-game releases supply regional PPSA and publisher identifiers. These are identifier "
                                       "mappings, not observed DLC products or verified DLC compatibility."));
    rh->addWidget(field(FW_TR("Game identifier mapping"), mapping, {}, rrow), 1);
    s->body()->addWidget(rrow);
    auto* status = caption(onlineLookupAllowed() ? FW_TR("Finding game and regional releases\u2026")
                                                 : FW_TR("Online lookup is off (Settings \u203a DLC). Use Manual identifiers."),
                           s);
    status->setWordWrap(true);
    s->body()->addWidget(status);
    // Releases
    auto* grid = new QTableWidget(0, 7, s);
    grid->setHorizontalHeaderLabels({FW_TR("Title ID"), FW_TR("DLC / add-on"), FW_TR("Edition"),
                                     FW_TR("Store region"), FW_TR("Product / _NN"),
                                     FW_TR("Identifier evidence"), FW_TR("Observed release content ID")});
    grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    grid->setSelectionMode(QAbstractItemView::SingleSelection);
    grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid->verticalHeader()->hide();
    grid->horizontalHeader()->setStretchLastSection(true);
    grid->setMinimumHeight(200);
    s->body()->addWidget(grid);
    // Target
    auto* trow = new QWidget(s);
    auto* th = hbox(trow, space::md);
    auto* targetTid = textInput({}, true, trow);
    targetTid->setText(sourceTid);
    targetTid->setReadOnly(true);
    auto* targetCid = textInput({}, true, trow);
    targetCid->setText(sourceCid);
    targetCid->setReadOnly(true);
    th->addWidget(field(FW_TR("Target title ID"), targetTid, {}, trow), 1);
    th->addWidget(field(FW_TR("DLC content ID"), targetCid, {}, trow), 3);
    s->body()->addWidget(trow);
    auto* proposal = caption(FW_TR("Choose a release to propose a region swap. DLC compatibility remains unverified."), s);
    proposal->setWordWrap(true);
    s->body()->addWidget(proposal);

    struct State {
        std::shared_ptr<dlc::DlcLookupResult> result;
        std::vector<dlc::DlcCandidate> shown;   // grid rows
        std::vector<dlc::DlcCandidate> maps;    // mapping combo rows (index - 1)
        std::optional<dlc::DlcCandidate> selected;
        QString evidence = FW_TR("Manual target; compatibility unverified");
        QString market;
        bool selectedOk = false;
    };
    auto st = std::make_shared<State>();

    const auto select = [=, this](const dlc::DlcCandidate& c) {
        auto p = dlc::propose(c, sourceCid, first.item.title);
        if (!p.ok()) {
            status->setText(p.error().message);
            proposal->setText(FW_TR("No valid release was selected. Choose another release or enter manual identifiers."));
            st->selectedOk = false;
            return;
        }
        st->selected = c;
        st->selectedOk = true;
        st->market = c.region;
        manual->setChecked(false);
        targetTid->setText(p->titleId);
        targetCid->setText(p->contentId);
        bool exact = false;
        if (st->result)
            for (const auto& d : st->result->candidates)
                if (d.productKind == u"DLC" && d.contentId == p->contentId)
                    exact = true;
        st->evidence = (exact ? FW_TR("Exact DLC SKU observed: %1 / %2; DLC compatibility unverified")
                              : FW_TR("DLC SKU not observed; proposed from regional identifier mapping: %1 / %2; DLC compatibility unverified"))
                           .arg(c.region, c.titleId);
        proposal->setText((p->contentId == sourceCid
                               ? FW_TR("This regional release shares the original PPSA/content identity; no identifier change is needed.")
                               : FW_TR("Proposed: %1 \u2192 %2. Publisher prefix and game ID updated; DLC suffix retained.").arg(sourceCid, p->contentId))
                          + QStringLiteral("\n") + FW_TR("%1. Identifier source: %2 \u2014 %3. In-game DLC compatibility remains unverified.")
                                .arg(st->evidence, c.name, c.region));
        status->setText(FW_TR("Release selected. Review the target identifiers, then apply."));
    };
    const auto filter = [=, this] {
        grid->setRowCount(0);
        mapping->blockSignals(true);
        mapping->clear();
        st->shown.clear();
        st->maps.clear();
        if (!st->result) {
            mapping->blockSignals(false);
            return;
        }
        const QString code = region->currentData().toString();
        st->shown = dlc::dlcForRegion(st->result->candidates, code, sourceCid);
        st->maps = dlc::gameMappings(st->result->candidates, code, sourceCid);
        mapping->addItem(st->maps.empty() ? FW_TR("No observed base-game identifier mapping")
                                          : FW_TR("Choose a regional game identifier mapping\u2026"));
        for (const auto& c : st->maps)
            mapping->addItem(QStringLiteral("%1 / %2 \u2014 %3%4 \u2014 %5")
                                 .arg(c.titleId, c.contentId.left(6), c.name,
                                      c.edition.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(c.edition), c.region));
        mapping->setEnabled(!st->maps.empty());
        mapping->blockSignals(false);
        for (const auto& c : st->shown) {
            const int r = grid->rowCount();
            grid->insertRow(r);
            const QStringList cells{c.titleId, c.name, c.edition, c.region,
                                    QStringLiteral("%1 / %2").arg(c.productKind, c.contentId.mid(17, 2)), c.mappingStatus, c.contentId};
            for (int i = 0; i < cells.size(); ++i)
                grid->setItem(r, i, new QTableWidgetItem(cells[i]));
        }
        if (code.isEmpty()) {
            proposal->setText(st->shown.empty()
                                  ? FW_TR("No DLC products observed. Regional game identifiers are available separately; a proposed DLC SKU remains unverified.")
                                  : FW_TR("The table shows observed DLC products only. Choose a target region; base-game identifiers are listed separately."));
            return;
        }
        if (manual->isChecked())
            return;
        std::vector<dlc::DlcCandidate> all = st->shown;
        all.insert(all.end(), st->maps.begin(), st->maps.end());
        if (dlc::hasOneIdentity(all, sourceCid)) {
            for (const auto& c : all)
                if (dlc::isExactDlcSku(c, sourceCid)) {
                    select(c);
                    return;
                }
            select(all.front());
        } else if (!all.empty()) {
            status->setText(FW_TR("Several identifiers or DLC SKUs match. Choose the game mapping or an exact DLC product."));
            proposal->setText(FW_TR("Ambiguous results: no region swap has been selected. The table contains DLC only; game "
                                             "identifiers are in the selector above."));
        } else {
            status->setText(FW_TR("No observed release for this region. Retry by game name or use Manual identifiers."));
            proposal->setText(FW_TR("No automatic identifiers are available for the selected region."));
        }
    };
    const auto runLookup = [=, this](bool edited) {
        if (!onlineLookupAllowed())
            return;
        find->setEnabled(false);
        status->setText(FW_TR("Finding game and regional releases\u2026"));
        const QString key = lookupKey(first);
        if (!edited && m_lookups.contains(key)) {
            st->result = m_lookups.value(key);
            find->setEnabled(true);
            filter();
            return;
        }
        const QString q = query->text().trimmed();
        const QString cacheDir = m_cacheDir;
        const QString title = first.item.title;
        auto* w = new QFutureWatcher<std::shared_ptr<dlc::DlcLookupResult>>(s);
        connect(w, &QFutureWatcher<std::shared_ptr<dlc::DlcLookupResult>>::finished, s, [=, this] {
            w->deleteLater();
            st->result = w->result();
            find->setEnabled(true);
            if (!st->result->resolvedName.isEmpty())
                query->setText(st->result->resolvedName);
            int observed = 0;
            for (const auto& c : st->result->candidates)
                if (c.productKind == u"DLC")
                    ++observed;
            status->setText(st->result->candidates.empty()
                                ? FW_TR("No regional mapping found. Enter a game name and retry, or use Manual identifiers.")
                                : (st->result->isStale ? FW_TR("%1 observed DLC products; game identifier mappings are separate (stale cache). Choose a target region.")
                                   : st->result->fromCache ? FW_TR("%1 observed DLC products; game identifier mappings are separate (cached). Choose a target region.")
                                                           : FW_TR("%1 observed DLC products; game identifier mappings are separate. Choose a target region."))
                                      .arg(observed));
            status->setToolTip(st->result->warnings.join(u'\n'));
            filter();
        });
        w->setFuture(QtConcurrent::run([=, this] {
            CancellationSource cancel;
            dlc::DlcLookupService svc(cacheDir);
            return std::make_shared<dlc::DlcLookupResult>(edited ? svc.lookup(q, cancel.token())
                                                                 : svc.resolveRegions(sourceTid, sourceCid, title, cancel.token()));
        }));
    };
    connect(find, &QPushButton::clicked, s, [=, this] { runLookup(query->text().trimmed() != sourceTid); });
    connect(region, &QComboBox::currentIndexChanged, s, [=, this](int) { filter(); });
    connect(mapping, &QComboBox::currentIndexChanged, s, [=, this](int i) {
        if (i > 0 && size_t(i - 1) < st->maps.size())
            select(st->maps[size_t(i - 1)]);
        else
            proposal->setText(FW_TR("Choose a game identifier mapping or an observed DLC product before applying."));
    });
    connect(grid, &QTableWidget::cellDoubleClicked, s, [=, this](int r, int) {
        if (size_t(r) < st->shown.size())
            select(st->shown[size_t(r)]);
    });
    connect(grid, &QTableWidget::itemSelectionChanged, s, [=, this] {
        const int r = grid->currentRow();
        if (r >= 0 && size_t(r) < st->shown.size())
            select(st->shown[size_t(r)]);
    });
    connect(manual, &QCheckBox::toggled, s, [=, this](bool on) {
        targetTid->setReadOnly(!on);
        targetCid->setReadOnly(!on);
        if (on) {
            st->selected.reset();
            st->selectedOk = false;
            st->market.clear();
            st->evidence = FW_TR("Manual target; compatibility unverified");
            proposal->setText(FW_TR("Enter the complete target identifiers. Manual entries are syntax-checked; mapping and "
                                             "DLC compatibility remain unverified."));
        }
    });
    connect(targetTid, &QLineEdit::editingFinished, s, [=, this] {
        if (!manual->isChecked())
            return;
        auto re = dlc::retargetContentId(targetCid->text().trimmed().toUpper(), targetTid->text().trimmed().toUpper());
        if (re.ok())
            targetCid->setText(re.value());
        else
            status->setText(re.error().message);
    });

    s->addButton(FW_TR("Close"), Sheet::Role::Safe);
    auto* addBtn = s->addButton(FW_TR("Add another target"), Sheet::Role::Safe);
    auto* setBtn = s->addButton(FW_TR("Set selected targets"), Sheet::Role::Primary);
    for (QPushButton* b : {addBtn, setBtn}) {
        QObject::disconnect(b, &QPushButton::clicked, s, nullptr);
        const bool add = b == addBtn;
        connect(b, &QPushButton::clicked, s, [=, this] {
            if (!manual->isChecked() && !st->selectedOk) {
                status->setText(FW_TR("Choose an observed regional release, or enable Manual identifiers."));
                return;
            }
            const QString err = applyTarget(rows, targetTid->text(), targetCid->text(), st->evidence,
                                            manual->isChecked() ? QString() : st->market, add);
            if (!err.isEmpty()) {
                status->setText(err);
                return;
            }
            appendLog(QStringLiteral("Target set: %1 → %2 / %3 (%4)")
                          .arg(rows.size() == 1 ? QStringLiteral("1 row") : QStringLiteral("%1 rows").arg(rows.size()))
                          .arg(targetTid->text(), targetCid->text(), st->evidence));
            s->close(add ? 1 : 2);
        });
    }
    QTimer::singleShot(0, s, [=, this] { runLookup(false); });
    s->exec();
}

// ---- preview / INIs ------------------------------------------------------------------------------------

QString DlcPage::previewText(bool iniMode) const { return dlc::buildPlanText(items(), iniMode, outputFolder()); }

void DlcPage::showPreview()
{
    if (m_rows.empty())
        return;
    auto* s = new Sheet(window(), FW_TR("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    keepLeftToRight(box); // log, code or hex: left to right in any language
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(false));
    box->setMinimumHeight(420);
    s->body()->addWidget(box);
    s->addButton(FW_TR("Close"), Sheet::Role::Safe);
    s->exec();
}

std::vector<dlc::IniExportResult> DlcPage::saveUpdatedInis(QStringList* log)
{
    QStringList lines;
    const QString out = outputFolder();
    auto results = dlc::exportUpdatedInis(items(), out, lines);
    for (const QString& l : lines)
        appendLog(l);
    int saved = 0, failed = 0;
    for (const auto& r : results) {
        for (int idx : r.items) {
            if (idx < 0 || idx >= int(m_rows.size()))
                continue;
            m_rows[size_t(idx)].result = r.error.isEmpty() ? FW_TR("INI validated: %1").arg(nativePath(r.path))
                                                           : FW_TR("Failed: %1").arg(r.error);
            refreshRow(idx);
        }
        (r.error.isEmpty() ? saved : failed)++;
    }
    m_loadRow->set(FW_TR("INI export: %1 saved, %2 failed").arg(saved).arg(failed), failed ? Tone::Warn : Tone::Ok);
    if (m_ctx.settings().value(dlc::keys::kSaveReport).toBool()) {
        QStringList outs, errs;
        for (size_t n = 0; n < m_rows.size(); ++n) {
            outs << QString();
            errs << QString();
        }
        for (const auto& r : results)
            for (int idx : r.items)
                if (idx >= 0 && idx < outs.size()) {
                    outs[idx] = r.path;
                    errs[idx] = r.error;
                }
        const QString rep = dlc::saveBatchReport(out, items(), outs, errs, false, m_log->toPlainText().split(u'\n'));
        if (!rep.isEmpty())
            appendLog(QStringLiteral("Results saved: %1").arg(nativePath(rep)));
    }
    if (log)
        *log = lines;
    return results;
}

void DlcPage::saveInisInteractive()
{
    if (m_rows.empty())
        return;
    if (outputFolder().isEmpty()) {
        m_loadRow->set(FW_TR("Select the output folder."), Tone::Err);
        return;
    }
    auto* s = new Sheet(window(), FW_TR("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    keepLeftToRight(box); // log, code or hex: left to right in any language
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(true));
    box->setMinimumHeight(420);
    s->body()->addWidget(box);
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Save updated INIs"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    saveUpdatedInis();
}

// ---- submit ---------------------------------------------------------------------------------------------

QString DlcPage::validateForm()
{
    if (m_rows.empty())
        return FW_TR("Add DLC INIs or DLC packages first.");
    if (m_loading > 0)
        return FW_TR("Packages are still being read. Try again in a moment.");
    if (const QString k = dlc::hex32Problem(m_key->text().trimmed()); !k.isEmpty()) {
        setInputError(m_key, true);
        return FW_TR("The entitlement key %1").arg(k); // the engine's wording, with the length
    }
    if (const QString p = dlc::hex32Problem(m_passcode->text().trimmed()); !p.isEmpty()) {
        setInputError(m_passcode, true);
        return FW_TR("The PKG source passcode %1").arg(p);
    }
    if (outputFolder().isEmpty())
        return FW_TR("Select the output folder.");
    int buildable = 0;
    for (const Row& r : m_rows)
        if (dlc::itemProblem(r.item, false).isEmpty() && (!r.package || r.package->canRebuild))
            ++buildable;
    if (buildable == 0)
        return FW_TR("None of the entries can be built. Check the Result column (Preview changes shows why).");
    if (options() && options()->hasErrors())
        return options()->errorMessages().join(u' ');
    return {};
}

Result<QString> DlcPage::submit(bool)
{
    // The review step of the Companion.
    auto* s = new Sheet(window(), FW_TR("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    keepLeftToRight(box); // log, code or hex: left to right in any language
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(false));
    box->setMinimumHeight(380);
    s->body()->addWidget(box);
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Build PKG outputs"), Sheet::Role::Primary);
    if (!m_demo && !qApp->property("fwScreenshotMode").toBool() && !qEnvironmentVariableIsSet("FW_TEST_AUTO_ACCEPT")) {
        if (s->exec() != 1)
            return Error(ErrorCode::Cancelled, QStringLiteral("Not started."));
    } else {
        s->close(1);
    }
    appendLog(previewText(false));
    QVariantMap overrides = options() ? options()->overrides() : QVariantMap();
    overrides.insert(dlc::keys::kEntitlementKey, m_key->text().trimmed().toUpper());
    overrides.insert(dlc::keys::kSourcePasscode, m_passcode->text().trimmed());
    dlc::DlcConvertRequest req;
    for (const Row& r : m_rows)
        if (dlc::itemProblem(r.item, false).isEmpty() && (!r.package || r.package->canRebuild))
            req.items.push_back(r.item);
    req.output = outputFolder();
    auto res = jobs::submitDlcConvert(m_ctx, req.toJson(), overrides);
    if (res.ok())
        appendLog(QStringLiteral("Queued %1 DLC entr%2 \u2192 %3").arg(req.items.size())
                      .arg(req.items.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"), nativePath(req.output)));
    return res;
}

// ---- screenshots ---------------------------------------------------------------------------------------

QStringList DlcPage::demoStates() const { return {QStringLiteral("list")}; }

void DlcPage::applyDemoState(const QString& name)
{
    if (name != QLatin1String("list"))
        return;
    m_demo = true;
    clearList();
    const QStringList cids{QStringLiteral("EP9000-PPSA01342_00-DSDIGITALDELUXE0"), QStringLiteral("EP9000-PPSA01342_00-DSARTBOOK0000000"),
                           QStringLiteral("UP9000-PPSA03016_00-RETURNALSOUNDTRK")};
    const QStringList titles{QStringLiteral("Digital Deluxe Upgrade"), QStringLiteral("Digital Art Book"), QStringLiteral("Original Soundtrack")};
    for (int i = 0; i < cids.size(); ++i) {
        Row r;
        r.item.source = QStringLiteral("D:/PS5/DLC/dlc_emu.ini");
        r.item.entry = i;
        r.item.contentId = r.item.detectedContentId = cids[i];
        r.item.titleId = r.item.detectedTitleId = dlc::titleIdOf(cids[i]);
        r.item.title = titles[i];
        r.result = i == 0 ? QStringLiteral("Exact DLC SKU observed: Americas (US) / PPSA01341; DLC compatibility unverified")
                          : QStringLiteral("Compatibility unverified");
        if (i == 0) {
            r.item.contentId = QStringLiteral("UP9000-PPSA01341_00-DSDIGITALDELUXE0");
            r.item.titleId = QStringLiteral("PPSA01341");
            r.item.targetMarket = QStringLiteral("Americas (US)");
        }
        addRow(std::move(r));
    }
    m_regionLine->setText(QStringLiteral("Demon's Souls \u2014 Regional game IDs: US: PPSA01341 / UP9000; UK: PPSA01342 / EP9000; Japan: "
                                         "PPSA01343 / JP9000 (cached). Observed DLC products: 4. DLC compatibility unverified."));
    m_output->setPath(QStringLiteral("D:\\PS5\\DLC packages"));
    updateState();
}

void DlcPage::clearDemoState()
{
    if (!m_demo)
        return;
    m_demo = false;
    clearList();
    m_output->setPath(m_ctx.settings().value(dlc::keys::kOutputFolder).toString());
}

} // namespace fw::pages
