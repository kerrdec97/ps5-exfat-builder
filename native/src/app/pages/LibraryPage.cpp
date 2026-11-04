#include "app/pages/LibraryPage.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/gameinfo/DumpRename.h"
#include "engines/gameinfo/Module.h"
#include "engines/tools/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>

namespace fw::pages {

using namespace ui;
using namespace theme;

LibraryPage::LibraryPage(AppContext& ctx, JobQueueModel& queueModel, LibraryStore& library, QWidget* parent)
    : Page(ctx, QStringLiteral("library"), tr("Library"),
           tr("Your game dumps: build images, package them, backport them, or trim languages you do not need."), parent),
      m_library(library)
{
    Q_UNUSED(queueModel);
    m_reader = new GameInfoReader(this);
    connect(m_reader, &GameInfoReader::ready, this, [this](const GameSummary& s) {
        if (s.exists)
            m_library.upsertGame(s);
        if (!m_pendingAdds.isEmpty())
            m_reader->summarize(m_pendingAdds.takeFirst(), true);
    });

    m_tabs = new QTabBar(this);
    m_tabs->addTab(tr("Games"));
    m_tabs->addTab(tr("Languages"));
    m_tabs->addTab(tr("DLC emulator"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setProperty("fwNoRing", true);
    auto* tabWrap = new QWidget(this);
    auto* tl = vbox(tabWrap, 0);
    tl->addWidget(m_tabs);
    tl->addWidget(hairline(tabWrap, true));
    content()->addWidget(tabWrap);
    m_stack = new QStackedWidget(this);
    m_stack->setProperty("fwNoRing", true);
    content()->addWidget(m_stack);
    connect(m_tabs, &QTabBar::currentChanged, m_stack, &QStackedWidget::setCurrentIndex);

    // ---- Games
    auto* games = new QWidget(m_stack);
    auto* gl = vbox(games, space::lg);
    m_gamesCard = new Card(games, Card::Variant::Table);
    auto* h = m_gamesCard->header();
    h->addWidget(heading(tr("Games"), TextStyle::Subtitle, m_gamesCard));
    m_gamesCount = caption({}, m_gamesCard);
    h->addWidget(m_gamesCount);
    h->addStretch(1);
    m_search = textInput(tr("Search title or ID"), false, m_gamesCard);
    m_search->setFixedWidth(220);
    m_search->setClearButtonEnabled(true);
    h->addWidget(m_search);
    auto* add = button(tr("Add folders…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), m_gamesCard);
    auto* rescan = button(tr("Rescan"), ButtonVariant::Secondary, QStringLiteral("refresh"), m_gamesCard);
    auto* renameBtn = button(tr("Rename dumps…"), ButtonVariant::Secondary, QStringLiteral("rename"), m_gamesCard);
    renameBtn->setEnabled(jobs::isAvailable(ctx, QStringLiteral("library.rename")));
    renameBtn->setToolTip(renameBtn->isEnabled() ? tr("Rename dump folders after their title ID, name and version") : tr("Not available in this build"));
    connect(renameBtn, &QPushButton::clicked, this, &LibraryPage::renameDumps);
    h->addWidget(add);
    h->addWidget(rescan);
    h->addWidget(renameBtn);
    connect(add, &QPushButton::clicked, this, &LibraryPage::addFolders);
    connect(rescan, &QPushButton::clicked, this, &LibraryPage::scan);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { rebuildGames(); });
    m_locationsRow = new QWidget(m_gamesCard);
    m_locationsLayout = hbox(m_locationsRow, space::sm, QMargins(space::lg, 6, space::lg, 6));
    m_gamesCard->body()->addWidget(m_locationsRow);
    m_gamesCard->body()->addWidget(hairline(m_gamesCard));
    m_table = new GameTable(false, m_gamesCard);
    m_gamesCard->body()->addWidget(m_table);
    connect(m_table, &GameTable::contextRequested, this, &LibraryPage::gameMenu);
    connect(m_table, &GameTable::activatedFolder, this, [this](const QString& f) {
        emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), f}}});
    });
    m_empty = new EmptyState(QStringLiteral("library"), tr("No games found"),
                             tr("Add a folder that holds game dumps. Each dump is a folder with sce_sys\\param.json."), m_gamesCard);
    auto* emptyAdd = m_empty->addAction(tr("Add folders…"), ButtonVariant::Primary);
    connect(emptyAdd, &QPushButton::clicked, this, &LibraryPage::addFolders);
    m_gamesCard->body()->addWidget(m_empty);
    m_footer = caption({}, m_gamesCard);
    m_gamesCard->footer()->addWidget(m_footer, 1);
    auto* hint = caption(tr("Double-click a game to build an image · right-click for more"), m_gamesCard);
    m_gamesCard->footer()->addWidget(hint);
    gl->addWidget(m_gamesCard);
    gl->addStretch(1);
    m_stack->addWidget(games);

    // ---- Languages
    auto* lang = new QWidget(m_stack);
    auto* langLayout = vbox(lang, space::lg);
    auto* pick = new Card(lang);
    pick->setTitle(tr("Language stripper"));
    pick->body()->addWidget(caption(tr("Removes the language folders and audio packs a game ships for languages you do not use. Every removed file is zipped first; Restore puts it back."), pick));
    m_langFolder = new PathField(PathField::Mode::Directory, pick);
    m_langFolder->setDialogTitle(tr("Choose a game folder, or a folder of games"));
    auto* prow = new QWidget(pick);
    auto* ph = hbox(prow, space::sm);
    ph->addWidget(m_langFolder, 1);
    m_langBatch = new QCheckBox(tr("Folder of games"), prow);
    ph->addWidget(m_langBatch);
    m_langScan = button(tr("Scan"), ButtonVariant::Primary, QStringLiteral("search"), prow);
    ph->addWidget(m_langScan);
    pick->body()->addWidget(field(tr("Game folder"), prow, {}, pick));
    m_langUnavailable = new UnavailableState(tr("The language stripper"), QStringLiteral("tools.languages.scan"), pick);
    pick->body()->addWidget(m_langUnavailable);
    langLayout->addWidget(pick);
    connect(m_langScan, &QPushButton::clicked, this, &LibraryPage::languageScan);
    connect(m_langFolder, &PathField::pathChanged, this, [this](const QString& p) {
        m_langBatch->setChecked(!p.isEmpty() && !readGameSummary(p, false).isGame);
    });

    auto* table = new Card(lang, Card::Variant::Table);
    auto* th = table->header();
    th->addWidget(heading(tr("Languages found"), TextStyle::Subtitle, table));
    th->addStretch(1);
    m_chips = new QWidget(table);
    auto* chips = new FlowLayout(m_chips, space::xs, space::xs);
    struct Quick { const char* text; QStringList codes; };
    const std::vector<Quick> quick = {{"English only", {QStringLiteral("en")}}, {"Japanese only", {QStringLiteral("ja")}},
                                      {"EN + JA", {QStringLiteral("en"), QStringLiteral("ja")}},
                                      {"EU only", {QStringLiteral("en"), QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("it"), QStringLiteral("pt"), QStringLiteral("nl"), QStringLiteral("pl"), QStringLiteral("ru")}},
                                      {"Keep all", {QStringLiteral("*")}}, {"Keep none", {}}};
    for (const auto& q : quick) {
        auto* b = button(QString::fromLatin1(q.text), ButtonVariant::Secondary, {}, m_chips);
        setButtonSmall(b, true);
        const QStringList codes = q.codes;
        connect(b, &QPushButton::clicked, this, [this, codes] { applyQuickKeep(codes); });
        chips->addWidget(b);
    }
    th->addWidget(m_chips);
    m_langTable = new QTableWidget(0, 5, table);
    m_langTable->setHorizontalHeaderLabels({tr("Keep"), tr("Language"), tr("Code"), tr("Folder or file"), tr("Size")});
    m_langTable->horizontalHeader()->setFixedHeight(32);
    m_langTable->horizontalHeader()->setHighlightSections(false);
    m_langTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_langTable->setColumnWidth(0, 60);
    m_langTable->setColumnWidth(1, 160);
    m_langTable->setColumnWidth(2, 70);
    m_langTable->setColumnWidth(4, 100);
    m_langTable->verticalHeader()->hide();
    m_langTable->verticalHeader()->setDefaultSectionSize(size::row);
    m_langTable->setShowGrid(false);
    m_langTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_langTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_langTable->setMinimumHeight(6 * size::row + 40);
    table->body()->addWidget(m_langTable);
    m_langEmpty = new EmptyState(QStringLiteral("language"), tr("Nothing scanned yet"),
                                 tr("Choose a game folder above and scan it to see which languages it ships."), table);
    table->body()->addWidget(m_langEmpty);
    auto* tf = table->footer();
    m_langSummary = caption({}, table);
    tf->addWidget(m_langSummary, 1);
    m_langRestore = button(tr("Restore from backup…"), ButtonVariant::Secondary, QStringLiteral("undo"), table);
    m_langStrip = button(tr("Remove languages…"), ButtonVariant::Danger, QStringLiteral("trash"), table);
    tf->addWidget(m_langRestore);
    tf->addWidget(m_langStrip);
    connect(m_langTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (it->column() == 0)
            updateLanguageSummary();
    });
    connect(m_langStrip, &QPushButton::clicked, this, &LibraryPage::stripLanguages);
    connect(m_langRestore, &QPushButton::clicked, this, &LibraryPage::restoreLanguages);
    langLayout->addWidget(table);
    langLayout->addStretch(1);
    m_stack->addWidget(lang);
    m_dlc = new DlcEmuEditor(m_stack);
    connect(m_dlc, &DlcEmuEditor::toastRequested, this, &LibraryPage::toastRequested);
    m_stack->addWidget(m_dlc);

    connect(&m_library, &LibraryStore::changed, this, &LibraryPage::rebuildGames);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, &LibraryPage::onJobFinished);
    rebuildGames();
    rebuildLanguages();
}

