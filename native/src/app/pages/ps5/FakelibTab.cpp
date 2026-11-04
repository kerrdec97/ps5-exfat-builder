#include "app/pages/ps5/FakelibTab.h"
#include "app/i18n/I18n.h"

#include "app/pages/Page.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>

#include <optional>

namespace fw::pages {

using namespace ui;
using namespace theme;
namespace fl = ps5::fakelib;

namespace {
enum Col { StatusCol = 0, NameCol, SizeCol, ModifiedCol, FolderCol, ColumnCount };
QString kFilter() { return tr("PS5 libraries (*.prx *.sprx);;All files (*)"); }
} // namespace

FakelibTab::FakelibTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("fakelib"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* warn = new InfoBar(Tone::Warn, this);
    warn->setMessage(tr("The fakelib manager only handles library overlays (.prx / .sprx) that ShadowMount+ lays over "
                                    "every game. It cannot replace eboot.bin, game content or files inside images; rebuild those. "
                                    "Changes may need the game remounted or restarted."));
    l->addWidget(warn);

    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->addWidget(heading(tr("Global overlays"), TextStyle::Subtitle, card));
    h->addWidget(monoLabel(fl::kFolder, card, color::fg_3));
    h->addStretch(1);
    m_add = button(tr("Add…"), ButtonVariant::Primary, QStringLiteral("plus"), card);
    m_replace = button(tr("Replace…"), ButtonVariant::Secondary, QStringLiteral("replace"), card);
    m_toggle = button(tr("Disable"), ButtonVariant::Secondary, QStringLiteral("toggle"), card);
    m_remove = button(tr("Remove"), ButtonVariant::Secondary, QStringLiteral("trash"), card);
    m_refresh = new IconButton(QStringLiteral("refresh"), tr("Refresh"), IconButton::Size::Regular, card);
    m_refresh->setToolTip(tr("List the overlays again"));
    for (auto* b : {m_add, m_replace, m_toggle, m_remove})
        h->addWidget(b);
    h->addWidget(m_refresh);

    m_table = new QTableWidget(0, ColumnCount, card);
    m_table->setHorizontalHeaderLabels({tr("State"), tr("Library"), tr("Size"), tr("Modified"), tr("Folder on the PS5")});
    styleToolTable(m_table, 6);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setSectionResizeMode(NameCol, QHeaderView::Stretch);
    m_table->setColumnWidth(StatusCol, 130);
    m_table->setColumnWidth(SizeCol, 100);
    m_table->setColumnWidth(ModifiedCol, 150);
    m_table->setColumnWidth(FolderCol, 300);
    m_table->setAccessibleName(tr("Fakelib overlays"));
    card->body()->addWidget(m_table);
    m_empty = new EmptyState(QStringLiteral("layers"), tr("Overlays not listed yet"),
                             tr("Refresh lists the .prx and .sprx files in /data/shadowmount/fakelib and its disabled folder."), card);
    auto* listBtn = m_empty->addAction(tr("Refresh"), ButtonVariant::Primary);
    card->body()->addWidget(m_empty);
    auto* f = card->footer();
    m_status = caption({}, card);
    f->addWidget(m_status, 1);
    l->addWidget(card);
    l->addWidget(caption(tr("Add and Replace upload under a temporary name and swap it in only when complete, so a dropped "
                                        "connection never leaves a half-written library. Disable moves an overlay to fakelib/disabled."), this));
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
            m_status->setText(tr("Talking to the console…"));
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
                                      m_status->setText(tr("Could not list the fakelib folder: %1").arg(r.error().message));
                                      setTextColor(m_status, color::danger_hi);
                                      emit toastRequested(Tone::Err, tr("Fakelib folder not listed"), r.error().message);
                                      return;
                                  }
                                  showListing(r.value());
                              });
}

