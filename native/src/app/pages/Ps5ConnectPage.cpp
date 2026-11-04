#include "app/pages/Ps5ConnectPage.h"

#include "app/AppContext.h"
#include "app/pages/ps5/DiagnosticsTab.h"
#include "app/pages/ps5/FindConsole.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/LogView.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/PayloadSender.h"
#include "connect/ps5/RemotePath.h"
#include "connect/ps5/Y2jb.h"
#include "core/FileUtil.h"

#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIntValidator>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

// ---- klog model ---------------------------------------------------------------------

KlogModel::KlogModel(QObject* parent) : QAbstractTableModel(parent) {}
int KlogModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_lines.size()); }
int KlogModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant KlogModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_lines.size()))
        return {};
    const ps5::KlogLine& l = m_lines[size_t(index.row())];
    if (role == TagRole)
        return int(l.tag);
    if (role == Qt::ToolTipRole)
        return l.text;
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case TimeColumn: return l.received.toString(QStringLiteral("HH:mm:ss.zzz"));
    case TagColumn: {
        QString t = ps5::klogTagName(l.tag);
        if (!t.isEmpty())
            t[0] = t[0].toUpper();
        return t;
    }
    case TextColumn: return l.text;
    default: return {};
    }
}

QVariant KlogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case TimeColumn: return QStringLiteral("Time");
    case TagColumn: return QStringLiteral("Tag");
    case TextColumn: return QStringLiteral("Line");
    default: return {};
    }
}

void KlogModel::append(const ps5::KlogLine& line)
{
    if (m_lines.size() >= m_max) {
        beginRemoveRows({}, 0, 0);
        if (m_lines.front().tag == ps5::KlogTag::Warning) --m_warnings;
        if (m_lines.front().tag == ps5::KlogTag::Error) --m_errors;
        m_lines.pop_front();
        endRemoveRows();
    }
    const int row = int(m_lines.size());
    beginInsertRows({}, row, row);
    m_lines.push_back(line);
    if (line.tag == ps5::KlogTag::Warning) ++m_warnings;
    if (line.tag == ps5::KlogTag::Error) ++m_errors;
    endInsertRows();
}

void KlogModel::clear()
{
    beginResetModel();
    m_lines.clear();
    m_warnings = m_errors = 0;
    endResetModel();
}

QString KlogModel::allText() const
{
    QString out;
    for (const auto& l : m_lines)
        out += l.received.toString(QStringLiteral("HH:mm:ss.zzz")) + u'\t' + ps5::klogTagName(l.tag) + u'\t' + l.text + u'\n';
    return out;
}

const ps5::KlogLine* KlogModel::lineAt(int row) const
{
    return row >= 0 && row < int(m_lines.size()) ? &m_lines[size_t(row)] : nullptr;
}

namespace {

class KlogDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        return {QStyledItemDelegate::sizeHint(o, i).width(), size::rowCompact};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        p->save();
        if (option.state & QStyle::State_Selected)
            p->fillRect(option.rect, color::accent_lo);
        p->fillRect(QRect(option.rect.left(), option.rect.bottom(), option.rect.width(), 1), color::border_1);
        const auto tag = ps5::KlogTag(index.data(KlogModel::TagRole).toInt());
        const LogLevel level = tag == ps5::KlogTag::Error ? LogLevel::Error : tag == ps5::KlogTag::Warning ? LogLevel::Warning
                             : tag == ps5::KlogTag::Debug ? LogLevel::Debug : LogLevel::Info;
        const QRect r = option.rect.adjusted(index.column() == 0 ? 12 : 8, 0, -8, 0);
        if (index.column() == KlogModel::TagColumn) {
            p->drawPixmap(QRect(r.left(), r.center().y() - size::iconSm / 2, size::iconSm, size::iconSm),
                          iconPixmap(levelIcon(level), size::iconSm, level == LogLevel::Info ? color::accent_hi : level == LogLevel::Debug ? color::fg_3 : toneMark(level == LogLevel::Error ? Tone::Err : Tone::Warn), p->device()->devicePixelRatioF()));
            p->setFont(font::caption());
            p->setPen(level == LogLevel::Info || level == LogLevel::Debug ? color::fg_2 : levelColor(level));
            p->drawText(r.adjusted(size::iconSm + 6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());
        } else {
            p->setFont(font::mono());
            p->setPen(index.column() == KlogModel::TimeColumn ? color::fg_4 : levelColor(level));
            p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(font::mono()).elidedText(index.data().toString(), Qt::ElideRight, r.width()));
        }
        p->restore();
    }
};

QTableWidgetItem* monoItem(const QString& text, const QColor& c = color::fg_1)
{
    auto* it = new QTableWidgetItem(text);
    it->setFont(font::mono());
    it->setForeground(c);
    return it;
}

} // namespace

// ---- page ---------------------------------------------------------------------------

Ps5ConnectPage::Ps5ConnectPage(AppContext& ctx, JobQueueModel& queueModel, PayloadStore& payloads, QWidget* parent)
    : Page(ctx, QStringLiteral("ps5"), QStringLiteral("PS5 connect"),
           QStringLiteral("Browse the console over FTP, send payloads and follow the kernel log."), parent),
      m_payloads(payloads)
{
    Q_UNUSED(queueModel);
    m_session = new ConsoleSession(this);

    m_host = textInput(QStringLiteral("192.168.1.40"), true, this);
    m_host->setFixedWidth(160);
    m_host->setAccessibleName(QStringLiteral("Console IP"));
    m_host->setText(ctx.settings().value(ps5::keys::kConsoleIp).toString());
    connect(m_host, &QLineEdit::editingFinished, this, [this] { (void)m_ctx.settings().setValue(ps5::keys::kConsoleIp, m_host->text().trimmed()); });
    m_ftpPort = textInput(QStringLiteral("2121"), true, this);
    m_ftpPort->setFixedWidth(72);
    m_ftpPort->setValidator(new QIntValidator(1, 65535, m_ftpPort));
    m_ftpPort->setAccessibleName(QStringLiteral("FTP port"));
    m_ftpPort->setToolTip(QStringLiteral("FTP port (ftpsrv 2121, etaHEN 1337)"));
    m_ftpPort->setText(QString::number(settingPort(ps5::keys::kFtpPort, 2121)));
    connect(m_ftpPort, &QLineEdit::editingFinished, this, [this] { (void)m_ctx.settings().setValue(ps5::keys::kFtpPort, m_ftpPort->text().toInt()); });
    m_connect = button(QStringLiteral("Connect"), ButtonVariant::Primary, QStringLiteral("plug"), this);
    m_find = new FindConsoleButton(ctx, this); // fills ps5.consoleIp / ps5.ftpPort, which the fields follow
    header()->addAction(m_host);
    header()->addAction(m_ftpPort);
    header()->addAction(m_find);
    header()->addAction(m_connect);
    connect(m_connect, &QPushButton::clicked, this, &Ps5ConnectPage::toggleConnection);
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant& v) {
        if (key == ps5::keys::kConsoleIp && m_host->text().trimmed() != v.toString())
            m_host->setText(v.toString());
        if (key == ps5::keys::kFtpPort && m_ftpPort->text().toInt() != v.toInt())
            m_ftpPort->setText(v.toString());
    });

    auto* statusRow = new QWidget(this);
    auto* sh = hbox(statusRow, space::sm);
    m_badge = new Badge(QStringLiteral("Not connected"), Tone::Neutral, statusRow);
    m_badge->setIconName(QStringLiteral("plug"));
    sh->addWidget(m_badge);
    m_welcome = caption(QStringLiteral("Connect to browse the console. Ports for payloads, the kernel log and the installer are in Settings › PS5 connection."), statusRow);
    sh->addWidget(m_welcome, 1);
    content()->addWidget(statusRow);

    m_tabs = new QTabBar(this);
    // Stable keys in the tab data: other pages open a tab by key, never by position.
    const std::pair<QString, QString> tabs[] = {{QStringLiteral("ftp"), QStringLiteral("FTP browser")},
                                                {QStringLiteral("payloads"), QStringLiteral("Payloads")},
                                                {QStringLiteral("klog"), QStringLiteral("Kernel log")},
                                                {QStringLiteral("y2jb"), QStringLiteral("Y2JB")},
                                                {QStringLiteral("mount"), QStringLiteral("Mount config")},
                                                {QStringLiteral("diagnostics"), QStringLiteral("Diagnostics")}};
    for (const auto& [key, label] : tabs)
        m_tabs->setTabData(m_tabs->addTab(label), key);
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setProperty("fwNoRing", true);
    auto* tabWrap = new QWidget(this);
    auto* tl = vbox(tabWrap, 0);
    tl->addWidget(m_tabs);
    tl->addWidget(hairline(tabWrap, true));
    content()->addWidget(tabWrap);
    m_stack = new QStackedWidget(this);
    m_stack->setProperty("fwNoRing", true);
    content()->addWidget(m_stack);
    connect(m_tabs, &QTabBar::currentChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_stack->addWidget(buildFtpTab());
    m_stack->addWidget(buildPayloadsTab());
    m_stack->addWidget(buildKlogTab());
    m_stack->addWidget(buildY2jbTab());
    m_stack->addWidget(buildMountTab());
    m_diagnostics = new DiagnosticsTab(ctx, {[this] { return host(); }, [this] { return ftpPort(); }, [this] { return m_remoteDir; }}, m_stack);
    m_stack->addWidget(m_diagnostics);

    connect(m_session, &ConsoleSession::connected, this, [this](const QString& welcome) {
        setConnectedUi(true, welcome.simplified());
        listRemote(m_remoteDir.isEmpty() ? m_ctx.settings().value(ps5::keys::kGamesFolder).toString() : m_remoteDir);
        m_session->siteAvail();
    });
    connect(m_session, &ConsoleSession::disconnected, this, [this](const QString& reason) { setConnectedUi(false, reason); });
    connect(m_session, &ConsoleSession::listed, this, &Ps5ConnectPage::onListed);
    connect(m_session, &ConsoleSession::operationDone, this, [this](const QString&, bool ok, const QString& message) {
        emit toastRequested(ok ? Tone::Ok : Tone::Err, ok ? QStringLiteral("Done") : QStringLiteral("The console refused"), message);
        if (ok)
            listRemote(m_remoteDir);
    });
    connect(m_session, &ConsoleSession::freeSpace, this, [this](const QString& text) {
        m_remoteFree->setText(text.isEmpty() ? QString() : QStringLiteral("SITE AVAIL: %1").arg(text));
    });
    connect(m_session, &ConsoleSession::mountConfigLoaded, this, [this](int tool, bool ok, const QString& text, const QString& message) {
        if (tool != int(mountTool()))
            return;
        if (ok && !text.isEmpty())
            mountSetValues(ps5::parseMountConfig(mountTool(), text));
        else if (ok)
            mountLoadDefaults();
        m_mountStatus->setText(message);
        setTextColor(m_mountStatus, ok ? color::fg_3 : color::danger_hi);
    });
    connect(m_session, &ConsoleSession::mountConfigPushed, this, [this](int, bool ok, const QString& message) {
        m_mountStatus->setText(message);
        setTextColor(m_mountStatus, ok ? color::success_hi : color::danger_hi);
        emit toastRequested(ok ? Tone::Ok : Tone::Err, ok ? QStringLiteral("Config pushed") : QStringLiteral("Push failed"), message);
    });
    connect(&m_payloads, &PayloadStore::changed, this, &Ps5ConnectPage::rebuildPayloads);
    rebuildPayloads();
    setConnectedUi(false, {});
}

