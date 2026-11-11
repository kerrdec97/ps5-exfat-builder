#include "app/pages/DumpCheckSheet.h"

#include "app/pages/Page.h"
#include "app/ui/Icons.h"
#include "core/FileUtil.h"
#include "core/Tr.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QStyledItemDelegate>
#include <QTimer>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

enum Role { PathRole = Qt::UserRole, MessageRole, FindingRole, GroupRole };

struct Group {
    QString severity; // as in the details
    QString countKey; // details.counts
    QString title;
    Tone tone;
};

const std::vector<Group>& groups()
{
    static const std::vector<Group> g = {
        {QStringLiteral("error"), QStringLiteral("errors"), FW_TR_NOOP("Problems"), Tone::Err},
        {QStringLiteral("warning"), QStringLiteral("warnings"), FW_TR_NOOP("Warnings"), Tone::Warn},
        {QStringLiteral("info"), QStringLiteral("infos"), FW_TR_NOOP("Notes"), Tone::Info},
    };
    return g;
}

QJsonObject countsOf(const QJsonObject& d) { return d.value(QStringLiteral("counts")).toObject(); }
QJsonObject factsOf(const QJsonObject& d) { return d.value(QStringLiteral("facts")).toObject(); }

// The sheet shows its texts translated; the copied report is English, as
// problem reports are. `shown` says which one a text is for.
QString shownOr(const QString& english, bool shown) { return shown ? i18n::translate(english) : english; }

// "3 files": `one` and `many` are whole phrases with %1 for the number.
QString plural(qint64 n, const QString& one, const QString& many, bool shown)
{
    return shownOr(n == 1 ? one : many, shown).arg(formatCount(n));
}

// "PPSA01325 · v01.002.000 · PS5"
QString gameLine(const QJsonObject& facts, bool shown)
{
    QStringList parts;
    const QString id = facts.value(QStringLiteral("titleId")).toString();
    const QString version = facts.value(QStringLiteral("contentVersion")).toString();
    const QString platform = facts.value(QStringLiteral("platform")).toString();
    if (!id.isEmpty())
        parts << id;
    if (!version.isEmpty())
        parts << QStringLiteral("v%1").arg(version);
    if (!platform.isEmpty() && platform != QLatin1String("Unknown"))
        parts << platform;
    return parts.isEmpty() ? shownOr(FW_TR_NOOP("Not known (no readable param.json)"), shown) : parts.join(QStringLiteral(" · "));
}

// "37 fake-signed · 1 plain ELF"
QString executablesLine(const QJsonObject& counts, bool shown)
{
    static const std::vector<std::pair<QString, QString>> classes = {
        {QStringLiteral("fake-signed"), FW_TR_NOOP("%1 fake-signed")}, {QStringLiteral("plain-elf"), FW_TR_NOOP("%1 plain ELF")},
        {QStringLiteral("encrypted"), FW_TR_NOOP("%1 encrypted")},     {QStringLiteral("compressed"), FW_TR_NOOP("%1 compressed")},
        {QStringLiteral("truncated"), FW_TR_NOOP("%1 cut short")},     {QStringLiteral("malformed"), FW_TR_NOOP("%1 not valid")},
        {QStringLiteral("empty"), FW_TR_NOOP("%1 empty")},             {QStringLiteral("unknown"), FW_TR_NOOP("%1 unknown format")},
        {QStringLiteral("unreadable"), FW_TR_NOOP("%1 unreadable")},
    };
    const QJsonObject exec = counts.value(QStringLiteral("executables")).toObject();
    QStringList parts;
    for (const auto& [key, text] : classes)
        if (const qint64 n = exec.value(key).toInteger(); n > 0)
            parts << shownOr(text, shown).arg(formatCount(n));
    return parts.isEmpty() ? shownOr(FW_TR_NOOP("None found"), shown) : parts.join(QStringLiteral(" · "));
}