void FakelibTab::runAndRefresh(const QString& what, std::function<Status(ps5::FtpClient&, const CancellationToken&)> op)
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
        [this, what](const Result<Outcome>& r) {
            if (!r.ok()) { // no connection
                m_status->setText(tr("%1 failed: %2").arg(what, r.error().message));
                setTextColor(m_status, color::danger_hi);
                emit toastRequested(Tone::Err, tr("%1 failed").arg(what), r.error().message);
                return;
            }
            if (r->listing)
                showListing(*r->listing);
            if (!r->status.ok()) {
                m_status->setText(tr("%1 failed: %2").arg(what, r->status.error().message));
                setTextColor(m_status, color::danger_hi);
                emit toastRequested(Tone::Err, tr("%1 failed").arg(what), r->status.error().message);
            } else {
                emit toastRequested(Tone::Ok, tr("%1 done").arg(what), tr("Remount the game or restart it for the change to apply."));
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
        setBadgeCell(m_table, r, StatusCol, o.enabled ? tr("Enabled") : tr("Disabled"), o.enabled ? Tone::Ok : Tone::Neutral,
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
    QString text = !m_listing.folderExists ? QStringLiteral("/data/shadowmount/fakelib does not exist yet; Add creates it.")
                 : n == 0                  ? tr("No .prx / .sprx overlays yet. Add deploys one.")
                                           : tr("%1 overlay%2 · %3 enabled · %4 disabled").arg(n).arg(n == 1 ? QString() : tr("s")).arg(on).arg(n - on);
    if (!m_listing.skipped.isEmpty())
        text += tr(" · %1 name%2 skipped (not usable safely)").arg(m_listing.skipped.size()).arg(m_listing.skipped.size() == 1 ? QString() : tr("s"));
    m_status->setText(text);
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
    m_toggle->setText(enabled ? tr("Disable") : tr("Enable"));
}

bool FakelibTab::checkWithUser(const QString& localPath, bool* confirmed)
{
    const fl::LocalCheck check = fl::checkLocalFile(localPath);
    *confirmed = false;
    if (check.verdict == fl::Verdict::Reject) {
        Sheet::message(window(), tr("Unsupported file"), check.message.toHtmlEscaped(), Sheet::Icon::Warning);
        return false;
    }
    if (check.verdict == fl::Verdict::Confirm) {
        if (!Sheet::confirm(window(), tr("Upload anyway?"), check.message.toHtmlEscaped(), tr("Cancel"),
                            tr("Upload"), false, Sheet::Icon::Warning))
            return false;
        *confirmed = true;
    }
    return true;
}

void FakelibTab::askAdd()
{
    const QString path = QFileDialog::getOpenFileName(window(), tr("Choose a .prx / .sprx library overlay"), QString(), kFilter());
    if (path.isEmpty())
        return;
    bool confirmed = false;
    if (!checkWithUser(path, &confirmed))
        return;
    const QString name = QFileInfo(path).fileName();
    for (const fl::Overlay& o : m_listing.overlays) {
        if (o.name != name)
            continue;
        const QString text = o.enabled ? tr("%1 is already enabled on the console. Replace it with the chosen file?").arg(Sheet::monoSpan(name))
                                       : tr("A disabled %1 is on the console. The new one is added as enabled and the disabled copy stays.").arg(Sheet::monoSpan(name));
        if (!Sheet::confirm(window(), o.enabled ? tr("Replace %1?").arg(name.toHtmlEscaped()) : tr("Add %1?").arg(name.toHtmlEscaped()), text,
                            tr("Cancel"), o.enabled ? tr("Replace") : tr("Add"), o.enabled, Sheet::Icon::Warning))
            return;
    }
    addFile(path, confirmed);
}

void FakelibTab::addFile(const QString& localPath, bool confirmed)
{
    const QString name = QFileInfo(localPath).fileName();
    runAndRefresh(tr("Adding %1").arg(name), [localPath, confirmed](ps5::FtpClient& c, const CancellationToken& t) {
        return fl::add(c, localPath, confirmed, t);
    });
}

void FakelibTab::askReplace()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    const QString path = QFileDialog::getOpenFileName(window(), tr("Choose the replacement for %1").arg(target.name), QString(), kFilter());
    if (path.isEmpty())
        return;
    bool confirmed = false;
    if (!checkWithUser(path, &confirmed))
        return;
    replaceSelected(path, confirmed);
}

void FakelibTab::replaceSelected(const QString& localPath, bool confirmed)
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    runAndRefresh(tr("Replacing %1").arg(target.name), [target, localPath, confirmed](ps5::FtpClient& c, const CancellationToken& t) {
        return fl::replace(c, target, localPath, confirmed, t);
    });
}

void FakelibTab::askRemove()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    if (!Sheet::confirm(window(), tr("Remove %1 from the PS5?").arg(target.name.toHtmlEscaped()),
                        tr("Deletes %1. There is no recycle bin on the console; this cannot be undone. Disable keeps the file instead.")
                            .arg(Sheet::monoSpan(target.path())),
                        tr("Keep"), tr("Remove")))
        return;
    removeSelected();
}

void FakelibTab::removeSelected()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    runAndRefresh(tr("Removing %1").arg(target.name), [target](ps5::FtpClient& c, const CancellationToken&) { return fl::remove(c, target); });
}

void FakelibTab::toggleSelected()
{
    const int r = selectedRow();
    if (r < 0)
        return;
    const fl::Overlay target = m_listing.overlays[size_t(r)];
    const bool enable = !target.enabled;
    runAndRefresh(enable ? tr("Enabling %1").arg(target.name) : tr("Disabling %1").arg(target.name),
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