Ps5ConnectPage::~Ps5ConnectPage()
{
    if (m_klogClient)
        m_klogClient->stop();
}

int Ps5ConnectPage::settingPort(const QString& key, int fallback) const
{
    const int v = m_ctx.settings().value(key).toInt();
    return v > 0 ? v : fallback;
}

QString Ps5ConnectPage::host() const { return m_host->text().trimmed(); }
int Ps5ConnectPage::ftpPort() const { return m_ftpPort->text().toInt() > 0 ? m_ftpPort->text().toInt() : 2121; }

void Ps5ConnectPage::toggleConnection()
{
    if (m_session->isConnected()) {
        m_session->disconnect();
        return;
    }
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address first."));
        return;
    }
    setInputError(m_host, false);
    (void)m_ctx.settings().setValue(ps5::keys::kConsoleIp, host());
    m_badge->set(QStringLiteral("Connecting…"), Tone::Info);
    m_badge->setIconName(QStringLiteral("loader"));
    m_connect->setEnabled(false);
    m_session->connectTo(host(), quint16(ftpPort()), settingPort(ps5::keys::kConnectTimeout, 10) * 1000,
                         m_ctx.settings().value(ps5::keys::kPassiveMode).toBool());
}

void Ps5ConnectPage::setConnectedUi(bool on, const QString& detail)
{
    m_connect->setEnabled(true);
    m_connect->setText(on ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    setButtonVariant(m_connect, on ? ButtonVariant::Secondary : ButtonVariant::Primary);
    setButtonIcon(m_connect, on ? QStringLiteral("unplug") : QStringLiteral("plug"));
    if (on) {
        m_badge->set(QStringLiteral("Connected · %1").arg(host()), Tone::Ok);
        m_welcome->setText(detail.isEmpty() ? QStringLiteral("FTP session open.") : detail);
    } else if (!detail.isEmpty()) {
        m_badge->set(QStringLiteral("Not connected"), Tone::Err);
        m_welcome->setText(detail);
        m_listing = {};
        m_remote->setRowCount(0);
    } else {
        m_badge->set(QStringLiteral("Not connected"), Tone::Neutral);
        m_badge->setIconName(QStringLiteral("plug"));
        m_welcome->setText(QStringLiteral("Connect to browse the console. Ports for payloads, the kernel log and the installer are in Settings › PS5 connection."));
        m_listing = {};
        m_remote->setRowCount(0);
    }
    const bool canUpload = jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.upload"));
    for (auto* b : {m_btnUpload, m_btnUploadFolder})
        b->setEnabled(on && canUpload);
    for (auto* b : {m_btnNewFolder, m_btnRename, m_btnDelete})
        b->setEnabled(on);
    m_btnDownload->setEnabled(on && jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.download")));
    m_btnUp->setEnabled(on);
    m_btnRefresh->setEnabled(on);
    m_remote->setVisible(on);
    m_remoteEmpty->setVisible(!on);
    m_mountLoad->setEnabled(on);
    m_mountPush->setEnabled(on);
    emit reachabilityChanged(on, host());
    emit statusContextChanged();
}

// ---- FTP browser -------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildFtpTab()
{
    auto* tab = new QWidget(m_stack);
    auto* l = vbox(tab, space::lg);
    auto* card = new Card(tab, Card::Variant::Table);
    auto* h = card->header();
    h->setSpacing(space::sm);
    m_btnUp = new IconButton(QStringLiteral("arrow_up"), QStringLiteral("Up one level"), IconButton::Size::Small, card);
    connect(m_btnUp, &QToolButton::clicked, this, [this] { listRemote(ps5::remoteParent(m_remoteDir)); });
    h->addWidget(m_btnUp);
    m_crumbs = new QWidget(card);
    m_crumbLayout = hbox(m_crumbs, 2);
    h->addWidget(m_crumbs);
    h->addStretch(1);
    m_btnUpload = button(QStringLiteral("Upload…"), ButtonVariant::Secondary, QStringLiteral("upload"), card);
    m_btnUploadFolder = button(QStringLiteral("Upload folder…"), ButtonVariant::Secondary, QStringLiteral("folder_up"), card);
    m_btnDownload = button(QStringLiteral("Download"), ButtonVariant::Secondary, QStringLiteral("download"), card);
    m_btnNewFolder = button(QStringLiteral("New folder"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), card);
    m_btnRename = button(QStringLiteral("Rename"), ButtonVariant::Secondary, QStringLiteral("edit"), card);
    m_btnDelete = button(QStringLiteral("Delete"), ButtonVariant::Secondary, QStringLiteral("trash"), card);
    m_btnRefresh = new IconButton(QStringLiteral("refresh"), QStringLiteral("Refresh"), IconButton::Size::Regular, card);
    for (auto* b : {m_btnUpload, m_btnUploadFolder, m_btnDownload, m_btnNewFolder, m_btnRename, m_btnDelete})
        h->addWidget(b);
    h->addWidget(m_btnRefresh);
    connect(m_btnUpload, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(window(), QStringLiteral("Choose files to send to %1").arg(m_remoteDir));
        if (!files.isEmpty())
            uploadFiles(files);
    });
    connect(m_btnUploadFolder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose a folder to send to %1").arg(m_remoteDir));
        if (!dir.isEmpty())
            uploadFiles({dir});
    });
    connect(m_btnDownload, &QPushButton::clicked, this, &Ps5ConnectPage::downloadSelected);
    connect(m_btnNewFolder, &QPushButton::clicked, this, &Ps5ConnectPage::newFolder);
    connect(m_btnRename, &QPushButton::clicked, this, &Ps5ConnectPage::renameSelected);
    connect(m_btnDelete, &QPushButton::clicked, this, &Ps5ConnectPage::deleteSelected);
    connect(m_btnRefresh, &QToolButton::clicked, this, [this] { listRemote(m_remoteDir); });

    m_remote = new QTableWidget(0, 3, card);
    m_remote->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Modified")});
    m_remote->horizontalHeader()->setFixedHeight(32);
    m_remote->horizontalHeader()->setHighlightSections(false);
    m_remote->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_remote->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_remote->setColumnWidth(1, 110);
    m_remote->setColumnWidth(2, 160);
    m_remote->verticalHeader()->hide();
    m_remote->verticalHeader()->setDefaultSectionSize(size::row);
    m_remote->setShowGrid(false);
    m_remote->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remote->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remote->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remote->setMinimumHeight(10 * size::row + 40);
    m_remote->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_remote, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && row < int(m_listing.entries.size()) && m_listing.entries[size_t(row)].isDir)
            if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(row)].name))
                listRemote(*child);
    });
    connect(m_remote, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        if (!m_remote->itemAt(p))
            return;
        QMenu menu(this);
        const int row = m_remote->itemAt(p)->row();
        const bool dir = row < int(m_listing.entries.size()) && m_listing.entries[size_t(row)].isDir;
        if (dir)
            menu.addAction(QStringLiteral("Open"), this, [this, row] {
                if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(row)].name))
                    listRemote(*child);
            });
        menu.addAction(QStringLiteral("Download"), this, &Ps5ConnectPage::downloadSelected)->setEnabled(m_btnDownload->isEnabled());
        menu.addAction(QStringLiteral("Rename…"), this, &Ps5ConnectPage::renameSelected);
        menu.addAction(QStringLiteral("Copy path"), this, [this] {
            const QStringList sel = selectedRemotePaths();
            if (!sel.isEmpty())
                QGuiApplication::clipboard()->setText(sel.first());
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete…"), this, &Ps5ConnectPage::deleteSelected);
        menu.exec(m_remote->viewport()->mapToGlobal(p));
    });
    connect(m_remote, &QTableWidget::itemSelectionChanged, this, [this] {
        const int n = selectedRemotePaths().size();
        m_btnRename->setEnabled(n == 1 && m_session->isConnected());
        m_btnDelete->setEnabled(n > 0 && m_session->isConnected());
        m_btnDownload->setEnabled(n > 0 && m_session->isConnected() && jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.download")));
        qint64 bytes = 0;
        for (const QModelIndex& i : m_remote->selectionModel()->selectedRows())
            if (i.row() < int(m_listing.entries.size()) && m_listing.entries[size_t(i.row())].size > 0)
                bytes += m_listing.entries[size_t(i.row())].size;
        m_remoteFooter->setText(n > 0 ? QStringLiteral("%1 items · %2 selected (%3)").arg(m_listing.entries.size()).arg(n).arg(formatBytes(bytes))
                                      : QStringLiteral("%1 items").arg(m_listing.entries.size()));
    });
    card->body()->addWidget(m_remote);
    m_remoteEmpty = new EmptyState(QStringLiteral("plug"), QStringLiteral("Not connected"),
                                   QStringLiteral("Enter the console's IP address and connect. ftpsrv or etaHEN's FTP server must be running on the PS5."), card);
    auto* b = m_remoteEmpty->addAction(QStringLiteral("Connect"), ButtonVariant::Primary);
    connect(b, &QPushButton::clicked, this, &Ps5ConnectPage::toggleConnection);
    card->body()->addWidget(m_remoteEmpty);
    auto* f = card->footer();
    m_remoteFooter = caption({}, card);
    m_destClass = caption({}, card);
    m_remoteFree = label({}, TextStyle::MonoSmall, color::fg_3, card);
    f->addWidget(m_remoteFooter, 1);
    f->addWidget(m_destClass);
    f->addWidget(m_remoteFree);
    l->addWidget(card);
    l->addWidget(caption(QStringLiteral("Drop files anywhere on this page to send them to the open folder. Writes to system folders ask first."), tab));
    l->addStretch(1);
    m_remoteDir = m_ctx.settings().value(ps5::keys::kGamesFolder).toString();
    if (m_remoteDir.isEmpty())
        m_remoteDir = QStringLiteral("/");
    rebuildBreadcrumb();
    return tab;
}

