#include "app/pages/LibraryPage.h"

#include "app/AppContext.h"
#include "app/pages/BuildImagePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/gameinfo/DumpRename.h"
#include "engines/gameinfo/Module.h"
#include "engines/gameinfo/Naming.h"
#include "engines/general/Module.h"
#include "engines/tools/Module.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QSet>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

QLineEdit* searchInput(const QString& placeholder, QWidget* parent)
{
    auto* e = textInput(placeholder, false, parent);
    e->setAccessibleName(placeholder);
    e->setClearButtonEnabled(true);
    e->setToolTip(QStringLiteral("Title, title ID or file name"));
    e->addAction(icon(QStringLiteral("search"), color::fg_4), QLineEdit::LeadingPosition);
    e->setMinimumWidth(170);
    e->setMaximumWidth(300);
    e->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return e;
}

QComboBox* combo(const QStringList& items, const QString& accessibleName, QWidget* parent)
{
    auto* c = dropdown(parent);
    c->addItems(items);
    c->setAccessibleName(accessibleName);
    c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    return c;
}

Segmented* viewToggle(QWidget* parent)
{
    auto* s = new Segmented(parent);
    s->addSegment(QStringLiteral("Grid"), QStringLiteral("grid"), QStringLiteral("grid"));
    s->addSegment(QStringLiteral("List"), QStringLiteral("list"), QStringLiteral("list"));
    s->setAccessibleName(QStringLiteral("View"));
    s->setCurrentIndex(0);
    return s;
}

// The bar that appears above a collection while items are selected.
Card* selectionBar(QLabel** text, QWidget* parent)
{
    auto* bar = new Card(parent, Card::Variant::Inset);
    bar->body()->setContentsMargins(4, 0, 4, 0);
    auto* h = new QHBoxLayout;
    h->setSpacing(space::sm);
    *text = label({}, TextStyle::BodyStrong, color::fg_0, bar);
    h->addWidget(*text);
    h->addSpacing(space::sm);
    bar->body()->addLayout(h);
    bar->hide();
    return bar;
}

QHBoxLayout* barLayout(Card* bar) { return static_cast<QHBoxLayout*>(bar->body()->itemAt(0)->layout()); }

QString versionText(const QString& v) { return v.isEmpty() ? QString() : QStringLiteral("v%1").arg(v); }

QString joinMeta(const QStringList& parts)
{
    QStringList out;
    for (const QString& p : parts)
        if (!p.isEmpty())
            out << p;
    return out.join(QStringLiteral(" · "));
}

QString plural(qint64 n, const QString& one, const QString& many) { return QStringLiteral("%1 %2").arg(formatCount(n), n == 1 ? one : many); }

std::vector<LibraryGame> demoGames()
{
    struct D {
        const char* title;
        const char* id;
        const char* version;
        qint64 bytes;
        const char* status;
    };
    const D d[] = {{"Astro Bot", "PPSA01325", "01.002.000", 48500000000LL, "Backported"},
                   {"Demon's Souls", "PPSA01341", "01.004.000", 31600000000LL, ""},
                   {"Ghost of Yōtei", "PPSA26344", "01.512.000", 74200000000LL, "Not backported"},
                   {"Returnal", "PPSA01284", "01.007.000", 58900000000LL, ""},
                   {"Ratchet & Clank: Rift Apart", "PPSA01474", "01.003.000", 39800000000LL, ""},
                   {"Gran Turismo 7", "PPSA01316", "01.049.000", 98100000000LL, "Not backported"},
                   {"Stellar Blade", "PPSA13198", "01.012.000", 41300000000LL, ""},
                   {"Sackboy: A Big Adventure", "PPSA01383", "01.012.000", 27600000000LL, ""},
                   {"Kena: Bridge of Spirits", "PPSA01535", "01.016.000", 21900000000LL, ""},
                   {"Returnal", "PPSA01284", "01.000.000", 55100000000LL, ""}};
    std::vector<LibraryGame> out;
    for (const D& x : d) {
        LibraryGame g;
        g.info.title = QString::fromUtf8(x.title);
        g.info.titleId = QString::fromLatin1(x.id);
        g.info.version = QString::fromLatin1(x.version);
        g.info.bytes = x.bytes;
        g.info.folder = QStringLiteral("D:/Games/%1 %2 (%3)").arg(g.info.titleId, g.info.title, g.info.version);
        g.info.isGame = true;
        g.info.exists = true;
        g.info.platform = QStringLiteral("PS5");
        g.info.kind = QStringLiteral("Game");
        g.status = QString::fromLatin1(x.status);
        out.push_back(g);
    }
    return out;
}

QList<ImageSummary> demoImages()
{
    struct D {
        const char* title;
        const char* id;
        const char* version;
        const char* format;
        qint64 bytes;
    };
    const D d[] = {{"Astro Bot", "PPSA01325", "01.002.000", "exfat", 48700000000LL},
                   {"Astro Bot", "PPSA01325", "01.002.000", "ffpfsc", 39200000000LL},
                   {"Demon's Souls", "PPSA01341", "01.004.000", "ffpkg", 31800000000LL},
                   {"Gran Turismo 7", "PPSA01316", "01.049.000", "ffpfs", 98400000000LL},
                   {"Stellar Blade", "PPSA13198", "01.012.000", "exfat", 41500000000LL}};
    QList<ImageSummary> out;
    for (const D& x : d) {
        ImageSummary s;
        s.title = QString::fromUtf8(x.title);
        s.titleId = QString::fromLatin1(x.id);
        s.version = QString::fromLatin1(x.version);
        s.format = QString::fromLatin1(x.format);
        s.bytes = x.bytes;
        s.exists = true;
        s.fromImage = true;
        s.path = QStringLiteral("D:/Images/%1 %2 (%3).%4").arg(s.titleId, s.title, s.version, s.format);
        out << s;
    }
    return out;
}

} // namespace

std::vector<LibraryGame> mergeScanResult(const std::vector<LibraryGame>& known, std::vector<LibraryGame> scanned,
                                         const QStringList& scanFolders, const QDateTime& now,
                                         const std::function<bool(const QString&)>& stillThere)
{
    const auto key = [](const QString& folder) { return QDir::cleanPath(QDir::fromNativeSeparators(folder)).toLower(); };
    QHash<QString, const LibraryGame*> before;
    for (const LibraryGame& g : known)
        before.insert(key(g.info.folder), &g);
    QSet<QString> listed;
    for (LibraryGame& g : scanned) {
        const QString k = key(g.info.folder);
        listed.insert(k);
        const LibraryGame* old = before.value(k);
        g.lastSeen = old && old->lastSeen.isValid() ? old->lastSeen : now;
        if (old) {
            g.status = old->status;
            g.issues = old->issues;
            g.canRun = old->canRun;
            g.plan = old->plan;
        }
    }
    for (const LibraryGame& g : known) {
        const QString k = key(g.info.folder);
        if (listed.contains(k))
            continue;
        bool underScanFolder = false, scanFolderReachable = false;
        for (const QString& f : scanFolders) {
            const QString fk = key(f);
            if (k != fk && !k.startsWith(fk.endsWith(u'/') ? fk : fk + u'/'))
                continue;
            underScanFolder = true;
            scanFolderReachable = scanFolderReachable || QFileInfo(f).isDir();
        }
        if (underScanFolder ? !scanFolderReachable : (!stillThere || stillThere(g.info.folder)))
            scanned.push_back(g);
    }
    return scanned;
}

QString imageRenameTarget(const QString& typed, const QString& sourcePath)
{
    QString n = sanitizeFileName(typed.trimmed());
    const QString ext = QFileInfo(sourcePath).suffix();
    if (!ext.isEmpty() && !n.endsWith(u'.' + ext, Qt::CaseInsensitive))
        n += u'.' + ext;
    return n;
}

LibraryPage::LibraryPage(AppContext& ctx, JobQueueModel& queueModel, LibraryStore& library, QWidget* parent)
    : Page(ctx, QStringLiteral("library"), QStringLiteral("Library"),
           QStringLiteral("Your game dumps and images in one place."), parent),
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
    m_scanner = new ImageScanner(this);
    connect(m_scanner, &ImageScanner::finished, this, [this](const QList<ImageSummary>& images) {
        if (m_demo)
            return;
        m_images = images;
        rebuildImages();
        rebuildGames();
    });

    auto* add = button(QStringLiteral("Add folder…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), this);
    add->setToolTip(QStringLiteral("Add a folder that holds game dumps (or one dump)"));
    connect(add, &QPushButton::clicked, this, &LibraryPage::addFolders);
    auto* rescan = button(QStringLiteral("Rescan"), ButtonVariant::Secondary, QStringLiteral("refresh"), this);
    rescan->setToolTip(QStringLiteral("Look for games and images in the library folders again (F5)"));
    connect(rescan, &QPushButton::clicked, this, &LibraryPage::refresh);
    auto* more = new IconButton(QStringLiteral("more"), QStringLiteral("More"), IconButton::Size::Regular, this);
    more->setToolTip(QStringLiteral("More"));
    connect(more, &QToolButton::clicked, this, [this, more] {
        QMenu menu(this);
        auto* r = menu.addAction(QStringLiteral("Rename dump folders…"), this, &LibraryPage::renameDumps);
        r->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("library.rename")));
        menu.addAction(QStringLiteral("Rename images…"), this, [this] {
            QStringList all;
            for (const auto& i : m_images)
                all << i.path;
            if (!all.isEmpty())
                renameImagesSheet(all);
        })->setEnabled(!m_images.isEmpty());
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    header()->addAction(more);
    header()->addAction(rescan);
    header()->addAction(add);

    m_tabs = new QTabBar(this);
    m_tabs->addTab(QStringLiteral("Games"));
    m_tabs->addTab(QStringLiteral("Images"));
    m_tabs->addTab(QStringLiteral("Languages"));
    m_tabs->addTab(QStringLiteral("DLC emulator"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setProperty("fwNoRing", true);
    content()->addWidget(m_tabs);
    m_stack = new QStackedWidget(this);
    m_stack->setProperty("fwNoRing", true);
    content()->addWidget(m_stack);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        m_stack->setCurrentIndex(i);
        emit statusContextChanged();
    });

    m_stack->addWidget(buildGamesTab());
    m_stack->addWidget(buildImagesTab());
    m_stack->addWidget(buildLanguagesTab());
    m_dlc = new DlcEmuEditor(m_stack);
    connect(m_dlc, &DlcEmuEditor::toastRequested, this, &LibraryPage::toastRequested);
    m_stack->addWidget(m_dlc);

    connect(&m_library, &LibraryStore::changed, this, [this] {
        rebuildGames();
        if (imageFolders().join(u'|') != m_imageFoldersKey)
            scanImages();
    });
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, &LibraryPage::onJobFinished);
    rebuildGames();
    rebuildImages();
    rebuildLanguages();
    scanImages();
}

