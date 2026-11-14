#include "app/pages/ps5/FakelibTab.h"

#include "app/pages/Page.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Tr.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>

#include <algorithm>
#include <optional>

namespace fw::pages {

using namespace ui;
using namespace theme;
namespace fl = ps5::fakelib;

namespace {
enum Col { StatusCol = 0, NameCol, SizeCol, ModifiedCol, FolderCol, ColumnCount };
QString libraryFilter()
{
    return FW_TR("PS5 libraries") + QStringLiteral(" (*.prx *.sprx);;") + FW_TR("All files") + QStringLiteral(" (*)");
}
} // namespace

FakelibTab::FakelibTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("fakelib"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* warn = new InfoBar(Tone::Info, this);
    warn->setMessage(FW_TR("Library overlays (.prx / .sprx) that ShadowMount+ lays over every game; restart a game to pick up a change. "
                                    "Do not run BackPork as well: ShadowMount+ 1.7 stops it at startup."));
    warn->setToolTip(FW_TR("Overlays cannot replace eboot.bin, game content or files inside images; rebuild those. "
                                    "libkernel.sprx is refused: ShadowMount+ deletes it from fakelib folders."));
    l->addWidget(warn);
    m_legacy = new InfoBar(Tone::Warn, this);
    m_legacy->setTitle(FW_TR("Disabled overlays inside the overlay folder"));
    m_legacy->setMessage(FW_TR("fakelib/disabled is copied into every game by ShadowMount+ 1.7. Move them to fakelib.disabled."));
    auto* move = m_legacy->addLink(FW_TR("Move them"));
    connect(move, &Link::activated, this, [this] {
        runAndRefresh(FW_TR("Disabled overlays moved"), FW_TR("Disabled overlays not moved"), [](ps5::FtpClient& c, const CancellationToken& t) -> Status {
            auto r = fl::moveLegacyDisabled(c, t);
            if (!r.ok())
                return r.takeError();
            return success();
        });
    });
    m_legacy->hide();
    l->addWidget(m_legacy);

    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->addWidget(heading(FW_TR("Global overlays"), TextStyle::Subtitle, card));
    h->addWidget(monoLabel(fl::kFolder, card, color::fg_3));
    h->addStretch(1);
    m_add = button(FW_TR("Add…"), ButtonVariant::Primary, QStringLiteral("plus"), card);
    setToolPrimary(m_add); // the tab's one gradient button
    m_replace = button(FW_TR("Replace…"), ButtonVariant::Secondary, QStringLiteral("replace"), card);
    m_toggle = button(FW_TR("Disable"), ButtonVariant::Secondary, QStringLiteral("toggle"), card);
    m_remove = button(FW_TR("Remove"), ButtonVariant::Secondary, QStringLiteral("trash"), card);
    m_refresh = new IconButton(QStringLiteral("refresh"), FW_TR("Refresh"), IconButton::Size::Regular, card);
    m_refresh->setToolTip(FW_TR("List the overlays again"));
    for (auto* b : {m_add, m_replace, m_toggle, m_remove})
        h->addWidget(b);
    h->addWidget(m_refresh);

    m_table = new QTableWidget(0, ColumnCount, card);
    m_table->setHorizontalHeaderLabels({FW_TR("State"), FW_TR("Library"), FW_TR("Size"), FW_TR("Modified"), FW_TR("Folder on the PS5")});
    for (const int c : {int(SizeCol), int(ModifiedCol), int(FolderCol)})
        keepColumnLeftToRight(m_table, c); // sizes, dates, paths, IDs: left to right in any language
    styleToolTable(m_table, 6);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setSectionResizeMode(NameCol, QHeaderView::Stretch);
    // Both states whole in every language (at least the width it always had).
    m_table->setColumnWidth(StatusCol, std::max(130, badgeColumnWidth(m_table, StatusCol, {FW_TR("Enabled"), FW_TR("Disabled")})));
    m_table->setColumnWidth(SizeCol, 100);
    m_table->setColumnWidth(ModifiedCol, 150);
    m_table->setColumnWidth(FolderCol, 300);
    m_table->setAccessibleName(FW_TR("Fakelib overlays"));
    card->body()->addWidget(m_table);
    m_empty = new EmptyState(QStringLiteral("layers"), FW_TR("Overlays not listed yet"),
                             FW_TR("Refresh lists the .prx and .sprx files in /data/shadowmount/fakelib and fakelib.disabled."), card);
    auto* listBtn = m_empty->addAction(FW_TR("Refresh"), ButtonVariant::Secondary);
    card->body()->addWidget(m_empty);
    auto* f = card->footer();
    m_status = caption({}, card);
    f->addWidget(m_status, 1);
    l->addWidget(card);
    l->addWidget(caption(FW_TR("Add and Replace upload under a temporary name and swap it in only when complete, so a dropped "
                                        "connection never leaves a half-written library."), this));
    l->addStretch(1);

    connect(m_refresh, &QToolButton::clicked, this, &FakelibTab::refresh);
    connect(listBtn, &QPushButton::clicked, this, &FakelibTab::refresh);
    connect(m_add, &QPushButton::clicked, this, &FakelibTab::askAdd);
    connect(m_replace, &QPushButton::clicked, this, &FakelibTab::askReplace);
    connect(m_remove, &QPushButton::clicked, this, &FakelibTab::askRemove);
    connect(m_toggle, &QPushButton::clicked, this, &FakelibTab::toggleSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &FakelibTab::updateActions);
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool busy) {
        if (busy)
            m_status->setText(FW_TR("Talking to the console…"));
        updateActions();
    });
    m_table->hide();
    updateActions();
}