// ---- Games -------------------------------------------------------------------------

void LibraryPage::rebuildGames()
{
    m_table->setGames(m_library.games(), m_search->text().trimmed());
    const bool any = m_table->gameCount() > 0;
    m_table->setVisible(any);
    m_empty->setVisible(!any);
    while (QLayoutItem* it = m_locationsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const QStringList folders = m_library.scanFolders();
    m_locationsLayout->addWidget(caption(folders.isEmpty() ? tr("Scan locations: none yet") : tr("Scan locations:"), m_locationsRow));
    for (const QString& f : folders) {
        auto* chip = new QWidget(m_locationsRow);
        auto* ch = hbox(chip, 2);
        ch->addWidget(label(nativePath(f), TextStyle::MonoSmall, color::fg_2, chip));
        auto* x = new IconButton(QStringLiteral("close"), tr("Remove %1").arg(nativePath(f)), IconButton::Size::Tiny, chip);
        connect(x, &QToolButton::clicked, this, [this, f] { m_library.removeScanFolder(f); });
        ch->addWidget(x);
        m_locationsLayout->addWidget(chip);
    }
    m_locationsLayout->addStretch(1);
    m_gamesCount->setText(tr("%1 game%2").arg(m_table->gameCount()).arg(m_table->gameCount() == 1 ? QString() : tr("s")));
    qint64 total = 0;
    for (const auto& g : m_library.games())
        total += g.info.bytes;
    m_footer->setText(tr("%1 · %2 in %3 location%4")
                          .arg(m_library.lastScan().isValid() ? tr("Last scan %1").arg(relativeTime(m_library.lastScan())) : tr("Not scanned yet"))
                          .arg(formatBytes(total)).arg(folders.size()).arg(folders.size() == 1 ? QString() : tr("s")));
    emit statusContextChanged();
}

void LibraryPage::addFolders()
{
    const QString dir = QFileDialog::getExistingDirectory(window(), tr("Add a folder of game dumps"));
    if (dir.isEmpty())
        return;
    if (readGameSummary(dir, false).isGame) {
        m_pendingAdds << dir;
        if (!m_reader->isBusy())
            m_reader->summarize(m_pendingAdds.takeFirst(), true);
        return;
    }
    m_library.addScanFolder(dir);
    scan();
}

void LibraryPage::scan()
{
    const QStringList folders = m_library.scanFolders();
    if (folders.isEmpty()) {
        emit toastRequested(Tone::Warn, tr("Nothing to scan"), tr("Add a folder of game dumps first."));
        return;
    }
    auto r = jobs::scanLibrary(m_ctx, folders);
    if (!r.ok()) {
        int found = 0;
        for (const QString& f : folders)
            for (const QFileInfo& fi : QDir(f).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (QFile::exists(fi.filePath() + QStringLiteral("/sce_sys/param.json")) || QFile::exists(fi.filePath() + QStringLiteral("/eboot.bin"))) {
                    m_pendingAdds << fi.filePath();
                    ++found;
                }
        if (!m_pendingAdds.isEmpty() && !m_reader->isBusy())
            m_reader->summarize(m_pendingAdds.takeFirst(), true);
        m_library.setLastScan(QDateTime::currentDateTime());
        emit toastRequested(Tone::Info, tr("Scanned without the engine"), tr("%1 game folder%2 found one level below the scan locations.").arg(found).arg(found == 1 ? QString() : tr("s")));
        return;
    }
    m_scanJobId = r.value();
    m_ctx.queue().holdQueue(false);
}

void LibraryPage::gameMenu(const QString& folder, const QPoint& pos)
{
    QMenu menu(this);
    menu.addAction(tr("Build image"), this, [this, folder] { emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), folder}}}); });
    menu.addAction(tr("Build FPKG"), this, [this, folder] { emit navigateRequested({QStringLiteral("fpkg"), {{QStringLiteral("source"), folder}}}); });
    menu.addAction(tr("Backport"), this, [this, folder] { emit navigateRequested({QStringLiteral("backport"), {{QStringLiteral("folder"), folder}}}); });
    menu.addAction(tr("Trim languages"), this, [this, folder] {
        m_tabs->setCurrentIndex(1);
        m_langFolder->setPath(folder);
    });
    menu.addAction(tr("Edit DLC list…"), this, [this, folder] {
        m_tabs->setCurrentIndex(2);
        m_dlc->openFolder(folder);
    });
    menu.addAction(tr("Edit image…"), this, [this, folder] {
        emit navigateRequested({QStringLiteral("edit"), {}});
        Q_UNUSED(folder);
    })->setVisible(false);
    menu.addSeparator();
    menu.addAction(tr("Rename folder…"), this, [this, folder] { renameGame(folder); });
    menu.addAction(tr("Show in folder"), this, [folder] { openInFileManager(folder); });
    menu.addSeparator();
    menu.addAction(tr("Remove from library"), this, [this, folder] { m_library.removeGame(folder); });
    menu.exec(pos);
}

