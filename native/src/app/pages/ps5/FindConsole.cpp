#include "app/pages/ps5/FindConsole.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/Module.h"

#include <QHeaderView>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

QString describeScope(const ps5::DiscoveryOptions& o)
{
    if (!o.hosts.isEmpty())
        return o.hosts.size() == 1 ? o.hosts.first() : tr("%1 addresses").arg(o.hosts.size());
    QStringList nets;
    for (const QString& p : ps5::localSubnetPrefixes())
        nets << p + QStringLiteral(".0/24");
    return nets.join(QStringLiteral(", "));
}

QString portsText(const ps5::DiscoveryOptions& o)
{
    QStringList p;
    for (const quint16 port : o.ports)
        p << QString::number(port);
    return p.size() <= 1 ? tr("port %1").arg(p.join(QString())) : tr("ports %1").arg(p.join(QStringLiteral(" and ")));
}

QTableWidgetItem* cell(const QString& text, const QColor& c, bool mono)
{
    auto* it = new QTableWidgetItem(text);
    if (mono)
        it->setFont(font::mono());
    it->setForeground(c);
    return it;
}

} // namespace

// ---- panel -----------------------------------------------------------------------------

FindConsolePanel::FindConsolePanel(QWidget* parent) : QWidget(parent)
{
    auto* l = vbox(this, space::md);
    auto* top = new QWidget(this);
    auto* tl = vbox(top, space::xs);
    m_bar = new ProgressBar(top);
    m_status = caption({}, top);
    m_status->setWordWrap(true);
    tl->addWidget(m_bar);
    tl->addWidget(m_status);
    l->addWidget(top);

    // Fixed height: the sheet sizes itself once, when it opens.
    auto* area = new QWidget(this);
    area->setFixedHeight(5 * size::row + 40);
    auto* al = vbox(area, 0);
    m_table = new QTableWidget(0, 4, area);
    m_table->setHorizontalHeaderLabels({tr("Address"), tr("Port"), tr("Server greeting"), tr("Answered in")});
    m_table->horizontalHeader()->setFixedHeight(32);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 140);
    m_table->setColumnWidth(1, 64);
    m_table->setColumnWidth(3, 116);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(size::row);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAccessibleName(tr("Consoles found"));
    al->addWidget(m_table);
    m_empty = new EmptyState(QStringLiteral("search"), tr("No PS5 found"), QString(), area);
    m_empty->setCompact(true);
    m_again = m_empty->addAction(tr("Search again"), ButtonVariant::Secondary);
    al->addWidget(m_empty);
    m_empty->hide();
    l->addWidget(area);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { emit selectionChanged(selected().has_value()); });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { emit activated(); });
    connect(m_table, &QAbstractItemView::activated, this, [this](const QModelIndex&) { emit activated(); });
    connect(m_again, &QPushButton::clicked, this, [this] { start(m_options); });
}

FindConsolePanel::~FindConsolePanel()
{
    m_cancel.cancel();
    m_future.waitForFinished();
}

void FindConsolePanel::start(const ps5::DiscoveryOptions& options)
{
    cancel();
    m_options = options;
    m_cancel = CancellationSource();
    const int gen = ++m_generation;
    m_running = true;
    m_results.clear();
    m_table->setRowCount(0);
    m_table->show();
    m_empty->hide();
    m_scope = describeScope(options);
    m_bar->setState(ProgressBar::State::Indeterminate);
    m_bar->show();
    m_status->setText(tr("Looking for an FTP server on %1 at %2…").arg(portsText(options), m_scope.isEmpty() ? tr("this PC's networks") : m_scope));
    setTextColor(m_status, color::fg_3);
    emit selectionChanged(false);
    const CancellationToken token = m_cancel.token();
    m_future = QtConcurrent::run([this, options, token, gen] {
        auto r = ps5::discoverConsoles(options, token, [this, gen](int done, int total, int found) {
            QMetaObject::invokeMethod(this, [this, gen, done, total, found] { onProgress(gen, done, total, found); }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this, gen, r] { onFinished(gen, r); }, Qt::QueuedConnection);
    });
}

void FindConsolePanel::cancel()
{
    if (!m_running)
        return;
    m_cancel.cancel();
    m_running = false;
    ++m_generation; // whatever the old run still posts is ignored
    m_bar->setState(ProgressBar::State::Paused);
    m_status->setText(tr("Search stopped."));
}

void FindConsolePanel::onProgress(int generation, int done, int total, int found)
{
    if (generation != m_generation || !m_running || total <= 0)
        return;
    m_bar->setState(ProgressBar::State::Determinate);
    m_bar->setFraction(double(done) / double(total));
    // "on 192.168.1.0/24" for a sweep; an explicit address list names no network.
    const QString where = m_options.hosts.isEmpty() ? tr(" on %1").arg(m_scope) : QString();
    m_status->setText(tr("Checked %1 of %2 addresses%3 · %4 found").arg(done).arg(total).arg(where).arg(found));
}

void FindConsolePanel::onFinished(int generation, const Result<std::vector<ps5::DiscoveredConsole>>& result)
{
    if (generation != m_generation)
        return;
    m_running = false;
    if (!result.ok()) {
        m_bar->setState(ProgressBar::State::Failed);
        m_results.clear();
        m_table->setRowCount(0);
        m_table->hide();
        m_empty->show();
        m_status->setText(result.error().isCancelled() ? tr("Search stopped.") : result.error().message);
        setTextColor(m_status, result.error().isCancelled() ? color::fg_3 : color::danger_hi);
        emit finished(0);
        return;
    }
    setResults(result.value());
    emit finished(int(m_results.size()));
}

void FindConsolePanel::setResults(std::vector<ps5::DiscoveredConsole> results)
{
    m_results = std::move(results);
    m_bar->setState(ProgressBar::State::Done);
    m_bar->setFraction(1.0);
    m_table->setRowCount(0);
    for (const auto& c : m_results) {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        auto* addr = cell(c.host, color::fg_0, true);
        addr->setIcon(icon(QStringLiteral("ps5"), color::fg_3));
        m_table->setItem(r, 0, addr);
        QStringList ports;
        for (const quint16 p : c.openPorts)
            ports << QString::number(p);
        m_table->setItem(r, 1, cell(ports.join(QStringLiteral(", ")), color::fg_1, true));
        m_table->setItem(r, 2, cell(c.banner.isEmpty() ? tr("No greeting (port open)") : c.banner,
                                    c.banner.isEmpty() ? color::fg_3 : color::fg_1, !c.banner.isEmpty()));
        m_table->setItem(r, 3, cell(c.rttMs >= 0 ? tr("%1 ms").arg(c.rttMs) : QStringLiteral("—"), color::fg_3, true));
    }
    const bool any = !m_results.empty();
    m_table->setVisible(any);
    m_empty->setVisible(!any);
    if (any) {
        m_status->setText(tr("%1 found on %2. Pick the console and choose Use this console.")
                              .arg(m_results.size() == 1 ? tr("1 FTP server") : tr("%1 FTP servers").arg(m_results.size()), m_scope));
        setTextColor(m_status, color::fg_2);
        m_table->selectRow(0);
    } else {
        m_status->setText(tr("Nothing answered on %1 at %2. Start the FTP server on the console (ftpsrv or etaHEN), check that "
                                         "the PS5 and this PC are on the same network, then search again.")
                              .arg(portsText(m_options), m_scope));
        setTextColor(m_status, color::fg_2);
    }
    emit selectionChanged(selected().has_value());
}

std::optional<ps5::DiscoveredConsole> FindConsolePanel::selected() const
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty() || rows.first().row() >= int(m_results.size()))
        return std::nullopt;
    return m_results[size_t(rows.first().row())];
}

