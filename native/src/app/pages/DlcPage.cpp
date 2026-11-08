#include "app/pages/DlcPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
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

QPixmap placeholderIcon()
{
    QPixmap pm(40, 40);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color::bg_3);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, 40, 40), 6, 6);
    icon(QStringLiteral("package"), color::fg_4).paint(&p, QRect(10, 10, 20, 20));
    return pm;
}

// The Companion's Resize512: 512x512, no alpha, stretched, high quality.
QByteArray resize512Png(const QString& path, QString* error)
{
    QImage img(path);
    if (img.isNull()) {
        if (error)
            *error = QStringLiteral("The image could not be read.");
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
    if (code == u"UP") return QStringLiteral("US");
    if (code == u"EP") return QStringLiteral("UK");
    if (code == u"JP") return QStringLiteral("Japan");
    if (code == u"HP") return QStringLiteral("Hong Kong");
    if (code == u"KP") return QStringLiteral("Korea");
    return code;
}

} // namespace

// ---- construction -------------------------------------------------------------------------------

DlcPage::DlcPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("dlc"), QStringLiteral("DLC"),
                  QStringLiteral("Turn DLC lists (.ini) and DLC packages into installable PS5 DLC packages."),
                  QStringLiteral("Convert DLC"), QStringLiteral("DLC conversion"), parent)
{
    setRequiredKinds({dlc::kJobConvert});
    m_cacheDir = QDir(Paths::cacheDir()).filePath(QStringLiteral("dlc-lookup"));
    m_iconDir = QDir(Paths::tempRoot()).filePath(QStringLiteral("dlc-icons"));
    buildListCard();
    buildSaveCard();
    buildOptionsCard();
    addJobOptionsCard(QStringLiteral("Engine and verification"), {QStringLiteral("DLC")},
                      {dlc::keys::kOutputFolder, dlc::keys::kEntitlementKey, dlc::keys::kSourcePasscode},
                      QStringLiteral("All DLC settings"));
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == dlc::keys::kFpkgCliPath)
            m_ctx.checkDependenciesAsync();
        if (key == dlc::keys::kOutputFolder)
            updateState();
    });
    connect(&ctx, &AppContext::dependenciesChecked, this, [this] { updateState(); });
    updateState();
}

DlcPage::~DlcPage() = default;

