#include "app/pages/ps5/ConsoleToolTab.h"


#include <QHeaderView>

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
    case NotStarted::NoHost: return QStringLiteral("Enter the console's IP address at the top of the page first.");
    case NotStarted::Busy: return QStringLiteral("Wait for the current step to finish.");
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

void setBadgeCell(QTableWidget* table, int row, int column, const QString& text, Tone tone, const QString& icon)
{
    auto* wrap = new QWidget(table);
    auto* h = hbox(wrap, 0, QMargins(8, 0, 8, 0));
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

} // namespace fw::pages