QString FindConsolePanel::statusText() const { return m_status->text(); }

void FindConsolePanel::showSpecimen()
{
    cancel();
    m_options = {};
    m_scope = QStringLiteral("192.168.1.0/24 (specimen, not a real search)");
    ps5::DiscoveredConsole a;
    a.host = QStringLiteral("192.168.1.42");
    a.port = 2121;
    a.openPorts = {2121};
    a.banner = tr("FTP server ready (specimen)");
    a.rttMs = 3;
    ps5::DiscoveredConsole b;
    b.host = QStringLiteral("192.168.1.77");
    b.port = 2122;
    b.openPorts = {2122};
    b.rttMs = 11;
    setResults({a, b});
}

// ---- button ----------------------------------------------------------------------------

FindConsoleButton::FindConsoleButton(AppContext& ctx, QWidget* parent) : QPushButton(parent), m_ctx(ctx)
{
    setText(tr("Find my PS5"));
    setButtonVariant(this, ButtonVariant::Secondary);
    setButtonIcon(this, QStringLiteral("search"));
    setToolTip(tr("Search this PC's network for the console's FTP server (ports 2121 and 2122)"));
    setAccessibleName(tr("Find my PS5"));
    connect(this, &QPushButton::clicked, this, [this] { (void)run(); });
}

Sheet* FindConsoleButton::makeSheet(FindConsolePanel*& panel, QPushButton*& use)
{
    auto* s = new Sheet(window(), tr("Find my PS5"), Sheet::Icon::None, size::dialogWide);
    s->addText(tr("Looks for an FTP server at every address of this PC's network, 0.3 s per address, 50 at a time. "
                              "Start the FTP server on the console first; nothing is sent to the consoles found."));
    panel = new FindConsolePanel(s);
    s->body()->addWidget(panel);
    s->addButton(tr("Cancel"), Sheet::Role::Safe);
    use = s->addButton(tr("Use this console"), Sheet::Role::Primary);
    use->setEnabled(false);
    connect(panel, &FindConsolePanel::selectionChanged, use, &QPushButton::setEnabled);
    connect(panel, &FindConsolePanel::activated, s, [s, panel] {
        if (panel->selected())
            s->close(1);
    });
    return s;
}

std::optional<ps5::DiscoveredConsole> FindConsoleButton::run()
{
    FindConsolePanel* panel = nullptr;
    QPushButton* use = nullptr;
    Sheet* s = makeSheet(panel, use);
    panel->start(m_options);
    const int r = s->exec();
    // The sheet (and the panel) are deleted later; read the choice now.
    const auto chosen = r == 1 ? panel->selected() : std::nullopt;
    panel->cancel();
    if (!chosen)
        return std::nullopt;
    (void)m_ctx.settings().setValue(ps5::keys::kConsoleIp, chosen->host);
    (void)m_ctx.settings().setValue(ps5::keys::kFtpPort, int(chosen->port));
    emit consolePicked(chosen->host, chosen->port);
    return chosen;
}

Sheet* FindConsoleButton::openSpecimen()
{
    FindConsolePanel* panel = nullptr;
    QPushButton* use = nullptr;
    Sheet* s = makeSheet(panel, use);
    panel->showSpecimen();
    s->open();
    return s;
}

} // namespace fw::pages
