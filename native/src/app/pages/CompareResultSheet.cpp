#include "app/pages/CompareResultSheet.h"

#include "app/ui/Icons.h"
#include "core/FileUtil.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QJsonArray>
#include <QTimer>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

struct Group {
    QString key;      // details list
    QString countKey; // details.counts
    QString title;
    bool dirs = false;
};

const std::vector<Group>& groups()
{
    static const std::vector<Group> g = {
        {QStringLiteral("missing"), QStringLiteral("missing"), QStringLiteral("Only in A (missing from B)"), false},
        {QStringLiteral("missingDirs"), QStringLiteral("missingDirs"), QStringLiteral("Folders only in A"), true},
        {QStringLiteral("extra"), QStringLiteral("extra"), QStringLiteral("Only in B (extra)"), false},
        {QStringLiteral("extraDirs"), QStringLiteral("extraDirs"), QStringLiteral("Folders only in B"), true},
        {QStringLiteral("sizeDifferent"), QStringLiteral("sizeDifferent"), QStringLiteral("Different size"), false},
        {QStringLiteral("contentDifferent"), QStringLiteral("contentDifferent"), QStringLiteral("Different content"), false},
        {QStringLiteral("unreadable"), QStringLiteral("unreadable"), QStringLiteral("Could not be read"), false},
    };
    return g;
}

qint64 countOf(const QJsonObject& d, const QString& key)
{
    return d.value(QStringLiteral("counts")).toObject().value(key).toInteger();
}

// (path, detail) for one list entry.
std::pair<QString, QString> entry(const Group& g, const QJsonValue& v)
{
    if (v.isString())
        return {g.dirs ? v.toString() + u'/' : v.toString(), g.dirs ? QStringLiteral("folder") : QString()};
    const QJsonObject o = v.toObject();
    const QString path = o.value(QStringLiteral("path")).toString();
    if (g.key == u"sizeDifferent")
        return {path, QStringLiteral("A %1 · B %2 bytes")
                          .arg(formatCount(o.value(QStringLiteral("a")).toInteger()), formatCount(o.value(QStringLiteral("b")).toInteger()))};
    if (g.key == u"contentDifferent") {
        const qint64 off = o.value(QStringLiteral("offset")).toInteger();
        return {path, QStringLiteral("differs at byte %1 (0x%2)").arg(formatCount(off)).arg(off, 0, 16)};
    }
    return {path, o.value(QStringLiteral("message")).toString()};
}

QString sideLine(const QJsonObject& side)
{
    return QStringLiteral("%1 · %2 files · %3 folders · %4")
        .arg(side.value(QStringLiteral("kind")).toString(), formatCount(side.value(QStringLiteral("files")).toInteger()),
             formatCount(side.value(QStringLiteral("dirs")).toInteger()), formatBytes(side.value(QStringLiteral("bytes")).toInteger()));
}

QString depthLine(const QJsonObject& d)
{
    return d.value(QStringLiteral("depth")).toString() == u"structure" ? QStringLiteral("Compared names and sizes (contents not read)")
                                                                        : QStringLiteral("Compared names, sizes and contents");
}

} // namespace

QString CompareResultSheet::reportText(const QJsonObject& d, const QString& summary, const QStringList& warnings)
{
    QStringList lines;
    lines << QStringLiteral("IMAGE COMPARISON");
    for (const QString& s : {QStringLiteral("a"), QStringLiteral("b")}) {
        const QJsonObject side = d.value(s).toObject();
        lines << QStringLiteral("%1: %2").arg(s.toUpper(), nativePath(side.value(QStringLiteral("path")).toString()));
        lines << QStringLiteral("   %1").arg(sideLine(side));
    }
    lines << depthLine(d) << QString() << summary;
    const int limit = d.value(QStringLiteral("listLimit")).toInt();
    for (const Group& g : groups()) {
        const qint64 n = countOf(d, g.countKey);
        if (n <= 0)
            continue;
        lines << QString() << QStringLiteral("%1 (%2)").arg(g.title.toUpper(), formatCount(n));
        const QJsonArray list = d.value(g.key).toArray();
        for (const QJsonValue& v : list) {
            const auto [path, detail] = entry(g, v);
            lines << (detail.isEmpty() || g.dirs ? QStringLiteral("  %1").arg(path) : QStringLiteral("  %1   %2").arg(path, detail));
        }
        if (n > list.size())
            lines << QStringLiteral("  … and %1 more (only the first %2 are listed)").arg(formatCount(n - list.size())).arg(limit);
    }
    if (!warnings.isEmpty()) {
        lines << QString() << QStringLiteral("NOTES");
        for (const QString& w : warnings)
            lines << QStringLiteral("  %1").arg(w);
    }
    return lines.join(u'\n') + u'\n';
}

