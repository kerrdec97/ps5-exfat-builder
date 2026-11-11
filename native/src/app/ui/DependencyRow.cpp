#include "app/ui/DependencyRow.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "core/Tr.h"

#include <QDir>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QVBoxLayout>

namespace fw::ui {

using namespace theme;

DependencyRow::DependencyRow(const QString& id, const QString& name, QWidget* parent) : QWidget(parent), m_id(id)
{
    setProperty("fwNoRing", true);
    auto* h = hbox(this, space::md, QMargins(0, 6, 0, 6));
    m_icon = new QLabel(this);
    m_icon->setFixedSize(size::icon, size::icon);
    h->addWidget(m_icon, 0, Qt::AlignVCenter);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    m_name = new ElideLabel(name, Qt::ElideRight, this);
    m_name->setFont(font::body());
    setTextColor(m_name, color::fg_1);
    m_detail = new ElideLabel({}, Qt::ElideRight, this);
    m_detail->setFont(font::caption());
    setTextColor(m_detail, color::fg_3);
    col->addWidget(m_name);
    col->addWidget(m_detail);
    auto* textCol = new QWidget(this);
    textCol->setLayout(col);
    textCol->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    textCol->setMinimumWidth(120);
    h->addWidget(textCol, 1);
    m_badge = new Badge(FW_TR("Checking…"), Tone::Neutral, this);
    h->addWidget(m_badge);
    m_fix = new Link(FW_TR("How to fix…"), this);
    m_fix->hide();
    connect(m_fix, &Link::activated, this, [this] { emit fixRequested(m_id, m_info.howToFix, m_info.settingKey); });
    h->addWidget(m_fix);
    setChecking();
}

void DependencyRow::setChecking()
{
    m_icon->setPixmap(iconPixmap(QStringLiteral("loader"), size::icon, color::fg_3, devicePixelRatioF()));
    m_badge->set(FW_TR("Checking…"), Tone::Neutral);
    m_badge->setIconName(QStringLiteral("clock"));
}

void DependencyRow::setInfo(const DependencyInfo& info)
{
    m_info = info;
    // The tool's name and purpose are in the source list; detail and how-to-fix are composed by the probes (English).
    const QString name = i18n::translate(info.name), purpose = i18n::translate(info.purpose);
    m_name->setFullText(info.version.isEmpty() ? name : QStringLiteral("%1 %2").arg(name, info.version));
    QString detail = info.detail;
    if (!info.path.isEmpty())
        detail = detail.isEmpty() ? QDir::toNativeSeparators(info.path) : QStringLiteral("%1 · %2").arg(detail, QDir::toNativeSeparators(info.path));
    if (detail.isEmpty())
        detail = purpose;
    // The font first: the text is elided with it.
    m_detail->setFont(info.path.isEmpty() ? font::caption() : font::mono());
    m_detail->setFullText(detail);
    QStringList tip;
    for (const QString& part : {purpose, info.detail, info.path.isEmpty() ? QString() : QDir::toNativeSeparators(info.path), info.howToFix})
        if (!part.trimmed().isEmpty())
            tip << part.trimmed();
    m_detail->setToolTip(plainTip(tip.join(u'\n')));
    QString iconName;
    QColor c;
    Tone tone = Tone::Neutral;
    QString word;
    switch (info.status) {
    case DependencyStatus::Ready: iconName = QStringLiteral("success"); c = color::success; tone = Tone::Ok; word = FW_TR("Ready"); break;
    case DependencyStatus::Outdated: iconName = QStringLiteral("warning"); c = color::warn; tone = Tone::Warn; word = FW_TR("Outdated"); break;
    case DependencyStatus::Missing: iconName = QStringLiteral("error"); c = color::danger; tone = Tone::Err; word = FW_TR("Not found"); break;
    case DependencyStatus::Error: iconName = QStringLiteral("error"); c = color::danger; tone = Tone::Err; word = FW_TR("Error"); break;
    case DependencyStatus::NotApplicable: iconName = QStringLiteral("circle"); c = color::fg_3; tone = Tone::Neutral; word = FW_TR("Not needed"); break;
    }
    m_icon->setPixmap(iconPixmap(iconName, size::icon, c, devicePixelRatioF()));
    m_badge->set(word, tone);
    m_badge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
    const bool fixable = !info.howToFix.isEmpty() && info.status != DependencyStatus::Ready;
    m_fix->setVisible(fixable);
    m_fix->setText(!info.settingKey.isEmpty() ? FW_TR("Set path…") : FW_TR("How to fix…"));
}

void DependencyRow::setDivider(bool on)
{
    m_divider = on;
    update();
}

void DependencyRow::paintEvent(QPaintEvent*)
{
    if (!m_divider)
        return;
    QPainter p(this);
    p.fillRect(QRect(0, height() - 1, width(), 1), color::border_1);
}

// ---------------------------------------------------------------------------

DependencyList::DependencyList(AppContext& ctx, const QStringList& ids, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_ids(ids)
{
    setProperty("fwNoRing", true);
    auto* l = vbox(this, 0);
    const auto all = m_ctx.dependencies().ids();
    QStringList order;
    for (const auto& id : all)
        if (m_ids.isEmpty() || m_ids.contains(id))
            order << id;
    for (int i = 0; i < order.size(); ++i) {
        const QString id = order[i];
        const DependencyInfo last = m_ctx.dependencies().last(id);
        auto* row = new DependencyRow(id, last.name.isEmpty() ? id : last.name, this);
        row->setDivider(i + 1 < order.size());
        connect(row, &DependencyRow::fixRequested, this, &DependencyList::fixRequested);
        m_rows.insert(id, row);
        m_order << id;
        l->addWidget(row);
    }
    connect(&m_ctx, &AppContext::dependenciesChecked, this, &DependencyList::refresh);
    refresh();
}

void DependencyList::refresh()
{
    m_last.clear();
    // In the order of the rows: the copied report lists the tools the same way every time.
    for (const QString& id : std::as_const(m_order)) {
        DependencyRow* row = m_rows.value(id);
        const DependencyInfo info = m_ctx.dependencies().last(id);
        if (info.id.isEmpty())
            row->setChecking();
        else {
            row->setInfo(info);
            m_last << info;
        }
    }
    emit changed();
}

void DependencyList::checkAgain()
{
    for (auto* row : m_rows)
        row->setChecking();
    m_ctx.checkDependenciesAsync();
}

int DependencyList::readyCount() const
{
    int n = 0;
    for (const auto& i : m_last)
        if (i.status == DependencyStatus::Ready || i.status == DependencyStatus::NotApplicable)
            ++n;
    return n;
}

int DependencyList::problemCount() const
{
    int n = 0;
    for (const auto& i : m_last)
        if (i.status == DependencyStatus::Missing || i.status == DependencyStatus::Outdated || i.status == DependencyStatus::Error)
            ++n;
    return n;
}

QString DependencyList::report() const
{
    QJsonArray arr;
    for (const auto& i : m_last)
        arr.append(i.toJson());
    return QString::fromUtf8(QJsonDocument(arr).toJson());
}

} // namespace fw::ui