// ---- Games -------------------------------------------------------------------------

QWidget* LibraryPage::buildGamesTab()
{
    auto* page = new QWidget(m_stack);
    auto* l = vbox(page, space::md);

    auto* tools = new QWidget(page);
    auto* th = hbox(tools, space::sm);
    m_search = searchInput(QStringLiteral("Search games"), tools);
    th->addWidget(m_search, 3);
    m_filter = combo({QStringLiteral("All games"), QStringLiteral("With an image"), QStringLiteral("Without an image")}, QStringLiteral("Show"), tools);
    th->addWidget(m_filter);
    m_sort = combo({QStringLiteral("Title A–Z"), QStringLiteral("Title ID"), QStringLiteral("Largest first"), QStringLiteral("Recently added")}, QStringLiteral("Sort by"), tools);
    th->addWidget(m_sort);
    th->addStretch(1);
    m_gamesCount = caption({}, tools);
    m_gamesCount->setWordWrap(false);
    th->addWidget(m_gamesCount);
    th->addSpacing(space::sm);
    m_gamesView = viewToggle(tools);
    th->addWidget(m_gamesView);
    l->addWidget(tools);

    m_folders = new QWidget(page);
    new FlowLayout(m_folders, space::xs, space::xs);
    l->addWidget(m_folders);

    m_gamesSelection = selectionBar(&m_gamesSelectionText, page);
    {
        auto* h = barLayout(m_gamesSelection);
        auto* build = button(QStringLiteral("Build images…"), ButtonVariant::Secondary, QStringLiteral("build"), m_gamesSelection);
        connect(build, &QPushButton::clicked, this, &LibraryPage::buildSelected);
        h->addWidget(build);
        auto* bp = button(QStringLiteral("Backport…"), ButtonVariant::Secondary, QStringLiteral("backports"), m_gamesSelection);
        connect(bp, &QPushButton::clicked, this, [this] {
            // One request for the whole selection: one scan on the Backport page.
            const QStringList selected = m_gamesGrid->selectedKeys();
            if (!selected.isEmpty())
                emit navigateRequested({QStringLiteral("backport"), {{QStringLiteral("folders"), selected}}});
        });
        h->addWidget(bp);
        auto* remove = button(QStringLiteral("Remove from library"), ButtonVariant::Subtle, QStringLiteral("close"), m_gamesSelection);
        remove->setToolTip(QStringLiteral("Only the library entry goes; the folders stay on disk."));
        connect(remove, &QPushButton::clicked, this, [this] {
            if (!m_demo)
                removeGames(m_gamesGrid->selectedKeys());
        });
        h->addWidget(remove);
        h->addStretch(1);
        auto* clear = new Link(QStringLiteral("Clear selection"), m_gamesSelection);
        connect(clear, &Link::activated, this, [this] { m_gamesGrid->clearSelection(); });
        h->addWidget(clear);
    }
    l->addWidget(m_gamesSelection);

    m_gamesGrid = new LibraryGrid(page);
    m_gamesGrid->setAccessibleName(QStringLiteral("Games"));
    m_gamesGrid->setListHeaders({QStringLiteral("Title"), QStringLiteral("ID · version"), QStringLiteral("Size"), QStringLiteral("Formats"), QStringLiteral("Backport")});
    l->addWidget(m_gamesGrid);
    connect(m_gamesGrid, &LibraryGrid::selectionChanged, this, &LibraryPage::updateGameSelection);
    connect(m_gamesGrid, &LibraryGrid::contextRequested, this, &LibraryPage::gameMenu);
    connect(m_gamesGrid, &LibraryGrid::activated, this, [this](const QString& f) {
        emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), f}}});
    });

    m_gamesDrop = new DropZone(QStringLiteral("library"), QStringLiteral("Add a library folder"),
                               QStringLiteral("Drop the folder that holds your game dumps here, or click to browse"), page);
    m_gamesDrop->setToolTip(QStringLiteral("Every dump inside it is listed: a folder with sce_sys\\param.json or eboot.bin."));
    connect(m_gamesDrop, &DropZone::clicked, this, &LibraryPage::addFolders);
    l->addWidget(m_gamesDrop);
    m_gamesNoMatch = new EmptyState(QStringLiteral("search"), QStringLiteral("No games match"), QStringLiteral("Try another search or show all games."), page);
    connect(m_gamesNoMatch->addAction(QStringLiteral("Show all games")), &QPushButton::clicked, this, [this] {
        m_search->clear();
        m_filter->setCurrentIndex(0);
    });
    l->addWidget(m_gamesNoMatch);
    m_gamesNone = new EmptyState(QStringLiteral("library"), QStringLiteral("No games found yet"),
                                 QStringLiteral("Rescan the library folders, or add the folder that holds your dumps."), page);
    connect(m_gamesNone->addAction(QStringLiteral("Rescan"), ButtonVariant::Primary), &QPushButton::clicked, this, &LibraryPage::scan);
    connect(m_gamesNone->addAction(QStringLiteral("Add folder…")), &QPushButton::clicked, this, &LibraryPage::addFolders);
    l->addWidget(m_gamesNone);
    l->addStretch(1);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString&) { rebuildGames(); });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this](int) { rebuildGames(); });
    connect(m_sort, &QComboBox::currentIndexChanged, this, [this](int) { rebuildGames(); });
    connect(m_gamesView, &Segmented::currentChanged, this, [this](int) {
        m_gamesGrid->setMode(m_gamesView->currentValue().toString() == QLatin1String("list") ? LibraryGrid::Mode::List : LibraryGrid::Mode::Grid);
    });
    return page;
}

const std::vector<LibraryGame>& LibraryPage::games() const { return m_demo ? m_demoGames : m_library.games(); }

// Several entries at once: one save of the library and one rebuild of the grid.
void LibraryPage::removeGames(const QStringList& folders)
{
    std::vector<LibraryGame> keep;
    for (const LibraryGame& g : m_library.games())
        if (!folders.contains(g.info.folder))
            keep.push_back(g);
    if (keep.size() != m_library.games().size())
        m_library.setGames(std::move(keep));
}

const LibraryGame* LibraryPage::game(const QString& folder) const
{
    for (const auto& g : games())
        if (g.info.folder == folder)
            return &g;
    return nullptr;
}