CompareResultSheet::CompareResultSheet(QWidget* host, const QJsonObject& d, const QString& summary, const QStringList& warnings)
    : Sheet(host, QStringLiteral("Comparison result"), Icon::None, size::dialogWide)
{
    m_report = reportText(d, summary, warnings);
    const bool identical = d.value(QStringLiteral("identical")).toBool();

    // What was compared.
    for (const QString& s : {QStringLiteral("a"), QStringLiteral("b")}) {
        const QJsonObject side = d.value(s).toObject();
        auto* row = new QWidget(this);
        auto* h = hbox(row, space::sm);
        auto* tag = label(s.toUpper(), TextStyle::BodyStrong, color::fg_0, row);
        tag->setFixedWidth(16);
        h->addWidget(tag, 0, Qt::AlignTop);
        auto* col = new QVBoxLayout;
        col->setSpacing(0);
        auto* path = new ElideLabel(nativePath(side.value(QStringLiteral("path")).toString()), Qt::ElideMiddle, row);
        path->setFont(font::mono());
        setTextColor(path, color::fg_1);
        path->setToolTip(nativePath(side.value(QStringLiteral("path")).toString()));
        col->addWidget(path);
        col->addWidget(caption(sideLine(side), row));
        h->addLayout(col, 1);
        body()->addWidget(row);
    }

    // Counts.
    auto* badges = new QWidget(this);
    auto* flow = new FlowLayout(badges, space::sm, space::sm);
    if (identical) {
        flow->addWidget(new Badge(QStringLiteral("Identical"), Tone::Ok, badges));
    } else {
        for (const Group& g : groups()) {
            const qint64 n = countOf(d, g.countKey);
            if (n > 0)
                flow->addWidget(new Badge(QStringLiteral("%1 · %2").arg(g.title.section(QStringLiteral(" ("), 0, 0), formatCount(n)),
                                          g.key == u"unreadable" || g.key == u"contentDifferent" ? Tone::Err : Tone::Warn, badges));
        }
    }
    body()->addWidget(badges);
    auto* sum = label(summary, TextStyle::Body, color::fg_1, this);
    sum->setWordWrap(true);
    body()->addWidget(sum);
    body()->addWidget(caption(depthLine(d), this));

    // The differences, grouped.
    if (!identical) {
        m_list = new QTreeWidget(this);
        m_list->setHeaderLabels({QStringLiteral("Path"), QStringLiteral("Detail")});
        m_list->header()->setFixedHeight(32);
        m_list->header()->setStretchLastSection(false);
        m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_list->header()->setSectionResizeMode(1, QHeaderView::Interactive);
        m_list->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_list->setColumnWidth(1, 270);
        m_list->setUniformRowHeights(true);
        m_list->setRootIsDecorated(true);
        m_list->setIndentation(16);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setAccessibleName(QStringLiteral("Differences"));
        m_list->setTextElideMode(Qt::ElideMiddle);
        QFont bold = font::bodyStrong();
        for (const Group& g : groups()) {
            const qint64 n = countOf(d, g.countKey);
            if (n <= 0)
                continue;
            auto* top = new QTreeWidgetItem(m_list);
            top->setText(0, QStringLiteral("%1 · %2").arg(g.title, formatCount(n)));
            top->setFont(0, bold);
            top->setFirstColumnSpanned(true);
            const QJsonArray list = d.value(g.key).toArray();
            for (const QJsonValue& v : list) {
                const auto [path, detail] = entry(g, v);
                auto* it = new QTreeWidgetItem(top);
                it->setText(0, path);
                it->setFont(0, font::mono());
                it->setToolTip(0, path);
                it->setText(1, detail);
                it->setToolTip(1, detail);
                it->setForeground(1, color::fg_3);
            }
            if (n > list.size()) {
                auto* more = new QTreeWidgetItem(top);
                more->setText(0, QStringLiteral("… and %1 more (only the first %2 are listed)")
                                     .arg(formatCount(n - list.size()))
                                     .arg(d.value(QStringLiteral("listLimit")).toInt()));
                more->setForeground(0, color::fg_3);
                more->setFirstColumnSpanned(true);
            }
            top->setExpanded(true);
        }
        // As tall as the rows need, up to what the window leaves.
        int rows = 0;
        for (int i = 0; i < m_list->topLevelItemCount(); ++i)
            rows += 1 + m_list->topLevelItem(i)->childCount();
        const int rowH = std::max(m_list->sizeHintForRow(0), font::lineMono + 4);
        const int hostH = host ? host->height() : 900;
        const int maxH = std::clamp(hostH - 430, 140, 360);
        m_list->setFixedHeight(std::clamp(m_list->header()->height() + rows * rowH + 2 * m_list->frameWidth() + 4, 96, maxH));
        body()->addWidget(m_list);
    }

    // Notes (tree warnings, PFS padding): glyph and text aligned to the top.
    for (const QString& w : warnings.mid(0, 3)) {
        auto* row = new QWidget(this);
        auto* h = hbox(row, 6);
        auto* ic = new QLabel(row);
        ic->setFixedSize(size::iconSm, font::lineCaption);
        ic->setAlignment(Qt::AlignCenter);
        ic->setPixmap(iconPixmap(QStringLiteral("warning"), size::iconSm, toneMark(Tone::Warn), devicePixelRatioF()));
        h->addWidget(ic, 0, Qt::AlignTop);
        auto* t = caption(w, row);
        t->setWordWrap(true);
        t->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        setTextColor(t, toneText(Tone::Warn));
        h->addWidget(t, 1, Qt::AlignTop);
        body()->addWidget(row);
    }
    if (warnings.size() > 3)
        body()->addWidget(caption(QStringLiteral("%1 more notes in Copy report.").arg(warnings.size() - 3), this));

    auto* copy = addButton(QStringLiteral("Copy report"), Role::Safe);
    QObject::disconnect(copy, &QPushButton::clicked, this, nullptr); // copying keeps the sheet open
    connect(copy, &QPushButton::clicked, this, [this, copy] {
        if (auto* cb = QApplication::clipboard())
            cb->setText(m_report);
        copy->setText(QStringLiteral("Copied"));
        QTimer::singleShot(1500, copy, [copy] { copy->setText(QStringLiteral("Copy report")); });
    });
    addButton(QStringLiteral("Close"), Role::Primary);
}

QStringList CompareResultSheet::groupTitles() const
{
    QStringList out;
    if (!m_list)
        return out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        out << m_list->topLevelItem(i)->text(0);
    return out;
}

} // namespace fw::pages