void DlcPage::buildListCard()
{
    auto* card = new Card(this);
    m_listTools = new QWidget(card);
    auto* th = hbox(m_listTools, space::sm);
    auto* add = button(QStringLiteral("Add files\u2026"), ButtonVariant::Secondary, QStringLiteral("file_plus"), m_listTools);
    add->setToolTip(QStringLiteral("Add DLC INIs (.ini) or DLC packages (.pkg). You can also drop them onto this page."));
    connect(add, &QPushButton::clicked, this, &DlcPage::browseInputs);
    th->addWidget(add);
    auto* titleRow = card->setTitle(QStringLiteral("DLC to convert"), m_listTools);
    m_countBadge = new Badge(QStringLiteral("Nothing added"), Tone::Neutral, card);
    m_countBadge->setIconName(QString());
    titleRow->insertWidget(1, m_countBadge);

    m_drop = new DropZone(QStringLiteral("package_plus"), QStringLiteral("Drop DLC INIs or DLC packages here"),
                          QStringLiteral("dlc_emu.ini and other DLC lists (.ini), or PS5 DLC .pkg files. Click to browse."), card);
    connect(m_drop, &QAbstractButton::clicked, this, &DlcPage::browseInputs);
    card->body()->addWidget(m_drop);

    m_table = new QTableWidget(0, ColCount, card);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Icon"), QStringLiteral("Title"), QStringLiteral("Content ID"),
                                        QStringLiteral("Title ID"), QStringLiteral("Region"),
                                        QStringLiteral("Version"), QStringLiteral("Source file"), QStringLiteral("Result")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(52);
    m_table->setIconSize(QSize(40, 40));
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->setAccessibleName(QStringLiteral("DLC entries"));
    m_table->setMinimumHeight(300);
    auto* hh = m_table->horizontalHeader();
    hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hh->setSectionResizeMode(QHeaderView::Interactive);
    hh->resizeSection(IconCol, 48);
    hh->resizeSection(TitleCol, 144);
    hh->resizeSection(ContentIdCol, 290);
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
    auto* removeAct = new QAction(QStringLiteral("Remove from the list"), m_table);
    removeAct->setShortcut(QKeySequence::Delete);
    removeAct->setShortcutContext(Qt::WidgetShortcut);
    connect(removeAct, &QAction::triggered, this, [this] {
        QList<int> rows = selectedRows();
        std::sort(rows.begin(), rows.end(), std::greater<>());
        for (int r : rows) {
            m_rows.erase(m_rows.begin() + r);
            m_table->removeRow(r);
        }
        updateState();
    });
    m_table->addAction(removeAct);
    card->body()->addWidget(m_table, 1);

    auto* actions = new QWidget(card);
    auto* ah = hbox(actions, space::sm);
    m_regionBtn = button(QStringLiteral("Change region\u2026"), ButtonVariant::Secondary, QStringLiteral("globe"), actions);
    m_regionBtn->setToolTip(QStringLiteral("Retarget the selected DLC to another region's game: the publisher prefix and title "
                                           "ID change, each DLC keeps its own suffix."));
    connect(m_regionBtn, &QPushButton::clicked, this, &DlcPage::changeRegion);
    m_previewBtn = button(QStringLiteral("Preview changes"), ButtonVariant::Secondary, QStringLiteral("eye"), actions);
    connect(m_previewBtn, &QPushButton::clicked, this, &DlcPage::showPreview);
    m_saveIniBtn = button(QStringLiteral("Save updated INIs"), ButtonVariant::Secondary, QStringLiteral("save"), actions);
    m_saveIniBtn->setToolTip(QStringLiteral("Write \"<input>-<TitleId>-updated.ini\" files with the edited identifiers; "
                                            "comments, unknown keys and the encoding are kept."));
    connect(m_saveIniBtn, &QPushButton::clicked, this, &DlcPage::saveInisInteractive);
    ah->addWidget(m_regionBtn);
    ah->addWidget(m_previewBtn);
    ah->addWidget(m_saveIniBtn);
    ah->addStretch(1);
    m_clearLink = new Link(QStringLiteral("Clear list"), actions);
    connect(m_clearLink, &Link::activated, this, &DlcPage::clearList);
    ah->addWidget(m_clearLink);
    card->body()->addWidget(actions);

    m_regionLine = caption(QString(), card);
    m_regionLine->setWordWrap(true);
    m_regionLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    card->body()->addWidget(m_regionLine);
    m_loadRow = new MessageRow(card);
    card->body()->addWidget(m_loadRow);

    auto* logTitle = new Disclosure(QStringLiteral("Conversion log"), QString(), card);
    m_log = new QPlainTextEdit(logTitle);
    m_log->setReadOnly(true);
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
    card->setTitle(QStringLiteral("Save to"));
    m_output = new PathField(PathField::Mode::Directory, card);
    m_output->setDialogTitle(QStringLiteral("Choose the output folder"));
    m_output->edit()->setAccessibleName(QStringLiteral("Output folder"));
    m_output->setPath(m_ctx.settings().value(dlc::keys::kOutputFolder).toString());
    connect(m_output, &PathField::pathChanged, this, [this](const QString& p) {
        if (!m_demo && !qApp->property("fwScreenshotMode").toBool())
            (void)m_ctx.settings().setValue(dlc::keys::kOutputFolder, p.trimmed());
        updateState();
    });
    card->body()->addWidget(field(QStringLiteral("Folder"), m_output,
                                  QStringLiteral("Packages are named <ContentId>.pkg; an existing file is never replaced (-2, "
                                                 "-3 \u2026)."),
                                  card));
    form()->addWidget(card);
}