QStringList LibraryPage::formatsOf(const GameSummary& g) const
{
    QStringList out;
    if (g.titleId.isEmpty())
        return out;
    for (const QString& f : {QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfsc"), QStringLiteral("ffpfs")})
        for (const ImageSummary& s : m_images)
            if (s.format == f && s.titleId.compare(g.titleId, Qt::CaseInsensitive) == 0) {
                out << imageFormatName(f);
                break;
            }
    return out;
}

void LibraryPage::rebuildGames()
{
    const QString q = m_search->text().trimmed();
    const int filter = m_filter->currentIndex();
    std::vector<const LibraryGame*> shown;
    qint64 total = 0;
    for (const auto& g : games()) {
        total += g.info.bytes;
        const GameSummary& s = g.info;
        if (!q.isEmpty() && !s.title.contains(q, Qt::CaseInsensitive) && !s.titleId.contains(q, Qt::CaseInsensitive)
            && !s.folder.contains(q, Qt::CaseInsensitive))
            continue;
        const bool hasImage = !formatsOf(s).isEmpty();
        if ((filter == 1 && !hasImage) || (filter == 2 && hasImage))
            continue;
        shown.push_back(&g);
    }
    const int sort = m_sort->currentIndex();
    const auto title = [](const LibraryGame* g) { return g->info.title.isEmpty() ? QFileInfo(g->info.folder).fileName() : g->info.title; };
    std::stable_sort(shown.begin(), shown.end(), [&](const LibraryGame* a, const LibraryGame* b) {
        switch (sort) {
        case 1: return a->info.titleId.compare(b->info.titleId, Qt::CaseInsensitive) < 0;
        case 2: return a->info.bytes > b->info.bytes;
        case 3: return a->lastSeen > b->lastSeen;
        default: return title(a).compare(title(b), Qt::CaseInsensitive) < 0;
        }
    });
    std::vector<GridItem> items;
    items.reserve(shown.size());
    for (const LibraryGame* g : shown) {
        const GameSummary& s = g->info;
        GridItem it;
        it.key = s.folder;
        it.title = title(g);
        it.subtitle = joinMeta({s.titleId, versionText(s.version)});
        it.size = s.bytes > 0 ? formatBytes(s.bytes) : QString();
        it.path = nativePath(s.folder);
        it.coverPath = s.coverPath;
        if (it.coverPath.isEmpty() && !m_demo && QFileInfo::exists(s.folder + QStringLiteral("/sce_sys/icon0.png")))
            it.coverPath = s.folder + QStringLiteral("/sce_sys/icon0.png");
        it.chips = QStringList{QStringLiteral("Folder")} + formatsOf(s);
        it.firstChipNeutral = true;
        it.badge = g->status;
        it.badgeTone = !g->issues.isEmpty() ? Tone::Warn : g->status == QLatin1String("Backported") ? Tone::Ok : Tone::Neutral;
        QStringList tip;
        if (!s.platform.isEmpty())
            tip << joinMeta({s.platform, s.kind, s.sdk.isEmpty() ? QString() : QStringLiteral("SDK %1").arg(s.sdk)});
        tip << QStringLiteral("Double-click to build an image · right-click for more");
        it.tooltip = tip.join(u'\n');
        items.push_back(it);
    }
    m_gamesGrid->setItems(std::move(items));

    const bool anyGames = !games().empty();
    const bool anyFolders = m_demo || !m_library.scanFolders().isEmpty();
    m_gamesGrid->setVisible(!shown.empty());
    m_gamesDrop->setVisible(!anyGames && !anyFolders);
    m_gamesNone->setVisible(!anyGames && anyFolders);
    m_gamesNoMatch->setVisible(anyGames && shown.empty());
    QString count = plural(qint64(games().size()), QStringLiteral("game"), QStringLiteral("games"));
    if (shown.size() != games().size())
        count = QStringLiteral("%1 of %2").arg(shown.size()).arg(count);
    if (total > 0)
        count += QStringLiteral(" · %1").arg(formatBytes(total));
    m_gamesCount->setText(anyGames ? count : QString());
    m_search->parentWidget()->setVisible(anyGames);
    m_gamesCount->setToolTip(m_library.lastScan().isValid() ? QStringLiteral("Last scan %1").arg(relativeTime(m_library.lastScan())) : QStringLiteral("Not scanned yet"));
    rebuildFolders();
    updateGameSelection();
    emit statusContextChanged();
}

void LibraryPage::rebuildFolders()
{
    auto* fl = static_cast<FlowLayout*>(m_folders->layout());
    while (QLayoutItem* it = fl->takeAt(0)) {
        // This runs from the click handlers of the widgets it replaces (the
        // remove button of a chip, the "Add folder…" link): they must outlive
        // the event that is still being delivered to them.
        if (QWidget* w = it->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    const QStringList folders = m_demo ? QStringList{QStringLiteral("D:/Games"), QStringLiteral("E:/PS5")} : m_library.scanFolders();
    m_folders->setVisible(!folders.isEmpty());
    if (folders.isEmpty())
        return;
    auto* title = label(QStringLiteral("Library folders"), TextStyle::Caption, color::fg_4, m_folders);
    fl->addWidget(title);
    QWidget* firstChip = nullptr;
    for (const QString& f : folders) {
        auto* chip = new QWidget(m_folders);
        chip->setObjectName(QStringLiteral("libraryFolderChip"));
        chip->setAttribute(Qt::WA_StyledBackground, true);
        auto* ch = hbox(chip, 4, QMargins(8, 2, 2, 2));
        auto* ic = new QLabel(chip);
        ic->setPixmap(iconPixmap(QStringLiteral("folder"), size::iconSm, color::fg_3, devicePixelRatioF()));
        ch->addWidget(ic);
        ch->addWidget(label(nativePath(f), TextStyle::MonoSmall, color::fg_2, chip));
        auto* x = new IconButton(QStringLiteral("close"), QStringLiteral("Remove %1 from the library").arg(nativePath(f)), IconButton::Size::Tiny, chip);
        x->setToolTip(QStringLiteral("Stop scanning this folder"));
        connect(x, &QToolButton::clicked, this, [this, f] {
            if (!m_demo)
                m_library.removeScanFolder(f);
        });
        ch->addWidget(x);
        chip->setStyleSheet(QStringLiteral("#libraryFolderChip { background: %1; border: 1px solid %2; border-radius: 12px; }").arg(hex(color::bg_2), hex(color::border_3)));
        fl->addWidget(chip);
        if (!firstChip)
            firstChip = chip;
    }
    auto* add = new Link(QStringLiteral("Add folder…"), m_folders);
    connect(add, &Link::activated, this, &LibraryPage::addFolders, Qt::QueuedConnection);
    fl->addWidget(add);
    // The flow layout aligns tops; the label and the link take the chips' height to line up.
    const int h = firstChip->sizeHint().height();
    for (QLabel* l : {static_cast<QLabel*>(title), static_cast<QLabel*>(add)}) {
        l->setFixedHeight(h);
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
}

void LibraryPage::updateGameSelection()
{
    const int n = int(m_gamesGrid->selectedKeys().size());
    m_gamesSelection->setVisible(n > 0);
    m_gamesSelectionText->setText(QStringLiteral("%1 selected").arg(n));
}

void LibraryPage::addFolders()
{
    const QString dir = QFileDialog::getExistingDirectory(window(), QStringLiteral("Add a folder of game dumps"));
    if (!dir.isEmpty())
        addFolder(dir);
}

void LibraryPage::addFolder(const QString& dir)
{
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
        scanImages();
        if (games().empty())
            emit toastRequested(Tone::Warn, QStringLiteral("Nothing to scan"), QStringLiteral("Add a folder of game dumps first."));
        return;
    }
    if (!m_scanJobId.isEmpty()) {
        const auto running = m_ctx.queue().record(m_scanJobId);
        if (running && !isTerminal(running->state))
            return; // that scan is still at work; its result is the newest there is
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
        emit toastRequested(Tone::Info, QStringLiteral("Scanned without the engine"), QStringLiteral("%1 game folder%2 found one level below the library folders.").arg(found).arg(found == 1 ? QString() : QStringLiteral("s")));
        scanImages();
        return;
    }
    m_scanJobId = r.value();
    m_ctx.queue().startNow(m_scanJobId); // this job only; a paused queue stays paused
    scanImages();
}

void LibraryPage::gameMenu(const QString& folder, const QPoint& pos)
{
    const QStringList selected = m_gamesGrid->selectedKeys();
    QMenu menu(this);
    if (selected.size() > 1) {
        menu.addAction(QStringLiteral("Build %1 images…").arg(selected.size()), this, &LibraryPage::buildSelected);
        menu.addAction(QStringLiteral("Backport %1 games…").arg(selected.size()), this, [this, selected] {
            emit navigateRequested({QStringLiteral("backport"), {{QStringLiteral("folders"), selected}}});
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Remove %1 from the library").arg(selected.size()), this, [this, selected] {
            if (!m_demo)
                removeGames(selected);
        });
        menu.exec(pos);
        return;
    }
    menu.addAction(QStringLiteral("Build image…"), this, [this, folder] { emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("source"), folder}}}); });
    menu.addAction(QStringLiteral("Build FPKG…"), this, [this, folder] { emit navigateRequested({QStringLiteral("fpkg"), {{QStringLiteral("source"), folder}}}); });
    menu.addAction(QStringLiteral("Pack with zstd…"), this, [this, folder] {
        emit navigateRequested({QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatZstdFolder}, {QStringLiteral("source"), folder}}});
    });
    menu.addAction(QStringLiteral("Backport…"), this, [this, folder] { emit navigateRequested({QStringLiteral("backport"), {{QStringLiteral("folder"), folder}}}); });
    if (const LibraryGame* g = game(folder); g && !g->info.titleId.isEmpty()) {
        QMenu* images = nullptr;
        for (const ImageSummary& s : m_images)
            if (s.titleId.compare(g->info.titleId, Qt::CaseInsensitive) == 0) {
                if (!images)
                    images = menu.addMenu(QStringLiteral("Its images"));
                const QString path = s.path;
                images->addAction(QStringLiteral("%1 · %2").arg(QFileInfo(path).fileName(), formatBytes(s.bytes)), this,
                                  [this, path] { openInImageTools(path, QString()); });
            }
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Trim languages…"), this, [this, folder] {
        setTab(LanguagesTab);
        m_langFolder->setPath(folder);
    });
    menu.addAction(QStringLiteral("Edit DLC list…"), this, [this, folder] {
        setTab(DlcTab);
        m_dlc->openFolder(folder);
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Rename folder…"), this, [this, folder] { renameGame(folder); });
    menu.addAction(QStringLiteral("Show in folder"), this, [folder] { openInFileManager(folder); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Remove from library"), this, [this, folder] {
        if (!m_demo)
            m_library.removeGame(folder);
    });
    menu.exec(pos);
}

void LibraryPage::buildSelected()
{
    const QStringList folders = m_gamesGrid->selectedKeys();
    if (folders.isEmpty())
        return;
    auto* s = new Sheet(window(), folders.size() == 1 ? QStringLiteral("Build an image") : QStringLiteral("Build %1 images").arg(folders.size()), Sheet::Icon::None, size::dialogWide);
    s->addText(QStringLiteral("One job per game, named after its title ID, name and version."));
    auto* fmt = new Segmented(s);
    fmt->addSegment(QStringLiteral("exFAT"), QStringLiteral("drive"), jobs::kFormatExfat);
    fmt->addSegment(QStringLiteral("FFPKG"), QStringLiteral("package"), jobs::kFormatFfpkg);
    fmt->addSegment(QStringLiteral("FFPFSC"), QStringLiteral("compress"), jobs::kFormatFfpfsc);
    fmt->addSegment(QStringLiteral("Packed folder"), QStringLiteral("archive"), pages::kFormatZstdFolder);
    fmt->setToolTip(QStringLiteral("Packed folder: no image, the game folder with its files in zstd packs (Build image › Packed folder)."));
    fmt->setCurrentIndex(0);
    s->body()->addWidget(field(QStringLiteral("Format"), fmt, {}, s));
    auto* out = new PathField(PathField::Mode::Directory, s);
    out->setDialogTitle(QStringLiteral("Choose the output folder"));
    const QString def = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    out->setPlaceholder(def.isEmpty() ? QStringLiteral("Next to each dump") : nativePath(def));
    s->body()->addWidget(field(QStringLiteral("Save to"), out, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(folders.size() == 1 ? QStringLiteral("Build") : QStringLiteral("Build %1").arg(folders.size()), Sheet::Role::Primary);
    if (s->exec() != 1 || m_demo)
        return;
    const QString format = fmt->currentValue().toString();
    int queued = 0;
    QStringList failed;
    QStringList started;
    for (const QString& f : folders) {
        const LibraryGame* g = game(f);
        const GameSummary info = g ? g->info : readGameSummary(f, false);
        QString dir = out->path().isEmpty() ? def : out->path();
        if (dir.isEmpty())
            dir = QFileInfo(f).absolutePath();
        const QJsonObject extra{{QStringLiteral("title"), info.title}, {QStringLiteral("titleId"), info.titleId},
                                {QStringLiteral("version"), info.version}, {QStringLiteral("sourceBytes"), info.bytes}};
        // The same name the Build page proposes (Settings › Build › Output name).
        const auto& st = m_ctx.settings();
        const auto preset = gameinfo::imageNamePresetFromKey(st.value(gameinfo::keys::kNamePreset).toString())
                                .value_or(gameinfo::ImageNamePreset::TitleIdTitleVersion);
        gameinfo::ImageNameFields nf;
        nf.titleId = info.titleId;
        nf.title = info.title;
        nf.version = info.version;
        const QString fwOverride = st.value(gameinfo::keys::kFirmwareOverride).toString();
        nf.firmware = fwOverride.isEmpty() || fwOverride == QLatin1String("auto") ? gameinfo::firmwareMajor(info.requiredFw) : fwOverride;
        nf.sdk = info.sdk;
        nf.type = format == jobs::kFormatExfat ? QStringLiteral("exFAT") : format == jobs::kFormatFfpkg ? QStringLiteral("ffpkg")
                : format == pages::kFormatZstdFolder ? QStringLiteral("zstd") : QStringLiteral("PFS");
        nf.folderName = QFileInfo(f).fileName();
        QString name = gameinfo::renderImageName(preset, st.value(gameinfo::keys::kNameTemplate).toString(), nf);
        Result<QString> r = Error(ErrorCode::Internal, QStringLiteral("Nothing to build."));
        if (format == pages::kFormatZstdFolder) {
            // A packed folder next to the dump under the dump's own name would be the dump itself (as on the Build page).
            if (QString::compare(QDir::cleanPath(QDir(dir).filePath(name)), QDir::cleanPath(QFileInfo(f).absoluteFilePath()), Qt::CaseInsensitive) == 0)
                name += QStringLiteral(" (zstd)");
            r = jobs::submitPackFolder(m_ctx, f, QDir(dir).filePath(name), QVariantMap{}, extra);
        } else {
            r = jobs::submitImageBuild(m_ctx, format, f, dir, name + jobs::outputExtension(format), QVariantMap{}, extra);
        }
        if (r.ok()) {
            ++queued;
            started << r.value();
        } else
            failed << QStringLiteral("%1: %2").arg(QFileInfo(f).fileName(), r.error().message);
    }
    // These jobs run now, in this order, ahead of other waiting jobs; a paused queue stays paused for the rest.
    for (auto it = started.crbegin(); it != started.crend(); ++it)
        m_ctx.queue().startNow(*it);
    if (!failed.isEmpty())
        emit toastRequested(Tone::Err, QStringLiteral("%1 not queued").arg(plural(failed.size(), QStringLiteral("build"), QStringLiteral("builds"))), failed.join(u'\n'));
    if (queued > 0)
        emit toastRequested(Tone::Info, QStringLiteral("%1 queued").arg(plural(queued, QStringLiteral("build"), QStringLiteral("builds"))),
                            QStringLiteral("Follow them in the Queue."));
}

void LibraryPage::renameGame(const QString& folder)
{
    const LibraryGame* g = game(folder);
    if (!g || m_demo)
        return;
    const GameSummary info = g->info;
    auto* s = new Sheet(window(), QStringLiteral("Rename the dump folder"), Sheet::Icon::None, size::dialogWide);
    s->addText(QStringLiteral("Current name: %1").arg(Sheet::monoSpan(QFileInfo(folder).fileName())));
    auto* pattern = new Segmented(s);
    pattern->addSegment(QStringLiteral("PPSA only"), {}, QStringLiteral("id"));
    pattern->addSegment(QStringLiteral("PPSA Title"), {}, QStringLiteral("id-title"));
    pattern->addSegment(QStringLiteral("PPSA Title (version)"), {}, QStringLiteral("id-title-version"));
    pattern->setCurrentIndex(2);
    s->body()->addWidget(field(QStringLiteral("Pattern"), pattern, {}, s));
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
    s->body()->addWidget(field(QStringLiteral("New name"), name, QStringLiteral("Only the folder name changes."), s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Rename"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    const QString newName = sanitizeFileName(name->text().trimmed());
    if (newName.isEmpty() || newName == QFileInfo(folder).fileName())
        return;
    const QString target = QFileInfo(folder).absolutePath() + u'/' + newName;
    // On Windows the folder itself "exists" under a name that differs in case only.
    const bool caseOnly = QString::compare(QDir::cleanPath(target), QDir::cleanPath(folder), Qt::CaseInsensitive) == 0;
    if (!caseOnly && QFileInfo::exists(target)) {
        Sheet::message(window(), QStringLiteral("A folder with that name exists"), QStringLiteral("%1 is already there. Choose another name.").arg(Sheet::monoSpan(nativePath(target))), Sheet::Icon::Error);
        return;
    }
    if (!QDir().rename(folder, target)) {
        Sheet::message(window(), QStringLiteral("The folder could not be renamed"), QStringLiteral("Windows refused the rename. Close programs that use the folder and try again."), Sheet::Icon::Error);
        return;
    }
    m_library.renameGame(folder, target);
    emit toastRequested(Tone::Ok, QStringLiteral("Folder renamed"), newName);
}

// ---- Dump rename ----------------------------------------------------------------------

void LibraryPage::renameDumps()
{
    auto* s = new Sheet(window(), QStringLiteral("Rename dump folders"), Sheet::Icon::None, size::dialogWide);
    s->addText(QStringLiteral("Each dump folder inside the source gets a name from its title ID, name and version. You confirm a preview first; an undo log is kept."));
    auto* source = new PathField(PathField::Mode::Directory, s);
    source->setDialogTitle(QStringLiteral("Choose the folder that holds the dumps"));
    const QStringList folders = m_library.scanFolders();
    if (!folders.isEmpty())
        source->setPath(folders.first());
    s->body()->addWidget(field(QStringLiteral("Folder of dumps"), source, {}, s));
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
    s->body()->addWidget(field(QStringLiteral("Pattern"), preset, {}, s));
    auto* dest = new PathField(PathField::Mode::Directory, s);
    dest->setPlaceholder(QStringLiteral("Rename in place"));
    dest->setToolTip(QStringLiteral("Must be on the same drive; folders are renamed, never copied."));
    s->body()->addWidget(field(QStringLiteral("Move into (optional)"), dest, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Undo last rename…"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Preview"), Sheet::Role::Primary);
    const int r = s->exec();
    if (r == 1) {
        const QString log = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the rename undo log"), gameinfo::defaultRenameUndoDir(), QStringLiteral("Undo logs (*.json)"));
        if (log.isEmpty())
            return;
        auto res = jobs::submitLibraryRename(m_ctx, QStringLiteral("undo"), {}, {}, {}, {}, log);
        if (!res.ok())
            emit toastRequested(Tone::Err, QStringLiteral("Undo not started"), res.error().message);
        else {
            m_renameUndoId = res.value();
            m_ctx.queue().startNow(m_renameUndoId); // this job only; a paused queue stays paused
        }
        return;
    }
    if (r != 2 || source->path().isEmpty())
        return;
    m_renameSource = source->path();
    m_renamePreset = preset->currentValue().toString();
    m_renameDest = dest->path();
    auto res = jobs::submitLibraryRename(m_ctx, QStringLiteral("preview"), m_renameSource, m_renamePreset, m_renameDest, {}, {});
    if (!res.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Preview not started"), res.error().message);
        return;
    }
    m_renamePreviewId = res.value();
    m_ctx.queue().startNow(m_renamePreviewId); // this job only; a paused queue stays paused
    emit toastRequested(Tone::Info, QStringLiteral("Previewing names…"), nativePath(m_renameSource));
}

void LibraryPage::showRenamePreview(const QJsonArray& items, const QString& preset)
{
    if (items.isEmpty()) {
        Sheet::message(window(), QStringLiteral("No dumps found"), QStringLiteral("%1 has no sub-folders that look like dumps.").arg(Sheet::monoSpan(nativePath(m_renameSource))), Sheet::Icon::Info);
        return;
    }
    auto* s = new Sheet(window(), QStringLiteral("Preview: %1").arg(plural(items.size(), QStringLiteral("dump"), QStringLiteral("dumps"))), Sheet::Icon::None, 900);
    s->addText(QStringLiteral("Pattern %1. You can edit a proposed name; names that exist already are skipped.").arg(Sheet::monoSpan(preset)));
    auto* table = new QTableWidget(0, 4, s);
    table->setAccessibleName(QStringLiteral("Folders to rename"));
    table->setHorizontalHeaderLabels({QStringLiteral("Rename"), QStringLiteral("Current name"), QStringLiteral("Proposed name"), QStringLiteral("Status")});
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
        st->setForeground(ready ? color::success_hi : status == QLatin1String("Already named") || status == QLatin1String("AlreadyNamed") ? color::fg_3 : color::warn_hi);
        st->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        st->setToolTip(QStringLiteral("%1 · %2 · %3").arg(it.value(QStringLiteral("titleId")).toString(), it.value(QStringLiteral("title")).toString(), it.value(QStringLiteral("version")).toString()));
        table->setItem(r, 3, st);
    }
    s->body()->addWidget(table);
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Rename"), Sheet::Role::Primary);
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
        emit toastRequested(Tone::Err, QStringLiteral("Rename not started"), res.error().message);
        return;
    }
    m_renameApplyId = res.value();
    m_ctx.queue().startNow(m_renameApplyId); // this job only; a paused queue stays paused
}

// ---- Images ------------------------------------------------------------------------

QWidget* LibraryPage::buildImagesTab()
{
    auto* page = new QWidget(m_stack);
    auto* l = vbox(page, space::md);
    auto* tools = new QWidget(page);
    auto* th = hbox(tools, space::sm);
    m_imageSearch = searchInput(QStringLiteral("Search images"), tools);
    th->addWidget(m_imageSearch, 3);
    m_imageFormat = combo({QStringLiteral("All formats"), QStringLiteral("exFAT"), QStringLiteral("FFPKG"), QStringLiteral("FFPFSC"), QStringLiteral("FFPFS")}, QStringLiteral("Format"), tools);
    th->addWidget(m_imageFormat);
    m_imageSort = combo({QStringLiteral("Title A–Z"), QStringLiteral("Title ID"), QStringLiteral("Largest first"), QStringLiteral("Newest first")}, QStringLiteral("Sort by"), tools);
    th->addWidget(m_imageSort);
    th->addStretch(1);
    m_imagesCount = caption({}, tools);
    m_imagesCount->setWordWrap(false);
    th->addWidget(m_imagesCount);
    th->addSpacing(space::sm);
    m_imagesView = viewToggle(tools);
    th->addWidget(m_imagesView);
    l->addWidget(tools);

    m_imagesSelection = selectionBar(&m_imagesSelectionText, page);
    {
        auto* h = barLayout(m_imagesSelection);
        m_imagesOpen = button(QStringLiteral("Open in Image tools"), ButtonVariant::Secondary, QStringLiteral("extract"), m_imagesSelection);
        connect(m_imagesOpen, &QPushButton::clicked, this, [this] {
            const QStringList sel = m_imagesGrid->selectedKeys();
            if (sel.size() == 1)
                openInImageTools(sel.first(), QString());
        });
        h->addWidget(m_imagesOpen);
        auto* verify = button(QStringLiteral("Verify"), ButtonVariant::Secondary, QStringLiteral("shield_check"), m_imagesSelection);
        verify->setToolTip(QStringLiteral("Queue a verify job for each selected image"));
        connect(verify, &QPushButton::clicked, this, [this] { verifyImages(m_imagesGrid->selectedKeys()); });
        h->addWidget(verify);
        auto* rename = button(QStringLiteral("Rename…"), ButtonVariant::Secondary, QStringLiteral("rename"), m_imagesSelection);
        connect(rename, &QPushButton::clicked, this, [this] { renameImagesSheet(m_imagesGrid->selectedKeys()); });
        h->addWidget(rename);
        h->addStretch(1);
        auto* clear = new Link(QStringLiteral("Clear selection"), m_imagesSelection);
        connect(clear, &Link::activated, this, [this] { m_imagesGrid->clearSelection(); });
        h->addWidget(clear);
    }
    l->addWidget(m_imagesSelection);

    m_imagesGrid = new LibraryGrid(page);
    m_imagesGrid->setAccessibleName(QStringLiteral("Images"));
    m_imagesGrid->setListHeaders({QStringLiteral("Title"), QStringLiteral("ID · version"), QStringLiteral("Size"), QStringLiteral("Format"), QStringLiteral("Note")});
    l->addWidget(m_imagesGrid);
    connect(m_imagesGrid, &LibraryGrid::selectionChanged, this, &LibraryPage::updateImageSelection);
    connect(m_imagesGrid, &LibraryGrid::contextRequested, this, &LibraryPage::imageMenu);
    connect(m_imagesGrid, &LibraryGrid::activated, this, [this](const QString& p) { openInImageTools(p, QString()); });

    m_imagesEmpty = new EmptyState(QStringLiteral("drive"), QStringLiteral("No images yet"),
                                   QStringLiteral("Images in your library and output folders show up here."), page);
    connect(m_imagesEmpty->addAction(QStringLiteral("Build an image"), ButtonVariant::Primary), &QPushButton::clicked, this,
            [this] { emit navigateRequested({QStringLiteral("build"), {}}); });
    connect(m_imagesEmpty->addAction(QStringLiteral("Open an image…")), &QPushButton::clicked, this,
            [this] { emit navigateRequested({QStringLiteral("imagetools"), {}}); });
    l->addWidget(m_imagesEmpty);
    m_imagesNoMatch = new EmptyState(QStringLiteral("search"), QStringLiteral("No images match"), QStringLiteral("Try another search or show all formats."), page);
    connect(m_imagesNoMatch->addAction(QStringLiteral("Show all images")), &QPushButton::clicked, this, [this] {
        m_imageSearch->clear();
        m_imageFormat->setCurrentIndex(0);
    });
    l->addWidget(m_imagesNoMatch);
    l->addStretch(1);

    connect(m_imageSearch, &QLineEdit::textChanged, this, [this](const QString&) { rebuildImages(); });
    connect(m_imageFormat, &QComboBox::currentIndexChanged, this, [this](int) { rebuildImages(); });
    connect(m_imageSort, &QComboBox::currentIndexChanged, this, [this](int) { rebuildImages(); });
    connect(m_imagesView, &Segmented::currentChanged, this, [this](int) {
        m_imagesGrid->setMode(m_imagesView->currentValue().toString() == QLatin1String("list") ? LibraryGrid::Mode::List : LibraryGrid::Mode::Grid);
    });
    return page;
}

QStringList LibraryPage::imageFolders() const
{
    QStringList out = m_library.scanFolders();
    const QString def = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (!def.isEmpty())
        out << def;
    // Images are built next to the dump by default.
    for (const auto& g : m_library.games())
        out << QFileInfo(g.info.folder).absolutePath();
    QStringList unique;
    for (const QString& f : out) {
        const QString c = QDir::cleanPath(f);
        if (!c.isEmpty() && !unique.contains(c, Qt::CaseInsensitive))
            unique << c;
    }
    return unique;
}

void LibraryPage::scanImages()
{
    if (m_demo)
        return;
    const QStringList folders = imageFolders();
    m_imageFoldersKey = folders.join(u'|');
    if (folders.isEmpty()) {
        m_images.clear();
        rebuildImages();
        return;
    }
    m_imagesCount->setText(QStringLiteral("Looking for images…"));
    m_scanner->scan(folders);
}

bool LibraryPage::isScanningImages() const { return m_scanner->isBusy(); }

const ImageSummary* LibraryPage::image(const QString& path) const
{
    for (const ImageSummary& s : m_images)
        if (s.path == path)
            return &s;
    return nullptr;
}

void LibraryPage::rebuildImages()
{
    const QString q = m_imageSearch->text().trimmed();
    static const QStringList formats = {QString(), QStringLiteral("exfat"), QStringLiteral("ffpkg"), QStringLiteral("ffpfsc"), QStringLiteral("ffpfs")};
    const QString fmt = formats.value(m_imageFormat->currentIndex());
    std::vector<const ImageSummary*> shown;
    qint64 total = 0;
    for (const ImageSummary& s : m_images) {
        total += s.bytes;
        if (!fmt.isEmpty() && s.format != fmt)
            continue;
        if (!q.isEmpty() && !s.title.contains(q, Qt::CaseInsensitive) && !s.titleId.contains(q, Qt::CaseInsensitive)
            && !QFileInfo(s.path).fileName().contains(q, Qt::CaseInsensitive))
            continue;
        shown.push_back(&s);
    }
    const int sort = m_imageSort->currentIndex();
    std::stable_sort(shown.begin(), shown.end(), [sort](const ImageSummary* a, const ImageSummary* b) {
        switch (sort) {
        case 1: return a->titleId.compare(b->titleId, Qt::CaseInsensitive) < 0;
        case 2: return a->bytes > b->bytes;
        case 3: return a->modified > b->modified;
        default: return a->displayTitle().compare(b->displayTitle(), Qt::CaseInsensitive) < 0;
        }
    });
    std::vector<GridItem> items;
    for (const ImageSummary* s : shown) {
        GridItem it;
        it.key = s->path;
        it.title = s->displayTitle();
        it.subtitle = joinMeta({s->titleId, versionText(s->version)});
        if (it.subtitle.isEmpty())
            it.subtitle = QFileInfo(s->path).fileName();
        it.size = formatBytes(s->bytes);
        it.path = nativePath(s->path);
        it.coverPath = s->coverPath;
        it.icon = imageFormatIcon(s->format);
        it.chips = {s->formatName()};
        if (!s->inner.isEmpty())
            it.chips << imageFormatName(imageFormatOf(s->inner));
        if (!s->error.isEmpty()) {
            it.badge = QStringLiteral("Not readable");
            it.badgeTone = Tone::Warn;
            it.tooltip = s->error;
        } else if (!s->fromImage) {
            it.badge = QStringLiteral("Name only");
            it.tooltip = QStringLiteral("No param.json inside; the title ID comes from the file name.");
        }
        items.push_back(it);
    }
    m_imagesGrid->setItems(std::move(items));
    const bool any = !m_images.isEmpty();
    m_imagesGrid->setVisible(!shown.empty());
    m_imagesEmpty->setVisible(!any && (!isScanningImages() || m_demo));
    m_imagesNoMatch->setVisible(any && shown.empty());
    m_imageSearch->parentWidget()->setVisible(any || (isScanningImages() && !m_demo));
    if (!isScanningImages() || m_demo) {
        QString count = plural(m_images.size(), QStringLiteral("image"), QStringLiteral("images"));
        if (int(shown.size()) != m_images.size())
            count = QStringLiteral("%1 of %2").arg(shown.size()).arg(count);
        if (total > 0)
            count += QStringLiteral(" · %1").arg(formatBytes(total));
        m_imagesCount->setText(any ? count : QString());
    }
    m_imagesCount->setToolTip(QStringLiteral("Looked in: %1").arg(m_demo ? QStringLiteral("D:\\Images") : nativePath(imageFolders().join(QStringLiteral(", ")))));
    updateImageSelection();
    emit statusContextChanged();
}

void LibraryPage::updateImageSelection()
{
    const int n = int(m_imagesGrid->selectedKeys().size());
    m_imagesSelection->setVisible(n > 0);
    m_imagesSelectionText->setText(QStringLiteral("%1 selected").arg(n));
    m_imagesOpen->setVisible(n == 1);
}

void LibraryPage::openInImageTools(const QString& image, const QString& action)
{
    QVariantMap args{{QStringLiteral("image"), image}};
    if (!action.isEmpty())
        args.insert(QStringLiteral("action"), action);
    emit navigateRequested({QStringLiteral("imagetools"), args});
}

void LibraryPage::verifyImages(const QStringList& paths)
{
    if (m_demo)
        return;
    int queued = 0;
    QStringList started, failed;
    for (const QString& p : paths) {
        if (auto r = jobs::submitVerify(m_ctx, p, {}); r.ok()) {
            ++queued;
            started << r.value();
        } else {
            failed << QStringLiteral("%1: %2").arg(QFileInfo(p).fileName(), r.error().message);
        }
    }
    for (auto it = started.crbegin(); it != started.crend(); ++it)
        m_ctx.queue().startNow(*it); // in this order; a paused queue stays paused for the rest
    if (queued > 0)
        emit toastRequested(Tone::Info, QStringLiteral("%1 queued").arg(plural(queued, QStringLiteral("verify job"), QStringLiteral("verify jobs"))), QString());
    if (!failed.isEmpty())
        emit toastRequested(queued > 0 ? Tone::Warn : Tone::Err,
                            queued > 0 ? QStringLiteral("%1 not verified").arg(plural(failed.size(), QStringLiteral("image"), QStringLiteral("images")))
                                       : QStringLiteral("Nothing was verified"),
                            failed.mid(0, 4).join(u'\n') + (failed.size() > 4 ? QStringLiteral("\n… and %1 more").arg(failed.size() - 4) : QString()));
}

void LibraryPage::imageMenu(const QString& path, const QPoint& pos)
{
    const QStringList selected = m_imagesGrid->selectedKeys();
    QMenu menu(this);
    if (selected.size() > 1) {
        menu.addAction(QStringLiteral("Verify %1 images").arg(selected.size()), this, [this, selected] { verifyImages(selected); });
        menu.addAction(QStringLiteral("Rename %1 images…").arg(selected.size()), this, [this, selected] { renameImagesSheet(selected); });
        menu.exec(pos);
        return;
    }
    const QString f = imageFormatOf(path);
    menu.addAction(QStringLiteral("Extract…"), this, [this, path] { openInImageTools(path, QStringLiteral("extract")); });
    menu.addAction(QStringLiteral("Verify…"), this, [this, path] { openInImageTools(path, QStringLiteral("verify")); });
    menu.addAction(QStringLiteral("Convert…"), this, [this, path] { openInImageTools(path, QStringLiteral("convert")); });
    menu.addAction(QStringLiteral("Edit…"), this, [this, path] { openInImageTools(path, QStringLiteral("edit")); })
        ->setEnabled(f == u"exfat" || f == u"ffpkg");
    menu.addSeparator();
    menu.addAction(QStringLiteral("Rename…"), this, [this, path] { renameImagesSheet({path}); });
    menu.addAction(QStringLiteral("Show in folder"), this, [path] { openInFileManager(path); });
    menu.exec(pos);
}

QString LibraryPage::imageFileName(const ImageSummary& s, const QString& preset)
{
    const QFileInfo fi(s.path);
    QString n = s.titleId.isEmpty() ? fi.completeBaseName() : s.titleId;
    if (preset != QLatin1String("id") && !s.title.isEmpty() && !s.titleId.isEmpty())
        n += u' ' + s.title;
    if (preset == QLatin1String("id-title-version") && !s.version.isEmpty() && !s.titleId.isEmpty())
        n += QStringLiteral(" (%1)").arg(s.version);
    return sanitizeFileName(n.simplified()) + u'.' + fi.suffix();
}

int LibraryPage::renameImages(const QList<QPair<QString, QString>>& renames, QStringList* skipped)
{
    int done = 0;
    for (const auto& [from, name] : renames) {
        const QFileInfo fi(from);
        const QString to = fi.absolutePath() + u'/' + name;
        if (name.isEmpty() || name == fi.fileName())
            continue;
        if (QFileInfo::exists(to) && QDir::cleanPath(to).compare(QDir::cleanPath(from), Qt::CaseInsensitive) != 0) {
            if (skipped)
                *skipped << QStringLiteral("%1: %2 exists").arg(fi.fileName(), name);
            continue;
        }
        if (!QFile::rename(from, to)) {
            if (skipped)
                *skipped << QStringLiteral("%1: in use or read-only").arg(fi.fileName());
            continue;
        }
        ++done;
    }
    return done;
}

void LibraryPage::renameImagesSheet(const QStringList& paths)
{
    if (paths.isEmpty() || m_demo)
        return;
    QList<ImageSummary> list;
    for (const QString& p : paths)
        list << (image(p) ? *image(p) : readImageSummary(p, false));
    auto* s = new Sheet(window(), paths.size() == 1 ? QStringLiteral("Rename the image") : QStringLiteral("Rename %1 images").arg(paths.size()),
                        Sheet::Icon::None, paths.size() == 1 ? size::dialogWide : 900);
    auto* pattern = new Segmented(s);
    pattern->addSegment(QStringLiteral("PPSA only"), {}, QStringLiteral("id"));
    pattern->addSegment(QStringLiteral("PPSA Title"), {}, QStringLiteral("id-title"));
    pattern->addSegment(QStringLiteral("PPSA Title (version)"), {}, QStringLiteral("id-title-version"));
    pattern->setCurrentIndex(2);
    s->body()->addWidget(field(QStringLiteral("Pattern"), pattern, {}, s));
    QLineEdit* name = nullptr;
    QTableWidget* table = nullptr;
    if (list.size() == 1) {
        s->addText(QStringLiteral("Current name: %1").arg(Sheet::monoSpan(QFileInfo(list.first().path).fileName())));
        name = textInput({}, true, s);
        s->body()->addWidget(field(QStringLiteral("New name"), name, QStringLiteral("The extension stays; nothing inside the image changes."), s));
    } else {
        table = new QTableWidget(int(list.size()), 3, s);
        table->setAccessibleName(QStringLiteral("Images to rename"));
        table->setHorizontalHeaderLabels({QStringLiteral("Rename"), QStringLiteral("Current name"), QStringLiteral("New name")});
        table->horizontalHeader()->setFixedHeight(32);
        table->horizontalHeader()->setHighlightSections(false);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->setColumnWidth(0, 70);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(size::row);
        table->setShowGrid(false);
        table->setMinimumHeight(std::min(12, int(list.size())) * size::row + 40);
        for (int r = 0; r < list.size(); ++r) {
            auto* sel = new QTableWidgetItem;
            sel->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            sel->setCheckState(list[r].titleId.isEmpty() ? Qt::Unchecked : Qt::Checked);
            sel->setData(Qt::UserRole, list[r].path);
            table->setItem(r, 0, sel);
            auto* cur = new QTableWidgetItem(QFileInfo(list[r].path).fileName());
            cur->setFont(font::mono());
            cur->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            table->setItem(r, 1, cur);
            auto* prop = new QTableWidgetItem;
            prop->setFont(font::mono());
            prop->setForeground(color::fg_0);
            table->setItem(r, 2, prop);
        }
        s->body()->addWidget(table);
    }
    const auto fill = [list, pattern, name, table] {
        const QString preset = pattern->currentValue().toString();
        if (name)
            name->setText(imageFileName(list.first(), preset));
        if (table)
            for (int r = 0; r < list.size(); ++r)
                table->item(r, 2)->setText(imageFileName(list[r], preset));
    };
    fill();
    connect(pattern, &Segmented::currentChanged, s, [fill](int) { fill(); });
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Rename"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    QList<QPair<QString, QString>> renames;
    if (name) {
        renames.append({list.first().path, imageRenameTarget(name->text(), list.first().path)});
    } else {
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 0)->checkState() == Qt::Checked) {
                const QString source = table->item(r, 0)->data(Qt::UserRole).toString();
                renames.append({source, imageRenameTarget(table->item(r, 2)->text(), source)});
            }
    }
    QStringList skipped;
    const int done = renameImages(renames, &skipped);
    if (done > 0)
        emit toastRequested(Tone::Ok, QStringLiteral("%1 renamed").arg(plural(done, QStringLiteral("image"), QStringLiteral("images"))), QString());
    if (!skipped.isEmpty())
        emit toastRequested(Tone::Warn, QStringLiteral("%1 not renamed").arg(plural(skipped.size(), QStringLiteral("image"), QStringLiteral("images"))), skipped.join(u'\n'));
    scanImages();
}

// ---- Languages ---------------------------------------------------------------------

QWidget* LibraryPage::buildLanguagesTab()
{
    auto* lang = new QWidget(m_stack);
    auto* langLayout = vbox(lang, space::lg);
    auto* pick = new Card(lang);
    pick->setTitle(QStringLiteral("Game folder"));
    m_langFolder = new PathField(PathField::Mode::Directory, pick);
    m_langFolder->setDialogTitle(QStringLiteral("Choose a game folder, or a folder of games"));
    m_langFolder->setPlaceholder(QStringLiteral("A game, or a folder of games"));
    auto* prow = new QWidget(pick);
    auto* ph = hbox(prow, space::sm);
    ph->addWidget(m_langFolder, 1);
    m_langBatch = new QCheckBox(QStringLiteral("Folder of games"), prow);
    m_langBatch->setToolTip(QStringLiteral("Scan every game inside this folder"));
    ph->addWidget(m_langBatch);
    m_langScan = button(QStringLiteral("Scan"), ButtonVariant::Primary, QStringLiteral("search"), prow);
    ph->addWidget(m_langScan);
    pick->body()->addWidget(prow);
    auto* help = caption(QStringLiteral("Removes the languages you do not use; every removed file is zipped first."), pick);
    help->setToolTip(QStringLiteral("Language folders and audio packs are removed from the game. Restore from backup puts them back from the ZIP."));
    pick->body()->addWidget(help);
    m_langUnavailable = new UnavailableState(QStringLiteral("The language stripper"), QStringLiteral("tools.languages.scan"), pick);
    pick->body()->addWidget(m_langUnavailable);
    langLayout->addWidget(pick);
    connect(m_langScan, &QPushButton::clicked, this, &LibraryPage::languageScan);
    connect(m_langFolder, &PathField::pathChanged, this, [this](const QString& p) {
        m_langBatch->setChecked(!p.isEmpty() && !readGameSummary(p, false).isGame);
    });

    m_langCard = new Card(lang, Card::Variant::Table);
    auto* th = m_langCard->header();
    th->addWidget(heading(QStringLiteral("Languages"), TextStyle::Subtitle, m_langCard));
    th->addStretch(1);
    m_chips = new QWidget(m_langCard);
    auto* chips = new FlowLayout(m_chips, space::xs, space::xs);
    struct Quick {
        const char* text;
        QStringList codes;
    };
    const std::vector<Quick> quick = {{"English only", {QStringLiteral("en")}}, {"Japanese only", {QStringLiteral("ja")}},
                                      {"EN + JA", {QStringLiteral("en"), QStringLiteral("ja")}},
                                      {"EU only", {QStringLiteral("en"), QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("es"), QStringLiteral("it"), QStringLiteral("pt"), QStringLiteral("nl"), QStringLiteral("pl"), QStringLiteral("ru")}},
                                      {"Keep all", {QStringLiteral("*")}}, {"Keep none", {}}};
    chips->addWidget(label(QStringLiteral("Keep:"), TextStyle::Caption, color::fg_4, m_chips));
    for (const auto& q : quick) {
        auto* b = button(QString::fromLatin1(q.text), ButtonVariant::Secondary, {}, m_chips);
        setButtonSmall(b, true);
        const QStringList codes = q.codes;
        connect(b, &QPushButton::clicked, this, [this, codes] { applyQuickKeep(codes); });
        chips->addWidget(b);
    }
    th->addWidget(m_chips);
    m_langTable = new QTableWidget(0, 5, m_langCard);
    m_langTable->setAccessibleName(QStringLiteral("Languages"));
    m_langTable->setHorizontalHeaderLabels({QStringLiteral("Keep"), QStringLiteral("Language"), QStringLiteral("Code"), QStringLiteral("Folder or file"), QStringLiteral("Size")});
    m_langTable->horizontalHeader()->setFixedHeight(32);
    m_langTable->horizontalHeader()->setHighlightSections(false);
    m_langTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
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
    m_langCard->body()->addWidget(m_langTable);
    m_langEmpty = new EmptyState(QStringLiteral("language"), QStringLiteral("Nothing scanned yet"),
                                 QStringLiteral("Scan a game to see the languages it ships."), m_langCard);
    m_langEmpty->setCompact(true);
    m_langCard->body()->addWidget(m_langEmpty);
    auto* tf = m_langCard->footer();
    m_langSummary = caption({}, m_langCard);
    tf->addWidget(m_langSummary, 1);
    m_langRestore = button(QStringLiteral("Restore from backup…"), ButtonVariant::Secondary, QStringLiteral("undo"), m_langCard);
    m_langStrip = button(QStringLiteral("Remove languages…"), ButtonVariant::Danger, QStringLiteral("trash"), m_langCard);
    tf->addWidget(m_langRestore);
    tf->addWidget(m_langStrip);
    connect(m_langTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
        if (it->column() == 0)
            updateLanguageSummary();
    });
    connect(m_langStrip, &QPushButton::clicked, this, &LibraryPage::stripLanguages);
    connect(m_langRestore, &QPushButton::clicked, this, &LibraryPage::restoreLanguages);
    langLayout->addWidget(m_langCard);
    langLayout->addStretch(1);
    return lang;
}