void Ps5ConnectPage::listRemote(const QString& path)
{
    if (!m_session->isConnected())
        return;
    m_remoteFooter->setText(QStringLiteral("Listing…"));
    m_session->list(path.isEmpty() ? QStringLiteral("/") : path);
}

void Ps5ConnectPage::onListed(const RemoteListing& listing)
{
    if (!listing.error.isEmpty()) {
        emit toastRequested(Tone::Err, QStringLiteral("Could not list %1").arg(listing.path), listing.error);
        m_remoteFooter->setText(listing.error);
        return;
    }
    m_listing = listing;
    m_remoteDir = listing.path;
    std::stable_sort(m_listing.entries.begin(), m_listing.entries.end(), [](const ps5::FtpEntry& a, const ps5::FtpEntry& b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    m_remote->setRowCount(0);
    for (const ps5::FtpEntry& e : m_listing.entries) {
        const int r = m_remote->rowCount();
        m_remote->insertRow(r);
        auto* name = monoItem(e.name, e.isDir ? color::fg_0 : color::fg_1);
        name->setIcon(icon(e.isDir ? QStringLiteral("folder") : e.isLink ? QStringLiteral("link") : QStringLiteral("file"), color::fg_3));
        m_remote->setItem(r, 0, name);
        m_remote->setItem(r, 1, monoItem(e.isDir || e.size < 0 ? QStringLiteral("—") : formatBytes(e.size), color::fg_3));
        m_remote->setItem(r, 2, monoItem(e.modified.isValid() ? humanTime(e.modified.toLocalTime()) : QString(), color::fg_3));
    }
    m_remoteFooter->setText(QStringLiteral("%1 items").arg(m_listing.entries.size()));
    const ps5::DestinationInfo d = ps5::classifyDestination(m_remoteDir);
    m_destClass->setText(d.label);
    rebuildBreadcrumb();
    emit statusContextChanged();
}

void Ps5ConnectPage::rebuildBreadcrumb()
{
    while (QLayoutItem* it = m_crumbLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    QString acc;
    auto* root = new Link(QStringLiteral("/"), m_crumbs);
    root->setFont(font::mono());
    connect(root, &Link::activated, this, [this] { listRemote(QStringLiteral("/")); });
    m_crumbLayout->addWidget(root);
    const QStringList parts = m_remoteDir.split(u'/', Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        acc += u'/' + parts[i];
        if (i + 1 == parts.size()) {
            m_crumbLayout->addWidget(label(parts[i], TextStyle::Mono, color::fg_0, m_crumbs));
        } else {
            auto* l = new Link(parts[i], m_crumbs);
            l->setFont(font::mono());
            const QString target = acc;
            connect(l, &Link::activated, this, [this, target] { listRemote(target); });
            m_crumbLayout->addWidget(l);
            m_crumbLayout->addWidget(label(QStringLiteral("/"), TextStyle::Mono, color::fg_4, m_crumbs));
        }
    }
}

QStringList Ps5ConnectPage::selectedRemotePaths() const
{
    QStringList out;
    for (const QModelIndex& i : m_remote->selectionModel()->selectedRows())
        if (i.row() < int(m_listing.entries.size()))
            // Names from the console's listing: one with a slash, a backslash or ".." never becomes a path.
            if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(i.row())].name))
                out << *child;
    return out;
}

bool Ps5ConnectPage::confirmProtected(const QString& path, const QString& action)
{
    if (!ps5::isProtectedRemotePath(path))
        return true;
    return Sheet::confirm(window(), QStringLiteral("Write to a system folder?"), ps5::protectedPathWarning(path, action).toHtmlEscaped(),
                          QStringLiteral("Cancel"), QStringLiteral("Continue anyway"), true, Sheet::Icon::Warning);
}

void Ps5ConnectPage::uploadFiles(const QStringList& localPaths)
{
    if (localPaths.isEmpty())
        return;
    const bool protectedDir = ps5::isProtectedRemotePath(m_remoteDir);
    if (protectedDir && !confirmProtected(m_remoteDir, QStringLiteral("upload")))
        return;
    auto r = jobs::submitFtpUploadMany(m_ctx, host(), ftpPort(), localPaths, m_remoteDir, protectedDir);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Upload not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, QStringLiteral("Upload queued"), QStringLiteral("%1 item%2 → %3").arg(localPaths.size()).arg(localPaths.size() == 1 ? QString() : QStringLiteral("s")).arg(m_remoteDir));
}

