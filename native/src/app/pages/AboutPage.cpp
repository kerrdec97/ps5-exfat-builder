#include "app/pages/AboutPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/HomeKit.h"
#include "core/BuildInfo.h"
#include "core/Paths.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

const char* const kRepoUrl = "https://github.com/kerrdec97/ps5-exfat-builder";

// The product card: a quiet surface with a soft brand glow behind the mark.
class ProductCard : public QWidget {
public:
    explicit ProductCard(QWidget* parent) : QWidget(parent) { setProperty("fwNoRing", true); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(r, radius::lg, radius::lg);
        p.setClipPath(clip);
        QLinearGradient base(r.topLeft(), r.bottomLeft());
        base.setColorAt(0, QColor(0x15, 0x1a, 0x27));
        base.setColorAt(1, color::bg_2);
        p.fillRect(r, base);
        QRadialGradient glow(QPointF(r.left() + 70, r.center().y()), 220);
        glow.setColorAt(0, QColor(0x3d, 0x7b, 0xff, 60));
        glow.setColorAt(0.5, QColor(0x8b, 0x5c, 0xf6, 28));
        glow.setColorAt(1, QColor(0x8b, 0x5c, 0xf6, 0));
        p.fillRect(r, glow);
        p.setClipping(false);
        p.setPen(QPen(color::border_2, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius::lg, radius::lg);
    }
};

} // namespace

QList<ShortcutInfo> shortcutTable()
{
    return {{QStringLiteral("Ctrl+B"), QStringLiteral("Run the current page's primary action (Build now, Build package, Backport…)")},
            {QStringLiteral("Esc"), QStringLiteral("Cancel the running job (after confirming)")},
            {QStringLiteral("Ctrl+L"), QStringLiteral("Open Logs")},
            {QStringLiteral("Ctrl+,"), QStringLiteral("Open Settings")},
            {QStringLiteral("Ctrl+F"), QStringLiteral("Search settings (on Settings)")},
            {QStringLiteral("Ctrl+1 … Ctrl+9"), QStringLiteral("Jump to the nth page in the sidebar")},
            {QStringLiteral("F5"), QStringLiteral("Rescan on Library and Backport; refresh elsewhere")},
            {QStringLiteral("Ctrl+Q"), QStringLiteral("Quit")},
            {QStringLiteral("Tab / Shift+Tab, arrows"), QStringLiteral("Move the keyboard focus; the blue ring shows where it is")},
            {QStringLiteral("Drop a folder or image"), QStringLiteral("Use it as the source on the current page")}};
}