void LibraryPage::languageScan()
{
    const QString folder = m_langFolder->path();
    if (folder.isEmpty()) {
        m_langFolder->setError(QStringLiteral("Choose a game folder first."));
        return;
    }
    m_langFolder->setError({});
    auto r = jobs::submitLanguageScan(m_ctx, folder, m_langBatch->isChecked());
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Scan not started"), r.error().message);
        return;
    }
    m_langJobId = r.value();
    m_ctx.queue().startNow(m_langJobId); // this job only; a paused queue stays paused
    m_langSummary->setText(QStringLiteral("Scanning…"));
}

void LibraryPage::onJobFinished(const QString& id)
{
    auto rec = m_ctx.queue().record(id);
    if (!rec)
        return;
    if (id == m_scanJobId && !rec->outcome) {
        m_scanJobId.clear(); // a failed or cancelled scan is over too
        if (rec->state == JobState::Failed)
            emit toastRequested(Tone::Err, QStringLiteral("The library scan failed"), rec->error ? rec->error->message : QStringLiteral("See the log."));
    }
    if (id == m_scanJobId && rec->outcome) {
        std::vector<LibraryGame> games;
        for (const auto& v : rec->outcome->details.value(QStringLiteral("games")).toArray()) {
            LibraryGame g;
            g.info = GameSummary::fromJson(v.toObject());
            games.push_back(g);
        }
        // The scan covers the library folders only: a dump that was added by
        // hand from somewhere else stays, and so does everything the list
        // knows about a game beyond what the scan reads.
        m_library.setGames(mergeScanResult(m_library.games(), std::move(games), m_library.scanFolders(), QDateTime::currentDateTime(),
                                           [](const QString& folder) { return readGameSummary(folder, false).isGame; }));
        m_library.setLastScan(QDateTime::currentDateTime());
        m_scanJobId.clear();
    }
    if (id == m_renamePreviewId) {
        m_renamePreviewId.clear();
        if (rec->state != JobState::Succeeded || !rec->outcome)
            emit toastRequested(Tone::Err, QStringLiteral("Preview failed"), rec->error ? rec->error->message : QStringLiteral("See the log."));
        else
            showRenamePreview(rec->outcome->details.value(QStringLiteral("items")).toArray(), rec->outcome->details.value(QStringLiteral("preset")).toString());
    }
    // A rename or undo that was cancelled, or met a folder in use or a taken
    // name, still reports what it did: folders that moved are rescanned and
    // the toast says "done" only when everything was.
    if (id == m_renameApplyId) {
        m_renameApplyId.clear();
        const int renamed = rec->outcome ? rec->outcome->details.value(QStringLiteral("renamed")).toInt() : 0;
        const QString undo = rec->outcome ? rec->outcome->details.value(QStringLiteral("undoLog")).toString() : QString();
        const QString undoHint = undo.isEmpty() ? QString() : QStringLiteral("An undo log was written: More › Rename dump folders… › Undo last rename.");
        if (rec->state == JobState::Succeeded && rec->outcome) {
            const bool complete = renamed > 0 && rec->outcome->verification == Verification::Passed;
            emit toastRequested(complete ? Tone::Ok : Tone::Warn, rec->outcome->summary, undoHint);
        } else {
            const QString why = rec->error ? rec->error->message : rec->outcome ? rec->outcome->summary : QStringLiteral("See the log.");
            emit toastRequested(Tone::Err, QStringLiteral("Rename failed"), renamed > 0 && !undoHint.isEmpty() ? why + u' ' + undoHint : why);
        }
        if (rec->state == JobState::Succeeded || renamed > 0)
            scan();
    }
    if (id == m_renameUndoId) {
        m_renameUndoId.clear();
        const int restored = rec->outcome ? rec->outcome->details.value(QStringLiteral("renamed")).toInt() : 0;
        if (rec->state == JobState::Succeeded && rec->outcome) {
            // The summary says how many names were not restored.
            const bool complete = rec->outcome->verification == Verification::Passed;
            emit toastRequested(complete ? Tone::Ok : Tone::Warn,
                                complete ? QStringLiteral("Rename undone") : restored > 0 ? QStringLiteral("Rename partly undone") : QStringLiteral("Rename not undone"),
                                rec->outcome->summary);
        } else {
            emit toastRequested(Tone::Err, QStringLiteral("Undo failed"),
                                rec->error ? rec->error->message : rec->outcome ? rec->outcome->summary : QStringLiteral("See the log."));
        }
        if (rec->state == JobState::Succeeded || restored > 0)
            scan();
    }
    if (id == m_langJobId) {
        m_langJobId.clear();
        m_langGames = rec->outcome ? rec->outcome->details.value(QStringLiteral("games")).toArray() : QJsonArray();
        rebuildLanguages();
        if (rec->error)
            emit toastRequested(Tone::Err, QStringLiteral("Language scan failed"), rec->error->message);
    }
    if (rec->kind == QLatin1String("tools.languages.strip") || rec->kind == QLatin1String("tools.languages.restore")) {
        if (rec->state == JobState::Succeeded && !m_langFolder->path().isEmpty())
            languageScan();
        // What a restore left alone (files that exist already, a backup of a
        // game with another name) is said, not only logged.
        if (rec->kind == QLatin1String("tools.languages.restore") && rec->state == JobState::Succeeded && rec->outcome
            && !rec->outcome->warnings.isEmpty()) {
            const QStringList& w = rec->outcome->warnings;
            emit toastRequested(Tone::Warn, QStringLiteral("Restored, with something to check"),
                                w.size() > 2 ? QStringLiteral("%1\n… and %2 more in the job's details").arg(w.mid(0, 2).join(u'\n')).arg(w.size() - 2)
                                             : w.join(u'\n'));
        }
    }
    // A finished build, convert or edit may have added or changed an image.
    if (rec->state == JobState::Succeeded && (rec->kind.endsWith(QLatin1String(".build")) || rec->kind == QLatin1String("image.convert")
                                              || rec->kind.endsWith(QLatin1String(".edit"))))
        scanImages();
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
    m_langStrip->setVisible(any);
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
    int n = 0;
    const int total = m_langTable->rowCount();
    for (int r = 0; r < total; ++r)
        if (m_langTable->item(r, 0)->checkState() != Qt::Checked) {
            remove += m_langTable->item(r, 0)->data(Qt::UserRole + 2).toLongLong();
            ++n;
        }
    m_langSummary->setText(total == 0 ? QString() : n == 0 ? QStringLiteral("%1 · untick what to remove").arg(plural(total, QStringLiteral("item"), QStringLiteral("items")))
                                                          : QStringLiteral("%1 of %2 to remove · saves about %3").arg(n).arg(total).arg(formatBytes(remove)));
    m_langStrip->setEnabled(n > 0 && jobs::isAvailable(m_ctx, QStringLiteral("tools.languages.strip")));
    m_langStrip->setText(n > 0 ? QStringLiteral("Remove %1…").arg(plural(n, QStringLiteral("item"), QStringLiteral("items"))) : QStringLiteral("Remove languages…"));
}