void Ps5ConnectPage::downloadSelected()
{
    const QStringList sel = selectedRemotePaths();
    if (sel.isEmpty())
        return;
    const QString dir = QFileDialog::getExistingDirectory(window(), QStringLiteral("Download %1 item%2 to").arg(sel.size()).arg(sel.size() == 1 ? QString() : QStringLiteral("s")));
    if (dir.isEmpty())
        return;
    auto r = jobs::submitFtpDownload(m_ctx, host(), ftpPort(), sel, dir, false);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Download not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, QStringLiteral("Download queued"), nativePath(dir));
}

void Ps5ConnectPage::deleteSelected()
{
    const QStringList sel = selectedRemotePaths();
    if (sel.isEmpty())
        return;
    for (const QString& p : sel)
        if (!confirmProtected(p, QStringLiteral("delete")))
            return;
    QStringList names;
    for (const QString& p : sel)
        names << ps5::remoteFileName(p);
    if (!Sheet::confirm(window(), sel.size() == 1 ? QStringLiteral("Delete %1 from the console?").arg(names.first().toHtmlEscaped()) : QStringLiteral("Delete %1 items from the console?").arg(sel.size()),
                        QStringLiteral("Folders are removed with everything inside them. There is no recycle bin on the PS5."),
                        QStringLiteral("Keep"), QStringLiteral("Delete")))
        return;
    for (const QModelIndex& i : m_remote->selectionModel()->selectedRows())
        if (i.row() < int(m_listing.entries.size())) {
            const auto& e = m_listing.entries[size_t(i.row())];
            if (const auto child = ps5::remoteChild(m_remoteDir, e.name))
                m_session->remove(*child, e.isDir);
        }
}