void LibraryPage::renameGame(const QString& folder)
{
    GameSummary info;
    for (const auto& g : m_library.games())
        if (g.info.folder == folder)
            info = g.info;
    if (info.folder.isEmpty())
        return;
    auto* s = new Sheet(window(), tr("Rename the dump folder"), Sheet::Icon::None, size::dialogWide);
    s->addText(tr("Current name: %1").arg(Sheet::monoSpan(QFileInfo(folder).fileName())));
    auto* pattern = new Segmented(s);
    pattern->addSegment(tr("PPSA only"), {}, QStringLiteral("id"));
    pattern->addSegment(tr("PPSA Title"), {}, QStringLiteral("id-title"));
    pattern->addSegment(tr("PPSA Title (version)"), {}, QStringLiteral("id-title-version"));
    pattern->setCurrentIndex(2);
    s->body()->addWidget(field(tr("Pattern"), pattern, {}, s));
    auto* name = textInput({}, true, s);
    auto compute = [info, pattern] {
        const QString v = pattern->currentValue().toString();
        QString n = info.titleId.isEmpty() ? QFileInfo(info.folder).fileName() : info.titleId;
        if (v != QLatin1String("id") && !info.title.isEmpty())
            n += u' ' + info.title;
        if (v == QLatin1String("id-title-version") && !info.version.isEmpty())
            n += QStringLiteral(" (%1)").arg(info.version);
        return sanitizeFileName(n.simplified());
    };
    name->setText(compute());
    connect(pattern, &Segmented::currentChanged, s, [name, compute](int) { name->setText(compute()); });
    s->body()->addWidget(field(tr("New name"), name, tr("Only the folder name changes; the files inside stay as they are."), s));
    s->addButton(tr("Cancel"), Sheet::Role::Safe);
    s->addButton(tr("Rename"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    const QString newName = sanitizeFileName(name->text().trimmed());
    if (newName.isEmpty() || newName == QFileInfo(folder).fileName())
        return;
    const QString target = QFileInfo(folder).absolutePath() + u'/' + newName;
    if (QFileInfo::exists(target)) {
        Sheet::message(window(), tr("A folder with that name exists"), tr("%1 is already there. Choose another name.").arg(Sheet::monoSpan(nativePath(target))), Sheet::Icon::Error);
        return;
    }
    if (!QDir().rename(folder, target)) {
        Sheet::message(window(), tr("The folder could not be renamed"), tr("Windows refused the rename. Close programs that use the folder and try again."), Sheet::Icon::Error);
        return;
    }
    m_library.renameGame(folder, target);
    emit toastRequested(Tone::Ok, tr("Folder renamed"), newName);
}

// ---- Dump rename ----------------------------------------------------------------------

void LibraryPage::renameDumps()
{
    auto* s = new Sheet(window(), tr("Rename dumps"), Sheet::Icon::None, size::dialogWide);
    s->addText(tr("Every dump folder directly inside the source folder gets a name from its title ID, name and version. Nothing is renamed until you confirm the preview; an undo log is written."));
    auto* source = new PathField(PathField::Mode::Directory, s);
    source->setDialogTitle(tr("Choose the folder that holds the dumps"));
    const QStringList folders = m_library.scanFolders();
    if (!folders.isEmpty())
        source->setPath(folders.first());
    s->body()->addWidget(field(tr("Folder of dumps"), source, {}, s));
    auto* preset = new Segmented(s);
    QString current = m_ctx.settings().value(gameinfo::keys::kRenamePreset).toString();
    if (const SettingDef* d = m_ctx.schema().find(gameinfo::keys::kRenamePreset)) {
        for (const auto& c : d->choices)
            preset->addSegment(c.description.isEmpty() ? c.label : c.description, {}, c.value);
    } else {
        preset->addSegment(QStringLiteral("PPSA01234"), {}, QStringLiteral("ppsa"));
        preset->addSegment(QStringLiteral("PPSA01234 Spider-Man"), {}, QStringLiteral("ppsa_title"));
        preset->addSegment(QStringLiteral("PPSA01234 Spider-Man (01.050.000)"), {}, QStringLiteral("ppsa_title_v"));
        if (current.isEmpty())
            current = QStringLiteral("ppsa_title_v");
    }
    preset->setCurrentValue(current);
    s->body()->addWidget(field(tr("Pattern"), preset, {}, s));
    auto* dest = new PathField(PathField::Mode::Directory, s);
    dest->setPlaceholder(tr("Rename in place"));
    s->body()->addWidget(field(tr("Move into (optional)"), dest, tr("Must be on the same drive; folders are renamed, never copied."), s));
    s->addButton(tr("Cancel"), Sheet::Role::Safe);
    auto* undo = s->addButton(tr("Undo last rename…"), Sheet::Role::Safe);
    s->addButton(tr("Preview"), Sheet::Role::Primary);
    const int r = s->exec();
    if (r == 1) {
        const QString log = QFileDialog::getOpenFileName(window(), tr("Choose the rename undo log"), gameinfo::defaultRenameUndoDir(), tr("Undo logs (*.json)"));
        if (log.isEmpty())
            return;
        auto res = jobs::submitLibraryRename(m_ctx, QStringLiteral("undo"), {}, {}, {}, {}, log);
        if (!res.ok())
            emit toastRequested(Tone::Err, tr("Undo not started"), res.error().message);
        else {
            m_renameUndoId = res.value();
            m_ctx.queue().holdQueue(false);
        }
        return;
    }
    Q_UNUSED(undo);
    if (r != 2 || source->path().isEmpty())
        return;
    m_renameSource = source->path();
    m_renamePreset = preset->currentValue().toString();
    m_renameDest = dest->path();
    auto res = jobs::submitLibraryRename(m_ctx, QStringLiteral("preview"), m_renameSource, m_renamePreset, m_renameDest, {}, {});
    if (!res.ok()) {
        emit toastRequested(Tone::Err, tr("Preview not started"), res.error().message);
        return;
    }
    m_renamePreviewId = res.value();
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, tr("Previewing names…"), nativePath(m_renameSource));
}

void LibraryPage::showRenamePreview(const QJsonArray& items, const QString& preset)
{
    if (items.isEmpty()) {
        Sheet::message(window(), tr("No dumps found"), tr("%1 has no sub-folders that look like dumps.").arg(Sheet::monoSpan(nativePath(m_renameSource))), Sheet::Icon::Info);
        return;
    }
    auto* s = new Sheet(window(), tr("Preview: %1 dump%2").arg(items.size()).arg(items.size() == 1 ? QString() : tr("s")), Sheet::Icon::None, 900);
    s->addText(tr("Pattern %1. Edit a proposed name before applying; names that already exist or repeat are skipped.").arg(Sheet::monoSpan(preset)));
    auto* table = new QTableWidget(0, 4, s);
    table->setHorizontalHeaderLabels({tr("Rename"), tr("Current name"), tr("Proposed name"), tr("Status")});
    table->horizontalHeader()->setFixedHeight(32);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->setColumnWidth(0, 70);
    table->setColumnWidth(3, 180);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(size::row);
    table->setShowGrid(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(std::min(12, int(items.size())) * size::row + 40);
    for (const auto& v : items) {
        const QJsonObject it = v.toObject();
        const int r = table->rowCount();
        table->insertRow(r);
        auto* sel = new QTableWidgetItem;
        sel->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        const QString status = it.value(QStringLiteral("status")).toString();
        const bool ready = status == QLatin1String("Ready");
        sel->setCheckState(ready && it.value(QStringLiteral("selected")).toBool(true) ? Qt::Checked : Qt::Unchecked);
        sel->setData(Qt::UserRole, it.value(QStringLiteral("source")).toString());
        table->setItem(r, 0, sel);
        auto* cur = new QTableWidgetItem(it.value(QStringLiteral("currentName")).toString());
        cur->setFont(font::mono());
        cur->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table->setItem(r, 1, cur);
        auto* prop = new QTableWidgetItem(it.value(QStringLiteral("proposedName")).toString());
        prop->setFont(font::mono());
        prop->setForeground(color::fg_0);
        table->setItem(r, 2, prop);
        const QString msg = it.value(QStringLiteral("message")).toString();
        auto* st = new QTableWidgetItem(msg.isEmpty() ? status : QStringLiteral("%1 — %2").arg(status, msg));
        st->setForeground(ready ? color::success_hi : status == QLatin1String("AlreadyNamed") ? color::fg_3 : color::warn_hi);
        st->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        st->setToolTip(QStringLiteral("%1 · %2 · %3").arg(it.value(QStringLiteral("titleId")).toString(), it.value(QStringLiteral("title")).toString(), it.value(QStringLiteral("version")).toString()));
        table->setItem(r, 3, st);
    }
    s->body()->addWidget(table);
    s->addButton(tr("Cancel"), Sheet::Role::Safe);
    s->addButton(tr("Rename"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    QJsonArray edits;
    for (int r = 0; r < table->rowCount(); ++r)
        edits.append(QJsonObject{{QStringLiteral("source"), table->item(r, 0)->data(Qt::UserRole).toString()},
                                 {QStringLiteral("name"), table->item(r, 2)->text().trimmed()},
                                 {QStringLiteral("selected"), table->item(r, 0)->checkState() == Qt::Checked}});
    applyRename(edits);
}

void LibraryPage::applyRename(const QJsonArray& items)
{
    auto res = jobs::submitLibraryRename(m_ctx, QStringLiteral("apply"), m_renameSource, m_renamePreset, m_renameDest, items, {});
    if (!res.ok()) {
        emit toastRequested(Tone::Err, tr("Rename not started"), res.error().message);
        return;
    }
    m_renameApplyId = res.value();
    m_ctx.queue().holdQueue(false);
}

// ---- Languages ---------------------------------------------------------------------

void LibraryPage::languageScan()
{
    const QString folder = m_langFolder->path();
    if (folder.isEmpty()) {
        m_langFolder->setError(tr("Choose a game folder first."));
        return;
    }
    m_langFolder->setError({});
    auto r = jobs::submitLanguageScan(m_ctx, folder, m_langBatch->isChecked());
    if (!r.ok()) {
        emit toastRequested(Tone::Err, tr("Scan not started"), r.error().message);
        return;
    }
    m_langJobId = r.value();
    m_ctx.queue().holdQueue(false);
    m_langSummary->setText(tr("Scanning…"));
}

void LibraryPage::onJobFinished(const QString& id)
{
    auto rec = m_ctx.queue().record(id);
    if (!rec)
        return;
    if (id == m_scanJobId && rec->outcome) {
        std::vector<LibraryGame> games;
        for (const auto& v : rec->outcome->details.value(QStringLiteral("games")).toArray()) {
            LibraryGame g;
            g.info = GameSummary::fromJson(v.toObject());
            g.lastSeen = QDateTime::currentDateTime();
            games.push_back(g);
        }
        if (!games.empty())
            m_library.setGames(std::move(games));
        m_library.setLastScan(QDateTime::currentDateTime());
        m_scanJobId.clear();
    }
    if (id == m_renamePreviewId) {
        m_renamePreviewId.clear();
        if (rec->state != JobState::Succeeded || !rec->outcome) {
            emit toastRequested(Tone::Err, tr("Preview failed"), rec->error ? rec->error->message : tr("See the log."));
        } else {
            showRenamePreview(rec->outcome->details.value(QStringLiteral("items")).toArray(), rec->outcome->details.value(QStringLiteral("preset")).toString());
        }
    }
    if (id == m_renameApplyId) {
        m_renameApplyId.clear();
        const QString undo = rec->outcome ? rec->outcome->details.value(QStringLiteral("undoLog")).toString() : QString();
        if (rec->state == JobState::Succeeded && rec->outcome) {
            const int n = rec->outcome->details.value(QStringLiteral("renamed")).toInt();
            emit toastRequested(n > 0 ? Tone::Ok : Tone::Warn, rec->outcome->summary, undo.isEmpty() ? QString() : tr("An undo log was written; use Rename dumps… › Undo last rename to reverse it."));
            scan();
        } else {
            emit toastRequested(Tone::Err, tr("Rename failed"), rec->error ? rec->error->message : tr("See the log."));
        }
    }
    if (id == m_renameUndoId) {
        m_renameUndoId.clear();
        emit toastRequested(rec->state == JobState::Succeeded ? Tone::Ok : Tone::Err, rec->state == JobState::Succeeded ? tr("Rename undone") : tr("Undo failed"),
                            rec->outcome ? rec->outcome->summary : (rec->error ? rec->error->message : QString()));
        if (rec->state == JobState::Succeeded)
            scan();
    }
    if (id == m_langJobId) {
        m_langJobId.clear();
        if (rec->outcome)
            m_langGames = rec->outcome->details.value(QStringLiteral("games")).toArray();
        else
            m_langGames = {};
        rebuildLanguages();
        if (rec->error)
            emit toastRequested(Tone::Err, tr("Language scan failed"), rec->error->message);
    }
    if (rec->kind == QLatin1String("tools.languages.strip") || rec->kind == QLatin1String("tools.languages.restore")) {
        if (rec->state == JobState::Succeeded && !m_langFolder->path().isEmpty())
            languageScan();
    }
}

void LibraryPage::rebuildLanguages()
{
    const bool available = jobs::isAvailable(m_ctx, QStringLiteral("tools.languages.scan"));
    m_langUnavailable->setVisible(!available);
    m_langScan->setEnabled(available);
    m_langRestore->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("tools.languages.restore")));
    m_langTable->blockSignals(true);
    m_langTable->setRowCount(0);
    for (const auto& gv : m_langGames) {
        const QJsonObject g = gv.toObject();
        const QString root = g.value(QStringLiteral("gameRoot")).toString();
        for (const auto& iv : g.value(QStringLiteral("items")).toArray()) {
            const QJsonObject it = iv.toObject();
            const int r = m_langTable->rowCount();
            m_langTable->insertRow(r);
            auto* keep = new QTableWidgetItem;
            keep->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            keep->setCheckState(Qt::Checked);
            keep->setData(Qt::UserRole, root);
            keep->setData(Qt::UserRole + 1, it.value(QStringLiteral("path")).toString());
            keep->setData(Qt::UserRole + 2, qint64(it.value(QStringLiteral("bytes")).toDouble()));
            keep->setData(Qt::UserRole + 3, it.value(QStringLiteral("code")).toString());
            m_langTable->setItem(r, 0, keep);
            auto* name = new QTableWidgetItem(it.value(QStringLiteral("language")).toString());
            name->setForeground(color::fg_0);
            name->setFont(font::bodyStrong());
            m_langTable->setItem(r, 1, name);
            auto* code = new QTableWidgetItem(it.value(QStringLiteral("code")).toString());
            code->setFont(font::mono());
            m_langTable->setItem(r, 2, code);
            const QString shown = m_langGames.size() > 1 ? QStringLiteral("%1 › %2").arg(QFileInfo(root).fileName(), it.value(QStringLiteral("path")).toString()) : it.value(QStringLiteral("path")).toString();
            auto* path = new QTableWidgetItem(nativePath(shown));
            path->setFont(font::mono());
            path->setForeground(color::fg_3);
            m_langTable->setItem(r, 3, path);
            auto* sizeItem = new QTableWidgetItem(formatBytes(qint64(it.value(QStringLiteral("bytes")).toDouble())));
            sizeItem->setFont(font::mono());
            m_langTable->setItem(r, 4, sizeItem);
        }
    }
    m_langTable->blockSignals(false);
    const bool any = m_langTable->rowCount() > 0;
    m_langTable->setVisible(any);
    m_chips->setVisible(any);
    m_langEmpty->setVisible(!any);
    if (!any && available)
        m_langEmpty->setVisible(true);
    updateLanguageSummary();
}

void LibraryPage::applyQuickKeep(const QStringList& keepCodes)
{
    m_langTable->blockSignals(true);
    for (int r = 0; r < m_langTable->rowCount(); ++r) {
        auto* it = m_langTable->item(r, 0);
        const QString code = it->data(Qt::UserRole + 3).toString().toLower();
        const bool keep = keepCodes.contains(QStringLiteral("*")) || keepCodes.contains(code) || keepCodes.contains(code.section(u'-', 0, 0));
        it->setCheckState(keep ? Qt::Checked : Qt::Unchecked);
    }
    m_langTable->blockSignals(false);
    updateLanguageSummary();
}

void LibraryPage::updateLanguageSummary()
{
    qint64 remove = 0;
    int n = 0, total = m_langTable->rowCount();
    for (int r = 0; r < total; ++r)
        if (m_langTable->item(r, 0)->checkState() != Qt::Checked) {
            remove += m_langTable->item(r, 0)->data(Qt::UserRole + 2).toLongLong();
            ++n;
        }
    m_langSummary->setText(total == 0 ? QString() : n == 0 ? tr("%1 language item%2 · nothing selected for removal").arg(total).arg(total == 1 ? QString() : tr("s"))
                                                          : tr("%1 of %2 items selected for removal · saves about %3").arg(n).arg(total).arg(formatBytes(remove)));
    m_langStrip->setEnabled(n > 0 && jobs::isAvailable(m_ctx, QStringLiteral("tools.languages.strip")));
    m_langStrip->setText(n > 0 ? tr("Remove %1 item%2…").arg(n).arg(n == 1 ? QString() : tr("s")) : tr("Remove languages…"));
}

void LibraryPage::stripLanguages()
{
    QHash<QString, QStringList> byRoot;
    QHash<QString, QStringList> codesByRoot;
    QHash<QString, int> totalByRoot;
    for (int r = 0; r < m_langTable->rowCount(); ++r) {
        auto* it = m_langTable->item(r, 0);
        const QString root = it->data(Qt::UserRole).toString();
        totalByRoot[root]++;
        if (it->checkState() != Qt::Checked) {
            byRoot[root] << it->data(Qt::UserRole + 1).toString();
            codesByRoot[root] << it->data(Qt::UserRole + 3).toString();
        }
    }
    if (byRoot.isEmpty())
        return;
    const QString backup = m_ctx.settings().value(tools::keys::kLanguageBackupFolder).toString();
    bool everything = false;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it)
        if (it.value().size() == totalByRoot.value(it.key()))
            everything = true;
    QStringList lines;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it)
        lines << QStringLiteral("%1: %2").arg(QFileInfo(it.key()).fileName().toHtmlEscaped(), it.value().join(QStringLiteral(", ")).toHtmlEscaped());
    QString text = tr("These files and folders are removed from the game:<br>%1<br><br>A backup ZIP is written first to %2. Restore from backup puts everything back.")
                       .arg(Sheet::monoSpan(lines.join(QStringLiteral("<br>"))), Sheet::monoSpan(backup.isEmpty() ? tr("the default backup folder") : nativePath(backup)));
    if (!Sheet::confirm(window(), tr("Remove the selected languages?"), text, tr("Keep everything"), tr("Remove languages"), true, Sheet::Icon::Warning))
        return;
    if (everything && !Sheet::confirm(window(), tr("Remove every language?"),
                                      tr("At least one game would lose all of its language files, including the one it needs to start. Only do this if you know the game has its text elsewhere."),
                                      tr("Go back"), tr("Remove every language anyway"), true, Sheet::Icon::Error))
        return;
    int queued = 0;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it) {
        auto r = jobs::submitLanguageStrip(m_ctx, it.key(), it.value(), backup);
        if (!r.ok()) {
            emit toastRequested(Tone::Err, tr("Not started"), r.error().message);
            return;
        }
        ++queued;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, tr("Removing languages"), tr("%1 job%2 queued; each one backs up first.").arg(queued).arg(queued == 1 ? QString() : tr("s")));
}

