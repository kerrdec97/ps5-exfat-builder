#include "app/pages/AboutPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "core/BuildInfo.h"
#include "core/Paths.h"

#include <QDesktopServices>
#include <QGridLayout>
#include <QUrl>

namespace fw::pages {

using namespace ui;
using namespace theme;

QList<ShortcutInfo> shortcutTable()
{
    return {{QStringLiteral("Ctrl+B"), QStringLiteral("Run the current page's primary action (Build now, Build package, Backport…)")},
            {QStringLiteral("Esc"), QStringLiteral("Cancel the running job (after confirming)")},
            {QStringLiteral("Ctrl+L"), QStringLiteral("Open Logs")},
            {QStringLiteral("Ctrl+,"), QStringLiteral("Open Settings")},
            {QStringLiteral("Ctrl+1 … Ctrl+9"), QStringLiteral("Jump to the nth page in the sidebar")},
            {QStringLiteral("F5"), QStringLiteral("Rescan on Library and Backport; refresh elsewhere")},
            {QStringLiteral("Ctrl+Q"), QStringLiteral("Quit")},
            {QStringLiteral("Tab / Shift+Tab, arrows"), QStringLiteral("Move the keyboard focus; the blue ring shows where it is")},
            {QStringLiteral("Drop a folder or image"), QStringLiteral("Use it as the source on the current page")}};
}

AboutPage::AboutPage(AppContext& ctx, QWidget* parent)
    : Page(ctx, QStringLiteral("about"), QStringLiteral("About"), QStringLiteral("Version, licences and the people behind this tool."), parent)
{
    auto* version = new Card(this);
    m_updateBadge = new Badge(QStringLiteral("Updates not checked"), Tone::Neutral, version);
    m_updateBadge->setIconName({});
    version->setTitle(QStringLiteral("%1 — %2").arg(QLatin1String(build::kProductName), QLatin1String(build::kEditionName)), m_updateBadge);
    version->body()->addWidget(label(QStringLiteral("Version %1 · native edition").arg(QLatin1String(build::kVersion)), TextStyle::Body, color::fg_1, version));
    version->body()->addWidget(caption(QStringLiteral("The final release of exFAT Image Builder, rebuilt in C++ with Qt. Licensed under the GNU General Public License, version 3 or later. "
                                                      "Use it only with games and content you legally own and dumped yourself. Not affiliated with or endorsed by Sony Interactive Entertainment."), version));
    auto* actions = new QWidget(version);
    auto* al = new FlowLayout(actions, space::sm, space::sm);
    m_checkUpdates = button(QStringLiteral("Check for updates"), ButtonVariant::Secondary, QStringLiteral("refresh"), actions);
    m_checkUpdates->setEnabled(jobs::isAvailable(ctx, QStringLiteral("update.check")));
    m_checkUpdates->setToolTip(m_checkUpdates->isEnabled() ? QString() : QStringLiteral("Not available in this build"));
    connect(m_checkUpdates, &QPushButton::clicked, this, &AboutPage::checkForUpdatesRequested);
    al->addWidget(m_checkUpdates);
    auto* report = button(QStringLiteral("Report a problem…"), ButtonVariant::Secondary, QStringLiteral("bug"), actions);
    connect(report, &QPushButton::clicked, this, &AboutPage::reportProblemRequested);
    al->addWidget(report);
    auto* repo = button(QStringLiteral("GitHub repository"), ButtonVariant::Secondary, QStringLiteral("github"), actions);
    connect(repo, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder"))); });
    al->addWidget(repo);
    auto* changelog = button(QStringLiteral("Changelog"), ButtonVariant::Secondary, QStringLiteral("file_text"), actions);
    connect(changelog, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/kerrdec97/ps5-exfat-builder/blob/main/CHANGELOG.md"))); });
    al->addWidget(changelog);
    auto* shortcuts = button(QStringLiteral("Keyboard shortcuts"), ButtonVariant::Secondary, QStringLiteral("keyboard"), actions);
    connect(shortcuts, &QPushButton::clicked, this, &AboutPage::shortcutsRequested);
    al->addWidget(shortcuts);
    auto* logs = button(QStringLiteral("Open logs folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), actions);
    connect(logs, &QPushButton::clicked, this, [] { openInFileManager(Paths::logsDir()); });
    al->addWidget(logs);
    version->body()->addWidget(actions);
    content()->addWidget(version);

    auto* credits = new Card(this);
    credits->setTitle(QStringLiteral("Credits"));
    credits->body()->addWidget(label(QStringLiteral("Created and maintained by DecKerr97 (kerrdec97). Inspired by NookieAI and stonemodder (Porkfolio)."), TextStyle::Body, color::fg_1, credits));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(space::lg);
    grid->setVerticalSpacing(space::xs);
    const QList<QPair<QString, QString>> people = {
        {QStringLiteral("Nazky"), QStringLiteral("Auto-Backport pipeline and Lazy_MkPFS")},
        {QStringLiteral("BestPig"), QStringLiteral("Backport research and the BackPork payload")},
        {QStringLiteral("SvenGDK"), QStringLiteral("UFS2Tool")},
        {QStringLiteral("drakmor"), QStringLiteral("ShadowMount+ / MicroMount, DLC-emu and AMPR research")},
        {QStringLiteral("PSBrew"), QStringLiteral("MkPFS and PFS research")},
        {QStringLiteral("john-tornblom"), QStringLiteral("PS5 payload SDK")},
        {QStringLiteral("ps5-payload-dev"), QStringLiteral("klogsrv kernel log streamer")},
        {QStringLiteral("idlesauce, NookieAI, stonemodder"), QStringLiteral("Research and inspiration")},
        {QStringLiteral("alik70952"), QStringLiteral("UI modernisation proposal (pull request #57)")},
    };
    int row = 0;
    for (const auto& [name, what] : people) {
        grid->addWidget(label(name, TextStyle::BodyStrong, color::fg_0, credits), row, 0, Qt::AlignTop);
        auto* w = label(what, TextStyle::Body, color::fg_2, credits);
        w->setWordWrap(true);
        grid->addWidget(w, row, 1);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    credits->body()->addLayout(grid);
    credits->body()->addWidget(caption(QStringLiteral("…and every translator, tester, issue reporter and supporter of the Python edition."), credits));
    content()->addWidget(credits);

    auto* third = new Card(this);
    third->setTitle(QStringLiteral("Third-party notices"));
    third->body()->addWidget(caption(QStringLiteral("The native app contains or links these components. Full licence texts ship in the LICENSES folder next to the application."), third));
    auto* tgrid = new QGridLayout;
    tgrid->setHorizontalSpacing(space::lg);
    tgrid->setVerticalSpacing(space::xs);
    const QList<QStringList> rows = {
        {QStringLiteral("Qt 6 (Core, Gui, Widgets, Svg, Network, Concurrent)"), QStringLiteral("Application framework, dynamically linked"), QStringLiteral("LGPL-3.0")},
        {QStringLiteral("zlib"), QStringLiteral("Compression and ZIP archives"), QStringLiteral("zlib licence")},
        {QStringLiteral("Lazy_MkPFS / PSBrew MkPFS"), QStringLiteral("The PFS/FFPFSC engine is a C++ port of this code"), QStringLiteral("GPL-3.0")},
        {QStringLiteral("UFS2Tool (SvenGDK)"), QStringLiteral("The UFS2/FFPKG engine follows its image layout"), QStringLiteral("BSD-2-Clause")},
        {QStringLiteral("Lucide icons"), QStringLiteral("Interface icons"), QStringLiteral("ISC")},
        {QStringLiteral("ampr_emu (drakmor)"), QStringLiteral("AMPR index format"), QStringLiteral("GPL-3.0")},
    };
    row = 0;
    tgrid->addWidget(caption(QStringLiteral("Component"), third), row, 0);
    tgrid->addWidget(caption(QStringLiteral("Use"), third), row, 1);
    tgrid->addWidget(caption(QStringLiteral("Licence"), third), row, 2);
    ++row;
    for (const auto& r : rows) {
        tgrid->addWidget(label(r[0], TextStyle::BodyStrong, color::fg_0, third), row, 0, Qt::AlignTop);
        auto* use = label(r[1], TextStyle::Body, color::fg_2, third);
        use->setWordWrap(true);
        tgrid->addWidget(use, row, 1);
        tgrid->addWidget(label(r[2], TextStyle::Mono, color::fg_1, third), row, 2, Qt::AlignTop);
        ++row;
    }
    tgrid->setColumnStretch(1, 1);
    third->body()->addLayout(tgrid);
    third->body()->addWidget(caption(QStringLiteral("No Sony software is included. FPKG packages are built with your own Publishing Tools, located through Settings › Tools."), third));
    content()->addWidget(third);
}

void AboutPage::setUpdateStatus(const QString& text, Tone tone)
{
    m_updateBadge->set(text, tone);
    m_updateBadge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
}

QString AboutPage::statusContext() const
{
    return QStringLiteral("Version %1 · GPL-3.0-or-later").arg(QLatin1String(build::kVersion));
}

} // namespace fw::pages