AboutPage::AboutPage(AppContext& ctx, QWidget* parent)
    : Page(ctx, QStringLiteral("about"), QStringLiteral("About"), QStringLiteral("Version, licence and the people behind this tool."), parent)
{
    // ---- Product
    auto* product = new ProductCard(this);
    product->setObjectName(QStringLiteral("about-product"));
    auto* ph = hbox(product, space::xl, QMargins(28, 24, 24, 24));
    ph->addWidget(new BrandMark(72, product), 0, Qt::AlignVCenter);
    auto* pcol = new QVBoxLayout;
    pcol->setSpacing(6);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(space::md);
    nameRow->addWidget(label(QLatin1String(build::kProductName), TextStyle::Display, color::fg_0, product));
    auto* version = new Badge(QStringLiteral("Version %1").arg(QLatin1String(build::kVersion)), Tone::Info, product);
    version->setIconName({});
    nameRow->addWidget(version, 0, Qt::AlignVCenter);
    nameRow->addStretch(1);
    pcol->addLayout(nameRow);
    auto* tagline = label(QStringLiteral("Build, backport and send PS5 game images. Native C++ and Qt."), TextStyle::Body, color::fg_2, product);
    tagline->setWordWrap(true);
    pcol->addWidget(tagline);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(space::sm);
    m_updateBadge = new Badge(QStringLiteral("Updates not checked"), Tone::Neutral, product);
    m_updateBadge->setIconName({});
    statusRow->addWidget(m_updateBadge);
    auto* licence = new Badge(QStringLiteral("All rights reserved"), Tone::Neutral, product);
    licence->setIconName(QStringLiteral("shield"));
    statusRow->addWidget(licence);
    statusRow->addStretch(1);
    pcol->addSpacing(4);
    pcol->addLayout(statusRow);
    ph->addLayout(pcol, 1);
    auto* pactions = new QVBoxLayout;
    pactions->setSpacing(space::sm);
    m_checkUpdates = button(QStringLiteral("Check for updates"), ButtonVariant::Primary, QStringLiteral("refresh"), product);
    m_checkUpdates->setEnabled(jobs::isAvailable(ctx, QStringLiteral("update.check")));
    m_checkUpdates->setToolTip(m_checkUpdates->isEnabled() ? QStringLiteral("Asks GitHub for the latest release") : QStringLiteral("Not available in this build"));
    connect(m_checkUpdates, &QPushButton::clicked, this, &AboutPage::checkForUpdatesRequested);
    pactions->addWidget(m_checkUpdates);
    auto* logs = button(QStringLiteral("Open logs folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), product);
    connect(logs, &QPushButton::clicked, this, [] { openInFileManager(Paths::logsDir()); });
    pactions->addWidget(logs);
    ph->addLayout(pactions, 0);
    content()->addWidget(product);

    // ---- Links
    auto* links = new QWidget(this);
    auto* lg = new TileGrid(links, 230, 4);
    lg->setBalanced(true);
    auto* repo = new ActionCard(QStringLiteral("github"), QStringLiteral("GitHub repository"), QStringLiteral("Releases, issues and discussions."), links);
    repo->setToolTip(QString::fromLatin1(kRepoUrl));
    connect(repo, &QAbstractButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl))); });
    auto* report = new ActionCard(QStringLiteral("bug"), QStringLiteral("Report a problem"), QStringLiteral("Bundle the logs into a report you can share."), links);
    connect(report, &QAbstractButton::clicked, this, &AboutPage::reportProblemRequested);
    auto* changes = new ActionCard(QStringLiteral("sparkles"), QStringLiteral("What's new"), QStringLiteral("The changelog of every release."), links);
    connect(changes, &QAbstractButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl) + QStringLiteral("/blob/main/CHANGELOG.md")));
    });
    auto* keys = new ActionCard(QStringLiteral("keyboard"), QStringLiteral("Keyboard shortcuts"), QStringLiteral("Every shortcut on one sheet."), links);
    connect(keys, &QAbstractButton::clicked, this, &AboutPage::shortcutsRequested);
    for (QWidget* w : {static_cast<QWidget*>(repo), static_cast<QWidget*>(report), static_cast<QWidget*>(changes), static_cast<QWidget*>(keys)})
        lg->addWidget(w);
    content()->addWidget(links);

    // ---- Credits | licence
    auto* columns = new QWidget(this);
    auto* cl = new TwoColumnLayout(columns, 380, 100000);
    auto* credits = new Card(columns);
    credits->setTitle(QStringLiteral("Credits"));
    auto* lead = label(QStringLiteral("Created and maintained by Deckerr97 (kerrdec97). Inspired by NookieAI and stonemodder (Porkfolio)."), TextStyle::Body, color::fg_1, credits);
    lead->setWordWrap(true);
    credits->body()->addWidget(lead);
    auto* people = new QWidget(credits);
    auto* pg = new TileGrid(people, 220, 2, space::lg, space::md);
    // Name, what they made, their GitHub (README "Credits" has the same list).
    const QList<QStringList> list = {
        {QStringLiteral("quer3q"), QStringLiteral("ampr-emu-ng (bundled zstd runtime) and ps5-dump-forge"), QStringLiteral("github.com/quer3q")},
        {QStringLiteral("Lapy"), QStringLiteral("DLC package builder"), QString()},
        {QStringLiteral("PSVIETHOA (Nguyễn Thanh Sơn, Ngô Phi Phương)"), QStringLiteral("fpkg-cli · PSVIETHOA FPKG Builder"), QStringLiteral("github.com/thanhsondev")},
        {QStringLiteral("drakmor"), QStringLiteral("LibProsperoPkg, ShadowMount+, ampr_emu, dlc_emu"), QStringLiteral("github.com/drakmor")},
        {QStringLiteral("Nazky"), QStringLiteral("Lazy_MkPFS, Lazy_AMPR and the Auto-Backport pipeline"), QStringLiteral("github.com/Nazky")},
        {QStringLiteral("PSBrew"), QStringLiteral("MkPFS and PFS research"), QStringLiteral("github.com/PSBrew")},
        {QStringLiteral("SvenGDK"), QStringLiteral("UFS2Tool and LibProsperoPKG"), QStringLiteral("github.com/SvenGDK")},
        {QStringLiteral("john-tornblom · ps5-payload-dev"), QStringLiteral("PS5 payload SDK, ftpsrv, elfldr, klogsrv"), QStringLiteral("github.com/ps5-payload-dev")},
        {QStringLiteral("BestPig"), QStringLiteral("Backport research and the BackPork payload"), QStringLiteral("github.com/BestPig")},
        {QStringLiteral("Gezine · notmaj0r · itsPLK"), QStringLiteral("Y2JB, Relapse and the Y2JB autoloader"), QStringLiteral("github.com/Gezine")},
        {QStringLiteral("etaHEN team · EchoStretch"), QStringLiteral("etaHEN and kstuff-lite"), QStringLiteral("github.com/etaHEN")},
        {QStringLiteral("idlesauce, NookieAI, stonemodder"), QStringLiteral("Research and inspiration"), QString()},
        {QStringLiteral("alik70952"), QStringLiteral("UI modernisation proposal (pull request #57)"), QStringLiteral("github.com/alik70952")},
    };
    for (const QStringList& who : list) {
        auto* cell = new QWidget(people);
        auto* v = vbox(cell, 1);
        auto* n = label(who[0], TextStyle::BodyStrong, color::fg_0, cell);
        n->setWordWrap(true);
        v->addWidget(n);
        auto* w = label(who[1], TextStyle::Caption, color::fg_3, cell);
        w->setWordWrap(true);
        v->addWidget(w);
        if (!who[2].isEmpty()) {
            auto* g = label(who[2], TextStyle::Caption, color::fg_4, cell);
            g->setToolTip(QStringLiteral("https://") + who[2]);
            v->addWidget(g);
        }
        v->addStretch(1);
        pg->addWidget(cell);
    }
    credits->body()->addSpacing(space::xs);
    credits->body()->addWidget(people);
    credits->body()->addSpacing(space::xs);
    credits->body()->addWidget(meta(QStringLiteral("…and every translator, tester, issue reporter and supporter of the Python edition."), credits));
    cl->setLeft(credits);

    auto* legal = new Card(columns);
    legal->setTitle(QStringLiteral("Licence"));
    auto* terms = label(QStringLiteral("Copyright \u00a9 2026 Deckerr97 (kerrdec97). All rights reserved. Proprietary software for personal, "
                                       "non-commercial use: no modification, redistribution or reverse engineering without permission."),
                        TextStyle::Body, color::fg_1, legal);
    terms->setWordWrap(true);
    legal->body()->addWidget(terms);
    auto* thirdParty = caption(QStringLiteral("Third-party components keep their own licences (below and in THIRD_PARTY_NOTICES.md)."), legal);
    thirdParty->setWordWrap(true);
    legal->body()->addWidget(thirdParty);
    auto* own = caption(QStringLiteral("Use it only with games and content you own and dumped yourself."), legal);
    own->setWordWrap(true);
    legal->body()->addWidget(own);
    auto* sony = caption(affiliationNote(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools"))), legal);
    sony->setWordWrap(true);
    sony->setToolTip(QStringLiteral("FPKG packages are built with your own Publishing Tools, located through Settings › Tools."));
    legal->body()->addWidget(sony);
    legal->body()->addStretch(1);
    cl->setRight(legal);
    content()->addWidget(columns);

    // ---- Third-party notices (folded)
    auto* third = new Disclosure(QStringLiteral("Third-party notices"), QStringLiteral("Full licence texts ship in the LICENSES folder next to the app."), this);
    third->setObjectName(QStringLiteral("about-notices"));
    const QList<QStringList> rows = thirdPartyComponents();
    third->setSummary(QStringLiteral("%1 components").arg(rows.size()));
    auto* tgrid = new QGridLayout;
    tgrid->setHorizontalSpacing(space::lg);
    tgrid->setVerticalSpacing(space::sm);
    int row = 0;
    tgrid->addWidget(label(QStringLiteral("Component"), TextStyle::Overline, color::fg_4, third), row, 0);
    tgrid->addWidget(label(QStringLiteral("Use"), TextStyle::Overline, color::fg_4, third), row, 1);
    tgrid->addWidget(label(QStringLiteral("Licence"), TextStyle::Overline, color::fg_4, third), row, 2);
    ++row;
    for (const auto& r : rows) {
        auto* name = label(r[0], TextStyle::BodyStrong, color::fg_0, third);
        name->setWordWrap(true);
        tgrid->addWidget(name, row, 0, Qt::AlignTop);
        auto* use = label(r[1], TextStyle::Body, color::fg_2, third);
        use->setWordWrap(true);
        tgrid->addWidget(use, row, 1, Qt::AlignTop);
        tgrid->addWidget(label(r[2], TextStyle::Mono, color::fg_1, third), row, 2, Qt::AlignTop);
        ++row;
    }
    tgrid->setColumnStretch(0, 2);
    tgrid->setColumnStretch(1, 3);
    third->body()->addLayout(tgrid);
    content()->addWidget(third);
}

QList<QStringList> AboutPage::thirdPartyComponents()
{
    return {
        {QStringLiteral("Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent, Sql)"), QStringLiteral("Application framework, dynamically linked"), QStringLiteral("LGPL-3.0")},
        {QStringLiteral("zlib"), QStringLiteral("Compression and ZIP archives"), QStringLiteral("zlib licence")},
        {QStringLiteral("Lazy_MkPFS / PSBrew MkPFS"), QStringLiteral("The PFS/FFPFSC engine is a C++ port of this code"), QStringLiteral("GPL-3.0")},
        {QStringLiteral("UFS2Tool (SvenGDK)"), QStringLiteral("The UFS2/FFPKG engine follows its image layout"), QStringLiteral("BSD-2-Clause")},
        {QStringLiteral("make_fself (john-tornblom, modified by Nazky)"), QStringLiteral("The fake-SELF reader behind the Backport tab's game list is a port of this code"), QStringLiteral("GPL-3.0")},
        {QStringLiteral("LibProsperoPKG (SvenGDK)"), QStringLiteral("Facts used by the FPKG engine: PS5 flat-path hash, UCP layout"), QStringLiteral("GPL-3.0-or-later")},
        {QStringLiteral("zstd 1.5.7 (Meta Platforms)"), QStringLiteral("zstd packs"), QStringLiteral("BSD-3-Clause")},
        {QStringLiteral("LZ4 1.10.0 (Yann Collet)"), QStringLiteral("Reading LZ4 packs"), QStringLiteral("BSD-2-Clause")},
        {QStringLiteral("ps5-dump-forge (quer3q)"), QStringLiteral("zstd pack writer and rules, LZ4 pack reader follow its code"), QStringLiteral("GPL-3.0-or-later")},
        {QStringLiteral("ampr-emu-ng 0.3.3 (quer3q)"), QStringLiteral("Format of the zstd packs; its PS5 runtime is bundled unchanged; source: github.com/quer3q/ampr-emu-ng"), QStringLiteral("GPL-3.0-or-later")},
        {QStringLiteral("Lucide icons"), QStringLiteral("Interface icons"), QStringLiteral("ISC")},
        {QStringLiteral("Microsoft Visual C++ runtime"), QStringLiteral("Runtime libraries shipped with the program"), QStringLiteral("Microsoft redistributable")},
        {QStringLiteral("PS5 Backport engine 0.3.4 (Deckerr97)"), QStringLiteral("Bundled separate program run by the Backport tab"), QStringLiteral("Its own terms")},
    };
}

QString AboutPage::affiliationNote(const QString& toolsDir)
{
    // The three private switches of the release script are independent: say
    // only what this build really carries.
    const QDir tools(toolsDir);
    QStringList carried;
    if (QFileInfo(tools.filePath(QStringLiteral("fpkg-toolkit"))).isDir())
        carried << QStringLiteral("Sony's Publishing Tools");
    if (QFileInfo(tools.filePath(QStringLiteral("fpkg-cli"))).isDir())
        carried << QStringLiteral("fpkg-cli");
    if (QFileInfo(tools.filePath(QStringLiteral("youtube"))).isDir())
        carried << QStringLiteral("the YouTube app packages");
    if (carried.isEmpty())
        return QStringLiteral("Not affiliated with or endorsed by Sony Interactive Entertainment. The app includes no Sony "
                              "software; the bundled PS5 Backport engine is a separate program with its own notices.");
    QString list = carried.last();
    if (carried.size() > 1)
        list = carried.mid(0, carried.size() - 1).join(QStringLiteral(", ")) + QStringLiteral(" and ") + carried.last();
    return QStringLiteral("Private build: it carries your own copy of %1 for personal use. Do not share this exe. Not affiliated "
                          "with or endorsed by Sony Interactive Entertainment.")
        .arg(list);
}

void AboutPage::setUpdateStatus(const QString& text, Tone tone)
{
    m_updateBadge->set(text, tone);
    m_updateBadge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
}

QString AboutPage::statusContext() const
{
    return QStringLiteral("Version %1 · All rights reserved").arg(QLatin1String(build::kVersion));
}

} // namespace fw::pages