void LibraryPage::restoreLanguages()
{
    const QString backupDir = m_ctx.settings().value(tools::keys::kLanguageBackupFolder).toString();
    const QString zip = QFileDialog::getOpenFileName(window(), tr("Choose the language backup ZIP"), backupDir, tr("Backup ZIP (*.zip)"));
    if (zip.isEmpty())
        return;
    QString root = m_langFolder->path();
    if (root.isEmpty() || m_langBatch->isChecked())
        root = QFileDialog::getExistingDirectory(window(), tr("Choose the game folder to restore into"));
    if (root.isEmpty())
        return;
    auto r = jobs::submitLanguageRestore(m_ctx, zip, root);
    if (!r.ok())
        emit toastRequested(Tone::Err, tr("Restore not started"), r.error().message);
    else
        m_ctx.queue().holdQueue(false);
}

// ---- misc ------------------------------------------------------------------------------

void LibraryPage::refresh() { scan(); }

bool LibraryPage::acceptsDrop(const QList<QUrl>& urls) const
{
    for (const auto& u : urls)
        if (QFileInfo(u.toLocalFile()).isDir())
            return true;
    return false;
}

void LibraryPage::dropUrls(const QList<QUrl>& urls)
{
    for (const auto& u : urls) {
        const QString f = u.toLocalFile();
        if (!QFileInfo(f).isDir())
            continue;
        if (m_tabs->currentIndex() == 1) {
            m_langFolder->setPath(f);
            continue;
        }
        if (readGameSummary(f, false).isGame) {
            m_pendingAdds << f;
            if (!m_reader->isBusy())
                m_reader->summarize(m_pendingAdds.takeFirst(), true);
        } else {
            m_library.addScanFolder(f);
        }
    }
}

void LibraryPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("tab")))
        m_tabs->setCurrentIndex(args.value(QStringLiteral("tab")).toInt());
    if (args.contains(QStringLiteral("dlcFolder"))) {
        m_tabs->setCurrentIndex(2);
        m_dlc->openFolder(args.value(QStringLiteral("dlcFolder")).toString());
    }
    if (args.contains(QStringLiteral("languagesFolder"))) {
        m_tabs->setCurrentIndex(1);
        m_langFolder->setPath(args.value(QStringLiteral("languagesFolder")).toString());
    }
}

QString LibraryPage::statusContext() const
{
    return tr("%1 games in the library").arg(m_library.games().size());
}

void LibraryPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("languages"))
        m_tabs->setCurrentIndex(1);
    else if (name == QLatin1String("dlc"))
        m_tabs->setCurrentIndex(2);
}

void LibraryPage::clearDemoState() { m_tabs->setCurrentIndex(0); }

} // namespace fw::pages