void LibraryPage::stripLanguages()
{
    QHash<QString, QStringList> byRoot;
    QHash<QString, int> totalByRoot;
    for (int r = 0; r < m_langTable->rowCount(); ++r) {
        auto* it = m_langTable->item(r, 0);
        const QString root = it->data(Qt::UserRole).toString();
        totalByRoot[root]++;
        if (it->checkState() != Qt::Checked)
            byRoot[root] << it->data(Qt::UserRole + 1).toString();
    }
    if (byRoot.isEmpty())
        return;
    const QString backup = m_ctx.settings().value(tools::keys::kLanguageBackupFolder).toString();
    bool everything = false;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it)
        if (it.value().size() == totalByRoot.value(it.key()))
            everything = true;
    // One line per game (monoLines escapes what it is given); a long batch is
    // cut so the buttons stay on screen.
    QStringList lines;
    const int kShown = 10;
    for (auto it = byRoot.cbegin(); it != byRoot.cend() && lines.size() < kShown; ++it)
        lines << QStringLiteral("%1: %2").arg(QFileInfo(it.key()).fileName(), elideMiddle(it.value().join(QStringLiteral(", ")), 160));
    QString list = Sheet::monoLines(lines);
    if (byRoot.size() > kShown)
        list += QStringLiteral("<br>… and %1 more").arg(plural(byRoot.size() - kShown, QStringLiteral("game"), QStringLiteral("games")));
    const QString text = QStringLiteral("These files and folders are removed from the game:<br>%1<br><br>A backup ZIP is written first to %2. Restore from backup puts everything back.")
                             .arg(list, Sheet::monoSpan(backup.isEmpty() ? QStringLiteral("the default backup folder") : nativePath(backup)));
    if (!Sheet::confirm(window(), QStringLiteral("Remove the selected languages?"), text, QStringLiteral("Keep everything"), QStringLiteral("Remove languages"), true, Sheet::Icon::Warning))
        return;
    if (everything && !Sheet::confirm(window(), QStringLiteral("Remove every language?"),
                                      QStringLiteral("At least one game would lose all of its language files, including the one it needs to start. Only do this if you know the game has its text elsewhere."),
                                      QStringLiteral("Go back"), QStringLiteral("Remove every language anyway"), true, Sheet::Icon::Error))
        return;
    int queued = 0;
    QStringList started, failed;
    for (auto it = byRoot.cbegin(); it != byRoot.cend(); ++it) {
        auto r = jobs::submitLanguageStrip(m_ctx, it.key(), it.value(), backup);
        if (!r.ok()) {
            failed << QStringLiteral("%1: %2").arg(QFileInfo(it.key()).fileName(), r.error().message);
            continue;
        }
        ++queued;
        started << r.value();
    }
    for (auto it = started.crbegin(); it != started.crend(); ++it)
        m_ctx.queue().startNow(*it); // in this order; a paused queue stays paused for the rest
    if (queued > 0)
        emit toastRequested(Tone::Info, QStringLiteral("Removing languages"), QStringLiteral("%1 queued; each one backs up first.").arg(plural(queued, QStringLiteral("job"), QStringLiteral("jobs"))));
    if (!failed.isEmpty())
        emit toastRequested(Tone::Err, QStringLiteral("Not started"), failed.mid(0, 4).join(u'\n'));
}