// "eboot.bin SDK 4.50 · param.json SDK 9.00 · needs firmware 9.00"
QString sdkLine(const QJsonObject& facts, bool shown)
{
    QStringList parts;
    if (const QString v = facts.value(QStringLiteral("ebootSdk")).toString(); !v.isEmpty())
        parts << shownOr(FW_TR_NOOP("eboot.bin SDK %1"), shown).arg(v);
    if (const QString v = facts.value(QStringLiteral("paramSdk")).toString(); !v.isEmpty())
        parts << shownOr(FW_TR_NOOP("param.json SDK %1"), shown).arg(v);
    if (const QString v = facts.value(QStringLiteral("requiredFirmware")).toString(); !v.isEmpty())
        parts << shownOr(FW_TR_NOOP("needs firmware %1"), shown).arg(v);
    return parts.join(QStringLiteral(" · "));
}

QString totalsLine(const QJsonObject& counts, bool shown)
{
    return QStringLiteral("%1 · %2 · %3")
        .arg(plural(counts.value(QStringLiteral("files")).toInteger(), FW_TR_NOOP("%1 file"), FW_TR_NOOP("%1 files"), shown),
             plural(counts.value(QStringLiteral("folders")).toInteger(), FW_TR_NOOP("%1 folder"), FW_TR_NOOP("%1 folders"), shown),
             formatBytes(counts.value(QStringLiteral("bytes")).toInteger()));
}

// The findings of one severity, in the order of the details.
QList<QJsonObject> findingsOf(const QJsonObject& d, const QString& severity)
{
    QList<QJsonObject> out;
    for (const QJsonValue& v : d.value(QStringLiteral("findings")).toArray())
        if (const QJsonObject f = v.toObject(); f.value(QStringLiteral("severity")).toString() == severity)
            out << f;
    return out;
}

QString dumpFolder(const QJsonObject& d, const QString& folder)
{
    return folder.isEmpty() ? d.value(QStringLiteral("folder")).toString() : folder;
}

const QString kReadOnlyLine = FW_TR_NOOP("The check reads the dump and changes nothing.");

constexpr int kGroupRow = size::badge + 12; // a heading with its badge
constexpr int kFindingRow = size::rowCompact;

// A path that is too long keeps both ends; a sentence keeps its beginning.
class FindingDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->textElideMode = index.column() == 1 ? Qt::ElideRight : Qt::ElideMiddle;
    }
};

} // namespace

QString DumpCheckSheet::reportText(const QJsonObject& d, const QString& summary, const QString& title, const QString& folder,
                                   const QDateTime& checkedAt)
{
    const QJsonObject counts = countsOf(d), facts = factsOf(d);
    QStringList lines;
    lines << QStringLiteral("DUMP CHECK");
    lines << QStringLiteral("Game:    %1").arg(title.isEmpty() ? gameLine(facts, false) : QStringLiteral("%1 (%2)").arg(title, gameLine(facts, false)));
    lines << QStringLiteral("Folder:  %1").arg(nativePath(dumpFolder(d, folder)));
    if (checkedAt.isValid())
        lines << QStringLiteral("Checked: %1").arg(humanTime(checkedAt));
    lines << QStringLiteral("Result:  %1").arg(summary);
    lines << QStringLiteral("         %1").arg(totalsLine(counts, false));
    lines << QString() << QStringLiteral("FACTS");
    lines << QStringLiteral("  Executables: %1").arg(executablesLine(counts, false));
    if (const QString state = facts.value(QStringLiteral("backportSummary")).toString(); !state.isEmpty())
        lines << QStringLiteral("  Backport state: %1").arg(state);
    if (const QString sdk = sdkLine(facts, false); !sdk.isEmpty())
        lines << QStringLiteral("  SDK: %1").arg(sdk);
    for (const Group& g : groups()) {
        const QList<QJsonObject> list = findingsOf(d, g.severity);
        const qint64 n = std::max<qint64>(counts.value(g.countKey).toInteger(), list.size());
        if (n <= 0)
            continue;
        lines << QString() << QStringLiteral("%1 (%2)").arg(g.title.toUpper(), formatCount(n));
        for (const QJsonObject& f : list) {
            const QString path = f.value(QStringLiteral("path")).toString();
            const QString message = f.value(QStringLiteral("message")).toString();
            lines << (path.isEmpty() ? QStringLiteral("  %1").arg(message) : QStringLiteral("  %1   %2").arg(path, message));
            const QJsonArray paths = f.value(QStringLiteral("paths")).toArray();
            for (const QJsonValue& p : paths)
                lines << QStringLiteral("      %1").arg(p.toString());
            if (const qint64 more = f.value(QStringLiteral("count")).toInteger() - paths.size(); more > 0 && !paths.isEmpty())
                lines << QStringLiteral("      … and %1 more").arg(formatCount(more));
        }
        if (n > list.size())
            lines << QStringLiteral("  … and %1 more that are counted but not listed").arg(formatCount(n - list.size()));
    }
    lines << QString() << kReadOnlyLine;
    return lines.join(u'\n') + u'\n';
}