void Ps5ConnectPage::renameSelected()
{
    const QStringList sel = selectedRemotePaths();
    if (sel.size() != 1)
        return;
    if (!confirmProtected(sel.first(), QStringLiteral("rename")))
        return;
    auto* s = new Sheet(window(), QStringLiteral("Rename on the console"), Sheet::Icon::None);
    auto* name = textInput({}, true, s);
    name->setText(ps5::remoteFileName(sel.first()));
    s->body()->addWidget(field(QStringLiteral("New name"), name, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Rename"), Sheet::Role::Primary);
    name->setFocus();
    name->selectAll();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    if (n.isEmpty() || n == ps5::remoteFileName(sel.first()))
        return;
    if (const QString why = ps5::remoteNameProblem(n); !why.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("Not renamed"), why);
        return;
    }
    m_session->rename(sel.first(), ps5::joinRemote(m_remoteDir, n));
}

void Ps5ConnectPage::newFolder()
{
    if (!confirmProtected(m_remoteDir, QStringLiteral("create a folder in")))
        return;
    auto* s = new Sheet(window(), QStringLiteral("New folder in %1").arg(m_remoteDir), Sheet::Icon::None);
    auto* name = textInput(QStringLiteral("Folder name"), true, s);
    s->body()->addWidget(field(QStringLiteral("Name"), name, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Create"), Sheet::Role::Primary);
    name->setFocus();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    if (n.isEmpty())
        return;
    if (const QString why = ps5::remoteNameProblem(n); !why.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("Folder not created"), why);
        return;
    }
    m_session->mkdir(ps5::joinRemote(m_remoteDir, n));
}

// ---- Payloads ------------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildPayloadsTab()
{
    auto* pay = new QWidget(m_stack);
    auto* pl = new TwoColumnLayout(pay);
    pl->setLeftMax(100000);
    auto* listCard = new Card(pay, Card::Variant::Table);
    auto* lh = listCard->header();
    lh->addWidget(heading(QStringLiteral("Payloads"), TextStyle::Subtitle, listCard));
    lh->addStretch(1);
    lh->addWidget(caption(QStringLiteral("Send to"), listCard));
    m_payloadTarget = new Segmented(listCard);
    m_payloadTarget->addSegment(QStringLiteral("Payload loader"), {}, QStringLiteral("payload"));
    m_payloadTarget->addSegment(QStringLiteral("ShadowMount+"), {}, QStringLiteral("shadowmount"));
    m_payloadTarget->addSegment(QStringLiteral("MicroMount"), {}, QStringLiteral("micromount"));
    m_payloadTarget->setAccessibleName(QStringLiteral("Payload target"));
    lh->addWidget(m_payloadTarget);
    auto* add = button(QStringLiteral("Add payload…"), ButtonVariant::Secondary, QStringLiteral("plus"), listCard);
    lh->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { addOrEditPayload(-1); });
    m_payloadList = new QListWidget(listCard);
    m_payloadList->setMinimumHeight(6 * size::row);
    listCard->body()->addWidget(m_payloadList);
    m_payloadEmpty = new EmptyState(QStringLiteral("rocket"), QStringLiteral("No payloads yet"),
                                    QStringLiteral("Add an .elf or .bin payload. It is sent to the port of the chosen loader (Settings › PS5 connection)."), listCard);
    auto* addFirst = m_payloadEmpty->addAction(QStringLiteral("Add payload…"), ButtonVariant::Primary);
    connect(addFirst, &QPushButton::clicked, this, [this] { addOrEditPayload(-1); });
    listCard->body()->addWidget(m_payloadEmpty);
    auto* warn = new InfoBar(Tone::Warn, listCard);
    warn->setMessage(QStringLiteral("Sending a payload while another is resident can crash the kernel. Reboot between unrelated payloads."));
    listCard->footer()->addWidget(warn, 1);
    pl->setLeft(listCard);
    m_payloadDetail = new Card(pay);
    m_pBadge = new Badge(QStringLiteral("Ready"), Tone::Ok, m_payloadDetail);
    m_payloadDetail->setTitle(QStringLiteral("Payload"), m_pBadge);
    m_pName = label({}, TextStyle::BodyStrong, color::fg_0, m_payloadDetail);
    m_pFile = label({}, TextStyle::Mono, color::fg_3, m_payloadDetail);
    m_pFile->setWordWrap(true);
    m_pNotes = label({}, TextStyle::Body, color::fg_2, m_payloadDetail);
    m_pNotes->setWordWrap(true);
    m_pLastSent = caption({}, m_payloadDetail);
    m_payloadDetail->body()->addWidget(m_pName);
    m_payloadDetail->body()->addWidget(m_pFile);
    m_payloadDetail->body()->addWidget(m_pNotes);
    m_payloadDetail->body()->addWidget(m_pLastSent);
    auto* pa = new QWidget(m_payloadDetail);
    auto* pal = hbox(pa, space::sm);
    m_pRemove = button(QStringLiteral("Remove"), ButtonVariant::Subtle, QStringLiteral("trash"), pa);
    m_pEdit = button(QStringLiteral("Edit…"), ButtonVariant::Secondary, QStringLiteral("edit"), pa);
    m_pSend = button(QStringLiteral("Send"), ButtonVariant::Primary, QStringLiteral("upload"), pa);
    pal->addWidget(m_pRemove);
    pal->addStretch(1);
    pal->addWidget(m_pEdit);
    pal->addWidget(m_pSend);
    m_payloadDetail->body()->addWidget(pa);
    pl->setRight(m_payloadDetail);
    connect(m_payloadList, &QListWidget::currentRowChanged, this, &Ps5ConnectPage::showPayload);
    connect(m_pSend, &QPushButton::clicked, this, [this] { sendPayload(m_currentPayload); });
    connect(m_pEdit, &QPushButton::clicked, this, [this] { addOrEditPayload(m_currentPayload); });
    connect(m_pRemove, &QPushButton::clicked, this, [this] {
        if (m_currentPayload < 0)
            return;
        const auto& e = m_payloads.entries()[size_t(m_currentPayload)];
        if (Sheet::confirm(window(), QStringLiteral("Remove this payload from the list?"),
                           QStringLiteral("%1 is removed from the list only; the file stays where it is.").arg(e.name.toHtmlEscaped()),
                           QStringLiteral("Keep"), QStringLiteral("Remove")))
            m_payloads.remove(m_currentPayload);
    });
    return pay;
}

void Ps5ConnectPage::rebuildPayloads()
{
    const int cur = m_payloadList->currentRow();
    m_payloadList->clear();
    for (const auto& e : m_payloads.entries()) {
        auto* it = new QListWidgetItem(QStringLiteral("%1   %2").arg(e.name, QFileInfo(e.path).fileName()));
        it->setToolTip(nativePath(e.path));
        it->setSizeHint(QSize(0, size::row));
        m_payloadList->addItem(it);
    }
    const bool any = m_payloadList->count() > 0;
    m_payloadList->setVisible(any);
    m_payloadEmpty->setVisible(!any);
    m_payloadDetail->setVisible(any);
    if (any)
        m_payloadList->setCurrentRow(std::clamp(cur, 0, m_payloadList->count() - 1));
    else
        showPayload(-1);
}

void Ps5ConnectPage::showPayload(int index)
{
    m_currentPayload = index;
    if (index < 0 || index >= int(m_payloads.entries().size())) {
        m_payloadDetail->hide();
        return;
    }
    const auto& e = m_payloads.entries()[size_t(index)];
    m_payloadDetail->show();
    m_pName->setText(e.name);
    const QFileInfo fi(e.path);
    m_pFile->setText(fi.exists() ? QStringLiteral("%1 · %2").arg(nativePath(e.path), formatBytes(fi.size())) : nativePath(e.path));
    m_pNotes->setText(e.notes);
    m_pNotes->setVisible(!e.notes.isEmpty());
    m_pLastSent->setText(e.lastSent.isValid() ? QStringLiteral("Last sent %1").arg(humanTime(e.lastSent)) : QStringLiteral("Never sent"));
    m_pBadge->set(fi.exists() ? QStringLiteral("Ready") : QStringLiteral("Missing"), fi.exists() ? Tone::Ok : Tone::Err);
    const bool can = fi.exists() && jobs::isAvailable(m_ctx, QStringLiteral("ps5.payload.send"));
    m_pSend->setEnabled(can);
    m_pSend->setToolTip(!fi.exists() ? QStringLiteral("The file is missing") : can ? QString() : QStringLiteral("Not available in this build"));
}

void Ps5ConnectPage::addOrEditPayload(int index)
{
    PayloadEntry e;
    if (index >= 0 && index < int(m_payloads.entries().size()))
        e = m_payloads.entries()[size_t(index)];
    auto* s = new Sheet(window(), index < 0 ? QStringLiteral("Add payload") : QStringLiteral("Edit payload"), Sheet::Icon::None, size::dialogWide);
    auto* name = textInput(QStringLiteral("etaHEN 2.1b"), false, s);
    name->setText(e.name);
    s->body()->addWidget(field(QStringLiteral("Name"), name, {}, s));
    auto* file = new PathField(PathField::Mode::OpenFile, s);
    file->setFilter(QStringLiteral("Payloads (*.elf *.bin *.jar);;All files (*)"));
    file->setPath(e.path);
    s->body()->addWidget(field(QStringLiteral("File"), file, {}, s));
    auto* notes = new QPlainTextEdit(s);
    notes->setPlainText(e.notes);
    notes->setFixedHeight(72);
    notes->setAccessibleName(QStringLiteral("Notes"));
    s->body()->addWidget(field(QStringLiteral("Notes"), notes, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(index < 0 ? QStringLiteral("Add") : QStringLiteral("Save"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    e.name = name->text().trimmed().isEmpty() ? QFileInfo(file->path()).fileName() : name->text().trimmed();
    e.path = file->path();
    e.notes = notes->toPlainText().trimmed();
    if (e.path.isEmpty())
        return;
    if (index < 0)
        m_payloads.add(e);
    else
        m_payloads.update(index, e);
}

void Ps5ConnectPage::sendPayload(int index)
{
    if (index < 0 || index >= int(m_payloads.entries().size()))
        return;
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address in the header first."));
        return;
    }
    const QString target = m_payloadTarget->currentValue().toString();
    const int port = target == QLatin1String("shadowmount") ? settingPort(ps5::keys::kShadowMountPort, 9021)
                   : target == QLatin1String("micromount") ? settingPort(ps5::keys::kMicroMountPort, 9021)
                                                           : settingPort(ps5::keys::kPayloadPort, 9090);
    const auto e = m_payloads.entries()[size_t(index)];
    auto r = jobs::submitPayload(m_ctx, host(), port, e.path);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Payload not sent"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    m_payloads.markSent(e.path);
    emit toastRequested(Tone::Info, QStringLiteral("Sending payload"), QStringLiteral("%1 → %2:%3").arg(e.name, host()).arg(port));
}

// ---- Kernel log ---------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildKlogTab()
{
    auto* tab = new QWidget(m_stack);
    auto* l = vbox(tab, space::lg);
    auto* card = new Card(tab, Card::Variant::Table);
    auto* h = card->header();
    h->setSpacing(space::md);
    m_klogConnect = button(QStringLiteral("Connect"), ButtonVariant::Primary, QStringLiteral("plug"), card);
    m_klogPause = button(QStringLiteral("Pause"), ButtonVariant::Secondary, QStringLiteral("pause"), card);
    m_klogPause->setEnabled(false);
    m_klogBadge = new Badge(QStringLiteral("Disconnected"), Tone::Neutral, card);
    m_klogBadge->setIconName({});
    h->addWidget(m_klogConnect);
    h->addWidget(m_klogPause);
    h->addWidget(m_klogBadge);
    m_klogFilter = textInput(QStringLiteral("Filter lines"), false, card);
    m_klogFilter->setFixedWidth(240);
    m_klogFilter->setClearButtonEnabled(true);
    h->addWidget(m_klogFilter);
    h->addStretch(1);
    auto* clear = button(QStringLiteral("Clear"), ButtonVariant::Secondary, QStringLiteral("trash"), card);
    auto* exportBtn = button(QStringLiteral("Export…"), ButtonVariant::Secondary, QStringLiteral("download"), card);
    h->addWidget(clear);
    h->addWidget(exportBtn);
    h->addWidget(label(QStringLiteral("Follow"), TextStyle::Body, color::fg_1, card));
    m_klogFollow = new Switch(card);
    m_klogFollow->setChecked(true);
    m_klogFollow->setAccessibleName(QStringLiteral("Follow new lines"));
    h->addWidget(m_klogFollow);

    m_klog = new KlogModel(this);
    m_klogProxy = new QSortFilterProxyModel(this);
    m_klogProxy->setSourceModel(m_klog);
    m_klogProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_klogProxy->setFilterKeyColumn(KlogModel::TextColumn);
    m_klogView = new QTableView(card);
    m_klogView->setModel(m_klogProxy);
    m_klogView->setItemDelegate(new KlogDelegate(m_klogView));
    m_klogView->setShowGrid(false);
    m_klogView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_klogView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_klogView->verticalHeader()->hide();
    m_klogView->verticalHeader()->setDefaultSectionSize(size::rowCompact);
    m_klogView->horizontalHeader()->setFont(font::caption());
    m_klogView->horizontalHeader()->setFixedHeight(32);
    m_klogView->horizontalHeader()->setStretchLastSection(true);
    m_klogView->horizontalHeader()->setHighlightSections(false);
    m_klogView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_klogView->setColumnWidth(KlogModel::TimeColumn, 116);
    m_klogView->setColumnWidth(KlogModel::TagColumn, 92);
    m_klogView->setMinimumHeight(12 * size::rowCompact + 40);
    card->body()->addWidget(m_klogView);
    m_klogStats = caption(QStringLiteral("0 lines"), card);
    card->footer()->addWidget(m_klogStats, 1);
    card->footer()->addWidget(caption(QStringLiteral("klogsrv · nothing is sent to the console · last 20,000 lines kept"), card));
    l->addWidget(card);
    l->addStretch(1);

    connect(m_klogConnect, &QPushButton::clicked, this, &Ps5ConnectPage::klogToggle);
    connect(m_klogPause, &QPushButton::clicked, this, [this] {
        m_klogPaused = !m_klogPaused;
        m_klogPause->setText(m_klogPaused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        setButtonIcon(m_klogPause, m_klogPaused ? QStringLiteral("play") : QStringLiteral("pause"));
        if (!m_klogPaused) {
            for (const auto& line : m_klogHeld)
                m_klog->append(line);
            m_klogHeld.clear();
            if (m_klogFollow->isChecked())
                m_klogView->scrollToBottom();
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] { m_klog->clear(); m_klogHeld.clear(); klogFilterChanged(); });
    connect(exportBtn, &QPushButton::clicked, this, &Ps5ConnectPage::klogExport);
    connect(m_klogFilter, &QLineEdit::textChanged, this, [this](const QString&) { klogFilterChanged(); });
    connect(m_klog, &QAbstractItemModel::rowsInserted, this, [this] {
        if (m_klogFollow->isChecked())
            QMetaObject::invokeMethod(m_klogView, &QTableView::scrollToBottom, Qt::QueuedConnection);
        klogFilterChanged();
    });
    return tab;
}

void Ps5ConnectPage::klogFilterChanged()
{
    m_klogProxy->setFilterFixedString(m_klogFilter->text());
    m_klogStats->setText(QStringLiteral("%1 lines · %2 warnings · %3 errors%4")
                             .arg(formatCount(m_klog->rowCount())).arg(m_klog->warnings()).arg(m_klog->errors())
                             .arg(m_klogFilter->text().isEmpty() ? QString() : QStringLiteral(" · %1 shown").arg(formatCount(m_klogProxy->rowCount()))));
    emit statusContextChanged();
}

void Ps5ConnectPage::klogToggle()
{
    if (m_klogClient) {
        m_klogClient->stop();
        m_klogClient.reset();
        klogState(ps5::KlogState::Stopped, {});
        return;
    }
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address in the header first."));
        return;
    }
    ps5::KlogClient::Options o;
    o.host = host();
    o.port = quint16(settingPort(ps5::keys::kKlogPort, 3232));
    o.connectTimeoutMs = settingPort(ps5::keys::kConnectTimeout, 10) * 1000;
    QPointer<Ps5ConnectPage> self(this);
    m_klogClient = std::make_unique<ps5::KlogClient>(
        o,
        [self](const ps5::KlogLine& line) {
            QMetaObject::invokeMethod(self.data(), [self, line] {
                if (!self)
                    return;
                if (self->m_klogPaused) {
                    self->m_klogHeld.push_back(line);
                    if (self->m_klogHeld.size() > 20000)
                        self->m_klogHeld.pop_front();
                } else {
                    self->m_klog->append(line);
                }
            }, Qt::QueuedConnection);
        },
        [self](ps5::KlogState st, const QString& detail) {
            QMetaObject::invokeMethod(self.data(), [self, st, detail] {
                if (self)
                    self->klogState(st, detail);
            }, Qt::QueuedConnection);
        });
    m_klogClient->start();
    klogState(ps5::KlogState::Connecting, {});
}

void Ps5ConnectPage::klogState(ps5::KlogState state, const QString& detail)
{
    const bool running = m_klogClient != nullptr;
    m_klogConnect->setText(running ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    setButtonVariant(m_klogConnect, running ? ButtonVariant::Secondary : ButtonVariant::Primary);
    setButtonIcon(m_klogConnect, running ? QStringLiteral("unplug") : QStringLiteral("plug"));
    m_klogPause->setEnabled(running);
    Tone tone = Tone::Neutral;
    QString word = ps5::klogStateName(state);
    if (!word.isEmpty())
        word[0] = word[0].toUpper();
    switch (state) {
    case ps5::KlogState::Connected: tone = Tone::Ok; word = QStringLiteral("Connected · %1:%2").arg(host()).arg(settingPort(ps5::keys::kKlogPort, 3232)); break;
    case ps5::KlogState::Connecting:
    case ps5::KlogState::Reconnecting: tone = Tone::Info; break;
    case ps5::KlogState::Disconnected: tone = Tone::Warn; break;
    case ps5::KlogState::Stopped: tone = Tone::Neutral; word = QStringLiteral("Disconnected"); break;
    }
    m_klogBadge->set(word, tone);
    m_klogBadge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
    m_klogBadge->setToolTip(detail);
    emit statusContextChanged();
}

void Ps5ConnectPage::klogExport()
{
    const QString path = QFileDialog::getSaveFileName(window(), QStringLiteral("Export the kernel log"),
                                                      QDir::homePath() + QStringLiteral("/klog_%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
                                                      QStringLiteral("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    auto st = writeFileAtomic(path, (QStringLiteral("PS5 kernel log export %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate)) + m_klog->allText()).toUtf8());
    emit toastRequested(st.ok() ? Tone::Ok : Tone::Err, st.ok() ? QStringLiteral("Kernel log exported") : QStringLiteral("Export failed"), st.ok() ? nativePath(path) : st.error().message);
}

// ---- Y2JB -----------------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildY2jbTab()
{
    auto* tab = new QWidget(m_stack);
    auto* l = vbox(tab, space::lg);
    auto* info = new InfoBar(Tone::Info, tab);
    info->setMessage(QStringLiteral("Y2JB installs the YouTube package through etaHEN's Direct Package Installer and blocks its updates. Needs etaHEN with DPI V2 and the FTP server running on the console."));
    l->addWidget(info);
    auto* regionCard = new Card(tab);
    regionCard->setTitle(QStringLiteral("Region"));
    m_region = new Segmented(regionCard);
    for (const auto r : ps5::allY2jbRegions())
        m_region->addSegment(QStringLiteral("%1 · %2").arg(ps5::y2jbRegionName(r), ps5::y2jbTitleId(r)), {}, ps5::y2jbRegionName(r));
    m_region->setAccessibleName(QStringLiteral("Region"));
    regionCard->body()->addWidget(m_region, 0, Qt::AlignLeft);
    l->addWidget(regionCard);

    auto* dpi = new Card(tab);
    dpi->setTitle(QStringLiteral("Install the YouTube package"));
    m_pkg = new PathField(PathField::Mode::OpenFile, dpi);
    m_pkg->setFilter(QStringLiteral("Packages (*.pkg)"));
    m_pkg->setDialogTitle(QStringLiteral("Choose the YouTube .pkg"));
    dpi->body()->addWidget(field(QStringLiteral("Package"), m_pkg, QStringLiteral("Sent to http://<console>:%1/upload (Settings › PS5 connection › DPI port).").arg(settingPort(ps5::keys::kDpiPort, 12800)), dpi));
    auto* dpiRow = new QWidget(dpi);
    auto* dh = hbox(dpiRow, space::sm);
    m_dpiStatus = caption({}, dpiRow);
    dh->addWidget(m_dpiStatus, 1);
    auto* test = button(QStringLiteral("Test DPI"), ButtonVariant::Secondary, QStringLiteral("plug"), dpiRow);
    m_install = button(QStringLiteral("Install on PS5"), ButtonVariant::Primary, QStringLiteral("upload"), dpiRow);
    m_install->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("ps5.dpi.install")));
    dh->addWidget(test);
    dh->addWidget(m_install);
    dpi->body()->addWidget(dpiRow);
    l->addWidget(dpi);
    connect(test, &QPushButton::clicked, this, [this] {
        if (host().isEmpty()) {
            m_dpiStatus->setText(QStringLiteral("Enter the console's IP address first."));
            return;
        }
        m_dpiStatus->setText(QStringLiteral("Testing…"));
        const QString h = host();
        const quint16 port = quint16(settingPort(ps5::keys::kDpiPort, 12800));
        const int timeout = settingPort(ps5::keys::kConnectTimeout, 10) * 1000;
        QPointer<Ps5ConnectPage> self(this);
        auto* thread = QThread::create([self, h, port, timeout] {
            auto st = ps5::probeTcpPort(h, port, timeout, QStringLiteral("Direct Package Installer"));
            QMetaObject::invokeMethod(self.data(), [self, st] {
                if (!self)
                    return;
                self->m_dpiStatus->setText(st.ok() ? QStringLiteral("DPI answers on port %1.").arg(self->settingPort(ps5::keys::kDpiPort, 12800)) : st.error().message);
                setTextColor(self->m_dpiStatus, st.ok() ? color::success_hi : color::danger_hi);
            }, Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    });
    connect(m_install, &QPushButton::clicked, this, [this] {
        if (m_pkg->path().isEmpty()) {
            m_pkg->setError(QStringLiteral("Choose the YouTube .pkg first."));
            return;
        }
        m_pkg->setError({});
        auto r = jobs::submitDpiInstall(m_ctx, host(), settingPort(ps5::keys::kDpiPort, 12800), m_pkg->path());
        if (!r.ok()) {
            emit toastRequested(Tone::Err, QStringLiteral("Install not started"), r.error().message);
            return;
        }
        m_ctx.queue().holdQueue(false);
        emit toastRequested(Tone::Info, QStringLiteral("Install queued"), QFileInfo(m_pkg->path()).fileName());
    });

    auto* block = new Card(tab);
    block->setTitle(QStringLiteral("Block YouTube updates"));
    auto* warn = new InfoBar(Tone::Warn, block);
    warn->setMessage(QStringLiteral("Patches param.json, appinfo.db and app.db directly on the console over FTP. Every file is backed up to this PC first and read back after the change. Stay in the PS5 Settings menu while it runs."));
    block->body()->addWidget(warn);
    auto* brow = new QWidget(block);
    auto* bh = hbox(brow, space::sm);
    bh->addStretch(1);
    m_autoBlock = button(QStringLiteral("Auto-block over FTP…"), ButtonVariant::Danger, QStringLiteral("shield"), brow);
    m_autoBlock->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("ps5.y2jb.autoBlock")));
    bh->addWidget(m_autoBlock);
    block->body()->addWidget(brow);
    connect(m_autoBlock, &QPushButton::clicked, this, [this] {
        if (host().isEmpty()) {
            emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address in the header first."));
            return;
        }
        const QString reg = region();
        const QString tid = ps5::y2jbTitleId(*ps5::y2jbRegionFromName(reg));
        if (!Sheet::confirm(window(), QStringLiteral("Block YouTube updates for %1?").arg(reg),
                            QStringLiteral("This patches param.json, appinfo.db and app.db for %1 (%2) on your PS5. Make sure you are in the PS5 Settings menu before continuing. Backups go to %3.")
                                .arg(reg, Sheet::monoSpan(tid), Sheet::monoSpan(nativePath(ps5::newConsoleBackupDir()))),
                            QStringLiteral("Not now"), QStringLiteral("Block updates"), true, Sheet::Icon::Warning))
            return;
        auto r = jobs::submitY2jbAutoBlock(m_ctx, host(), ftpPort(), reg, {});
        if (!r.ok()) {
            emit toastRequested(Tone::Err, QStringLiteral("Not started"), r.error().message);
            return;
        }
        m_ctx.queue().holdQueue(false);
        emit toastRequested(Tone::Info, QStringLiteral("Auto-block queued"), QStringLiteral("%1 · %2").arg(reg, tid));
    });
    l->addWidget(block);

    auto* local = new Card(tab);
    local->setTitle(QStringLiteral("Patch local files"));
    m_patchFolder = new PathField(PathField::Mode::Directory, local);
    local->body()->addWidget(field(QStringLiteral("Folder with param.json, appinfo.db and app.db"), m_patchFolder,
                                   QStringLiteral("A .bak copy is written next to each file before it changes; every patch is verified by reading it back."), local));
    auto* prow = new QWidget(local);
    auto* ph = hbox(prow, space::sm);
    ph->addStretch(1);
    m_patchLocal = button(QStringLiteral("Patch local files"), ButtonVariant::Secondary, QStringLiteral("edit"), prow);
    ph->addWidget(m_patchLocal);
    local->body()->addWidget(prow);
    connect(m_patchLocal, &QPushButton::clicked, this, [this] {
        const QString folder = m_patchFolder->path();
        if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
            m_patchFolder->setError(QStringLiteral("Choose the folder that holds the files."));
            return;
        }
        m_patchFolder->setError({});
        const auto reg = *ps5::y2jbRegionFromName(region());
        m_patchLocal->setEnabled(false);
        QPointer<Ps5ConnectPage> self(this);
        (void)QtConcurrent::run([self, folder, reg] {
            auto r = ps5::patchLocalFolder(folder, reg);
            QMetaObject::invokeMethod(self.data(), [self, r] {
                if (!self)
                    return;
                self->m_patchLocal->setEnabled(true);
                if (!r.ok()) {
                    Sheet::message(self->window(), QStringLiteral("Nothing was patched"), r.error().message, Sheet::Icon::Error);
                    return;
                }
                QStringList lines;
                for (const auto& f : r->files)
                    lines << QStringLiteral("%1 %2 — %3").arg(f.status, f.name, f.detail).toHtmlEscaped();
                Sheet::message(self->window(), r->allVerified() ? QStringLiteral("Local files patched") : QStringLiteral("Patched with problems"),
                               Sheet::monoSpan(lines.join(QStringLiteral("<br>"))), r->allVerified() ? Sheet::Icon::Info : Sheet::Icon::Warning);
            }, Qt::QueuedConnection);
        });
    });
    l->addWidget(local);
    l->addStretch(1);
    return tab;
}

QString Ps5ConnectPage::region() const { return m_region->currentValue().toString(); }

// ---- Mount config ---------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildMountTab()
{
    auto* tab = new QWidget(m_stack);
    auto* l = vbox(tab, space::lg);
    auto* card = new Card(tab, Card::Variant::Table);
    auto* h = card->header();
    m_tool = new Segmented(card);
    m_tool->addSegment(QStringLiteral("ShadowMount+"), {}, int(ps5::MountTool::ShadowMountPlus));
    m_tool->addSegment(QStringLiteral("MicroMount"), {}, int(ps5::MountTool::MicroMount));
    m_tool->setAccessibleName(QStringLiteral("Mount tool"));
    h->addWidget(m_tool);
    h->addStretch(1);
    m_mountLoad = button(QStringLiteral("Load from console"), ButtonVariant::Secondary, QStringLiteral("download"), card);
    auto* defaults = button(QStringLiteral("Defaults"), ButtonVariant::Secondary, QStringLiteral("undo"), card);
    auto* validate = button(QStringLiteral("Validate"), ButtonVariant::Secondary, QStringLiteral("check"), card);
    m_mountPush = button(QStringLiteral("Push to console"), ButtonVariant::Primary, QStringLiteral("upload"), card);
    for (auto* b : {m_mountLoad, defaults, validate, m_mountPush})
        h->addWidget(b);
    m_mountTable = new QTableWidget(0, 2, card);
    m_mountTable->setHorizontalHeaderLabels({QStringLiteral("Key"), QStringLiteral("Value")});
    m_mountTable->horizontalHeader()->setFixedHeight(32);
    m_mountTable->horizontalHeader()->setHighlightSections(false);
    m_mountTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_mountTable->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_mountTable->setColumnWidth(0, 240);
    m_mountTable->verticalHeader()->hide();
    m_mountTable->verticalHeader()->setDefaultSectionSize(size::row);
    m_mountTable->setShowGrid(false);
    m_mountTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mountTable->setMinimumHeight(10 * size::row + 40);
    card->body()->addWidget(m_mountTable);
    m_mountStatus = caption(QStringLiteral("Defaults shown. List values (one per line in config.ini) are separated with ; here."), card);
    card->footer()->addWidget(m_mountStatus, 1);
    l->addWidget(card);

    auto* tools = new Card(tab);
    tools->setTitle(QStringLiteral("Loader and shutdown"));
    m_mountElf = new PathField(PathField::Mode::OpenFile, tools);
    m_mountElf->setFilter(QStringLiteral("ELF payloads (*.elf *.bin);;All files (*)"));
    auto* erow = new QWidget(tools);
    auto* eh = hbox(erow, space::sm);
    eh->addWidget(m_mountElf, 1);
    m_mountSendElf = button(QStringLiteral("Send ELF"), ButtonVariant::Secondary, QStringLiteral("upload"), erow);
    eh->addWidget(m_mountSendElf);
    tools->body()->addWidget(field(QStringLiteral("Loader payload"), erow, QStringLiteral("Sent to the tool's ELF port from Settings › PS5 connection."), tools));
    auto* srow = new QWidget(tools);
    auto* sh = hbox(srow, space::md);
    sh->addWidget(caption(QStringLiteral("Safe to unplug writes the STOP sentinel and watches the kernel log until ShadowMount+ confirms every unmount."), srow), 1);
    m_mountStop = button(QStringLiteral("Safe to unplug"), ButtonVariant::Secondary, QStringLiteral("eject"), srow);
    sh->addWidget(m_mountStop);
    tools->body()->addWidget(srow);
    l->addWidget(tools);
    l->addStretch(1);

    connect(m_tool, &Segmented::currentChanged, this, [this](int) {
        mountLoadDefaults();
        m_mountStop->setVisible(mountTool() == ps5::MountTool::ShadowMountPlus);
    });
    connect(m_mountLoad, &QPushButton::clicked, this, &Ps5ConnectPage::mountLoad);
    connect(defaults, &QPushButton::clicked, this, &Ps5ConnectPage::mountLoadDefaults);
    connect(validate, &QPushButton::clicked, this, [this] {
        const QStringList problems = ps5::validateMountConfig(mountTool(), mountValuesFromTable());
        m_mountStatus->setText(problems.isEmpty() ? QStringLiteral("Looks good.") : problems.join(u' '));
        setTextColor(m_mountStatus, problems.isEmpty() ? color::success_hi : color::danger_hi);
    });
    connect(m_mountPush, &QPushButton::clicked, this, &Ps5ConnectPage::mountPush);
    connect(m_mountSendElf, &QPushButton::clicked, this, [this] {
        if (m_mountElf->path().isEmpty()) {
            m_mountElf->setError(QStringLiteral("Choose the loader ELF first."));
            return;
        }
        m_mountElf->setError({});
        const int port = mountTool() == ps5::MountTool::ShadowMountPlus ? settingPort(ps5::keys::kShadowMountPort, 9021) : settingPort(ps5::keys::kMicroMountPort, 9021);
        auto r = jobs::submitPayload(m_ctx, host(), port, m_mountElf->path());
        if (!r.ok())
            emit toastRequested(Tone::Err, QStringLiteral("Payload not sent"), r.error().message);
        else
            m_ctx.queue().holdQueue(false);
    });
    connect(m_mountStop, &QPushButton::clicked, this, [this] {
        auto r = jobs::submitShadowMountStop(m_ctx, host(), ftpPort());
        if (!r.ok())
            emit toastRequested(Tone::Err, QStringLiteral("Not started"), r.error().message);
        else {
            m_ctx.queue().holdQueue(false);
            emit toastRequested(Tone::Info, QStringLiteral("Stopping ShadowMount+"), QStringLiteral("Wait for the green confirmation before unplugging."));
        }
    });
    m_mountStop->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("ps5.shadowmount.stop")));
    m_mountSendElf->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("ps5.payload.send")));
    mountLoadDefaults();
    return tab;
}