void LibraryPage::restoreLanguages()
{
    const QString backupDir = m_ctx.settings().value(tools::keys::kLanguageBackupFolder).toString();
    const QString zip = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the language backup ZIP"), backupDir, QStringLiteral("Backup ZIP (*.zip)"));
    if (zip.isEmpty())
        return;
    QString root = m_langFolder->path();
    if (root.isEmpty() || m_langBatch->isChecked())
        root = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose the game folder to restore into"));
    if (root.isEmpty())
        return;
    auto r = jobs::submitLanguageRestore(m_ctx, zip, root);
    if (!r.ok())
        emit toastRequested(Tone::Err, QStringLiteral("Restore not started"), r.error().message);
    else
        m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
}

// ---- page hooks --------------------------------------------------------------------------

void LibraryPage::setTab(Tab t) { m_tabs->setCurrentIndex(int(t)); }

void LibraryPage::refresh()
{
    if (m_tabs->currentIndex() == LanguagesTab && !m_langFolder->path().isEmpty())
        languageScan();
    else
        scan();
}

bool LibraryPage::acceptsDrop(const QList<QUrl>& urls) const
{
    for (const auto& u : urls) {
        const QString f = u.toLocalFile();
        if (QFileInfo(f).isDir() || (!imageFormatOf(f, true).isEmpty() && QFileInfo(f).isFile()))
            return true;
    }
    return false;
}