void FakelibTab::refresh()
{
    runOnConsole<fl::Listing>([](ps5::FtpClient& c, const CancellationToken& t) { return fl::list(c, t); },
                              [this](const Result<fl::Listing>& r) {
                                  if (!r.ok()) {
                                      m_status->setText(FW_TR("Could not list the fakelib folder: %1").arg(r.error().message));
                                      setTextColor(m_status, color::danger_hi);
                                      emit toastRequested(Tone::Err, FW_TR("Fakelib folder not listed"), r.error().message);
                                      return;
                                  }
                                  showListing(r.value());
                              });
}

void FakelibTab::runAndRefresh(const QString& done, const QString& failed, std::function<Status(ps5::FtpClient&, const CancellationToken&)> op)
{
    struct Outcome {
        Status status;
        std::optional<fl::Listing> listing;
    };
    runOnConsole<Outcome>(
        [op](ps5::FtpClient& c, const CancellationToken& t) -> Result<Outcome> {
            Outcome o;
            o.status = op(c, t);
            if (c.isConnected()) {
                auto l = fl::list(c, t);
                if (l.ok())
                    o.listing = std::move(l.value());
            }
            return o;
        },
        [this, done, failed](const Result<Outcome>& r) {
            if (!r.ok()) { // no connection
                m_status->setText(QStringLiteral("%1: %2").arg(failed, r.error().message));
                setTextColor(m_status, color::danger_hi);
                emit toastRequested(Tone::Err, failed, r.error().message);
                return;
            }
            if (r->listing)
                showListing(*r->listing);
            if (!r->status.ok()) {
                m_status->setText(QStringLiteral("%1: %2").arg(failed, r->status.error().message));
                setTextColor(m_status, color::danger_hi);
                emit toastRequested(Tone::Err, failed, r->status.error().message);
            } else {
                emit toastRequested(Tone::Ok, done, FW_TR("Remount the game or restart it for the change to apply."));
            }
        });
}