ps5::MountTool Ps5ConnectPage::mountTool() const { return ps5::MountTool(m_tool->currentValue().toInt()); }

void Ps5ConnectPage::mountSetValues(const ps5::MountConfigValues& values)
{
    m_mountTable->setRowCount(0);
    QStringList keys = ps5::mountConfigKeys(mountTool());
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!keys.contains(it.key()))
            keys << it.key();
    for (const QString& k : keys) {
        const int r = m_mountTable->rowCount();
        m_mountTable->insertRow(r);
        auto* key = monoItem(k, color::fg_1);
        key->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_mountTable->setItem(r, 0, key);
        QString v = values.value(k);
        v.replace(u'\n', QStringLiteral("; "));
        m_mountTable->setItem(r, 1, monoItem(v, color::fg_0));
    }
}

ps5::MountConfigValues Ps5ConnectPage::mountValuesFromTable() const
{
    ps5::MountConfigValues out;
    for (int r = 0; r < m_mountTable->rowCount(); ++r) {
        QString v = m_mountTable->item(r, 1) ? m_mountTable->item(r, 1)->text().trimmed() : QString();
        v.replace(QStringLiteral("; "), QStringLiteral("\n"));
        v.replace(u';', u'\n');
        if (!v.isEmpty())
            out.insert(m_mountTable->item(r, 0)->text(), v);
    }
    return out;
}