void LibraryPage::dropUrls(const QList<QUrl>& urls)
{
    for (const auto& u : urls) {
        const QString f = u.toLocalFile();
        if (!imageFormatOf(f, true).isEmpty() && QFileInfo(f).isFile()) {
            openInImageTools(f, QString());
            return;
        }
        if (!QFileInfo(f).isDir())
            continue;
        if (m_tabs->currentIndex() == LanguagesTab)
            m_langFolder->setPath(f);
        else if (m_tabs->currentIndex() == DlcTab)
            m_dlc->openFolder(f);
        else
            addFolder(f);
    }
}

void LibraryPage::setDragHighlight(bool on) { m_gamesDrop->setHighlighted(on); }

void LibraryPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("tab"))) {
        const QString t = args.value(QStringLiteral("tab")).toString();
        static const QStringList names = {QStringLiteral("games"), QStringLiteral("images"), QStringLiteral("languages"), QStringLiteral("dlc")};
        bool isInt = false;
        const int i = t.toInt(&isInt);
        m_tabs->setCurrentIndex(isInt ? i : std::max(0, int(names.indexOf(t))));
    }
    if (args.contains(QStringLiteral("dlcFolder"))) {
        setTab(DlcTab);
        m_dlc->openFolder(args.value(QStringLiteral("dlcFolder")).toString());
    }
    if (args.contains(QStringLiteral("languagesFolder"))) {
        setTab(LanguagesTab);
        m_langFolder->setPath(args.value(QStringLiteral("languagesFolder")).toString());
    }
}