void FakelibTab::showListing(const fl::Listing& listing)
{
    m_listing = listing;
    m_table->setRowCount(0);
    for (const fl::Overlay& o : m_listing.overlays) {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        setBadgeCell(m_table, r, StatusCol, o.enabled ? FW_TR("Enabled") : FW_TR("Disabled"), o.enabled ? Tone::Ok : Tone::Neutral,
                     o.enabled ? QStringLiteral("check") : QStringLiteral("pause"));
        auto* name = toolItem(o.name, true, o.enabled ? color::fg_0 : color::fg_3);
        name->setIcon(icon(QStringLiteral("file"), o.enabled ? color::fg_2 : color::fg_4));
        m_table->setItem(r, NameCol, name);
        m_table->setItem(r, SizeCol, toolItem(o.size < 0 ? QStringLiteral("—") : formatBytes(o.size), true, color::fg_3));
        m_table->setItem(r, ModifiedCol, toolItem(o.modified.isValid() ? humanTime(o.modified.toLocalTime()) : QStringLiteral("—"), true, color::fg_3));
        m_table->setItem(r, FolderCol, toolItem(o.folder(), true, color::fg_4));
    }
    const int n = int(m_listing.overlays.size());
    const int on = m_listing.enabledCount();
    m_table->setVisible(n > 0);
    m_empty->setVisible(n == 0);
    if (n == 0)
        m_empty->setToolTip({});
    QString text = !m_listing.folderExists ? FW_TR("/data/shadowmount/fakelib does not exist yet; Add creates it.")
                 : n == 0                  ? FW_TR("No .prx / .sprx overlays yet. Add deploys one.")
                 : n == 1                  ? FW_TR("%1 overlay · %2 enabled · %3 disabled").arg(n).arg(on).arg(n - on)
                                           : FW_TR("%1 overlays · %2 enabled · %3 disabled").arg(n).arg(on).arg(n - on);
    if (const auto skipped = m_listing.skipped.size(); skipped > 0)
        text = (skipped == 1 ? FW_TR("%1 · %2 name skipped (not usable safely)") : FW_TR("%1 · %2 names skipped (not usable safely)"))
                   .arg(text, QString::number(skipped));
    m_status->setText(text);
    m_legacy->setVisible(m_listing.legacyCount() > 0);
    setTextColor(m_status, color::fg_3);
    updateActions();
}

int FakelibTab::selectedRow() const
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return -1;
    const int r = rows.first().row();
    return r < int(m_listing.overlays.size()) ? r : -1;
}

void FakelibTab::updateActions()
{
    const bool idle = !isBusy();
    const int r = selectedRow();
    m_add->setEnabled(idle);
    m_refresh->setEnabled(idle);
    m_replace->setEnabled(idle && r >= 0);
    m_remove->setEnabled(idle && r >= 0);
    m_toggle->setEnabled(idle && r >= 0);
    const bool enabled = r >= 0 ? m_listing.overlays[size_t(r)].enabled : true;
    m_toggle->setText(enabled ? FW_TR("Disable") : FW_TR("Enable"));
}

bool FakelibTab::checkWithUser(const QString& localPath, bool* confirmed)
{
    const fl::LocalCheck check = fl::checkLocalFile(localPath);
    *confirmed = false;
    // The message is plain text and the sheet shows it as such.
    if (check.verdict == fl::Verdict::Reject) {
        Sheet::message(window(), FW_TR("Unsupported file"), check.message, Sheet::Icon::Warning);
        return false;
    }
    if (check.verdict == fl::Verdict::Confirm) {
        if (!Sheet::confirm(window(), FW_TR("Upload anyway?"), check.message, FW_TR("Cancel"),
                            FW_TR("Upload"), false, Sheet::Icon::Warning))
            return false;
        *confirmed = true;
    }
    return true;
}

void FakelibTab::askAdd()
{
    const QString path = QFileDialog::getOpenFileName(window(), FW_TR("Choose a .prx / .sprx library overlay"), QString(), libraryFilter());
    if (path.isEmpty())
        return;
    bool confirmed = false;
    if (!checkWithUser(path, &confirmed))
        return;
    // What is on the console now decides the question, not the list on screen:
    // that one is empty until Refresh and may be old.
    const bool started = runOnConsole<fl::Listing>([](ps5::FtpClient& c, const CancellationToken& t) { return fl::list(c, t); },
                                                   [this, path, confirmed](const Result<fl::Listing>& r) {
                                                       if (!r.ok()) {
                                                           m_status->setText(FW_TR("Could not list the fakelib folder: %1").arg(r.error().message));
                                                           setTextColor(m_status, color::danger_hi);
                                                           emit toastRequested(Tone::Err, FW_TR("Overlay not added"), r.error().message);
                                                           return;
                                                       }
                                                       showListing(r.value());
                                                       if (confirmSameName(r->overlays, QFileInfo(path).fileName()))
                                                           addFile(path, confirmed);
                                                   });
    if (!started && lastNotStarted() == NotStarted::Busy)
        emit toastRequested(Tone::Info, FW_TR("Overlay not added"), whyNotStarted());
}