void Ps5ConnectPage::mountLoadDefaults()
{
    mountSetValues(ps5::mountConfigDefaults(mountTool()));
    m_mountStatus->setText(QStringLiteral("Defaults for %1 shown. Load from console to see what the PS5 has.").arg(ps5::mountToolName(mountTool())));
    setTextColor(m_mountStatus, color::fg_3);
}

void Ps5ConnectPage::mountLoad()
{
    if (!m_session->isConnected()) {
        emit toastRequested(Tone::Warn, QStringLiteral("Not connected"), QStringLiteral("Connect to the console first."));
        return;
    }
    m_mountStatus->setText(QStringLiteral("Loading %1…").arg(ps5::mountConfigRemotePath(mountTool())));
    m_session->loadMountConfig(mountTool());
}

void Ps5ConnectPage::mountPush()
{
    if (!m_session->isConnected()) {
        emit toastRequested(Tone::Warn, QStringLiteral("Not connected"), QStringLiteral("Connect to the console first."));
        return;
    }
    const auto values = mountValuesFromTable();
    const QStringList problems = ps5::validateMountConfig(mountTool(), values);
    if (!problems.isEmpty()) {
        m_mountStatus->setText(problems.join(u' '));
        setTextColor(m_mountStatus, color::danger_hi);
        return;
    }
    if (!Sheet::confirm(window(), QStringLiteral("Write %1 to the console?").arg(ps5::mountConfigRemotePath(mountTool())),
                        QStringLiteral("The current file on the PS5 is saved to this PC first, then the new one is written as config.ini.new and swapped in."),
                        QStringLiteral("Cancel"), QStringLiteral("Push"), false, Sheet::Icon::Info))
        return;
    m_mountStatus->setText(QStringLiteral("Pushing…"));
    m_session->pushMountConfig(mountTool(), ps5::renderMountConfig(mountTool(), values));
}