QString LibraryPage::unsavedChanges() const
{
    return m_dlc->hasUnsavedChanges() ? QStringLiteral("dlc_emu.ini has edits that are not saved") : QString();
}

QString LibraryPage::statusContext() const
{
    return QStringLiteral("%1 · %2").arg(plural(qint64(games().size()), QStringLiteral("game"), QStringLiteral("games")),
                                          plural(m_images.size(), QStringLiteral("image"), QStringLiteral("images")));
}

// ---- screenshots ---------------------------------------------------------------------------

QStringList LibraryPage::demoStates() const
{
    return {QStringLiteral("games"), QStringLiteral("games-list"), QStringLiteral("images"), QStringLiteral("languages"), QStringLiteral("dlc")};
}

void LibraryPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("languages")) {
        setTab(LanguagesTab);
        return;
    }
    if (name == QLatin1String("dlc")) {
        setTab(DlcTab);
        return;
    }
    m_demo = true;
    m_demoGames = demoGames();
    m_images = demoImages();
    rebuildImages();
    rebuildGames();
    if (name == QLatin1String("games")) {
        setTab(GamesTab);
    } else if (name == QLatin1String("games-list")) {
        setTab(GamesTab);
        m_gamesView->setCurrentValue(QStringLiteral("list"));
        const auto& items = m_gamesGrid->items();
        if (items.size() > 3)
            m_gamesGrid->setSelectedKeys({items[1].key, items[3].key});
    } else if (name == QLatin1String("images")) {
        setTab(ImagesTab);
    }
}

void LibraryPage::clearDemoState()
{
    if (m_demo) {
        m_demo = false;
        m_demoGames.clear();
        m_images.clear();
        m_gamesGrid->clearSelection();
        m_imagesGrid->clearSelection();
        m_gamesView->setCurrentValue(QStringLiteral("grid"));
        rebuildImages();
        rebuildGames();
        scanImages();
    }
    setTab(GamesTab);
}

} // namespace fw::pages
