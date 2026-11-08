#include "app/pages/GameTable.h"

#include "app/ui/Icons.h"
#include "core/FileUtil.h"

#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QScrollBar>

namespace fw::pages {

using namespace ui;
using namespace theme;

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

} // namespace fw::pages