// ---- misc ---------------------------------------------------------------------------------

bool Ps5ConnectPage::acceptsDrop(const QList<QUrl>& urls) const
{
    return !urls.isEmpty() && m_session->isConnected() && jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.upload"));
}

void Ps5ConnectPage::dropUrls(const QList<QUrl>& urls)
{
    QStringList paths;
    for (const auto& u : urls)
        if (!u.toLocalFile().isEmpty())
            paths << u.toLocalFile();
    m_tabs->setCurrentIndex(0);
    uploadFiles(paths);
}

void Ps5ConnectPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("tab"))) {
        // A key ("payloads", "klog"...) or, for older callers, an index.
        const QVariant tab = args.value(QStringLiteral("tab"));
        bool isIndex = false;
        const int index = tab.toInt(&isIndex);
        if (isIndex && tab.typeId() != QMetaType::QString) {
            m_tabs->setCurrentIndex(index);
        } else {
            for (int i = 0; i < m_tabs->count(); ++i)
                if (m_tabs->tabData(i).toString() == tab.toString())
                    m_tabs->setCurrentIndex(i);
        }
    }
    if (args.contains(QStringLiteral("upload"))) {
        m_tabs->setCurrentIndex(0);
        if (m_session->isConnected())
            uploadFiles({args.value(QStringLiteral("upload")).toString()});
        else
            emit toastRequested(Tone::Warn, QStringLiteral("Connect first"), QStringLiteral("Connect to the console, then drop or upload the file."));
    }
}

void Ps5ConnectPage::refresh()
{
    if (m_session->isConnected())
        listRemote(m_remoteDir);
}

QString Ps5ConnectPage::statusContext() const
{
    if (host().isEmpty())
        return QStringLiteral("No console address");
    QStringList parts;
    parts << (m_session->isConnected() ? QStringLiteral("Connected to %1 · FTP %2").arg(host()).arg(ftpPort()) : QStringLiteral("%1 · FTP %2").arg(host()).arg(ftpPort()));
    if (m_klogClient)
        parts << QStringLiteral("Kernel log: %1 lines").arg(formatCount(m_klog->rowCount()));
    return parts.join(QStringLiteral(" · "));
}

QStringList Ps5ConnectPage::demoStates() const
{
    return {QStringLiteral("payloads"), QStringLiteral("klog"), QStringLiteral("y2jb"), QStringLiteral("mount"), QStringLiteral("diagnostics"), QStringLiteral("find")};
}

void Ps5ConnectPage::applyDemoState(const QString& name)
{
    const QStringList tabs = {QStringLiteral("ftp"), QStringLiteral("payloads"), QStringLiteral("klog"), QStringLiteral("y2jb"), QStringLiteral("mount")};
    const int i = tabs.indexOf(name);
    if (i >= 0)
        m_tabs->setCurrentIndex(i);
    if (name == QLatin1String("diagnostics")) {
        m_tabs->setCurrentIndex(m_stack->indexOf(m_diagnostics));
        m_diagnostics->showSpecimen();
    }
    if (name == QLatin1String("find"))
        m_demoSheet = m_find->openSpecimen();
    if (name == QLatin1String("klog") && m_klog->rowCount() == 0) {
        // Specimen lines so the viewer's tags are visible in the evidence.
        for (const QString& t : {QStringLiteral("[KERNEL] sceKernelLoadStartModule: libSceNpTrophy2.sprx"), QStringLiteral("[shadowmount] unmount start /mnt/ext0"),
                                 QStringLiteral("warning: free pages low"), QStringLiteral("panic: specimen line, not from a console")}) {
            ps5::KlogLine l;
            l.received = QDateTime::currentDateTime();
            l.text = t;
            l.tag = ps5::klogTag(t);
            m_klog->append(l);
        }
    }
}

void Ps5ConnectPage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
    m_diagnostics->clearResult();
    m_klog->clear();
    klogFilterChanged();
    m_tabs->setCurrentIndex(0);
}

} // namespace fw::pages