void DlcPage::buildOptionsCard()
{
    auto* card = new Card(this);
    card->setTitle(QStringLiteral("Options"));
    auto* row = new QWidget(card);
    auto* h = hbox(row, space::md);
    h->setAlignment(Qt::AlignTop);
    m_key = textInput(QStringLiteral("32 hexadecimal characters"), true, row);
    m_key->setMaxLength(32);
    m_key->setText(m_ctx.settings().value(dlc::keys::kEntitlementKey).toString());
    m_key->setAccessibleName(QStringLiteral("Entitlement key"));
    m_key->setToolTip(QStringLiteral("Development entitlement records are generated for the target Content ID with this key. "
                                     "Default: 32 zeroes. The DLC engine builds additional content (entitlement DRM)."));
    h->addWidget(field(QStringLiteral("Entitlement key"), m_key, {}, row), 1);
    m_passcode = textInput(QStringLiteral("32 hexadecimal characters"), true, row);
    m_passcode->setMaxLength(32);
    m_passcode->setText(m_ctx.settings().value(dlc::keys::kSourcePasscode).toString());
    m_passcode->setAccessibleName(QStringLiteral("PKG source passcode"));
    m_passcode->setToolTip(QStringLiteral("Used to extract imported DLC packages and for the rebuilt package. Default: 32 zeroes."));
    h->addWidget(field(QStringLiteral("PKG source passcode"), m_passcode, {}, row), 1);
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
    eh->addWidget(label(QStringLiteral("DLC engine"), TextStyle::Body, color::fg_1, eng));
    m_engineBadge = new Badge(QStringLiteral("Checking\u2026"), Tone::Neutral, eng);
    eh->addWidget(m_engineBadge);
    m_engineDetail = caption(QString(), eng);
    m_engineDetail->setWordWrap(true);
    eh->addWidget(m_engineDetail, 1);
    auto* setup = new Link(QStringLiteral("Settings\u2026"), eng);
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
    const QStringList files = QFileDialog::getOpenFileNames(window(), QStringLiteral("Add DLC INIs or PKGs"), QString(),
                                                            QStringLiteral("DLC inputs (*.ini *.pkg);;INI (*.ini);;PKG (*.pkg)"));
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
                r.result = QStringLiteral("Compatibility unverified");
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
                QByteArray icon;
            };
            auto* watcher = new QFutureWatcher<std::shared_ptr<Loaded>>(this);
            connect(watcher, &QFutureWatcher<std::shared_ptr<Loaded>>::finished, this, [this, watcher, path] {
                watcher->deleteLater();
                --m_loading;
                m_loadingPaths.remove(path.toLower());
                const auto loaded = watcher->result();
                if (!loaded->info.ok()) {
                    appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 %2").arg(nativePath(path), loaded->info.error().message));
                    m_loadRow->set(QStringLiteral("%1 could not be read: %2").arg(QFileInfo(path).fileName(), loaded->info.error().message),
                                   Tone::Err);
                    updateState();
                    return;
                }
                const dlc::DlcPackageInfo& info = loaded->info.value();
                if (!info.isDlc) {
                    const QString why = QStringLiteral("This PKG is a game or patch, not DLC. Use Build image \u203a FPKG for games and "
                                                       "updates; the DLC page accepts additional-content PKGs only.");
                    appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 %2").arg(nativePath(path), why));
                    m_loadRow->set(why, Tone::Err);
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
                r.result = info.canRebuild ? QStringLiteral("Compatibility unverified") : info.rebuildLimitation;
                if (!loaded->icon.isEmpty())
                    r.icon.loadFromData(loaded->icon);
                appendLog(QStringLiteral("Loaded %1: %2 / %3").arg(nativePath(path), info.titleId, info.contentId));
                appendLog(QStringLiteral("  %1").arg(r.item.metadataLine));
                for (const QString& w : info.warnings)
                    appendLog(QStringLiteral("  %1").arg(w));
                addRow(std::move(r));
                updateState();
            });
            watcher->setFuture(QtConcurrent::run([cliPath, path, passcode, tempDir] {
                auto loaded = std::make_shared<Loaded>();
                CancellationSource cancel;
                const dlc::FpkgCli cli = dlc::locateFpkgCli(cliPath);
                if (!cli.ok() && !dlc::looksLikePs4Package(path)) {
                    loaded->info = Error(ErrorCode::DependencyMissing, cli.problems.join(u' '));
                    return loaded;
                }
                loaded->info = dlc::inspectDlcPackage(cli, path, passcode, cancel.token());
                if (loaded->info.ok() && loaded->info->isDlc) {
                    QDir().mkpath(tempDir);
                    loaded->icon = dlc::extractDlcIcon(cli, path, passcode, tempDir, cancel.token());
                }
                return loaded;
            }));
        } else {
            appendLog(QStringLiteral("LOAD FAILED: %1 \u2014 Choose a DLC .ini or .pkg file.").arg(nativePath(path)));
            ++failed;
        }
    }
    if (added > 0 || failed > 0)
        m_loadRow->set(QStringLiteral("%1 entries added; %2 files failed").arg(added).arg(failed), failed ? Tone::Warn : Tone::Ok);
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
    m_table->item(r, IconCol)->setIcon(QIcon(row.icon.isNull() ? placeholderIcon() : row.icon));
    m_table->item(r, IconCol)->setToolTip(QStringLiteral("Double-click to choose icon0.png"));
    m_table->item(r, TitleCol)->setText(row.item.title);
    m_table->item(r, TitleCol)->setToolTip(QStringLiteral("%1\nTitle ID: %2\nVersion: %3\nSource: %4%5")
                                               .arg(row.item.title, row.item.titleId,
                                                    row.item.version.isEmpty() ? QStringLiteral("-") : row.item.version,
                                                    nativePath(row.item.source),
                                                    row.isPackage ? QString() : QStringLiteral(" [entry %1]").arg(row.item.entry + 1)));
    m_table->item(r, ContentIdCol)->setText(row.item.contentId);
    m_table->item(r, ContentIdCol)->setFont(font::mono());
    m_table->item(r, TitleIdCol)->setText(row.item.titleId);
    m_table->item(r, TitleIdCol)->setFont(font::mono());
    {
        // Short label in the cell; the Companion's full text in the tooltip.
        const QString code = dlc::regionCode(row.item.contentId);
        const QString shortName = code == u"UP" ? QStringLiteral("Americas") : code == u"EP" ? QStringLiteral("Europe")
                                : code == u"JP" ? QStringLiteral("Japan") : code == u"HP" ? QStringLiteral("Asia")
                                : code == u"KP" ? QStringLiteral("Korea") : QStringLiteral("Unknown");
        m_table->item(r, RegionCol)->setText(row.item.targetMarket.isEmpty() ? QStringLiteral("%1 · %2").arg(shortName, code)
                                                                              : QStringLiteral("%1 Store").arg(row.item.targetMarket.section(u'(', 1).remove(u')')));
        m_table->item(r, RegionCol)->setToolTip(row.item.targetMarket.isEmpty() ? dlc::regionText(row.item.contentId)
                                                                                 : QStringLiteral("%1 (Store)").arg(row.item.targetMarket));
    }
    m_table->item(r, VersionCol)->setText(row.item.version);
    m_table->item(r, SourceCol)->setText(QFileInfo(row.item.source).fileName()
                                         + (row.isPackage ? QString() : QStringLiteral(" [%1]").arg(row.item.entry + 1)));
    m_table->item(r, SourceCol)->setToolTip(nativePath(row.item.source));
    m_table->item(r, ResultCol)->setText(row.result);
    m_table->item(r, ResultCol)->setToolTip(row.warnings.isEmpty() ? row.result : row.result + QStringLiteral("\n") + row.warnings.join(u'\n'));
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
    const Status st = dlc::validateIdentity(row.item.titleId, row.item.contentId);
    row.result = st.ok() ? QStringLiteral("Manual target; compatibility unverified") : st.error().message;
    refreshRow(r);
    updateState();
}

