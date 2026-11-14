#include "app/pages/ps5/ConsoleToolTab.h"
#include "core/Tr.h"


#include <QHeaderView>
#include <QStyleOptionViewItem>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

ConsoleToolTab::ConsoleToolTab(AppContext& ctx, EndpointFn endpointFn, const QString& demoKey, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_endpoint(std::move(endpointFn)), m_demoKey(demoKey),
      m_cancel(std::make_unique<CancellationSource>())
{
    setProperty("fwNoRing", true);
}

ConsoleToolTab::~ConsoleToolTab()
{
    // Every client wait has a timeout and checks the token every 25 ms.
    m_cancel->cancel(QStringLiteral("Closed"));
    m_future.waitForFinished();
}

void ConsoleToolTab::cancelWork()
{
    m_cancel->cancel(QStringLiteral("Cancelled"));
}

bool ConsoleToolTab::beginWork()
{
    if (m_busy)
        return false;
    m_cancel = std::make_unique<CancellationSource>();
    m_busy = true;
    emit busyChanged(true);
    return true;
}

QString ConsoleToolTab::whyNotStarted() const
{
    switch (m_notStarted) {
    case NotStarted::NoHost: return FW_TR("Enter the console's IP address at the top of the page first.");
    case NotStarted::Busy: return FW_TR("Wait for the current step to finish.");
    case NotStarted::None: break;
    }
    return {};
}

void ConsoleToolTab::endWork()
{
    m_busy = false;
    emit busyChanged(false);
}

void ConsoleToolTab::postToUi(std::function<void()> fn)
{
    QPointer<ConsoleToolTab> self(this);
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self, fn]() {
            if (self)
                fn();
        },
        Qt::QueuedConnection);
}

namespace {
constexpr char kToolPrimary[] = "fwToolPrimary";     // on a button: it is its tool's main action
constexpr char kConnectOffered[] = "fwConnectOffered"; // on the page: its console card shows Connect

bool connectOfferedAbove(const QWidget* w)
{
    for (const QWidget* p = w; p; p = p->parentWidget())
        if (const QVariant v = p->property(kConnectOffered); v.isValid())
            return v.toBool();
    return false;
}

void applyToolPrimary(QPushButton* b)
{
    const bool primary = b->property(kToolPrimary).toBool() && !connectOfferedAbove(b);
    setButtonVariant(b, primary ? ButtonVariant::Primary : ButtonVariant::Secondary);
}
} // namespace

void setToolPrimary(QPushButton* button, bool primary)
{
    button->setProperty(kToolPrimary, primary);
    applyToolPrimary(button);
}

void setConnectOffered(QWidget* root, bool offered)
{
    if (root->property(kConnectOffered).isValid() && root->property(kConnectOffered).toBool() == offered)
        return;
    root->setProperty(kConnectOffered, offered);
    for (QPushButton* b : root->findChildren<QPushButton*>())
        if (b->property(kToolPrimary).isValid())
            applyToolPrimary(b);
}

void styleToolTable(QTableWidget* table, int minimumRows)
{
    table->horizontalHeader()->setFixedHeight(32);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(size::row);
    table->setShowGrid(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setMinimumHeight(minimumRows * size::row + 40);
}

QTableWidgetItem* toolItem(const QString& text, bool mono, const QColor& c)
{
    auto* it = new QTableWidgetItem(text);
    if (mono)
        it->setFont(font::mono());
    it->setForeground(c);
    it->setToolTip(text);
    return it;
}

namespace {
constexpr int kBadgeMargin = space::sm; // around a badge in its cell, on each side
} // namespace

void setBadgeCell(QTableWidget* table, int row, int column, const QString& text, Tone tone, const QString& icon)
{
    auto* wrap = new QWidget(table);
    auto* h = hbox(wrap, 0, QMargins(kBadgeMargin, 0, kBadgeMargin, 0));
    auto* badge = new Badge(text, tone, wrap);
    if (!icon.isNull())
        badge->setIconName(icon);
    h->addWidget(badge);
    h->addStretch(1);
    table->setCellWidget(row, column, wrap);
    auto* anchor = new QTableWidgetItem; // the badge draws the text
    anchor->setData(Qt::UserRole, text);
    anchor->setData(Qt::AccessibleTextRole, text);
    anchor->setToolTip(text);
    table->setItem(row, column, anchor);
}

int cellPadding(QTableWidget* table)
{
    table->ensurePolished();
    QStyleOptionViewItem opt;
    opt.initFrom(table->viewport());
    opt.widget = table;
    opt.rect = QRect(0, 0, 200, size::row);
    opt.showDecorationSelected = true;
    return opt.rect.width() - table->style()->subElementRect(QStyle::SE_ItemViewItemText, &opt, table).width();
}

int badgeColumnWidth(QTableWidget* table, int column, const QStringList& badges)
{
    int w = 0;
    for (const QString& b : badges)
        w = std::max(w, badgeWidth(b, true));
    w += 2 * kBadgeMargin + cellPadding(table);
    const QString title = table->horizontalHeaderItem(column) ? table->horizontalHeaderItem(column)->text() : QString();
    return std::max(w, table->horizontalHeader()->fontMetrics().horizontalAdvance(title) + 2 * space::lg);
}

} // namespace fw::pages