DumpCheckSheet::DumpCheckSheet(QWidget* host, const QJsonObject& d, const QString& summary, const QString& title, const QString& folder,
                               const QDateTime& checkedAt)
    : Sheet(host, title.isEmpty() ? FW_TR("Dump check") : FW_TR("Dump check: %1").arg(title), Icon::None, 860),
      m_folder(dumpFolder(d, folder))
{
    m_report = reportText(d, summary, title, folder, checkedAt);
    const QJsonObject counts = countsOf(d), facts = factsOf(d);
    const qint64 errors = counts.value(QStringLiteral("errors")).toInteger();
    const qint64 warnings = counts.value(QStringLiteral("warnings")).toInteger();

    // What was checked. Paths, names and messages come from the dump: shown literally.
    auto* path = new ElideLabel(nativePath(m_folder), Qt::ElideMiddle, this);
    path->setFont(font::mono());
    keepLeftToRight(path);
    setTextColor(path, color::fg_1);
    path->setToolTip(plainTip(nativePath(m_folder)));
    body()->addWidget(path);

    // The verdict, in the app's badges.
    auto* badges = new QWidget(this);
    auto* flow = new FlowLayout(badges, space::sm, space::sm);
    if (errors == 0 && warnings == 0)
        flow->addWidget(new Badge(FW_TR("No problems found"), Tone::Ok, badges));
    if (errors > 0)
        flow->addWidget(new Badge(plural(errors, FW_TR_NOOP("%1 problem"), FW_TR_NOOP("%1 problems"), true), Tone::Err, badges));
    if (warnings > 0)
        flow->addWidget(new Badge(plural(warnings, FW_TR_NOOP("%1 warning"), FW_TR_NOOP("%1 warnings"), true), Tone::Warn, badges));
    auto* totals = caption(checkedAt.isValid() ? FW_TR("%1 · checked %2").arg(totalsLine(counts, true), humanTime(checkedAt)) : totalsLine(counts, true), badges);
    totals->setWordWrap(false);
    totals->setFixedHeight(size::badge);
    flow->addWidget(totals);
    body()->addWidget(badges);

    // The facts.
    auto* factsCard = new Card(this, Card::Variant::Inset);
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(space::lg);
    grid->setVerticalSpacing(space::xs);
    grid->setColumnStretch(1, 1);
    const auto fact = [&](const QString& name, const QString& value) {
        if (value.isEmpty())
            return;
        const int row = grid->rowCount();
        auto* n = label(name, TextStyle::Caption, color::fg_4, factsCard);
        n->setFixedHeight(font::lineBody);
        grid->addWidget(n, row, 0, Qt::AlignLeft | Qt::AlignTop);
        auto* v = label(value, TextStyle::Body, color::fg_1, factsCard);
        v->setWordWrap(true);
        grid->addWidget(v, row, 1);
    };
    fact(FW_TR("Game"), gameLine(facts, true));
    fact(FW_TR("Executables"), executablesLine(counts, true));
    fact(FW_TR("Backport state"), facts.value(QStringLiteral("backportSummary")).toString());
    fact(QStringLiteral("SDK"), sdkLine(facts, true));
    factsCard->body()->addLayout(grid);
    body()->addWidget(factsCard);

    // The findings, grouped by severity.
    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({FW_TR("Where"), FW_TR("What")});
    m_list->header()->setFixedHeight(size::tableHeader);
    m_list->header()->setStretchLastSection(true);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_list->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_list->setColumnWidth(0, 290);
    m_list->setItemDelegate(new FindingDelegate(m_list));
    m_list->setRootIsDecorated(true);
    m_list->setIndentation(16);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setAccessibleName(FW_TR("Findings"));
    m_list->setTextElideMode(Qt::ElideMiddle);
    int listHeight = 0;
    for (const Group& g : groups()) {
        const QList<QJsonObject> list = findingsOf(d, g.severity);
        const qint64 n = std::max<qint64>(counts.value(g.countKey).toInteger(), list.size());
        if (n <= 0)
            continue;
        const QString heading = QStringLiteral("%1 · %2").arg(i18n::translate(g.title), formatCount(n));
        auto* top = new QTreeWidgetItem(m_list);
        top->setFirstColumnSpanned(true);
        top->setFlags(Qt::ItemIsEnabled); // a heading, not a finding
        top->setData(0, GroupRole, heading);
        top->setData(0, Qt::AccessibleTextRole, heading);
        top->setSizeHint(0, QSize(0, kGroupRow));
        auto* holder = new QWidget(m_list);
        auto* hh = hbox(holder, space::sm, QMargins(2, 0, 2, 0));
        hh->addWidget(new Badge(heading, g.tone, holder), 0, Qt::AlignVCenter);
        hh->addStretch(1);
        m_list->setItemWidget(top, 0, holder);
        listHeight += kGroupRow;
        for (const QJsonObject& f : list) {
            const QString where = f.value(QStringLiteral("path")).toString();
            const QString message = f.value(QStringLiteral("message")).toString();
            auto* it = new QTreeWidgetItem(top);
            it->setText(0, where.isEmpty() ? FW_TR("The dump") : where);
            if (where.isEmpty())
                it->setForeground(0, color::fg_3);
            else
                it->setFont(0, font::mono());
            it->setToolTip(0, plainTip(where));
            it->setText(1, message);
            it->setToolTip(1, plainTip(message));
            it->setData(0, PathRole, where);
            it->setData(0, MessageRole, message);
            it->setData(0, FindingRole, true);
            it->setSizeHint(0, QSize(0, kFindingRow));
            listHeight += kFindingRow;
            // A finding about many files: the first of them underneath, folded away.
            const QJsonArray paths = f.value(QStringLiteral("paths")).toArray();
            for (const QJsonValue& p : paths) {
                auto* sub = new QTreeWidgetItem(it);
                sub->setText(0, p.toString());
                sub->setFont(0, font::mono());
                sub->setFirstColumnSpanned(true);
                sub->setToolTip(0, plainTip(p.toString()));
                sub->setData(0, PathRole, p.toString());
                sub->setData(0, MessageRole, message);
                sub->setData(0, FindingRole, true);
                sub->setSizeHint(0, QSize(0, kFindingRow));
            }
            if (const qint64 more = f.value(QStringLiteral("count")).toInteger() - paths.size(); more > 0 && !paths.isEmpty()) {
                auto* rest = new QTreeWidgetItem(it);
                rest->setText(0, FW_TR("… and %1 more").arg(formatCount(more)));
                rest->setForeground(0, color::fg_3);
                rest->setFirstColumnSpanned(true);
                rest->setFlags(Qt::ItemIsEnabled);
            }
        }
        if (n > list.size()) {
            auto* more = new QTreeWidgetItem(top);
            more->setText(0, FW_TR("… and %1 more that are counted but not listed").arg(formatCount(n - list.size())));
            more->setForeground(0, color::fg_3);
            more->setFirstColumnSpanned(true);
            more->setFlags(Qt::ItemIsEnabled);
            more->setSizeHint(0, QSize(0, kFindingRow));
            listHeight += kFindingRow;
        }
        top->setExpanded(true);
    }
    // As tall as its rows need, up to what the window leaves.
    const int hostH = host ? host->height() : 900;
    const int maxH = std::clamp(hostH - 520, 150, 330);
    m_list->setFixedHeight(std::clamp(m_list->header()->height() + listHeight + 2 * m_list->frameWidth() + 4, 110, maxH));
    m_list->setVisible(m_list->topLevelItemCount() > 0); // a clean dump has its verdict and its facts
    body()->addWidget(m_list);

    // The selected finding in full, and where it is.
    auto* detailRow = new QWidget(this);
    auto* dh = hbox(detailRow, space::md);
    m_detail = caption({}, detailRow);
    m_detail->setWordWrap(true);
    m_detail->setMinimumHeight(2 * font::lineCaption);
    m_detail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    dh->addWidget(m_detail, 1);
    m_show = button(FW_TR("Show in folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), detailRow);
    setButtonSmall(m_show, true);
    dh->addWidget(m_show, 0, Qt::AlignTop);
    body()->addWidget(detailRow);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &DumpCheckSheet::updateSelection);
    connect(m_show, &QPushButton::clicked, this, [this] {
        const QString target = selectedPath();
        if (!target.isEmpty())
            openInFileManager(target);
    });
    updateSelection();
    detailRow->setVisible(m_list->topLevelItemCount() > 0);
    body()->addWidget(caption(i18n::translate(kReadOnlyLine), this));

    auto* copy = addButton(FW_TR("Copy report"), Role::Safe);
    QObject::disconnect(copy, &QPushButton::clicked, this, nullptr); // copying keeps the sheet open
    connect(copy, &QPushButton::clicked, this, [this, copy] {
        if (auto* cb = QApplication::clipboard())
            cb->setText(m_report);
        copy->setText(FW_TR("Copied"));
        QTimer::singleShot(1500, copy, [copy] { copy->setText(FW_TR("Copy report")); });
    });
    // Repairing is the Backport tab's work: restore the originals, or backport again.
    auto* backport = addButton(FW_TR("Backport…"), Role::Safe);
    backport->setToolTip(FW_TR("Open this game in the Backport tab (restore originals, or backport again)"));
    connect(backport, &QPushButton::clicked, this, &DumpCheckSheet::backportRequested);
    addButton(FW_TR("Close"), Role::Primary);
}

QString DumpCheckSheet::selectedPath() const
{
    const QList<QTreeWidgetItem*> selected = m_list->selectedItems();
    if (selected.isEmpty() || !selected.first()->data(0, FindingRole).toBool())
        return {};
    const QString where = selected.first()->data(0, PathRole).toString();
    return where.isEmpty() ? m_folder : QDir(m_folder).filePath(where);
}

void DumpCheckSheet::updateSelection()
{
    const QList<QTreeWidgetItem*> selected = m_list->selectedItems();
    const bool finding = !selected.isEmpty() && selected.first()->data(0, FindingRole).toBool();
    if (finding) {
        const QString where = selected.first()->data(0, PathRole).toString();
        const QString message = selected.first()->data(0, MessageRole).toString();
        m_detail->setText(where.isEmpty() ? message : QStringLiteral("%1\n%2").arg(nativePath(where), message));
        setTextColor(m_detail, color::fg_1);
    } else {
        m_detail->setText(FW_TR("Select a finding to read it in full and to show its file."));
        setTextColor(m_detail, color::fg_3);
    }
    m_show->setEnabled(finding);
    m_show->setToolTip(finding ? plainTip(nativePath(selectedPath())) : FW_TR("Select a finding first"));
    if (isVisible())
        refit(); // a long message takes another line
}

QStringList DumpCheckSheet::groupTitles() const
{
    QStringList out;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        out << m_list->topLevelItem(i)->data(0, GroupRole).toString();
    return out;
}

QJsonObject DumpCheckSheet::demoDetails()
{
    const auto finding = [](const char* severity, const char* code, const char* path, const char* message) {
        QJsonObject f{{QStringLiteral("severity"), QString::fromLatin1(severity)},
                      {QStringLiteral("code"), QString::fromLatin1(code)},
                      {QStringLiteral("message"), QString::fromUtf8(message)}};
        if (*path)
            f.insert(QStringLiteral("path"), QString::fromLatin1(path));
        return f;
    };
    QJsonObject empties = finding("info", "empty-files", "", "3 files are empty (0 bytes).");
    empties.insert(QStringLiteral("count"), 3);
    empties.insert(QStringLiteral("paths"), QJsonArray{QStringLiteral("data/cache/a.tmp.bin"), QStringLiteral("data/cache/b.tmp.bin"), QStringLiteral("data/log/empty.txt")});
    QJsonObject fakeSigned = finding("info", "exec-fake-signed", "", "36 executables are fake-signed.");
    fakeSigned.insert(QStringLiteral("count"), 36);
    fakeSigned.insert(QStringLiteral("paths"), QJsonArray());
    const QJsonArray findings{
        finding("error", "exec-encrypted", "sce_module/libSceNpToolkit2.prx", "Encrypted retail executable: it cannot run from a dump. The dump was not decrypted."),
        finding("error", "exec-truncated", "sce_module/libfmod.prx", "The file is cut short: segment 1 ends after the end of the file."),
        finding("warning", "fakelib-eboot-not-signed", "eboot.bin",
                "fakelib is present but eboot.bin is not fake-signed (it is a plain ELF): a half-finished or half-restored backport."),
        finding("warning", "exec-unsigned", "eboot.bin", "Plain ELF, not signed: fine for a build, but it will not load on a console as it is."),
        finding("warning", "icon-missing", "sce_sys/icon0.png", "sce_sys/icon0.png is missing: the game shows without its cover."),
        fakeSigned,
        finding("info", "fakelib-present", "fakelib", "fakelib holds 3 files: libSceAgc.sprx, libSceAgcDriver.sprx, libScePsml.sprx."),
        finding("info", "backport-state", "", "Looks backported (fakelib present; executables target SDK 4.50)."),
        empties,
    };
    const QJsonObject counts{{QStringLiteral("files"), 18442},
                             {QStringLiteral("folders"), 1204},
                             {QStringLiteral("bytes"), 48500000000LL},
                             {QStringLiteral("executables"), QJsonObject{{QStringLiteral("total"), 39}, {QStringLiteral("fake-signed"), 36}, {QStringLiteral("plain-elf"), 1},
                                                                          {QStringLiteral("encrypted"), 1}, {QStringLiteral("truncated"), 1}}},
                             {QStringLiteral("errors"), 2},
                             {QStringLiteral("warnings"), 3},
                             {QStringLiteral("infos"), 4},
                             {QStringLiteral("listed"), 9},
                             {QStringLiteral("truncated"), false}};
    const QJsonObject facts{{QStringLiteral("platform"), QStringLiteral("PS5")},
                            {QStringLiteral("titleId"), QStringLiteral("PPSA01325")},
                            {QStringLiteral("contentVersion"), QStringLiteral("01.002.000")},
                            {QStringLiteral("title"), QStringLiteral("Astro Bot")},
                            {QStringLiteral("paramSdk"), QStringLiteral("9.00")},
                            {QStringLiteral("requiredFirmware"), QStringLiteral("9.00")},
                            {QStringLiteral("ebootSdk"), QStringLiteral("4.50")},
                            {QStringLiteral("backportState"), QStringLiteral("backported")},
                            {QStringLiteral("backportSummary"), QStringLiteral("Looks backported (fakelib present; executables target SDK 4.50).")}};
    return QJsonObject{{QStringLiteral("folder"), QStringLiteral("D:\\Games\\PPSA01325 Astro Bot (01.002.000)")},
                       {QStringLiteral("findings"), findings},
                       {QStringLiteral("counts"), counts},
                       {QStringLiteral("facts"), facts}};
}

} // namespace fw::pages