void DlcPage::chooseIcon(int r)
{
    if (r < 0 || r >= int(m_rows.size()))
        return;
    const QString file = QFileDialog::getOpenFileName(window(), QStringLiteral("Select the icon0.png"), QString(),
                                                      QStringLiteral("PNG (*.png)"));
    if (file.isEmpty())
        return;
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
        m_loadRow->set(QStringLiteral("The icon could not be prepared."), Tone::Err);
        return;
    }
    row.icon.loadFromData(png);
    row.iconChosen = true;
    row.item.iconPath = row.iconFile;
    refreshRow(r);
}

void DlcPage::clearList()
{
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
    m_regionBtn->setEnabled(any);
    m_previewBtn->setEnabled(any);
    m_saveIniBtn->setEnabled(any);
    m_clearLink->setVisible(any);
    if (m_loading > 0)
        m_countBadge->set(QStringLiteral("Reading %1 package%2\u2026").arg(m_loading).arg(m_loading == 1 ? QString() : QStringLiteral("s")),
                          Tone::Info);
    else if (any)
        m_countBadge->set(QStringLiteral("%1 entr%2").arg(m_rows.size()).arg(m_rows.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies")),
                          Tone::Info);
    else
        m_countBadge->set(QStringLiteral("Nothing added"), Tone::Neutral);
    m_output->setPlaceholder(m_rows.empty() ? QStringLiteral("A \"DLC packages\" folder next to the first input")
                                            : nativePath(outputFolder()));
    const DependencyInfo info = m_ctx.dependencies().last(QStringLiteral("dlc.fpkgcli"));
    if (info.id.isEmpty()) {
        m_engineBadge->set(QStringLiteral("Checking\u2026"), Tone::Neutral);
        m_engineDetail->setText(QStringLiteral("fpkg-cli (LibProsperoPkg) builds the DLC packages."));
    } else if (info.status == DependencyStatus::Ready) {
        m_engineBadge->set(info.version.isEmpty() ? QStringLiteral("Ready") : info.version, Tone::Ok);
        m_engineDetail->setText(nativePath(info.path));
    } else {
        m_engineBadge->set(info.status == DependencyStatus::Missing ? QStringLiteral("Not set") : dependencyStatusName(info.status),
                           Tone::Warn);
        m_engineDetail->setText(info.detail.isEmpty() ? info.howToFix : info.detail);
    }
    emit statusContextChanged();
}

QString DlcPage::statusContext() const
{
    return m_rows.empty() ? QString() : QStringLiteral("%1 DLC entr%2").arg(m_rows.size()).arg(m_rows.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
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
        connect(w, &QFutureWatcher<std::shared_ptr<dlc::DlcLookupResult>>::finished, this, [this, w, k] {
            w->deleteLater();
            m_lookupRunning.remove(k);
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
        w->setFuture(QtConcurrent::run([cacheDir, cid, title] {
            CancellationSource cancel;
            dlc::DlcLookupService svc(cacheDir);
            return std::make_shared<dlc::DlcLookupResult>(svc.resolveRegions(dlc::titleIdOf(cid), cid, title, cancel.token()));
        }));
    }
    updateRegionLine();
}

void DlcPage::updateRegionLine()
{
    const QList<int> rows = selectedRows();
    const int r = rows.isEmpty() ? (m_rows.empty() ? -1 : 0) : rows.first();
    if (r < 0) {
        m_regionLine->setText(QStringLiteral("Import DLC to find the game's regional releases. Change region\u2026 applies a reviewed target."));
        return;
    }
    const QString key = lookupKey(m_rows[size_t(r)]);
    if (key.isEmpty() || !onlineLookupAllowed()) {
        m_regionLine->setText(QStringLiteral("Regional lookup: use Change region\u2026 to find the game or enter manual identifiers."));
        return;
    }
    if (m_lookupRunning.contains(key) || m_lookupQueue.contains(key)) {
        m_regionLine->setText(QStringLiteral("Finding regional releases for %1\u2026 Local work remains available.").arg(key.section(u'|', 0, 0)));
        return;
    }
    const auto res = m_lookups.value(key);
    if (!res) {
        m_regionLine->setText(QStringLiteral("Regional lookup stopped. Use Change region\u2026 to retry or enter manual identifiers."));
        return;
    }
    const QString cid = m_rows[size_t(r)].item.detectedContentId;
    const auto maps = dlc::gameMappings(res->candidates, QString(), cid);
    if (maps.empty()) {
        m_regionLine->setText(QStringLiteral("No regional mapping found for %1. Change region\u2026 lets you enter the game name or "
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
    m_regionLine->setText(QStringLiteral("%1 \u2014 Regional game IDs: %2%3. Observed DLC products: %4. DLC compatibility unverified.")
                              .arg(name, parts.join(QStringLiteral("; ")),
                                   res->isStale ? QStringLiteral(" (stale cache)") : res->fromCache ? QStringLiteral(" (cached)") : QString())
                              .arg(observed));
    m_regionLine->setToolTip(QStringLiteral("Regional game IDs identify releases; observed DLC products are listed separately. "
                                            "Identifier mappings do not confirm in-game DLC compatibility."));
}

QString DlcPage::applyTarget(const QList<int>& rows, const QString& titleId, const QString& contentId, const QString& evidence,
                             const QString& market, bool addCopies)
{
    if (rows.isEmpty())
        return QStringLiteral("Select the DLC entries to retarget.");
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
            return QStringLiteral("A PS4 DLC cannot be converted into PS5 DLC, or vice versa.");
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

    auto* s = new Sheet(window(), QStringLiteral("DLC regions / targets"), Sheet::Icon::None, 1000);
    s->addText(QStringLiteral("Selected: %1 DLC entries. Original: %2 / %3<br>Find the game, choose a region, then apply. Each DLC "
                              "item keeps its own suffix; an exact observed DLC SKU is used when available. Game identifiers can "
                              "be shared across regions.")
                   .arg(rows.size())
                   .arg(Sheet::monoSpan(sourceTid), Sheet::monoSpan(sourceCid)));
    // Query row
    auto* qrow = new QWidget(s);
    auto* qh = hbox(qrow, space::sm);
    auto* query = textInput(QStringLiteral("Title ID, Content ID, game name or Store product URL"), false, qrow);
    query->setText(sourceTid);
    auto* find = button(QStringLiteral("Find regions"), ButtonVariant::Secondary, QStringLiteral("search"), qrow);
    auto* manual = new QCheckBox(QStringLiteral("Manual identifiers"), qrow);
    qh->addWidget(field(QStringLiteral("Game name / ID"), query, {}, qrow), 1);
    qh->addWidget(find, 0, Qt::AlignBottom);
    qh->addWidget(manual, 0, Qt::AlignBottom);
    s->body()->addWidget(qrow);
    // Region + mapping
    auto* rrow = new QWidget(s);
    auto* rh = hbox(rrow, space::md);
    auto* region = dropdown(rrow);
    region->addItem(QStringLiteral("Choose target region\u2026"), QString());
    region->addItem(QStringLiteral("Americas / US"), QStringLiteral("UP"));
    region->addItem(QStringLiteral("Europe / UK"), QStringLiteral("EP"));
    region->addItem(QStringLiteral("Japan"), QStringLiteral("JP"));
    region->addItem(QStringLiteral("Asia / Hong Kong"), QStringLiteral("HP"));
    region->addItem(QStringLiteral("Korea"), QStringLiteral("KP"));
    rh->addWidget(field(QStringLiteral("Target region"), region, {}, rrow), 0);
    auto* mapping = dropdown(rrow);
    mapping->setToolTip(QStringLiteral("Base-game releases supply regional PPSA and publisher identifiers. These are identifier "
                                       "mappings, not observed DLC products or verified DLC compatibility."));
    rh->addWidget(field(QStringLiteral("Game identifier mapping"), mapping, {}, rrow), 1);
    s->body()->addWidget(rrow);
    auto* status = caption(onlineLookupAllowed() ? QStringLiteral("Finding game and regional releases\u2026")
                                                 : QStringLiteral("Online lookup is off (Settings \u203a DLC). Use Manual identifiers."),
                           s);
    status->setWordWrap(true);
    s->body()->addWidget(status);
    // Releases
    auto* grid = new QTableWidget(0, 7, s);
    grid->setHorizontalHeaderLabels({QStringLiteral("Title ID"), QStringLiteral("DLC / add-on"), QStringLiteral("Edition"),
                                     QStringLiteral("Store region"), QStringLiteral("Product / _NN"),
                                     QStringLiteral("Identifier evidence"), QStringLiteral("Observed release content ID")});
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
    th->addWidget(field(QStringLiteral("Target title ID"), targetTid, {}, trow), 1);
    th->addWidget(field(QStringLiteral("DLC content ID"), targetCid, {}, trow), 3);
    s->body()->addWidget(trow);
    auto* proposal = caption(QStringLiteral("Choose a release to propose a region swap. DLC compatibility remains unverified."), s);
    proposal->setWordWrap(true);
    s->body()->addWidget(proposal);

    struct State {
        std::shared_ptr<dlc::DlcLookupResult> result;
        std::vector<dlc::DlcCandidate> shown;   // grid rows
        std::vector<dlc::DlcCandidate> maps;    // mapping combo rows (index - 1)
        std::optional<dlc::DlcCandidate> selected;
        QString evidence = QStringLiteral("Manual target; compatibility unverified");
        QString market;
        bool selectedOk = false;
    };
    auto st = std::make_shared<State>();

    const auto select = [=, this](const dlc::DlcCandidate& c) {
        auto p = dlc::propose(c, sourceCid, first.item.title);
        if (!p.ok()) {
            status->setText(p.error().message);
            proposal->setText(QStringLiteral("No valid release was selected. Choose another release or enter manual identifiers."));
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
        st->evidence = QStringLiteral("%1: %2 / %3; DLC compatibility unverified")
                           .arg(exact ? QStringLiteral("Exact DLC SKU observed") : QStringLiteral("DLC SKU not observed; proposed from regional identifier mapping"),
                                c.region, c.titleId);
        proposal->setText((p->contentId == sourceCid
                               ? QStringLiteral("This regional release shares the original PPSA/content identity; no identifier change is needed.")
                               : QStringLiteral("Proposed: %1 \u2192 %2. Publisher prefix and game ID updated; DLC suffix retained.").arg(sourceCid, p->contentId))
                          + QStringLiteral("\n%1. Identifier source: %2 \u2014 %3. In-game DLC compatibility remains unverified.")
                                .arg(st->evidence, c.name, c.region));
        status->setText(QStringLiteral("Release selected. Review the target identifiers, then apply."));
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
        mapping->addItem(st->maps.empty() ? QStringLiteral("No observed base-game identifier mapping")
                                          : QStringLiteral("Choose a regional game identifier mapping\u2026"));
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
                                  ? QStringLiteral("No DLC products observed. Regional game identifiers are available separately; a proposed DLC SKU remains unverified.")
                                  : QStringLiteral("The table shows observed DLC products only. Choose a target region; base-game identifiers are listed separately."));
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
            status->setText(QStringLiteral("Several identifiers or DLC SKUs match. Choose the game mapping or an exact DLC product."));
            proposal->setText(QStringLiteral("Ambiguous results: no region swap has been selected. The table contains DLC only; game "
                                             "identifiers are in the selector above."));
        } else {
            status->setText(QStringLiteral("No observed release for this region. Retry by game name or use Manual identifiers."));
            proposal->setText(QStringLiteral("No automatic identifiers are available for the selected region."));
        }
    };
    const auto runLookup = [=, this](bool edited) {
        if (!onlineLookupAllowed())
            return;
        find->setEnabled(false);
        status->setText(QStringLiteral("Finding game and regional releases\u2026"));
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
                                ? QStringLiteral("No regional mapping found. Enter a game name and retry, or use Manual identifiers.")
                                : QStringLiteral("%1 observed DLC products; game identifier mappings are separate%2. Choose a target region.")
                                      .arg(observed)
                                      .arg(st->result->isStale ? QStringLiteral(" (stale cache)") : st->result->fromCache ? QStringLiteral(" (cached)") : QString()));
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
            proposal->setText(QStringLiteral("Choose a game identifier mapping or an observed DLC product before applying."));
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
            st->evidence = QStringLiteral("Manual target; compatibility unverified");
            proposal->setText(QStringLiteral("Enter the complete target identifiers. Manual entries are syntax-checked; mapping and "
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

    s->addButton(QStringLiteral("Close"), Sheet::Role::Safe);
    auto* addBtn = s->addButton(QStringLiteral("Add another target"), Sheet::Role::Safe);
    auto* setBtn = s->addButton(QStringLiteral("Set selected targets"), Sheet::Role::Primary);
    for (QPushButton* b : {addBtn, setBtn}) {
        QObject::disconnect(b, &QPushButton::clicked, s, nullptr);
        const bool add = b == addBtn;
        connect(b, &QPushButton::clicked, s, [=, this] {
            if (!manual->isChecked() && !st->selectedOk) {
                status->setText(QStringLiteral("Choose an observed regional release, or enable Manual identifiers."));
                return;
            }
            const QString err = applyTarget(rows, targetTid->text(), targetCid->text(), st->evidence,
                                            manual->isChecked() ? QString() : st->market, add);
            if (!err.isEmpty()) {
                status->setText(err);
                return;
            }
            appendLog(QStringLiteral("Target set: %1 row(s) \u2192 %2 / %3 (%4)")
                          .arg(rows.size()).arg(targetTid->text(), targetCid->text(), st->evidence));
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
    auto* s = new Sheet(window(), QStringLiteral("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(false));
    box->setMinimumHeight(420);
    s->body()->addWidget(box);
    s->addButton(QStringLiteral("Close"), Sheet::Role::Safe);
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
            m_rows[size_t(idx)].result = r.error.isEmpty() ? QStringLiteral("INI validated: %1").arg(nativePath(r.path))
                                                           : QStringLiteral("Failed: %1").arg(r.error);
            refreshRow(idx);
        }
        (r.error.isEmpty() ? saved : failed)++;
    }
    m_loadRow->set(QStringLiteral("INI export: %1 saved, %2 failed").arg(saved).arg(failed), failed ? Tone::Warn : Tone::Ok);
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
        m_loadRow->set(QStringLiteral("Select the output folder."), Tone::Err);
        return;
    }
    auto* s = new Sheet(window(), QStringLiteral("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(true));
    box->setMinimumHeight(420);
    s->body()->addWidget(box);
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Save updated INIs"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    saveUpdatedInis();
}

// ---- submit ---------------------------------------------------------------------------------------------

QString DlcPage::validateForm()
{
    if (m_rows.empty())
        return QStringLiteral("Add DLC INIs or DLC packages first.");
    if (m_loading > 0)
        return QStringLiteral("Packages are still being read. Try again in a moment.");
    if (const QString k = dlc::hex32Problem(m_key->text().trimmed()); !k.isEmpty()) {
        setInputError(m_key, true);
        return QStringLiteral("The entitlement key must be exactly 32 hex characters.");
    }
    if (const QString p = dlc::hex32Problem(m_passcode->text().trimmed()); !p.isEmpty()) {
        setInputError(m_passcode, true);
        return QStringLiteral("PKG source passcode must be exactly 32 hex characters.");
    }
    if (outputFolder().isEmpty())
        return QStringLiteral("Select the output folder.");
    int buildable = 0;
    for (const Row& r : m_rows)
        if (dlc::itemProblem(r.item, false).isEmpty() && (!r.package || r.package->canRebuild))
            ++buildable;
    if (buildable == 0)
        return QStringLiteral("None of the entries can be built. Check the Result column (Preview changes shows why).");
    if (options() && options()->hasErrors())
        return options()->errorMessages().join(u' ');
    return {};
}

Result<QString> DlcPage::submit(bool)
{
    // The review step of the Companion.
    auto* s = new Sheet(window(), QStringLiteral("DLC changes \u2014 review outputs"), Sheet::Icon::None, 900);
    auto* box = new QPlainTextEdit(s);
    box->setReadOnly(true);
    box->setFont(font::mono());
    box->setPlainText(previewText(false));
    box->setMinimumHeight(380);
    s->body()->addWidget(box);
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Build PKG outputs"), Sheet::Role::Primary);
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