bool FakelibTab::confirmSameName(const std::vector<fl::Overlay>& overlays, const QString& name)
{
    for (const fl::Overlay& o : overlays) {
        if (o.name != name)
            continue;
        const QString text = o.enabled ? FW_TR("%1 is already enabled on the console. Replace it with the chosen file?").arg(Sheet::monoSpan(name))
                                       : FW_TR("A disabled %1 is on the console. The new one is added as enabled and the disabled copy stays.").arg(Sheet::monoSpan(name));
        if (!Sheet::confirm(window(), o.enabled ? FW_TR("Replace %1?").arg(name) : FW_TR("Add %1?").arg(name), text,
                            FW_TR("Cancel"), o.enabled ? FW_TR("Replace") : FW_TR("Add"), o.enabled, Sheet::Icon::Warning))
            return false;
    }
    return true;
}

void FakelibTab::addFile(const QString& localPath, bool confirmed)
{
    const QString name = QFileInfo(localPath).fileName();
    runAndRefresh(FW_TR("%1 added").arg(name), FW_TR("%1 not added").arg(name), [localPath, confirmed](ps5::FtpClient& c, const CancellationToken& t) {
        return fl::add(c, localPath, confirmed, t);
    });
}

void FakelibTab::askReplace()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    const QString path = QFileDialog::getOpenFileName(window(), FW_TR("Choose the replacement for %1").arg(target.name), QString(), libraryFilter());
    if (path.isEmpty())
        return;
    bool confirmed = false;
    if (!checkWithUser(path, &confirmed))
        return;
    replaceOverlay(target, path, confirmed); // the overlay the dialog named, whatever is selected now
}

void FakelibTab::replaceSelected(const QString& localPath, bool confirmed)
{
    const int r = selectedRow();
    if (r >= 0)
        replaceOverlay(m_listing.overlays[size_t(r)], localPath, confirmed);
}

void FakelibTab::replaceOverlay(const fl::Overlay& target, const QString& localPath, bool confirmed)
{
    runAndRefresh(FW_TR("%1 replaced").arg(target.name), FW_TR("%1 not replaced").arg(target.name), [target, localPath, confirmed](ps5::FtpClient& c, const CancellationToken& t) {
        return fl::replace(c, target, localPath, confirmed, t);
    });
}

void FakelibTab::askRemove()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    if (!Sheet::confirm(window(), FW_TR("Remove %1 from the PS5?").arg(target.name),
                        FW_TR("Deletes %1. There is no recycle bin on the console; this cannot be undone. Disable keeps the file instead.")
                            .arg(Sheet::monoSpan(target.path())),
                        FW_TR("Keep"), FW_TR("Remove")))
        return;
    removeOverlay(target); // the overlay the question named, whatever is selected now
}

void FakelibTab::removeSelected()
{
    const int r = selectedRow();
    if (r >= 0)
        removeOverlay(m_listing.overlays[size_t(r)]);
}

void FakelibTab::removeOverlay(const fl::Overlay& target)
{
    runAndRefresh(FW_TR("%1 removed").arg(target.name), FW_TR("%1 not removed").arg(target.name), [target](ps5::FtpClient& c, const CancellationToken&) { return fl::remove(c, target); });
}

void FakelibTab::toggleSelected()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    const bool enable = !target.enabled;
    runAndRefresh((enable ? FW_TR("%1 enabled") : FW_TR("%1 disabled")).arg(target.name),
                  (enable ? FW_TR("%1 not enabled") : FW_TR("%1 not disabled")).arg(target.name),
                  [target, enable](ps5::FtpClient& c, const CancellationToken&) { return fl::setEnabled(c, target, enable); });
}

void FakelibTab::applyDemo()
{
    fl::Listing demo;
    const QDateTime t = QDateTime::currentDateTimeUtc().addDays(-3);
    demo.overlays = {{QStringLiteral("libSceAmpr.sprx"), true, 211 * 1024, t},
                     {QStringLiteral("libSceAppContent.sprx"), true, 98 * 1024, t.addSecs(-7200)},
                     {QStringLiteral("libScePlayGo.sprx"), false, 143 * 1024, t.addDays(-10)},
                     {QStringLiteral("libSceSaveData.prx"), true, 64 * 1024, t.addDays(-1)}};
    showListing(demo);
    m_table->selectRow(2);
}

void FakelibTab::clearDemo()
{
    m_listing = {};
    m_table->setRowCount(0);
    m_table->hide();
    m_empty->show();
    m_status->clear();
    updateActions();
}

} // namespace fw::pages
