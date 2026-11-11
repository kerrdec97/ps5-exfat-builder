#include "app/pages/Ps5ConnectPage.h"

#include "app/AppContext.h"
#include "app/pages/ps5/AmprTab.h"
#include "app/pages/ps5/AutoloaderTab.h"
#include "app/pages/ps5/ConsoleExtras.h"
#include "app/pages/ps5/ConsoleHero.h"
#include "app/pages/ps5/DiagnosticsTab.h"
#include "app/pages/ps5/FakelibTab.h"
#include "app/pages/ps5/FindConsole.h"
#include "app/pages/ps5/ManagerTab.h"
#include "app/pages/ps5/ShadowMountTab.h"
#include "app/pages/ps5/ToolStack.h"
#include "app/pages/ps5/TransferPanel.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/LogView.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/PayloadSender.h"
#include "connect/ps5/RemotePath.h"
#include "connect/ps5/SmpApi.h"
#include "connect/ps5/Y2jb.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/fpkg/PackageInspect.h"
#include "jobs/JobQueue.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThread>
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
    case TagColumn:
        // The tag names of klogTagName(), capitalised (the export keeps its own).
        switch (l.tag) {
        case ps5::KlogTag::Error: return FW_TR("Error");
        case ps5::KlogTag::Warning: return FW_TR("Warning");
        case ps5::KlogTag::Debug: return FW_TR("Debug");
        case ps5::KlogTag::Info: break;
        }
        return FW_TR("Info");
    case TextColumn: return l.text;
    default: return {};
    }
}

QVariant KlogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case TimeColumn: return FW_TR("Time");
    case TagColumn: return FW_TR("Tag");
    case TextColumn: return FW_TR("Line");
    default: return {};
    }
}

void KlogModel::append(const ps5::KlogLine& line)
{
    append(std::vector<ps5::KlogLine>{line});
}

void KlogModel::append(const std::vector<ps5::KlogLine>& lines)
{
    if (lines.empty())
        return;
    // One removal and one insertion for the whole batch; of a batch larger than the buffer only the newest lines stay.
    const size_t take = std::min(lines.size(), m_max);
    const size_t over = m_lines.size() + take > m_max ? m_lines.size() + take - m_max : 0;
    if (over > 0) {
        beginRemoveRows({}, 0, int(over) - 1);
        for (size_t i = 0; i < over; ++i) {
            if (m_lines.front().tag == ps5::KlogTag::Warning) --m_warnings;
            if (m_lines.front().tag == ps5::KlogTag::Error) --m_errors;
            m_lines.pop_front();
        }
        endRemoveRows();
    }
    const int row = int(m_lines.size());
    beginInsertRows({}, row, row + int(take) - 1);
    for (size_t i = lines.size() - take; i < lines.size(); ++i) {
        m_lines.push_back(lines[i]);
        if (lines[i].tag == ps5::KlogTag::Warning) ++m_warnings;
        if (lines[i].tag == ps5::KlogTag::Error) ++m_errors;
    }
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
        // The selected row (with its accent bar) is the style's, painted before this.
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

// Payload library rows: name, file and size, last sent on the right.
class PayloadDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    enum Role { FileRole = Qt::UserRole + 1, SentRole, MissingRole };
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {0, 56}; }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(6, 3, -6, -3);
        const bool sel = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        if (sel || hover) {
            p->setPen(sel ? QPen(withAlpha(color::accent_hi, 90), 1) : Qt::NoPen);
            p->setBrush(sel ? color::accent_lo : color::bg_3);
            p->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), radius::control, radius::control);
        }
        const bool missing = index.data(MissingRole).toBool();
        const QRect tile(r.left() + 10, r.center().y() - 16, 32, 32);
        p->setPen(Qt::NoPen);
        p->setBrush(missing ? color::danger_bg : color::bg_4);
        p->drawRoundedRect(tile, radius::sm, radius::sm);
        p->drawPixmap(QRect(tile.center().x() - 8, tile.center().y() - 8, 16, 16),
                      iconPixmap(missing ? QStringLiteral("file_x") : QStringLiteral("rocket"), 16, missing ? color::danger_hi : color::accent_hi, p->device()->devicePixelRatioF()));
        const int x = tile.right() + 12;
        const QString sent = index.data(SentRole).toString();
        const int sentW = QFontMetrics(font::caption()).horizontalAdvance(sent) + 12;
        p->setFont(font::bodyStrong());
        p->setPen(color::fg_0);
        p->drawText(QRect(x, r.top() + 8, r.right() - x - sentW, 18), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(font::bodyStrong()).elidedText(index.data().toString(), Qt::ElideRight, r.right() - x - sentW));
        p->setFont(font::monoSmall());
        p->setPen(missing ? color::danger_hi : color::fg_4);
        p->drawText(QRect(x, r.top() + 28, r.right() - x - 10, 16), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(font::monoSmall()).elidedText(index.data(FileRole).toString(), Qt::ElideMiddle, r.right() - x - 10));
        p->setFont(font::caption());
        p->setPen(color::fg_4);
        p->drawText(QRect(r.right() - sentW, r.top() + 8, sentW - 8, 18), Qt::AlignRight | Qt::AlignVCenter, sent);
        p->restore();
    }
};

// Sortable browser cells: folders stay first in both directions.
class RemoteItem : public QTableWidgetItem {
public:
    enum Role { IndexRole = Qt::UserRole + 1, SortRole, DirRole };
    RemoteItem(const QString& text, bool dir, const QVariant& sortKey) : QTableWidgetItem(text)
    {
        setData(DirRole, dir);
        setData(SortRole, sortKey);
    }
    bool operator<(const QTableWidgetItem& other) const override
    {
        const bool a = data(DirRole).toBool(), b = other.data(DirRole).toBool();
        if (a != b) {
            const bool asc = !tableWidget() || tableWidget()->horizontalHeader()->sortIndicatorOrder() == Qt::AscendingOrder;
            return asc ? a : b;
        }
        const QVariant x = data(SortRole), y = other.data(SortRole);
        if (x.typeId() == QMetaType::QString)
            return x.toString().compare(y.toString(), Qt::CaseInsensitive) < 0;
        return x.toLongLong() < y.toLongLong();
    }
};

QString entryIcon(const ps5::FtpEntry& e)
{
    if (e.isDir)
        return QStringLiteral("folder");
    if (e.isLink)
        return QStringLiteral("link");
    const QString low = e.name.toLower();
    if (low.endsWith(QLatin1String(".exfat")) || low.endsWith(QLatin1String(".ffpkg")) || low.endsWith(QLatin1String(".ffpfs"))
        || low.endsWith(QLatin1String(".ffpfsc")) || low.endsWith(QLatin1String(".img")))
        return QStringLiteral("hard_drive");
    if (low.endsWith(QLatin1String(".pkg")))
        return QStringLiteral("package");
    if (low.endsWith(QLatin1String(".elf")) || low.endsWith(QLatin1String(".bin")) || low.endsWith(QLatin1String(".sprx")) || low.endsWith(QLatin1String(".prx")))
        return QStringLiteral("binary");
    if (low.endsWith(QLatin1String(".png")) || low.endsWith(QLatin1String(".jpg")) || low.endsWith(QLatin1String(".dds")))
        return QStringLiteral("image");
    if (low.endsWith(QLatin1String(".ini")) || low.endsWith(QLatin1String(".json")) || low.endsWith(QLatin1String(".txt")) || low.endsWith(QLatin1String(".log"))
        || low.endsWith(QLatin1String(".lst")))
        return QStringLiteral("file_text");
    return QStringLiteral("file");
}

struct Place {
    const char* icon;
    QString label;
    QString path;
};

QString youtubeHelp(const QString& firmware)
{
    const QString v = ps5::y2jbYoutubeVersionFor(firmware);
    if (firmware.trimmed().isEmpty())
        return FW_TR("Type the firmware to see which YouTube version it needs.");
    return v.isEmpty() ? FW_TR("No Y2JB release covers firmware %1.").arg(firmware.trimmed()) : FW_TR("Firmware %1 needs YouTube %2.").arg(firmware.trimmed(), v);
}

} // namespace

// ---- page ---------------------------------------------------------------------------

Ps5ConnectPage::Ps5ConnectPage(AppContext& ctx, JobQueueModel& queueModel, PayloadStore& payloads, QWidget* parent)
    : Page(ctx, QStringLiteral("ps5"), FW_TR("PS5 connect"),
           FW_TR("Your console's files, payloads, logs and ShadowMount+ in one place."), parent),
      m_payloads(payloads)
{
    Q_UNUSED(queueModel);
    m_session = new ConsoleSession(this);
    // Research §5: 9090 is etaHEN's DPI v1, never a payload loader; move a stored 9090 to 9021 once.
    if (ps5::migrateLegacyPayloadPort(ctx.settings(), QDir(Paths::dataDir()).filePath(QStringLiteral("ps5-payload-port-9021.marker"))))
        QTimer::singleShot(1500, this, [this] {
            emit toastRequested(Tone::Info, FW_TR("Payload port changed to 9021"),
                                FW_TR("9090 is etaHEN's package installer, not a payload loader. elfldr listens on 9021."));
        });

    // ---- console card
    m_hero = new ConsoleHero(ctx, this);
    m_host = m_hero->hostEdit();
    m_ftpPort = m_hero->portEdit();
    m_connect = m_hero->connectButton();
    m_find = m_hero->findButton();
    m_host->setText(ctx.settings().value(ps5::keys::kConsoleIp).toString());
    m_ftpPort->setText(QString::number(settingPort(ps5::keys::kFtpPort, 2121)));
    connect(m_host, &QLineEdit::editingFinished, this, [this] { (void)m_ctx.settings().setValue(ps5::keys::kConsoleIp, m_host->text().trimmed()); });
    connect(m_ftpPort, &QLineEdit::editingFinished, this, [this] { (void)m_ctx.settings().setValue(ps5::keys::kFtpPort, m_ftpPort->text().toInt()); });
    connect(m_host, &QLineEdit::returnPressed, this, [this] {
        // Return connects when there is no connection and none on its way.
        if (!m_session->isConnected() && !m_session->isConnecting())
            toggleConnection();
    });
    connect(m_connect, &QPushButton::clicked, this, &Ps5ConnectPage::toggleConnection);
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant& v) {
        if (key == ps5::keys::kConsoleIp && m_host->text().trimmed() != v.toString()) {
            m_host->setText(v.toString());
            updateHeroSubtitle();
        }
        if (key == ps5::keys::kFtpPort && m_ftpPort->text().toInt() != v.toInt())
            m_ftpPort->setText(v.toString());
        if (key == ps5::keys::kPayloadPort)
            updatePayloadChip();
        if (key == ps5::keys::kGamesFolder) {
            rebuildPlaces(); // the "Games" place follows the setting
            if (!m_session->isConnected() && !m_session->isConnecting() && !v.toString().isEmpty()) {
                m_remoteDir = ps5::normalizeRemotePath(v.toString()); // where the next connection starts
                rebuildBreadcrumb();
            }
        }
    });
    connect(m_hero, &ConsoleHero::chipClicked, this, [this](const QString& key) {
        static const QHash<QString, QString> target = {{QStringLiteral("ftp"), QStringLiteral("ftp")}, {QStringLiteral("klog"), QStringLiteral("klog")},
                                                       {QStringLiteral("payload"), QStringLiteral("payloads")}, {QStringLiteral("shadowmount"), QStringLiteral("mount")},
                                                       {QStringLiteral("installer"), QStringLiteral("install")}};
        showTool(target.value(key, QStringLiteral("ftp")));
    });
    connect(m_hero, &ConsoleHero::checkServicesRequested, this, &Ps5ConnectPage::checkServices);
    content()->addWidget(m_hero);

    // ---- groups
    m_groups = new QTabBar(this);
    m_groups->setObjectName(QStringLiteral("ps5Groups"));
    m_groups->setExpanding(false);
    m_groups->setDrawBase(false);
    m_groups->setProperty("fwNoRing", true);
    m_groups->setIconSize(QSize(size::icon, size::icon));
    auto* tabWrap = new QWidget(this);
    auto* tl = vbox(tabWrap, 0);
    tl->addWidget(m_groups);
    tl->addWidget(hairline(tabWrap, true));
    content()->addWidget(tabWrap);
    m_groupStack = new ToolStack(this);
    m_groupStack->setProperty("fwNoRing", true);
    content()->addWidget(m_groupStack, 1);
    connect(m_groups, &QTabBar::currentChanged, m_groupStack, &ToolStack::setCurrentIndex);
    connect(m_groups, &QTabBar::currentChanged, this, [this](int) { emit statusContextChanged(); });

    const auto endpointFn = [this] { return endpoint(); };
    m_manager = new Ps5ManagerTab(ctx, endpointFn, this);
    m_fakelib = new FakelibTab(ctx, endpointFn, this);
    m_autoloader = new AutoloaderTab(ctx, endpointFn, this);
    m_ampr = new AmprTab(ctx, endpointFn, this);
    m_smp = new ShadowMountTab(ctx, endpointFn, this);
    m_smpLog = new SmpLogTab(ctx, endpointFn, this);
    m_crash = new CrashDumpsTab(ctx, endpointFn, this);
    m_installTab = new InstallTab(ctx, endpointFn, this);
    m_diagnostics = new DiagnosticsTab(ctx, {[this] { return host(); }, [this] { return ftpPort(); }, [this] { return m_remoteDir; }}, this);
    for (ConsoleToolTab* tool : {static_cast<ConsoleToolTab*>(m_manager), static_cast<ConsoleToolTab*>(m_fakelib), static_cast<ConsoleToolTab*>(m_autoloader),
                                 static_cast<ConsoleToolTab*>(m_ampr), static_cast<ConsoleToolTab*>(m_smp), static_cast<ConsoleToolTab*>(m_smpLog),
                                 static_cast<ConsoleToolTab*>(m_crash), static_cast<ConsoleToolTab*>(m_installTab)})
        connect(tool, &ConsoleToolTab::toastRequested, this, &Page::toastRequested);

    addGroup(QStringLiteral("files"), FW_TR("Files"), QStringLiteral("folder_tree"),
             {{QStringLiteral("ftp"), FW_TR("Browser"), QStringLiteral("folder_open"), buildFilesBrowser(this)},
              {QStringLiteral("manager"), FW_TR("Games on the console"), QStringLiteral("game"), m_manager}});
    addGroup(QStringLiteral("payloads"), FW_TR("Payloads"), QStringLiteral("rocket"),
             {{QStringLiteral("payloads"), FW_TR("Payloads"), QStringLiteral("rocket"), buildPayloads(this)}});
    addGroup(QStringLiteral("logs"), FW_TR("Logs"), QStringLiteral("klog"),
             {{QStringLiteral("klog"), FW_TR("Kernel log"), QStringLiteral("klog"), buildKlog(this)},
              {QStringLiteral("smplog"), FW_TR("ShadowMount+ log"), QStringLiteral("file_text"), m_smpLog},
              {QStringLiteral("crashdumps"), FW_TR("Crash dumps"), QStringLiteral("bug"), m_crash}});
    addGroup(QStringLiteral("shadowmount"), QStringLiteral("ShadowMount+"), QStringLiteral("layers"),
             {{QStringLiteral("mount"), FW_TR("Settings"), QStringLiteral("sliders"), m_smp},
              {QStringLiteral("fakelib"), QStringLiteral("Fakelib"), QStringLiteral("layers_2"), m_fakelib},
              {QStringLiteral("autoloader"), FW_TR("Autoloader"), QStringLiteral("list"), m_autoloader},
              {QStringLiteral("ampr"), FW_TR("AMPR index"), QStringLiteral("hash"), m_ampr}});
    addGroup(QStringLiteral("tools"), FW_TR("Console tools"), QStringLiteral("wrench"),
             {{QStringLiteral("y2jb"), QStringLiteral("Y2JB"), QStringLiteral("youtube"), buildY2jb(this)},
              {QStringLiteral("install"), FW_TR("Install package"), QStringLiteral("package_plus"), m_installTab},
              {QStringLiteral("diagnostics"), FW_TR("Diagnostics"), QStringLiteral("gauge"), m_diagnostics}});

    // Every view of the connection follows the session's state.
    connect(m_session, &ConsoleSession::stateChanged, this, &Ps5ConnectPage::applyConnectionState);
    connect(m_session, &ConsoleSession::connected, this, [this] {
        const QString start = m_remoteDir.isEmpty() ? m_ctx.settings().value(ps5::keys::kGamesFolder).toString() : m_remoteDir;
        m_history.clear();
        m_historyIndex = -1;
        m_firstListing = true;
        m_firstListingStep = 0;
        listRemote(start);
        checkServices();
    });
    connect(m_session, &ConsoleSession::listed, this, &Ps5ConnectPage::onListed);
    connect(m_session, &ConsoleSession::operationDone, this, &Ps5ConnectPage::onOperationDone);
    connect(m_session, &ConsoleSession::freeSpace, this, [this](const QString& text) {
        if (m_storage.empty()) {
            // The FTP server's SITE AVAIL answer: bytes when it is a plain number.
            bool isNumber = false;
            const qint64 bytes = text.toLongLong(&isNumber);
            m_remoteFree->setText(text.isEmpty() ? QString()
                                  : isNumber && bytes > 0 ? FW_TR("%1 free").arg(formatBytes(bytes))
                                                          : FW_TR("Free: %1").arg(text));
        }
    });
    connect(&m_payloads, &PayloadStore::changed, this, &Ps5ConnectPage::rebuildPayloads);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) {
        if (id != m_payloadSendId)
            return;
        updatePayloadChip();
        // "Sent" is recorded when the console took the payload, not when the job was queued.
        const auto r = m_ctx.queue().record(id);
        if (r && r->state == JobState::Succeeded)
            m_payloads.markSent(m_payloadSendPath);
    });
    connect(m_smp, &ShadowMountTab::loaded, this, [this] {
        if (!m_smp->loadedValues().isEmpty() && m_smpVersion.isEmpty())
            m_hero->setChip(QStringLiteral("shadowmount"), Tone::Ok, FW_TR("config loaded"));
    });
    rebuildPayloads();
    applyConnectionState(m_session->state(), {});
    updatePayloadChip();
}

Ps5ConnectPage::~Ps5ConnectPage()
{
    if (m_klogClient)
        m_klogClient->stop();
    m_probe.waitForFinished();
}

int Ps5ConnectPage::settingPort(const QString& key, int fallback) const
{
    const int v = m_ctx.settings().value(key).toInt();
    return v > 0 ? v : fallback;
}

QString Ps5ConnectPage::host() const { return m_host->text().trimmed(); }
int Ps5ConnectPage::ftpPort() const { return m_ftpPort->text().toInt() > 0 ? m_ftpPort->text().toInt() : 2121; }

ps5::ConsoleEndpoint Ps5ConnectPage::endpoint() const
{
    ps5::ConsoleEndpoint ep = ps5::endpointFromSettings(m_ctx.settings().snapshot());
    ep.host = host();
    ep.ftpPort = quint16(ftpPort());
    return ep;
}

void Ps5ConnectPage::addGroup(const QString& key, const QString& title, const QString& iconName,
                              const QList<std::tuple<QString, QString, QString, QWidget*>>& tools)
{
    Group g;
    g.key = key;
    g.page = new QWidget(m_groupStack);
    auto* l = vbox(g.page, space::lg, QMargins(0, space::lg, 0, 0));
    g.stack = new ToolStack(g.page);
    g.stack->setProperty("fwNoRing", true);
    if (tools.size() > 1) {
        g.sub = new Segmented(g.page);
        g.sub->setAccessibleName(FW_TR("%1 tools").arg(title));
        l->addWidget(g.sub, 0, Qt::AlignLeft);
        connect(g.sub, &Segmented::currentChanged, g.stack, &ToolStack::setCurrentIndex);
        connect(g.sub, &Segmented::currentChanged, this, [this](int) { emit statusContextChanged(); });
    }
    for (const auto& [toolKey, toolTitle, toolIcon, widget] : tools) {
        if (g.sub)
            g.sub->addSegment(toolTitle, toolIcon, toolKey);
        widget->setParent(g.stack);
        g.stack->addWidget(widget);
        g.toolKeys << toolKey;
        m_toolGroup.insert(toolKey, key);
    }
    if (g.sub)
        g.sub->setCurrentIndex(0);
    l->addWidget(g.stack, 1);
    m_groupStack->addWidget(g.page);
    const int index = m_groups->addTab(icon(iconName, color::fg_3, color::fg_1, color::fg_0), title);
    m_groups->setTabData(index, key);
    m_toolGroup.insert(key, key);
    m_groupList.push_back(g);
}

void Ps5ConnectPage::showTool(const QString& keyIn)
{
    const QString groupKey = m_toolGroup.value(keyIn);
    if (groupKey.isEmpty())
        return;
    for (int i = 0; i < int(m_groupList.size()); ++i) {
        Group& g = m_groupList[size_t(i)];
        if (g.key != groupKey)
            continue;
        m_groups->setCurrentIndex(i);
        const int sub = g.toolKeys.indexOf(keyIn);
        if (sub >= 0) {
            if (g.sub)
                g.sub->setCurrentIndex(sub);
            g.stack->setCurrentIndex(sub);
        }
    }
}

QString Ps5ConnectPage::currentGroup() const
{
    const int i = m_groups->currentIndex();
    return i >= 0 && i < int(m_groupList.size()) ? m_groupList[size_t(i)].key : QString();
}

QString Ps5ConnectPage::currentTool() const
{
    const int i = m_groups->currentIndex();
    if (i < 0 || i >= int(m_groupList.size()))
        return {};
    const Group& g = m_groupList[size_t(i)];
    return g.toolKeys.value(g.stack->currentIndex());
}

// ---- connection ---------------------------------------------------------------------

void Ps5ConnectPage::toggleConnection()
{
    if (m_session->isConnected() || m_session->isConnecting()) {
        m_session->disconnect();
        return;
    }
    if (host().isEmpty()) {
        setInputError(m_host, true);
        m_host->setFocus();
        emit toastRequested(Tone::Warn, FW_TR("No console address"), FW_TR("Enter the console's IP address, or use Find my PS5."));
        return;
    }
    setInputError(m_host, false);
    (void)m_ctx.settings().setValue(ps5::keys::kConsoleIp, host());
    (void)m_ctx.settings().setValue(ps5::keys::kFtpPort, ftpPort());
    // The hero shows "Connecting" through the session's state change.
    m_session->connectTo(host(), quint16(ftpPort()), settingPort(ps5::keys::kConnectTimeout, 10) * 1000,
                         m_ctx.settings().value(ps5::keys::kPassiveMode).toBool());
}

void Ps5ConnectPage::updateHeroSubtitle()
{
    if (m_session->isConnected())
        return;
    m_hero->setSubtitle(host().isEmpty() ? FW_TR("Enter the console's address, or let Find my PS5 search the network.")
                                         : FW_TR("%1 · FTP %2 · not connected").arg(host()).arg(ftpPort()));
}

void Ps5ConnectPage::applyConnectionState(ConsoleSession::ConnectionState state, const QString& detail)
{
    using State = ConsoleSession::ConnectionState;
    const bool on = state == State::Connected;
    switch (state) {
    case State::Connected: {
        const ps5::FtpServerIdentity id = ps5::identifyFtpServer(detail);
        // The console the session is connected to, whatever the address fields say by now.
        m_hero->setState(ConsoleHero::State::Connected, detail.simplified());
        m_hero->setSubtitle(QStringLiteral("%1 · FTP %2 · %3").arg(browserHost()).arg(browserPort()).arg(id.label()));
        m_hero->setChip(QStringLiteral("ftp"), Tone::Ok, id.name.isEmpty() ? QString::number(browserPort()) : id.label(), detail.simplified());
        break;
    }
    case State::Connecting:
        m_hero->setState(ConsoleHero::State::Connecting);
        m_hero->setSubtitle(FW_TR("Connecting to %1:%2…").arg(m_session->host()).arg(m_session->port()));
        m_hero->setChip(QStringLiteral("ftp"), Tone::Info, FW_TR("connecting…"));
        break;
    case State::Offline:
    case State::Failed: {
        const bool failed = state == State::Failed;
        m_hero->setState(failed ? ConsoleHero::State::Failed : ConsoleHero::State::Offline, detail);
        if (failed)
            m_hero->setSubtitle(detail);
        else
            updateHeroSubtitle();
        m_hero->setChip(QStringLiteral("ftp"), failed ? Tone::Err : Tone::Neutral, failed ? FW_TR("not reachable") : QString::number(ftpPort()), detail);
        if (!m_specimenListing) {
            m_listing = {};
            m_remote->setRowCount(0);
        }
        // Requests that were dropped with the connection never answer.
        m_opsPending = m_opsTotal = m_opsOk = 0;
        m_opsError.clear();
        break;
    }
    }
    m_remoteArea->setVisible(on || m_specimenListing);
    m_remoteEmpty->setVisible(!on && !m_specimenListing);
    // The empty state's button is the card's button: Connect, or Cancel while an attempt runs.
    if (m_emptyConnect)
        m_emptyConnect->setText(state == State::Connecting ? FW_TR("Cancel") : FW_TR("Connect"));
    updateActions();
    if (state != m_lastState) {
        m_lastState = state;
        const QString consoleHost = state == State::Offline ? host() : m_session->host();
        emit connectionChanged(state, consoleHost, detail);
        emit reachabilityChanged(on, consoleHost);
    }
    emit statusContextChanged();
}

void Ps5ConnectPage::checkServices()
{
    if (m_probing || host().isEmpty())
        return;
    m_probing = true;
    m_hero->setChecking(true);
    for (const char* k : {"klog", "shadowmount", "installer"})
        m_hero->setChip(QLatin1String(k), Tone::Info, FW_TR("checking…"));
    const ps5::ConsoleEndpoint ep = endpoint();
    QPointer<Ps5ConnectPage> self(this);
    m_probe = QtConcurrent::run([self, ep] {
        ps5::ConsoleServices s = ps5::probeConsoleServices(ep);
        std::vector<ps5::smp::StorageItem> storage;
        if (s.smpVersion) {
            ps5::smp::ApiOptions api;
            api.connectTimeoutMs = 1500;
            if (auto st = ps5::smp::storage(ep.host, ep.smpApiPort, api); st.ok())
                storage = st.value();
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, s, storage] {
            if (!self)
                return;
            self->m_storage = storage;
            self->showServices(s);
        }, Qt::QueuedConnection);
    });
}

void Ps5ConnectPage::showServices(const ps5::ConsoleServices& s)
{
    m_probing = false;
    m_hero->setChecking(false);
    const ps5::ConsoleEndpoint ep = endpoint();
    m_hero->setChip(QStringLiteral("klog"), s.klog.reachable ? Tone::Ok : Tone::Warn, QString::number(ep.klogPort),
                    s.klog.reachable ? FW_TR("klogsrv or etaHEN's klog answers.") : s.klog.detail + QStringLiteral("\n") + FW_TR("klogsrv uses 3232; etaHEN 9081 with Klog=1."));
    m_hero->setChip(QStringLiteral("installer"), s.installer.reachable ? Tone::Ok : Tone::Neutral, QString::number(ep.dpiPort),
                    s.installer.reachable ? FW_TR("etaHEN's Direct Package Installer V2 answers.")
                                          : FW_TR("Off by default: set DPI_v2=1 in /data/etaHEN/config.ini."));
    if (s.smpVersion) {
        m_smpVersion = s.smpVersion->version;
        m_hero->setChip(QStringLiteral("shadowmount"), Tone::Ok, m_smpVersion,
                        FW_TR("HTTP API on port %1: %2").arg(ep.smpApiPort).arg(s.smpVersion->capabilities.join(QStringLiteral(", "))));
        m_smp->setApiVersion(m_smpVersion);
    } else {
        m_smpVersion.clear();
        m_hero->setChip(QStringLiteral("shadowmount"), Tone::Neutral, FW_TR("no API"),
                        FW_TR("The ShadowMount+ 1.7 API did not answer on port %1. Older versions and api_enabled=0 have none; the tools use FTP then.").arg(ep.smpApiPort));
    }
    updateFooter();
}

void Ps5ConnectPage::updatePayloadChip()
{
    const int port = settingPort(ps5::keys::kPayloadPort, 9021);
    const auto r = m_payloadSendId.isEmpty() ? std::nullopt : m_ctx.queue().record(m_payloadSendId);
    if (r && r->state == JobState::Succeeded)
        m_hero->setChip(QStringLiteral("payload"), Tone::Ok, FW_TR("%1 · sent").arg(r->outcome ? r->outcome->details.value(QStringLiteral("port")).toInt() : port),
                        r->outcome ? r->outcome->summary : QString());
    else if (r && r->state == JobState::Failed)
        m_hero->setChip(QStringLiteral("payload"), Tone::Warn, FW_TR("%1 · failed").arg(port), r->error ? r->error->message : QString());
    else
        m_hero->setChip(QStringLiteral("payload"), Tone::Neutral, QString::number(port),
                        FW_TR("Not checked: an empty connection would look like a payload to elfldr. Shown after a send."));
}

// ---- Files: browser -------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildFilesBrowser(QWidget* parent)
{
    auto* tab = new QWidget(parent);
    auto* l = vbox(tab, space::lg);
    m_filesCard = new Card(tab, Card::Variant::Table);
    auto* card = m_filesCard;
    auto* h = card->header();
    h->setSpacing(space::xs);
    m_btnBack = new IconButton(QStringLiteral("arrow_left"), FW_TR("Back"), IconButton::Size::Small, card);
    m_btnUp = new IconButton(QStringLiteral("arrow_up"), FW_TR("Up one level"), IconButton::Size::Small, card);
    m_btnRefresh = new IconButton(QStringLiteral("refresh"), FW_TR("Refresh"), IconButton::Size::Small, card);
    m_btnPlaces = new IconButton(QStringLiteral("folders"), FW_TR("Places"), IconButton::Size::Small, card);
    m_btnPlaces->setToolTip(FW_TR("Places on the console"));
    m_btnPlaces->hide();
    h->addWidget(m_btnPlaces);
    h->addWidget(m_btnBack);
    h->addWidget(m_btnUp);
    h->addWidget(m_btnRefresh);
    connect(m_btnPlaces, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        for (int r = 0; r < m_places->count(); ++r) {
            QListWidgetItem* it = m_places->item(r);
            const QString path = it->data(Qt::UserRole).toString();
            if (path.isEmpty())
                continue;
            QAction* a = menu.addAction(it->icon(), it->text(), this, [this, path] { listRemote(path); });
            a->setToolTip(path);
            a->setEnabled(m_session->isConnected());
        }
        menu.exec(m_btnPlaces->mapToGlobal(QPoint(0, m_btnPlaces->height())));
    });
    h->addSpacing(space::sm);
    m_crumbs = new QWidget(card);
    m_crumbLayout = hbox(m_crumbs, 2);
    h->addWidget(m_crumbs, 1);
    m_filter = textInput(FW_TR("Filter this folder"), false, card);
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);
    m_filter->setAccessibleName(FW_TR("Filter"));
    h->addWidget(m_filter);
    connect(m_btnBack, &QToolButton::clicked, this, &Ps5ConnectPage::goBack);
    connect(m_btnUp, &QToolButton::clicked, this, [this] { listRemote(ps5::remoteParent(m_remoteDir)); });
    connect(m_btnRefresh, &QToolButton::clicked, this, [this] { listRemote(m_remoteDir); });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& f) {
        for (int r = 0; r < m_remote->rowCount(); ++r)
            m_remote->setRowHidden(r, !f.isEmpty() && !(m_remote->item(r, 0) && m_remote->item(r, 0)->text().contains(f, Qt::CaseInsensitive)));
        updateFooter();
    });

    // Places | (actions + listing)
    auto* split = new QWidget(card);
    auto* sh = hbox(split, 0);
    m_places = new QListWidget(split);
    m_places->setObjectName(QStringLiteral("ps5Places"));
    m_places->setFixedWidth(184);
    m_places->setIconSize(QSize(size::icon, size::icon));
    m_places->setFrameShape(QFrame::NoFrame);
    m_places->setSpacing(1);
    m_places->setAccessibleName(FW_TR("Places on the console"));
    m_places->setStyleSheet(QStringLiteral("QListWidget{background:%1;padding:8px 6px;border:none;} QListWidget::item{padding:6px 6px;border:none;border-radius:6px;color:%2;} "
                                           "QListWidget::item:selected{background:%3;color:%4;} QListWidget::item:hover:!selected{background:%5;}")
                                .arg(hex(color::bg_1), hex(color::fg_2), hex(color::accent_lo), hex(color::fg_0), hex(color::bg_3)));
    sh->addWidget(m_places);
    auto* vline = new QFrame(split);
    vline->setFrameShape(QFrame::VLine);
    vline->setFixedWidth(1);
    vline->setStyleSheet(QStringLiteral("background:%1;border:none;").arg(hex(color::border_2)));
    sh->addWidget(vline);
    auto* right = new QWidget(split);
    auto* rl = vbox(right, 0);
    m_remoteArea = new QWidget(right);
    auto* al = vbox(m_remoteArea, 0);
    auto* bar = new QWidget(m_remoteArea);
    auto* bh = hbox(bar, space::sm, QMargins(space::md, space::sm, space::md, space::sm));
    m_btnUpload = button(FW_TR("Upload files…"), ButtonVariant::Primary, QStringLiteral("upload"), bar);
    m_btnUploadFolder = button(FW_TR("Upload folder…"), ButtonVariant::Secondary, QStringLiteral("folder_in"), bar);
    m_btnNewFolder = button(FW_TR("New folder…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), bar);
    m_btnDownload = button(FW_TR("Download…"), ButtonVariant::Secondary, QStringLiteral("download"), bar);
    m_btnRename = button(FW_TR("Rename…"), ButtonVariant::Subtle, QStringLiteral("rename"), bar);
    m_btnDelete = button(FW_TR("Delete…"), ButtonVariant::Subtle, QStringLiteral("trash"), bar);
    for (auto* b : {m_btnUpload, m_btnUploadFolder, m_btnNewFolder, m_btnDownload, m_btnRename, m_btnDelete})
        setButtonSmall(b, true);
    m_btnDownload->setToolTip(FW_TR("Download the selected items to a folder on this PC"));
    m_btnDelete->setToolTip(FW_TR("Delete the selected items from the console (Del)"));
    m_btnRename->setToolTip(FW_TR("Rename the selected item (F2)"));
    bh->addWidget(m_btnUpload);
    bh->addWidget(m_btnUploadFolder);
    bh->addWidget(m_btnNewFolder);
    bh->addStretch(1);
    bh->addWidget(m_btnDownload);
    bh->addWidget(m_btnRename);
    bh->addWidget(m_btnDelete);
    al->addWidget(bar);
    al->addWidget(hairline(m_remoteArea));
    connect(m_btnUpload, &QPushButton::clicked, this, [this] { uploadDialog(false); });
    connect(m_btnUploadFolder, &QPushButton::clicked, this, [this] { uploadDialog(true); });
    connect(m_btnDownload, &QPushButton::clicked, this, &Ps5ConnectPage::downloadSelected);
    connect(m_btnNewFolder, &QPushButton::clicked, this, &Ps5ConnectPage::newFolder);
    connect(m_btnRename, &QPushButton::clicked, this, &Ps5ConnectPage::renameSelected);
    connect(m_btnDelete, &QPushButton::clicked, this, &Ps5ConnectPage::deleteSelected);

    m_remote = new QTableWidget(0, 3, m_remoteArea);
    m_remote->setObjectName(QStringLiteral("ps5RemoteTable"));
    m_remote->setHorizontalHeaderLabels({FW_TR("Name"), FW_TR("Size"), FW_TR("Modified")});
    m_remote->horizontalHeader()->setFixedHeight(32);
    m_remote->horizontalHeader()->setHighlightSections(false);
    m_remote->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_remote->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_remote->horizontalHeader()->setSortIndicatorShown(true);
    m_remote->horizontalHeader()->setSectionsClickable(true);
    m_remote->setColumnWidth(1, 104);
    m_remote->setColumnWidth(2, 150);
    m_remote->verticalHeader()->hide();
    m_remote->verticalHeader()->setDefaultSectionSize(size::row);
    m_remote->setShowGrid(false);
    m_remote->setFrameShape(QFrame::NoFrame);
    m_remote->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remote->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_remote->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_remote->setMinimumHeight(11 * size::row + 32);
    m_remote->setContextMenuPolicy(Qt::CustomContextMenu);
    m_remote->setIconSize(QSize(size::icon, size::icon));
    m_remote->setWordWrap(false);
    m_remote->setTextElideMode(Qt::ElideMiddle);
    m_remote->setSortingEnabled(true);
    m_remote->sortByColumn(0, Qt::AscendingOrder);
    m_remote->setAcceptDrops(true);
    m_remote->viewport()->setAcceptDrops(true);
    m_remote->viewport()->installEventFilter(this);
    m_remote->installEventFilter(this);
    connect(m_remote, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const int i = entryIndex(row);
        if (i >= 0 && m_listing.entries[size_t(i)].isDir)
            if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(i)].name))
                listRemote(*child);
    });
    connect(m_remote, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        QMenu menu(this);
        QTableWidgetItem* it = m_remote->itemAt(p);
        const int i = it ? entryIndex(it->row()) : -1;
        // The folder's path, not its row: a listing that arrives while the menu is open replaces the entries.
        if (i >= 0 && m_listing.entries[size_t(i)].isDir)
            if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(i)].name))
                menu.addAction(icon(QStringLiteral("folder_open"), color::fg_2), FW_TR("Open"), this, [this, path = *child] { listRemote(path); });
        if (it) {
            menu.addAction(icon(QStringLiteral("download"), color::fg_2), FW_TR("Download…"), this, &Ps5ConnectPage::downloadSelected)->setEnabled(m_btnDownload->isEnabled());
            menu.addAction(icon(QStringLiteral("rename"), color::fg_2), FW_TR("Rename…"), this, &Ps5ConnectPage::renameSelected)->setEnabled(m_btnRename->isEnabled());
            menu.addAction(icon(QStringLiteral("copy"), color::fg_2), FW_TR("Copy path"), this, [this] {
                const QStringList sel = selectedRemotePaths();
                if (!sel.isEmpty())
                    QGuiApplication::clipboard()->setText(sel.join(u'\n'));
            });
            menu.addSeparator();
        }
        menu.addAction(icon(QStringLiteral("upload"), color::fg_2), FW_TR("Upload files here…"), this, [this] { uploadDialog(false); })->setEnabled(m_btnUpload->isEnabled());
        menu.addAction(icon(QStringLiteral("folder_plus"), color::fg_2), FW_TR("New folder…"), this, &Ps5ConnectPage::newFolder)->setEnabled(m_btnNewFolder->isEnabled());
        if (it) {
            menu.addSeparator();
            menu.addAction(icon(QStringLiteral("trash"), color::danger_hi), FW_TR("Delete…"), this, &Ps5ConnectPage::deleteSelected)->setEnabled(m_btnDelete->isEnabled());
        }
        menu.exec(m_remote->viewport()->mapToGlobal(p));
    });
    connect(m_remote, &QTableWidget::itemSelectionChanged, this, [this] {
        updateActions();
        updateFooter();
    });
    al->addWidget(m_remote, 1);
    rl->addWidget(m_remoteArea, 1);
    m_remoteEmpty = new EmptyState(QStringLiteral("plug"), FW_TR("Connect to browse the console"),
                                   FW_TR("Start ftpsrv, etaHEN's FTP or zftpd on the PS5, then connect. Once connected, drop files on the list to upload them."), right);
    m_emptyConnect = m_remoteEmpty->addAction(FW_TR("Connect"), ButtonVariant::Primary);
    connect(m_emptyConnect, &QPushButton::clicked, this, &Ps5ConnectPage::toggleConnection);
    auto* find = m_remoteEmpty->addAction(FW_TR("Find my PS5"), ButtonVariant::Secondary);
    connect(find, &QPushButton::clicked, m_find, &QPushButton::click);
    rl->addWidget(m_remoteEmpty, 1);
    sh->addWidget(right, 1);
    card->body()->addWidget(split);
    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        if (m_session->isConnected())
            listRemote(it->data(Qt::UserRole).toString());
    });

    auto* f = card->footer();
    m_remoteFooter = caption({}, card);
    m_destClass = caption({}, card);
    m_remoteFree = label({}, TextStyle::MonoSmall, color::fg_3, card);
    f->addWidget(m_remoteFooter, 1);
    f->addWidget(m_destClass);
    f->addWidget(m_remoteFree);
    l->addWidget(card);

    m_transfers = new TransferPanel(m_ctx, tab);
    connect(m_transfers, &TransferPanel::toastRequested, this, &Page::toastRequested);
    l->addWidget(m_transfers);
    l->addStretch(1);
    m_remoteDir = m_ctx.settings().value(ps5::keys::kGamesFolder).toString();
    if (m_remoteDir.isEmpty())
        m_remoteDir = QStringLiteral("/");
    rebuildPlaces();
    rebuildBreadcrumb();
    return tab;
}

void Ps5ConnectPage::rebuildPlaces()
{
    const QString games = ps5::normalizeRemotePath(m_ctx.settings().value(ps5::keys::kGamesFolder).toString().isEmpty()
                                                       ? QStringLiteral("/data/etaHEN/games") : m_ctx.settings().value(ps5::keys::kGamesFolder).toString());
    const QList<Place> places = {
        {"game", FW_TR("Games"), games},
        {"box", FW_TR("Homebrew"), QStringLiteral("/data/homebrew")},
        {"usb", FW_TR("USB drive 1"), QStringLiteral("/mnt/usb0")},
        {"usb", FW_TR("USB drive 2"), QStringLiteral("/mnt/usb1")},
        {"hard_drive", FW_TR("Extended storage"), QStringLiteral("/mnt/ext0")},
        {"hard_drive", QStringLiteral("M.2 SSD"), QStringLiteral("/mnt/ext1")},
        {"layers", QStringLiteral("ShadowMount+"), QStringLiteral("/data/shadowmount")},
        {"folder", QStringLiteral("etaHEN"), QStringLiteral("/data/etaHEN")},
        {"database", FW_TR("Internal data"), QStringLiteral("/data")},
        {"folder_tree", FW_TR("Root"), QStringLiteral("/")},
    };
    m_places->clear();
    for (const Place& p : places) {
        auto* it = new QListWidgetItem(icon(QLatin1String(p.icon), color::fg_3, color::fg_1, color::accent_hi), p.label, m_places);
        it->setData(Qt::UserRole, p.path);
        it->setToolTip(p.path);
        it->setSizeHint(QSize(0, 32));
    }
    auto* overline = new QListWidgetItem(FW_TR("PLACES"));
    overline->setFlags(Qt::NoItemFlags);
    overline->setFont(font::overline());
    overline->setForeground(color::fg_4);
    m_places->insertItem(0, overline);
    m_places->setMinimumHeight(m_places->count() * 34 + 20); // every place visible, no scroll bar
}

void Ps5ConnectPage::listRemote(const QString& path)
{
    if (!m_session->isConnected())
        return;
    m_remoteFooter->setText(FW_TR("Listing…"));
    m_session->list(path.isEmpty() ? QStringLiteral("/") : path);
}

void Ps5ConnectPage::beginOperations(int count, const QString& footer)
{
    m_opsPending += count;
    m_opsTotal += count;
    m_remoteFooter->setText(footer);
}

void Ps5ConnectPage::onOperationDone(const QString& what, bool ok, const QString& message)
{
    // One answer per request; a selection of 50 items gets one toast and one listing at its end.
    if (m_opsPending > 0)
        --m_opsPending;
    if (ok)
        ++m_opsOk;
    else
        m_opsError = message;
    if (m_opsPending > 0)
        return;
    const int total = std::max(m_opsTotal, 1);
    const int failed = total - m_opsOk;
    const QString error = m_opsError;
    m_opsTotal = m_opsOk = 0;
    m_opsError.clear();
    const bool isDelete = what == u"delete";
    if (total == 1) {
        const QString notDone = isDelete ? FW_TR("Not deleted") : what == u"rename" ? FW_TR("Not renamed") : FW_TR("Folder not created");
        emit toastRequested(ok ? Tone::Ok : Tone::Err, ok ? FW_TR("Done") : notDone, message);
    } else if (failed == 0) {
        emit toastRequested(Tone::Ok, FW_TR("Done"), FW_TR("Deleted %1 items").arg(total));
    } else {
        emit toastRequested(Tone::Err, FW_TR("%1 deleted, %2 not").arg(total - failed).arg(failed), error);
    }
    // Also after a failure: part of a folder may be gone.
    if (m_session->isConnected())
        listRemote(m_remoteDir);
    else
        updateFooter();
}

void Ps5ConnectPage::onListed(const RemoteListing& listing)
{
    if (!listing.error.isEmpty()) {
        // The folder of the last connection (or of the settings) may not exist on this console: open
        // the games folder, then the root, before reporting anything.
        if (m_firstListing && m_session->isConnected()) {
            const QString games = ps5::normalizeRemotePath(m_ctx.settings().value(ps5::keys::kGamesFolder).toString());
            const QString next = listing.path != games && !games.isEmpty() && m_firstListingStep == 0 ? games : QStringLiteral("/");
            if (listing.path != next && m_firstListingStep < 2) {
                ++m_firstListingStep;
                listRemote(next);
                return;
            }
        }
        m_firstListing = false;
        emit toastRequested(Tone::Err, FW_TR("Could not list %1").arg(listing.path), listing.error);
        m_remoteFooter->setText(listing.error);
        return;
    }
    m_firstListing = false;
    // History: a new folder (not Back) drops the forward entries.
    if (m_historyIndex < 0 || m_history.value(m_historyIndex) != listing.path) {
        while (m_history.size() > m_historyIndex + 1)
            m_history.removeLast();
        m_history << listing.path;
        m_historyIndex = int(m_history.size()) - 1;
    }
    showListing(listing);
}

void Ps5ConnectPage::showListing(const RemoteListing& listing)
{
    m_listing = listing;
    m_remoteDir = listing.path;
    m_remote->setSortingEnabled(false);
    m_remote->setRowCount(0);
    m_remote->setRowCount(int(m_listing.entries.size()));
    for (int i = 0; i < int(m_listing.entries.size()); ++i) {
        const ps5::FtpEntry& e = m_listing.entries[size_t(i)];
        auto* name = new RemoteItem(e.name, e.isDir, e.name);
        name->setData(RemoteItem::IndexRole, i);
        name->setIcon(icon(entryIcon(e), e.isDir ? color::accent_hi : color::fg_3));
        name->setForeground(e.isDir ? color::fg_0 : color::fg_1);
        name->setFont(e.isDir ? font::bodyStrong() : font::body());
        name->setToolTip(e.name);
        m_remote->setItem(i, 0, name);
        auto* sz = new RemoteItem(e.isDir || e.size < 0 ? QStringLiteral("—") : formatBytes(e.size), e.isDir, qint64(e.isDir ? -1 : e.size));
        sz->setFont(font::mono());
        sz->setForeground(color::fg_3);
        sz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_remote->setItem(i, 1, sz);
        auto* when = new RemoteItem(e.modified.isValid() ? humanTime(e.modified.toLocalTime()) : QString(), e.isDir,
                                    e.modified.isValid() ? e.modified.toSecsSinceEpoch() : qint64(0));
        when->setFont(font::mono());
        when->setForeground(color::fg_4);
        m_remote->setItem(i, 2, when);
    }
    m_remote->setSortingEnabled(true);
    m_remote->sortByColumn(m_remote->horizontalHeader()->sortIndicatorSection(), m_remote->horizontalHeader()->sortIndicatorOrder());
    if (!m_filter->text().isEmpty())
        emit m_filter->textChanged(m_filter->text());
    const ps5::DestinationInfo d = ps5::classifyDestination(m_remoteDir);
    m_destClass->setText(fw::i18n::translate(d.label)); // English in connect: also logged
    // The place that holds this folder.
    int best = -1, bestLen = -1;
    for (int r = 0; r < m_places->count(); ++r) {
        const QString p = m_places->item(r)->data(Qt::UserRole).toString();
        if (p.isEmpty())
            continue;
        const bool inside = m_remoteDir == p || m_remoteDir.startsWith(p == u"/" ? p : p + u'/');
        if (inside && p.size() > bestLen) {
            best = r;
            bestLen = int(p.size());
        }
    }
    m_places->setCurrentRow(best);
    rebuildBreadcrumb();
    updateActions();
    updateFooter();
    emit statusContextChanged();
}

QStringList Ps5ConnectPage::remoteNames() const
{
    QStringList out;
    for (int r = 0; r < m_remote->rowCount(); ++r)
        if (m_remote->item(r, 0))
            out << m_remote->item(r, 0)->text();
    return out;
}

int Ps5ConnectPage::entryIndex(int row) const
{
    const QTableWidgetItem* it = m_remote->item(row, 0);
    if (!it)
        return -1;
    const int i = it->data(RemoteItem::IndexRole).toInt();
    return i >= 0 && i < int(m_listing.entries.size()) ? i : -1;
}

void Ps5ConnectPage::updateActions()
{
    const bool on = m_session->isConnected();
    const int n = int(m_remote->selectionModel()->selectedRows().size());
    const bool canUpload = jobs::isAvailable(m_ctx, ps5::jobkinds::kUpload);
    m_btnUpload->setEnabled(on && canUpload);
    m_btnUploadFolder->setEnabled(on && canUpload);
    m_btnNewFolder->setEnabled(on);
    m_btnRename->setEnabled(on && n == 1);
    m_btnDelete->setEnabled(on && n > 0);
    m_btnDownload->setEnabled(on && n > 0 && jobs::isAvailable(m_ctx, ps5::jobkinds::kDownload));
    m_btnUp->setEnabled(on && m_remoteDir != u"/");
    m_btnBack->setEnabled(on && m_historyIndex > 0);
    m_btnRefresh->setEnabled(on);
    m_filter->setEnabled(on || m_specimenListing);
    m_places->setEnabled(on || m_specimenListing);
}

void Ps5ConnectPage::updateFooter()
{
    int shown = 0;
    for (int r = 0; r < m_remote->rowCount(); ++r)
        shown += !m_remote->isRowHidden(r);
    const int total = int(m_listing.entries.size());
    qint64 bytes = 0;
    int n = 0;
    for (const QModelIndex& i : m_remote->selectionModel()->selectedRows()) {
        const int e = entryIndex(i.row());
        ++n;
        if (e >= 0 && m_listing.entries[size_t(e)].size > 0 && !m_listing.entries[size_t(e)].isDir)
            bytes += m_listing.entries[size_t(e)].size;
    }
    QString text = shown != total ? FW_TR("%1 of %2 items").arg(shown).arg(total)
                 : total == 1     ? FW_TR("%1 item").arg(total)
                                  : FW_TR("%1 items").arg(total);
    if (n > 0)
        text += QStringLiteral(" · ") + (bytes > 0 ? FW_TR("%1 selected (%2)").arg(n).arg(formatBytes(bytes)) : FW_TR("%1 selected").arg(n));
    m_remoteFooter->setText(text);
    // Free space: only the ShadowMount+ 1.7 API knows it (no console FTP server has SITE AVAIL).
    if (const ps5::smp::StorageItem* fs = ps5::smp::storageFor(m_storage, m_remoteDir); fs && fs->availableBytes >= 0)
        m_remoteFree->setText(FW_TR("%1 free").arg(formatBytes(fs->availableBytes)));
    else if (!m_storage.empty())
        m_remoteFree->clear();
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
            m_crumbLayout->addWidget(label(QStringLiteral("/"), TextStyle::Mono, color::fg_5, m_crumbs));
        }
    }
    m_crumbLayout->addStretch(1);
}

QStringList Ps5ConnectPage::selectedRemotePaths() const
{
    QStringList out;
    for (const QModelIndex& i : m_remote->selectionModel()->selectedRows()) {
        const int e = entryIndex(i.row());
        // Names from the console's listing: one with a slash, a backslash or ".." never becomes a path.
        if (e >= 0)
            if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(e)].name))
                out << *child;
    }
    return out;
}

bool Ps5ConnectPage::confirmProtected(const QString& path, const QString& action)
{
    if (!ps5::isProtectedRemotePath(path))
        return true;
    // ps5::protectedPathWarning() as whole sentences, one per action: that
    // function's text is also an error message and stays English.
    const auto warning = [&]() -> QString {
        if (action == QLatin1String("upload"))
            return FW_TR("The target is a protected PS5 folder:\n\n    %1\n\nIt holds system files, installed apps or "
                                 "saves (/system, /system_ex, /preinst, /user/app, /user/home…). Changing it can brick your "
                                 "console or lose data. This is almost never what you want.\n\nContinue with \"upload\" anyway?").arg(path);
        if (action == QLatin1String("delete"))
            return FW_TR("The target is a protected PS5 folder:\n\n    %1\n\nIt holds system files, installed apps or "
                                 "saves (/system, /system_ex, /preinst, /user/app, /user/home…). Changing it can brick your "
                                 "console or lose data. This is almost never what you want.\n\nContinue with \"delete\" anyway?").arg(path);
        if (action == QLatin1String("rename"))
            return FW_TR("The target is a protected PS5 folder:\n\n    %1\n\nIt holds system files, installed apps or "
                                 "saves (/system, /system_ex, /preinst, /user/app, /user/home…). Changing it can brick your "
                                 "console or lose data. This is almost never what you want.\n\nContinue with \"rename\" anyway?").arg(path);
        if (action == QLatin1String("create a folder"))
            return FW_TR("The target is a protected PS5 folder:\n\n    %1\n\nIt holds system files, installed apps or "
                                 "saves (/system, /system_ex, /preinst, /user/app, /user/home…). Changing it can brick your "
                                 "console or lose data. This is almost never what you want.\n\nContinue with \"create a folder\" anyway?").arg(path);
        return ps5::protectedPathWarning(path, action);
    };
    // Plain text: the sheet shows the warning as it is written.
    return Sheet::confirm(window(), FW_TR("Change a system folder?"), warning(),
                          FW_TR("Cancel"), FW_TR("Continue anyway"), true, Sheet::Icon::Warning);
}

void Ps5ConnectPage::uploadDialog(bool folder)
{
    // The folder the dialog names: a listing that arrives while it is open must not redirect the upload.
    const QString target = m_remoteDir;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(window(), FW_TR("Choose a folder to send to %1").arg(target));
        if (!dir.isEmpty())
            uploadFiles({dir}, target);
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(window(), FW_TR("Choose files to send to %1").arg(target));
    if (!files.isEmpty())
        uploadFiles(files, target);
}

void Ps5ConnectPage::uploadFiles(const QStringList& localPaths, const QString& remoteDirIn)
{
    if (localPaths.isEmpty())
        return;
    const QString remoteDir = remoteDirIn.isEmpty() ? m_remoteDir : remoteDirIn;
    // The console whose listing is on screen (the address fields may have been edited since).
    const QString consoleHost = browserHost();
    const int consolePort = browserPort();
    // The folder itself, or the place a dropped folder's own name leads to
    // ("system_ex" sent to "/"): the job refuses both unless this was confirmed.
    const QString protectedTarget = ps5::protectedUploadTarget(remoteDir, localPaths);
    const bool protectedDir = !protectedTarget.isEmpty();
    if (protectedDir && !confirmProtected(protectedTarget, QStringLiteral("upload")))
        return;
    // The upload replaces files of the same name: say so when the open folder shows some.
    if (const QStringList taken = uploadCollisions(localPaths, remoteDir); !taken.isEmpty()) {
        const QString what = taken.size() == 1 ? FW_TR("%1 is already in %2.").arg(taken.first(), remoteDir)
                                               : FW_TR("%1 of these items are already in %2 (%3%4).")
                                                     .arg(taken.size()).arg(remoteDir, taken.mid(0, 3).join(QStringLiteral(", ")), taken.size() > 3 ? QStringLiteral(", …") : QString());
        if (!Sheet::confirm(window(), taken.size() == 1 ? FW_TR("Replace the file on the console?") : FW_TR("Replace the files on the console?"),
                            what + QStringLiteral(" ") + FW_TR("The upload replaces what is there; there is no recycle bin on the PS5."),
                            FW_TR("Cancel"), FW_TR("Replace"), true, Sheet::Icon::Warning))
            return;
    }
    // One transfer per dropped item (its own progress row), up to 20; more go as one job.
    QList<QStringList> batches;
    if (localPaths.size() <= 20)
        for (const QString& p : localPaths)
            batches << QStringList{p};
    else
        batches << localPaths;
    qsizetype items = 0;
    QStringList started;
    for (const QStringList& batch : batches) {
        auto r = jobs::submitFtpUploadMany(m_ctx, consoleHost, consolePort, batch, remoteDir, protectedDir);
        if (!r.ok()) {
            emit toastRequested(Tone::Err, FW_TR("Upload not started"), r.error().message);
            break;
        }
        items += batch.size();
        started << r.value();
    }
    if (started.isEmpty())
        return;
    // These transfers run now, in this order; a paused queue stays paused for the rest.
    for (auto it = started.crbegin(); it != started.crend(); ++it)
        m_ctx.queue().startNow(*it);
    showTool(QStringLiteral("ftp"));
    emit toastRequested(Tone::Info, FW_TR("Upload queued"),
                        (items == 1 ? FW_TR("%1 item → %2").arg(items) : FW_TR("%1 items → %2").arg(items)).arg(remoteDir));
}

QStringList Ps5ConnectPage::uploadCollisions(const QStringList& localPaths, const QString& remoteDir) const
{
    if (remoteDir != m_listing.path)
        return {}; // not the folder on screen: nothing is known about it
    const bool caseSensitive = ps5::classifyDestination(remoteDir).kind == ps5::DestinationKind::Internal;
    QStringList out;
    for (const QString& p : localPaths) {
        const QFileInfo fi(p);
        if (fi.isDir())
            continue; // a folder is merged into the one that is there
        for (const ps5::FtpEntry& e : m_listing.entries)
            if (!e.isDir && e.name.compare(fi.fileName(), caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0)
                out << e.name;
    }
    return out;
}

void Ps5ConnectPage::downloadSelected()
{
    const QStringList sel = selectedRemotePaths();
    if (sel.isEmpty())
        return;
    // The console the selection was made on.
    const QString consoleHost = browserHost();
    const int consolePort = browserPort();
    const QString dir = QFileDialog::getExistingDirectory(window(), sel.size() == 1 ? FW_TR("Download %1 item to").arg(sel.size()) : FW_TR("Download %1 items to").arg(sel.size()));
    if (dir.isEmpty())
        return;
    // Files that are already in the folder are skipped unless the user says to replace them.
    QStringList existing;
    for (const QString& p : sel)
        if (QFileInfo::exists(QDir(dir).filePath(ps5::remoteFileName(p))))
            existing << ps5::remoteFileName(p);
    bool overwrite = false;
    if (!existing.isEmpty()) {
        auto* s = new Sheet(window(), existing.size() == 1 ? FW_TR("%1 is already in the folder").arg(existing.first())
                                                         : FW_TR("%1 items are already in the folder").arg(existing.size()),
                            Sheet::Icon::Warning);
        s->addText(FW_TR("Replace what is in %1 with the copies from the console, or keep the files that are there and download only the rest?").arg(nativePath(dir)));
        s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
        s->addButton(FW_TR("Keep existing"), Sheet::Role::Primary);
        s->addButton(FW_TR("Replace"), Sheet::Role::Destructive);
        const int answer = s->exec();
        if (answer != 1 && answer != 2)
            return;
        overwrite = answer == 2;
    }
    QList<QStringList> batches;
    if (sel.size() <= 20)
        for (const QString& p : sel)
            batches << QStringList{p};
    else
        batches << sel;
    QStringList started;
    for (const QStringList& batch : batches) {
        auto r = jobs::submitFtpDownload(m_ctx, consoleHost, consolePort, batch, dir, overwrite);
        if (!r.ok()) {
            emit toastRequested(Tone::Err, FW_TR("Download not started"), r.error().message);
            return;
        }
        started << r.value();
    }
    for (auto it = started.crbegin(); it != started.crend(); ++it)
        m_ctx.queue().startNow(*it); // in this order; a paused queue stays paused for the rest
    emit toastRequested(Tone::Info, FW_TR("Download queued"), nativePath(dir));
}

void Ps5ConnectPage::deleteSelected()
{
    // What is selected now is what the question names and what is deleted: the
    // listing and the selection can change while the sheets are open.
    struct Target {
        QString path;
        bool isDir = false;
    };
    std::vector<Target> sel;
    for (const QModelIndex& i : m_remote->selectionModel()->selectedRows()) {
        const int e = entryIndex(i.row());
        if (e < 0)
            continue;
        const ps5::FtpEntry& entry = m_listing.entries[size_t(e)];
        // Deleting stays on the console, where ':' is an ordinary character
        // in a name: the looser rule, so such an entry can be removed.
        if (const auto child = ps5::listedChild(m_remoteDir, entry.name))
            sel.push_back({*child, entry.isDir});
    }
    if (sel.empty())
        return;
    for (const Target& t : sel)
        if (!confirmProtected(t.path, QStringLiteral("delete")))
            return;
    if (!Sheet::confirm(window(), sel.size() == 1 ? FW_TR("Delete %1 from the console?").arg(ps5::remoteFileName(sel.front().path))
                                                  : FW_TR("Delete %1 items from the console?").arg(sel.size()),
                        FW_TR("Folders are removed with everything inside them. There is no recycle bin on the PS5."),
                        FW_TR("Keep"), FW_TR("Delete")))
        return;
    if (!m_session->isConnected())
        return;
    beginOperations(int(sel.size()), FW_TR("Deleting…"));
    for (const Target& t : sel)
        m_session->remove(t.path, t.isDir);
}

QString Ps5ConnectPage::renameCollision(const QString& dir, const QString& oldName, const QString& newName) const
{
    if (dir != m_listing.path)
        return {}; // the folder is not the one on screen any more: nothing to compare with
    // USB and extended storage are exFAT (names differing only in case are the same file); the internal disk is not.
    const bool caseSensitive = ps5::classifyDestination(dir).kind == ps5::DestinationKind::Internal;
    for (const ps5::FtpEntry& e : m_listing.entries) {
        if (e.name == oldName)
            continue; // the item itself (a change of case only)
        if (e.name.compare(newName, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0)
            return e.name;
    }
    return {};
}

void Ps5ConnectPage::renameSelected()
{
    const QStringList sel = selectedRemotePaths();
    if (sel.size() != 1)
        return;
    if (!confirmProtected(sel.first(), QStringLiteral("rename")))
        return;
    auto* s = new Sheet(window(), FW_TR("Rename on the console"), Sheet::Icon::None);
    auto* name = textInput({}, true, s);
    name->setText(ps5::remoteFileName(sel.first()));
    s->body()->addWidget(field(FW_TR("New name"), name, {}, s));
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Rename"), Sheet::Role::Primary);
    name->setFocus();
    name->selectAll();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    const QString oldName = ps5::remoteFileName(sel.first());
    if (n.isEmpty() || n == oldName)
        return;
    if (const QString why = ps5::remoteNameProblem(n); !why.isEmpty()) {
        emit toastRequested(Tone::Warn, FW_TR("Not renamed"), why);
        return;
    }
    // The item's own folder, not the one open now (a listing may have arrived while the sheet was up).
    const QString dir = ps5::remoteParent(sel.first());
    // The console's RNTO replaces an existing file without a word: never rename onto a name that is taken.
    if (const QString taken = renameCollision(dir, oldName, n); !taken.isEmpty()) {
        emit toastRequested(Tone::Warn, FW_TR("Not renamed"),
                            FW_TR("%1 already exists in this folder. Choose another name, or delete or rename that item first.").arg(taken));
        return;
    }
    beginOperations(1, FW_TR("Renaming…"));
    m_session->rename(sel.first(), ps5::joinRemote(dir, n));
}

void Ps5ConnectPage::newFolder()
{
    const QString dir = m_remoteDir; // the folder the sheet names, whatever is listed when it closes
    if (!confirmProtected(dir, QStringLiteral("create a folder")))
        return;
    auto* s = new Sheet(window(), FW_TR("New folder in %1").arg(dir), Sheet::Icon::None);
    auto* name = textInput(FW_TR("Folder name"), true, s);
    s->body()->addWidget(field(FW_TR("Name"), name, {}, s));
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Create"), Sheet::Role::Primary);
    name->setFocus();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    if (n.isEmpty())
        return;
    if (const QString why = ps5::remoteNameProblem(n); !why.isEmpty()) {
        emit toastRequested(Tone::Warn, FW_TR("Folder not created"), why);
        return;
    }
    beginOperations(1, FW_TR("Creating the folder…"));
    m_session->mkdir(ps5::joinRemote(dir, n));
}

void Ps5ConnectPage::resizeEvent(QResizeEvent* event)
{
    Page::resizeEvent(event);
    // The page is about 860 px wide in a 1100 px window: the browser folds its places into a menu.
    applyCompact(width() < 1000);
}

void Ps5ConnectPage::applyCompact(bool compact)
{
    if (compact == m_compact)
        return;
    m_compact = compact;
    m_places->setVisible(!compact);
    m_btnPlaces->setVisible(compact);
    for (QPushButton* b : {m_btnUploadFolder, m_btnNewFolder, m_btnDownload, m_btnRename, m_btnDelete}) {
        if (!b->property("fwFullText").isValid())
            b->setProperty("fwFullText", b->text());
        const QString full = b->property("fwFullText").toString();
        b->setText(compact ? QString() : full);
        b->setMaximumWidth(compact ? 40 : QWIDGETSIZE_MAX);
        b->setAccessibleName(full);
        if (compact || b->toolTip().isEmpty())
            b->setToolTip(b->toolTip().isEmpty() ? full : b->toolTip());
    }
}

void Ps5ConnectPage::goBack()
{
    if (m_historyIndex <= 0)
        return;
    --m_historyIndex;
    listRemote(m_history.value(m_historyIndex));
}

bool Ps5ConnectPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_remote && event->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Delete && m_btnDelete->isEnabled()) {
            deleteSelected();
            return true;
        }
        if (k->key() == Qt::Key_F2 && m_btnRename->isEnabled()) {
            renameSelected();
            return true;
        }
        if (k->key() == Qt::Key_Backspace && m_btnUp->isEnabled()) {
            listRemote(ps5::remoteParent(m_remoteDir));
            return true;
        }
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && m_remote->currentRow() >= 0) {
            const int i = entryIndex(m_remote->currentRow());
            if (i >= 0 && m_listing.entries[size_t(i)].isDir)
                if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(i)].name))
                    listRemote(*child);
            return true;
        }
    }
    if (watched == m_remote->viewport()) {
        // Drop files on the list: into the folder under the cursor, else the open folder.
        auto folderAt = [this](const QPoint& pos) -> QString {
            const int row = m_remote->rowAt(pos.y());
            const int i = row >= 0 ? entryIndex(row) : -1;
            if (i >= 0 && m_listing.entries[size_t(i)].isDir)
                if (const auto child = ps5::remoteChild(m_remoteDir, m_listing.entries[size_t(i)].name))
                    return *child;
            return {};
        };
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* d = static_cast<QDropEvent*>(event);
            if (!d->mimeData()->hasUrls() || !m_session->isConnected())
                return false;
            d->acceptProposedAction();
            const int row = m_remote->rowAt(d->position().toPoint().y());
            m_remote->setCurrentCell(!folderAt(d->position().toPoint()).isEmpty() ? row : -1, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_filesCard->setDropHighlight(true);
            return true;
        }
        case QEvent::DragLeave:
            m_filesCard->setDropHighlight(false);
            return false;
        case QEvent::Drop: {
            auto* d = static_cast<QDropEvent*>(event);
            m_filesCard->setDropHighlight(false);
            QStringList paths;
            for (const QUrl& u : d->mimeData()->urls())
                if (!u.toLocalFile().isEmpty())
                    paths << u.toLocalFile();
            const QString target = folderAt(d->position().toPoint());
            d->acceptProposedAction();
            uploadFiles(paths, target);
            return true;
        }
        default:
            break;
        }
    }
    return Page::eventFilter(watched, event);
}

// ---- Payloads ------------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildPayloads(QWidget* parent)
{
    auto* tab = new QWidget(parent);
    auto* outer = vbox(tab, space::lg);
    m_payloadArea = new QWidget(tab);
    auto* pl = new TwoColumnLayout(m_payloadArea, 380);
    pl->setLeftMax(100000);
    auto* listCard = new Card(m_payloadArea, Card::Variant::Table);
    auto* lh = listCard->header();
    lh->addWidget(heading(FW_TR("Your payloads"), TextStyle::Subtitle, listCard));
    lh->addStretch(1);
    auto* add = button(FW_TR("Add payload…"), ButtonVariant::Secondary, QStringLiteral("plus"), listCard);
    setButtonSmall(add, true);
    lh->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { addOrEditPayload(-1); });
    m_payloadList = new QListWidget(listCard);
    m_payloadList->setItemDelegate(new PayloadDelegate(m_payloadList));
    m_payloadList->setMouseTracking(true);
    m_payloadList->setFrameShape(QFrame::NoFrame);
    m_payloadList->setMinimumHeight(6 * 56 + 8);
    m_payloadList->setStyleSheet(QStringLiteral("QListWidget{background:transparent;padding:6px;}"));
    listCard->body()->addWidget(m_payloadList);
    pl->setLeft(listCard);

    m_payloadDetail = new Card(m_payloadArea);
    m_pBadge = new Badge(FW_TR("Ready"), Tone::Ok, m_payloadDetail);
    m_payloadDetail->setTitle(FW_TR("Send"), m_pBadge);
    m_pName = label({}, TextStyle::Subtitle, color::fg_0, m_payloadDetail);
    m_pFile = label({}, TextStyle::MonoSmall, color::fg_3, m_payloadDetail);
    m_pFile->setWordWrap(true);
    m_pNotes = label({}, TextStyle::Body, color::fg_2, m_payloadDetail);
    m_pNotes->setWordWrap(true);
    m_pLastSent = caption({}, m_payloadDetail);
    m_payloadDetail->body()->addWidget(m_pName);
    m_payloadDetail->body()->addWidget(m_pFile);
    m_payloadDetail->body()->addWidget(m_pNotes);
    m_payloadDetail->body()->addWidget(m_pLastSent);
    m_payloadTarget = dropdown(m_payloadDetail);
    m_payloadTarget->setAccessibleName(FW_TR("Payload target"));
    auto fillTargets = [this] {
        const int keep = m_payloadTarget->currentIndex();
        m_payloadTarget->clear();
        const int loader = settingPort(ps5::keys::kPayloadPort, 9021);
        m_payloadTarget->addItem(icon(QStringLiteral("rocket"), color::fg_2), FW_TR("ELF loader (elfldr) · %1").arg(loader),
                                 QStringLiteral("payload:%1").arg(loader));
        m_payloadTarget->addItem(icon(QStringLiteral("zap"), color::fg_2), FW_TR("Exploit loader · 9020"), QStringLiteral("payload:9020"));
        m_payloadTarget->addItem(icon(QStringLiteral("layers"), color::fg_2), FW_TR("ShadowMount+ loader · %1").arg(settingPort(ps5::keys::kShadowMountPort, 9021)),
                                 QStringLiteral("shadowmount:%1").arg(settingPort(ps5::keys::kShadowMountPort, 9021)));
        m_payloadTarget->addItem(icon(QStringLiteral("youtube"), color::fg_2), FW_TR("Y2JB JS server · 50000 (.js)"), QStringLiteral("payload:50000"));
        m_payloadTarget->addItem(icon(QStringLiteral("archive"), color::fg_4), FW_TR("MicroMount loader (archived) · %1").arg(settingPort(ps5::keys::kMicroMountPort, 9021)),
                                 QStringLiteral("micromount:%1").arg(settingPort(ps5::keys::kMicroMountPort, 9021)));
        m_payloadTarget->setCurrentIndex(std::clamp(keep, 0, m_payloadTarget->count() - 1));
    };
    fillTargets();
    connect(&m_ctx.settings(), &SettingsStore::changed, this, [fillTargets](const QString& key, const QVariant&) {
        if (key == ps5::keys::kPayloadPort || key == ps5::keys::kShadowMountPort || key == ps5::keys::kMicroMountPort)
            fillTargets();
    });
    m_targetHint = caption({}, m_payloadDetail);
    m_targetHint->setWordWrap(true);
    auto targetHint = [this] {
        const QString v = m_payloadTarget->currentData().toString();
        m_targetHint->setText(v == u"payload:9020"   ? FW_TR("Only before elfldr runs: the exploit's own loader starts it.")
                            : v == u"payload:50000"  ? FW_TR("Y2JB's remote JS server takes .js files; the port can differ.")
                            : v.startsWith(QLatin1String("micromount")) ? FW_TR("MicroMount is archived; use ShadowMount+.")
                            : v.startsWith(QLatin1String("shadowmount")) ? FW_TR("For shadowmountplus.elf: the loader port set for ShadowMount+ in Settings.")
                                                                         : FW_TR("elfldr runs every payload as its own process."));
    };
    connect(m_payloadTarget, &QComboBox::currentIndexChanged, this, [targetHint](int) { targetHint(); });
    targetHint();
    m_payloadDetail->body()->addWidget(field(FW_TR("Send to"), m_payloadTarget, {}, m_payloadDetail));
    m_payloadDetail->body()->addWidget(m_targetHint);
    auto* pa = new QWidget(m_payloadDetail);
    auto* pal = hbox(pa, space::sm);
    m_pRemove = button(FW_TR("Remove"), ButtonVariant::Subtle, QStringLiteral("trash"), pa);
    m_pEdit = button(FW_TR("Edit…"), ButtonVariant::Secondary, QStringLiteral("edit"), pa);
    m_pSend = button(FW_TR("Send"), ButtonVariant::Primary, QStringLiteral("send"), pa);
    m_pSend->setMinimumWidth(110);
    pal->addWidget(m_pRemove);
    pal->addStretch(1);
    pal->addWidget(m_pEdit);
    pal->addWidget(m_pSend);
    m_payloadDetail->body()->addWidget(pa);
    auto* warn = new InfoBar(Tone::Warn, m_payloadDetail);
    warn->setMessage(FW_TR("A second resident payload can crash the kernel; reboot between unrelated ones."));
    m_payloadDetail->body()->addWidget(warn);
    pl->setRight(m_payloadDetail);
    outer->addWidget(m_payloadArea);

    m_payloadEmpty = new EmptyState(QStringLiteral("rocket"), FW_TR("No payloads yet"),
                                    FW_TR("Add the .elf and .bin payloads you use (elfldr, kstuff, ftpsrv, ShadowMount+…) to send them in one click."), tab);
    auto* addFirst = m_payloadEmpty->addAction(FW_TR("Add payload…"), ButtonVariant::Primary);
    connect(addFirst, &QPushButton::clicked, this, [this] { addOrEditPayload(-1); });
    outer->addWidget(m_payloadEmpty);
    outer->addStretch(1);

    connect(m_payloadList, &QListWidget::currentRowChanged, this, &Ps5ConnectPage::showPayload);
    connect(m_payloadList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { sendPayload(m_currentPayload); });
    connect(m_pSend, &QPushButton::clicked, this, [this] { sendPayload(m_currentPayload); });
    connect(m_pEdit, &QPushButton::clicked, this, [this] { addOrEditPayload(m_currentPayload); });
    connect(m_pRemove, &QPushButton::clicked, this, [this] {
        if (m_currentPayload < 0)
            return;
        if (m_currentPayload >= int(m_payloads.entries().size()))
            return;
        const PayloadEntry e = m_payloads.entries()[size_t(m_currentPayload)];
        if (!Sheet::confirm(window(), FW_TR("Remove this payload from the list?"),
                            FW_TR("%1 is removed from the list only; the file stays where it is.").arg(e.name),
                            FW_TR("Keep"), FW_TR("Remove")))
            return;
        // The entry the question named, wherever it is in the list now.
        m_payloads.remove(payloadIndex(e));
    });
    return tab;
}

void Ps5ConnectPage::rebuildPayloads()
{
    const int cur = m_payloadList->currentRow();
    m_payloadList->clear();
    for (const auto& e : m_payloads.entries()) {
        const QFileInfo fi(e.path);
        auto* it = new QListWidgetItem(e.name);
        it->setData(PayloadDelegate::FileRole, fi.exists() ? QStringLiteral("%1 · %2").arg(fi.fileName(), formatBytes(fi.size())) : FW_TR("%1 · missing").arg(fi.fileName()));
        it->setData(PayloadDelegate::SentRole, e.lastSent.isValid() ? FW_TR("Sent %1").arg(relativeTime(e.lastSent)) : QString());
        it->setData(PayloadDelegate::MissingRole, !fi.exists());
        it->setToolTip(nativePath(e.path));
        m_payloadList->addItem(it);
    }
    const bool any = m_payloadList->count() > 0;
    m_payloadArea->setVisible(any);
    m_payloadEmpty->setVisible(!any);
    if (any)
        m_payloadList->setCurrentRow(std::clamp(cur, 0, m_payloadList->count() - 1));
    else
        showPayload(-1);
}

void Ps5ConnectPage::showPayload(int index)
{
    m_currentPayload = index;
    if (index < 0 || index >= int(m_payloads.entries().size())) {
        m_payloadDetail->setEnabled(false);
        return;
    }
    m_payloadDetail->setEnabled(true);
    const auto& e = m_payloads.entries()[size_t(index)];
    m_pName->setText(e.name);
    const QFileInfo fi(e.path);
    m_pFile->setText(fi.exists() ? QStringLiteral("%1 · %2").arg(nativePath(e.path), formatBytes(fi.size())) : nativePath(e.path));
    m_pNotes->setText(e.notes);
    m_pNotes->setVisible(!e.notes.isEmpty());
    m_pLastSent->setText(e.lastSent.isValid() ? FW_TR("Last sent %1").arg(humanTime(e.lastSent)) : FW_TR("Never sent"));
    m_pBadge->set(fi.exists() ? FW_TR("Ready") : FW_TR("Missing"), fi.exists() ? Tone::Ok : Tone::Err);
    const bool can = fi.exists() && jobs::isAvailable(m_ctx, ps5::jobkinds::kPayloadSend);
    m_pSend->setEnabled(can);
    m_pSend->setToolTip(!fi.exists() ? FW_TR("The file is missing") : can ? QString() : FW_TR("Not available in this build"));
}

void Ps5ConnectPage::addOrEditPayload(int index)
{
    PayloadEntry e;
    if (index >= 0 && index < int(m_payloads.entries().size()))
        e = m_payloads.entries()[size_t(index)];
    else
        index = -1;
    const PayloadEntry before = e;
    auto* s = new Sheet(window(), index < 0 ? FW_TR("Add payload") : FW_TR("Edit payload"), Sheet::Icon::None, size::dialogWide);
    auto* name = textInput(QStringLiteral("elfldr"), false, s);
    name->setText(e.name);
    s->body()->addWidget(field(FW_TR("Name"), name, {}, s));
    auto* file = new PathField(PathField::Mode::OpenFile, s);
    file->setFilter(FW_TR("Payloads") + QStringLiteral(" (*.elf *.bin *.jar *.js);;") + FW_TR("All files") + QStringLiteral(" (*)"));
    file->setPath(e.path);
    s->body()->addWidget(field(FW_TR("File"), file, {}, s));
    auto* notes = new QPlainTextEdit(s);
    notes->setPlainText(e.notes);
    notes->setFixedHeight(72);
    notes->setAccessibleName(FW_TR("Notes"));
    s->body()->addWidget(field(FW_TR("Notes"), notes, {}, s));
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(index < 0 ? FW_TR("Add") : FW_TR("Save"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    e.name = name->text().trimmed().isEmpty() ? QFileInfo(file->path()).fileName() : name->text().trimmed();
    e.path = file->path();
    e.notes = notes->toPlainText().trimmed();
    if (e.path.isEmpty()) {
        emit toastRequested(Tone::Warn, index < 0 ? FW_TR("Payload not added") : FW_TR("Payload not changed"), FW_TR("Choose the payload's file."));
        return;
    }
    if (index < 0)
        m_payloads.add(e);
    else
        m_payloads.update(payloadIndex(before), e); // the entry that was opened, wherever it is now
}

int Ps5ConnectPage::payloadIndex(const PayloadEntry& entry) const
{
    const auto& all = m_payloads.entries();
    for (int i = 0; i < int(all.size()); ++i)
        if (all[size_t(i)].path == entry.path && all[size_t(i)].name == entry.name)
            return i;
    return -1;
}

void Ps5ConnectPage::sendPayload(int index)
{
    if (index < 0 || index >= int(m_payloads.entries().size()))
        return;
    // One send at a time: a second click (or a double click on the list) must not load the payload twice.
    if (const auto running = m_payloadSendId.isEmpty() ? std::nullopt : m_ctx.queue().record(m_payloadSendId); running && !isTerminal(running->state)) {
        emit toastRequested(Tone::Info, FW_TR("A payload is being sent"), FW_TR("Wait until it has finished, then send the next one."));
        return;
    }
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, FW_TR("No console address"), FW_TR("Enter the console's IP address at the top of the page first."));
        return;
    }
    const QString target = m_payloadTarget->currentData().toString();
    const int port = target.section(u':', 1).toInt() > 0 ? target.section(u':', 1).toInt() : settingPort(ps5::keys::kPayloadPort, 9021);
    const auto e = m_payloads.entries()[size_t(index)];
    auto r = jobs::submitPayload(m_ctx, host(), port, e.path);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, FW_TR("Payload not sent"), r.error().message);
        return;
    }
    m_payloadSendId = r.value();
    m_payloadSendPath = e.path;
    m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
    m_hero->setChip(QStringLiteral("payload"), Tone::Info, FW_TR("%1 · sending").arg(port));
    emit toastRequested(Tone::Info, FW_TR("Sending payload"), QStringLiteral("%1 → %2:%3").arg(e.name, host()).arg(port));
}

// ---- Kernel log ---------------------------------------------------------------------

QWidget* Ps5ConnectPage::buildKlog(QWidget* parent)
{
    auto* tab = new QWidget(parent);
    auto* l = vbox(tab, space::lg);
    auto* card = new Card(tab, Card::Variant::Table);
    auto* h = card->header();
    h->setSpacing(space::md);
    m_klogConnect = button(FW_TR("Connect"), ButtonVariant::Primary, QStringLiteral("plug"), card);
    m_klogPause = button(FW_TR("Pause"), ButtonVariant::Secondary, QStringLiteral("pause"), card);
    m_klogPause->setEnabled(false);
    m_klogBadge = new Badge(FW_TR("Disconnected"), Tone::Neutral, card);
    m_klogBadge->setIconName({});
    h->addWidget(m_klogConnect);
    h->addWidget(m_klogPause);
    h->addWidget(m_klogBadge);
    m_klogFilter = textInput(FW_TR("Filter lines"), false, card);
    m_klogFilter->setMaximumWidth(240);
    m_klogFilter->setClearButtonEnabled(true);
    h->addWidget(m_klogFilter, 1);
    h->addStretch(0);
    auto* snapshot = new IconButton(QStringLiteral("layers"), FW_TR("Snapshot from ShadowMount+"), IconButton::Size::Regular, card);
    snapshot->setToolTip(FW_TR("Read a kernel-log snapshot through the ShadowMount+ 1.7 API (when no klog server runs)"));
    auto* clear = new IconButton(QStringLiteral("trash"), FW_TR("Clear"), IconButton::Size::Regular, card);
    auto* exportBtn = new IconButton(QStringLiteral("save"), FW_TR("Export…"), IconButton::Size::Regular, card);
    h->addWidget(snapshot);
    h->addWidget(clear);
    h->addWidget(exportBtn);
    h->addWidget(label(FW_TR("Follow"), TextStyle::Body, color::fg_1, card));
    m_klogFollow = new Switch(card);
    m_klogFollow->setChecked(true);
    m_klogFollow->setAccessibleName(FW_TR("Follow new lines"));
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
    m_klogView->setFrameShape(QFrame::NoFrame);
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
    m_klogView->setMinimumHeight(14 * size::rowCompact + 40);
    card->body()->addWidget(m_klogView);
    m_klogStats = caption(FW_TR("0 lines"), card);
    card->footer()->addWidget(m_klogStats, 1);
    card->footer()->addWidget(caption(FW_TR("klogsrv 3232 · etaHEN 9081 (Klog=1) · read only · last 20,000 lines"), card));
    l->addWidget(card);
    l->addStretch(1);

    connect(m_klogConnect, &QPushButton::clicked, this, &Ps5ConnectPage::klogToggle);
    connect(m_klogPause, &QPushButton::clicked, this, [this] {
        m_klogPaused = !m_klogPaused;
        m_klogPause->setText(m_klogPaused ? FW_TR("Resume") : FW_TR("Pause"));
        setButtonIcon(m_klogPause, m_klogPaused ? QStringLiteral("play") : QStringLiteral("pause"));
        if (!m_klogPaused) {
            m_klog->append(std::vector<ps5::KlogLine>(m_klogHeld.begin(), m_klogHeld.end()));
            m_klogHeld.clear();
            if (m_klogFollow->isChecked())
                m_klogView->scrollToBottom();
        }
    });
    connect(clear, &QToolButton::clicked, this, [this] {
        m_klog->clear();
        m_klogHeld.clear();
        m_klogIncoming.clear();
        updateKlogStats();
    });
    connect(exportBtn, &QToolButton::clicked, this, &Ps5ConnectPage::klogExport);
    connect(snapshot, &QToolButton::clicked, this, &Ps5ConnectPage::klogSnapshot);
    connect(m_klogFilter, &QLineEdit::textChanged, this, [this](const QString&) { klogFilterChanged(); });
    // New rows pass through the proxy's filter by themselves: only the counts change here.
    connect(m_klog, &QAbstractItemModel::rowsInserted, this, [this] {
        if (m_klogFollow->isChecked())
            QMetaObject::invokeMethod(m_klogView, &QTableView::scrollToBottom, Qt::QueuedConnection);
        updateKlogStats();
    });
    // Lines arrive one by one from the reader thread; the view takes them in batches.
    m_klogFlush = new QTimer(this);
    m_klogFlush->setSingleShot(true);
    m_klogFlush->setInterval(120);
    connect(m_klogFlush, &QTimer::timeout, this, &Ps5ConnectPage::flushKlog);
    return tab;
}

void Ps5ConnectPage::klogFilterChanged()
{
    m_klogProxy->setFilterFixedString(m_klogFilter->text());
    updateKlogStats();
}

void Ps5ConnectPage::updateKlogStats()
{
    if (m_klogFilter->text().isEmpty())
        m_klogStats->setText(FW_TR("%1 lines · %2 warnings · %3 errors")
                                 .arg(formatCount(m_klog->rowCount())).arg(m_klog->warnings()).arg(m_klog->errors()));
    else
        m_klogStats->setText(FW_TR("%1 lines · %2 warnings · %3 errors · %4 shown")
                                 .arg(formatCount(m_klog->rowCount())).arg(m_klog->warnings()).arg(m_klog->errors())
                                 .arg(formatCount(m_klogProxy->rowCount())));
    emit statusContextChanged();
}

void Ps5ConnectPage::flushKlog()
{
    if (m_klogIncoming.empty())
        return;
    std::vector<ps5::KlogLine> lines;
    lines.swap(m_klogIncoming);
    if (!m_klogPaused) {
        m_klog->append(lines);
        return;
    }
    m_klogHeld.insert(m_klogHeld.end(), lines.begin(), lines.end());
    while (m_klogHeld.size() > 20000)
        m_klogHeld.pop_front();
}

void Ps5ConnectPage::klogToggle()
{
    // Lines and states a stopped client already posted are not applied any more.
    ++m_klogGen;
    if (m_klogClient) {
        m_klogClient->stop();
        m_klogClient.reset();
        flushKlog(); // what arrived before the stop is kept
        klogState(ps5::KlogState::Stopped, {});
        return;
    }
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, FW_TR("No console address"), FW_TR("Enter the console's IP address at the top of the page first."));
        return;
    }
    ps5::KlogClient::Options o;
    o.host = host();
    o.port = quint16(settingPort(ps5::keys::kKlogPort, 3232));
    o.connectTimeoutMs = settingPort(ps5::keys::kConnectTimeout, 10) * 1000;
    // Posted to the application object: the page may be gone when a call arrives, and only the UI thread may ask.
    QPointer<Ps5ConnectPage> self(this);
    const quint64 gen = m_klogGen;
    m_klogClient = std::make_unique<ps5::KlogClient>(
        o,
        [self, gen](const ps5::KlogLine& line) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, line] {
                if (!self || gen != self->m_klogGen)
                    return;
                self->m_klogIncoming.push_back(line);
                if (!self->m_klogFlush->isActive())
                    self->m_klogFlush->start();
            }, Qt::QueuedConnection);
        },
        [self, gen](ps5::KlogState st, const QString& detail) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, st, detail] {
                if (self && gen == self->m_klogGen)
                    self->klogState(st, detail);
            }, Qt::QueuedConnection);
        });
    m_klogClient->start();
    klogState(ps5::KlogState::Connecting, {});
}

void Ps5ConnectPage::klogState(ps5::KlogState state, const QString& detail)
{
    const bool running = m_klogClient != nullptr;
    m_klogConnect->setText(running ? FW_TR("Disconnect") : FW_TR("Connect"));
    setButtonVariant(m_klogConnect, running ? ButtonVariant::Secondary : ButtonVariant::Primary);
    setButtonIcon(m_klogConnect, running ? QStringLiteral("unplug") : QStringLiteral("plug"));
    m_klogPause->setEnabled(running);
    Tone tone = Tone::Neutral;
    QString word = ps5::klogStateName(state);
    if (!word.isEmpty())
        word[0] = word[0].toUpper();
    switch (state) {
    case ps5::KlogState::Connected:
        tone = Tone::Ok;
        word = FW_TR("Connected · %1:%2").arg(host()).arg(settingPort(ps5::keys::kKlogPort, 3232));
        m_hero->setChip(QStringLiteral("klog"), Tone::Ok, FW_TR("%1 · live").arg(settingPort(ps5::keys::kKlogPort, 3232)));
        break;
    // The words of klogStateName(), as the interface shows them.
    case ps5::KlogState::Connecting: tone = Tone::Info; word = FW_TR("Connecting"); break;
    case ps5::KlogState::Reconnecting: tone = Tone::Info; word = FW_TR("Reconnecting"); break;
    case ps5::KlogState::Disconnected: tone = Tone::Warn; word = FW_TR("Disconnected"); break;
    case ps5::KlogState::Stopped: tone = Tone::Neutral; word = FW_TR("Disconnected"); break;
    }
    // The card's chip said "live" while the stream ran; it does not any more.
    if (state != ps5::KlogState::Connected && m_klogLive)
        m_hero->setChip(QStringLiteral("klog"), Tone::Neutral, QString::number(settingPort(ps5::keys::kKlogPort, 3232)));
    m_klogLive = state == ps5::KlogState::Connected;
    m_klogBadge->set(word, tone);
    m_klogBadge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
    m_klogBadge->setToolTip(detail);
    emit statusContextChanged();
}

void Ps5ConnectPage::klogExport()
{
    const QString path = QFileDialog::getSaveFileName(window(), FW_TR("Export the kernel log"),
                                                      QDir::homePath() + QStringLiteral("/klog_%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
                                                      FW_TR("Text files") + QStringLiteral(" (*.txt)"));
    if (path.isEmpty())
        return;
    // Everything received: also the lines held back while paused.
    flushKlog();
    QString held;
    for (const ps5::KlogLine& l : m_klogHeld)
        held += l.received.toString(QStringLiteral("HH:mm:ss.zzz")) + u'\t' + ps5::klogTagName(l.tag) + u'\t' + l.text + u'\n';
    auto st = writeFileAtomic(path, (QStringLiteral("PS5 kernel log export %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate)) + m_klog->allText() + held).toUtf8());
    emit toastRequested(st.ok() ? Tone::Ok : Tone::Err, st.ok() ? FW_TR("Kernel log exported") : FW_TR("Export failed"), st.ok() ? nativePath(path) : st.error().message);
}

void Ps5ConnectPage::klogSnapshot()
{
    if (host().isEmpty()) {
        emit toastRequested(Tone::Warn, FW_TR("No console address"), FW_TR("Enter the console's IP address at the top of the page first."));
        return;
    }
    const ps5::ConsoleEndpoint ep = endpoint();
    QPointer<Ps5ConnectPage> self(this);
    (void)QtConcurrent::run([self, ep] {
        auto r = ps5::smp::kernelLog(ep.host, ep.smpApiPort);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, r] {
            if (!self)
                return;
            if (!r.ok()) {
                emit self->toastRequested(Tone::Warn, FW_TR("No snapshot"),
                                          FW_TR("%1 The snapshot needs ShadowMount+ 1.7 running with its API.").arg(r.error().message));
                return;
            }
            const QDateTime now = QDateTime::currentDateTime();
            std::vector<ps5::KlogLine> lines;
            for (const QString& t : r.value().split(u'\n', Qt::SkipEmptyParts)) {
                ps5::KlogLine l;
                l.received = now;
                l.text = t;
                l.tag = ps5::klogTag(t);
                lines.push_back(l);
            }
            self->m_klog->append(lines);
            emit self->toastRequested(Tone::Ok, FW_TR("Kernel log snapshot"), FW_TR("From ShadowMount+; a snapshot, not a live stream."));
        }, Qt::QueuedConnection);
    });
}

// ---- Y2JB -----------------------------------------------------------------------------

// The private build's own YouTube packages (make-release -PrivateYoutube).
QString bundledYoutubeFolder() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("tools/youtube")); }

void Ps5ConnectPage::installYoutube(const QString& path)
{
    auto r = jobs::submitDpiInstall(m_ctx, host(), settingPort(ps5::keys::kDpiPort, 12800), path);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, FW_TR("Install not started"), r.error().message);
        return;
    }
    m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
    emit toastRequested(Tone::Info, FW_TR("Install queued"), QFileInfo(path).fileName());
}

void Ps5ConnectPage::scanYoutubePackages()
{
    QStringList folders;
    if (QFileInfo(bundledYoutubeFolder()).isDir())
        folders << bundledYoutubeFolder();
    const QString mine = m_ytFolder ? m_ytFolder->path().trimmed() : QString();
    if (!mine.isEmpty() && QFileInfo(mine).isDir())
        folders << mine;
    const quint64 gen = ++m_ytScan;
    QPointer<Ps5ConnectPage> self(this);
    const QString bundled = bundledYoutubeFolder();
    (void)QtConcurrent::run([self, gen, folders, bundled] {
        std::vector<YoutubePackage> found;
        static const QRegularExpression tidRe(QStringLiteral("PPSA0165[0-2]"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression verRe(QStringLiteral("v?([0-9]{2}\\.[0-9]{3}\\.[0-9]{3})"));
        for (const QString& folder : folders) {
            const QFileInfoList pkgs = QDir(folder).entryInfoList({QStringLiteral("*.pkg")}, QDir::Files, QDir::Name);
            for (const QFileInfo& fi : pkgs) {
                YoutubePackage p;
                p.path = fi.absoluteFilePath();
                p.bundled = folder == bundled;
                CancellationSource c;
                if (auto id = fpkg::inspectPackage(p.path, c.token()); id.ok()) {
                    p.titleId = id->titleId;
                    p.version = ps5::normalizeYoutubeVersion(id->version);
                }
                // Fall back to the file name ("YouTube PPSA01650 v01.000.003.pkg").
                if (p.titleId.isEmpty())
                    if (const auto m = tidRe.match(fi.fileName()); m.hasMatch())
                        p.titleId = m.captured(0).toUpper();
                if (p.version.isEmpty())
                    if (const auto m = verRe.match(fi.fileName()); m.hasMatch())
                        p.version = ps5::normalizeYoutubeVersion(m.captured(1));
                if (!ps5::y2jbRegionFromTitleId(p.titleId))
                    continue; // not a YouTube package
                p.firmware = ps5::y2jbFirmwareRangeFor(p.version);
                found.push_back(p);
            }
        }
        QMetaObject::invokeMethod(qApp, [self, gen, found] {
            if (!self || gen != self->m_ytScan)
                return;
            self->m_ytPackages = found;
            self->rebuildYoutubeList();
        }, Qt::QueuedConnection);
    });
}

void Ps5ConnectPage::rebuildYoutubeList()
{
    if (!m_ytList)
        return;
    QLayout* l = m_ytList->layout();
    while (QLayoutItem* it = l->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
    const QString want = ps5::y2jbYoutubeVersionFor(m_firmware->text());
    const QString tid = ps5::y2jbTitleId(*ps5::y2jbRegionFromName(region()));
    bool haveBest = false;
    for (const YoutubePackage& p : m_ytPackages) {
        auto* row = new QWidget(m_ytList);
        auto* h = hbox(row, space::md, QMargins(0, space::xs, 0, space::xs));
        h->addWidget(label(p.version.isEmpty() ? FW_TR("unknown version") : p.version, TextStyle::Mono, color::fg_1, row));
        const auto reg = ps5::y2jbRegionFromTitleId(p.titleId);
        QString meta = QStringLiteral("%1 · %2").arg(reg ? ps5::y2jbRegionName(*reg) : QString(), p.titleId);
        meta += QStringLiteral(" · ") + (p.firmware.isEmpty() ? FW_TR("not a Y2JB version") : FW_TR("firmware %1").arg(p.firmware));
        if (p.bundled)
            meta += QStringLiteral(" · ") + FW_TR("included");
        auto* m = caption(meta, row);
        m->setToolTip(QDir::toNativeSeparators(p.path));
        h->addWidget(m, 1);
        const bool best = !want.isEmpty() && p.version == want;
        if (best) {
            auto* b = new Badge(p.titleId == tid ? FW_TR("Your firmware") : FW_TR("Your firmware, other region"),
                                p.titleId == tid ? Tone::Ok : Tone::Warn, row);
            h->addWidget(b);
            haveBest = true;
        }
        auto* install = button(FW_TR("Install"), ButtonVariant::Secondary, QStringLiteral("upload"), row);
        install->setAccessibleName(FW_TR("Install %1").arg(QFileInfo(p.path).fileName()));
        install->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kDpiInstall));
        connect(install, &QPushButton::clicked, this, [this, path = p.path] { installYoutube(path); });
        h->addWidget(install);
        l->addWidget(row);
    }
    if (m_ytPackages.empty()) {
        m_ytHint->setText(FW_TR("No YouTube packages yet. Put your own YouTube .pkg files in a folder and choose it above; "
                                         "the list shows which firmware each one is for."));
    } else if (want.isEmpty()) {
        m_ytHint->setText(m_ytPackages.size() == 1
                              ? FW_TR("%1 package found. Type the console's firmware in step 1 to pick the right one.").arg(m_ytPackages.size())
                              : FW_TR("%1 packages found. Type the console's firmware in step 1 to pick the right one.").arg(m_ytPackages.size()));
    } else if (!haveBest) {
        m_ytHint->setText(FW_TR("Firmware %1 needs YouTube %2, which is not in your packages.").arg(m_firmware->text().trimmed(), want));
    } else {
        m_ytHint->setText(FW_TR("Firmware %1 needs YouTube %2.").arg(m_firmware->text().trimmed(), want));
    }
    m_ytBest->setEnabled(haveBest && jobs::isAvailable(m_ctx, ps5::jobkinds::kDpiInstall));
}

QWidget* Ps5ConnectPage::buildY2jb(QWidget* parent)
{
    auto* tab = new QWidget(parent);
    auto* l = vbox(tab, space::lg);

    // 1. The YouTube version for this firmware, and the region.
    auto* ver = new Card(tab);
    auto* stepBadge = [](const QString& text, QWidget* p) {
        auto* b = new Badge(text, Tone::Neutral, p);
        b->setIconName({});
        return b;
    }(FW_TR("Step 1"), ver);
    ver->setTitle(FW_TR("YouTube version and region"), stepBadge);
    auto* vrow = new QWidget(ver);
    auto* vh = hbox(vrow, space::lg);
    m_firmware = textInput(FW_TR("e.g. 9.60"), true, vrow);
    m_firmware->setFixedWidth(120);
    m_firmware->setAccessibleName(FW_TR("Firmware"));
    vh->addWidget(field(FW_TR("Console firmware"), m_firmware, {}, vrow));
    m_region = new Segmented(vrow);
    for (const auto r : ps5::allY2jbRegions())
        m_region->addSegment(QStringLiteral("%1 · %2").arg(ps5::y2jbRegionName(r), ps5::y2jbTitleId(r)), {}, ps5::y2jbRegionName(r));
    m_region->setAccessibleName(FW_TR("Region"));
    vh->addWidget(field(FW_TR("YouTube app"), m_region, {}, vrow));
    vh->addStretch(1);
    ver->body()->addWidget(vrow);
    m_youtubeVersion = label(youtubeHelp({}), TextStyle::BodyStrong, color::fg_2, ver);
    ver->body()->addWidget(m_youtubeVersion);
    auto* table = new QWidget(ver);
    auto* tg = new QGridLayout(table);
    tg->setContentsMargins(0, 0, 0, 0);
    tg->setHorizontalSpacing(space::xxl);
    tg->setVerticalSpacing(space::xs);
    tg->addWidget(label(FW_TR("FIRMWARE"), TextStyle::Overline, color::fg_4, table), 0, 0);
    tg->addWidget(label(QStringLiteral("YOUTUBE"), TextStyle::Overline, color::fg_4, table), 0, 1);
    tg->addWidget(label(FW_TR("EXPLOIT"), TextStyle::Overline, color::fg_4, table), 0, 2);
    int row = 1;
    for (const ps5::Y2jbYoutubeVersion& v : ps5::y2jbYoutubeVersions()) {
        tg->addWidget(label(QStringLiteral("%1 – %2").arg(v.firmwareFrom, v.firmwareTo), TextStyle::Mono, color::fg_2, table), row, 0);
        tg->addWidget(label(v.youtube, TextStyle::Mono, color::fg_1, table), row, 1);
        auto* ex = label(fw::i18n::translate(v.exploit), TextStyle::Caption, color::fg_3, table);
        ex->setToolTip(FW_TR("From %1").arg(v.source));
        tg->addWidget(ex, row, 2);
        ++row;
    }
    tg->setColumnStretch(3, 1);
    ver->body()->addWidget(table);
    ver->body()->addWidget(caption(FW_TR("Firmware 12.41 to 12.59 is not covered by any Y2JB release. Above 13.60 nothing is "
                                                  "known to work yet."), ver));
    connect(m_firmware, &QLineEdit::textChanged, this, [this](const QString& fw) {
        m_youtubeVersion->setText(youtubeHelp(fw));
        setTextColor(m_youtubeVersion, ps5::y2jbYoutubeVersionFor(fw).isEmpty() && !fw.trimmed().isEmpty() ? color::warn_hi : color::fg_2);
        rebuildYoutubeList();
    });
    connect(m_region, &Segmented::currentChanged, this, [this](int) { rebuildYoutubeList(); });
    l->addWidget(ver);

    // 2. Install it.
    auto* dpi = new Card(tab);
    dpi->setTitle(FW_TR("Install the YouTube package"), [](QWidget* p) {
        auto* b = new Badge(FW_TR("Step 2"), Tone::Neutral, p);
        b->setIconName({});
        return b;
    }(dpi));
    m_ytFolder = new PathField(PathField::Mode::Directory, dpi);
    m_ytFolder->setDialogTitle(FW_TR("Choose the folder with your YouTube .pkg files"));
    m_ytFolder->setPlaceholder(FW_TR("The folder with your own YouTube .pkg files"));
    m_ytFolder->setPath(m_ctx.settings().value(ps5::keys::kYoutubePackages).toString());
    dpi->body()->addWidget(field(FW_TR("Your YouTube packages"), m_ytFolder, {}, dpi));
    connect(m_ytFolder, &PathField::pathChanged, this, [this](const QString& p) {
        (void)m_ctx.settings().setValue(ps5::keys::kYoutubePackages, p);
        scanYoutubePackages();
    });
    m_ytList = new QWidget(dpi);
    vbox(m_ytList, space::xs);
    dpi->body()->addWidget(m_ytList);
    auto* bestRow = new QWidget(dpi);
    auto* besth = hbox(bestRow, space::sm);
    m_ytHint = caption({}, bestRow);
    m_ytHint->setWordWrap(true);
    besth->addWidget(m_ytHint, 1);
    m_ytBest = button(FW_TR("Install the right version"), ButtonVariant::Primary, QStringLiteral("upload"), bestRow);
    m_ytBest->setAccessibleName(FW_TR("Install the right YouTube version"));
    besth->addWidget(m_ytBest);
    dpi->body()->addWidget(bestRow);
    connect(m_ytBest, &QPushButton::clicked, this, [this] {
        const QString want = ps5::y2jbYoutubeVersionFor(m_firmware->text());
        const QString tid = ps5::y2jbTitleId(*ps5::y2jbRegionFromName(region()));
        const YoutubePackage* pick = nullptr;
        for (const auto& p : m_ytPackages)
            if (p.version == want && (!pick || p.titleId == tid))
                pick = &p;
        if (pick)
            installYoutube(pick->path);
    });
    dpi->body()->addWidget(hairline(dpi));
    m_pkg = new PathField(PathField::Mode::OpenFile, dpi);
    m_pkg->setFilter(FW_TR("Packages") + QStringLiteral(" (*.pkg)"));
    m_pkg->setDialogTitle(FW_TR("Choose the YouTube .pkg"));
    dpi->body()->addWidget(field(FW_TR("Or choose one package"), m_pkg, {}, dpi));
    auto* dpiRow = new QWidget(dpi);
    auto* dh = hbox(dpiRow, space::sm);
    m_dpiStatus = caption(FW_TR("Through etaHEN's Direct Package Installer V2 (DPI_v2=1 in /data/etaHEN/config.ini)."), dpiRow);
    m_dpiStatus->setWordWrap(true);
    dh->addWidget(m_dpiStatus, 1);
    auto* test = button(FW_TR("Test installer"), ButtonVariant::Secondary, QStringLiteral("plug"), dpiRow);
    m_install = button(FW_TR("Install on PS5"), ButtonVariant::Primary, QStringLiteral("upload"), dpiRow);
    m_install->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kDpiInstall));
    dh->addWidget(test);
    dh->addWidget(m_install);
    dpi->body()->addWidget(dpiRow);
    l->addWidget(dpi);
    connect(test, &QPushButton::clicked, this, [this] {
        if (host().isEmpty()) {
            m_dpiStatus->setText(FW_TR("Enter the console's IP address first."));
            return;
        }
        m_dpiStatus->setText(FW_TR("Testing…"));
        const QString h = host();
        const quint16 port = quint16(settingPort(ps5::keys::kDpiPort, 12800));
        const int timeout = settingPort(ps5::keys::kConnectTimeout, 10) * 1000;
        QPointer<Ps5ConnectPage> self(this);
        (void)QtConcurrent::run([self, h, port, timeout] {
            auto st = ps5::probeTcpPort(h, port, timeout, QStringLiteral("Direct Package Installer"));
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, st, port] {
                if (!self)
                    return;
                self->m_dpiStatus->setText(st.ok() ? FW_TR("The installer answers on port %1.").arg(port)
                                                   : FW_TR("%1 Turn on DPI_v2 in etaHEN.").arg(st.error().message));
                setTextColor(self->m_dpiStatus, st.ok() ? color::success_hi : color::danger_hi);
            }, Qt::QueuedConnection);
        });
    });
    connect(m_install, &QPushButton::clicked, this, [this] {
        if (m_pkg->path().isEmpty()) {
            m_pkg->setError(FW_TR("Choose the YouTube .pkg first."));
            return;
        }
        m_pkg->setError({});
        installYoutube(m_pkg->path());
    });
    scanYoutubePackages();

    // 3. Block its updates, then reboot.
    auto* block = new Card(tab);
    block->setTitle(FW_TR("Block YouTube updates"), [](QWidget* p) {
        auto* b = new Badge(FW_TR("Step 3"), Tone::Neutral, p);
        b->setIconName({});
        return b;
    }(block));
    block->body()->addWidget(caption(FW_TR("Stay in the PS5 Settings menu while it runs, then reboot the PS5."), block));
    auto* brow = new QWidget(block);
    auto* bh = hbox(brow, space::sm);
    auto* what = caption(FW_TR("Upstream patches appinfo.db; this also patches param.json and app.db, backed up first."), brow);
    what->setWordWrap(true);
    bh->addWidget(what, 1);
    m_autoBlock = button(FW_TR("Auto-block over FTP…"), ButtonVariant::Danger, QStringLiteral("shield"), brow);
    m_autoBlock->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kY2jbAutoBlock));
    bh->addWidget(m_autoBlock);
    block->body()->addWidget(brow);
    connect(m_autoBlock, &QPushButton::clicked, this, [this] {
        if (host().isEmpty()) {
            emit toastRequested(Tone::Warn, FW_TR("No console address"), FW_TR("Enter the console's IP address at the top of the page first."));
            return;
        }
        const QString reg = region();
        const QString tid = ps5::y2jbTitleId(*ps5::y2jbRegionFromName(reg));
        // One folder: the one the question names is the one the job writes to.
        const QString backupDir = ps5::newConsoleBackupDir();
        const QString consoleHost = host();
        const int consolePort = ftpPort();
        if (!Sheet::confirm(window(), FW_TR("Block YouTube updates for %1?").arg(reg),
                            FW_TR("This patches param.json, appinfo.db and app.db for %1 (%2) on your PS5. Make sure you are in the PS5 Settings menu before continuing, and reboot the PS5 afterwards. Backups go to %3.")
                                .arg(reg.toHtmlEscaped(), Sheet::monoSpan(tid), Sheet::monoSpan(nativePath(backupDir))),
                            FW_TR("Not now"), FW_TR("Block updates"), true, Sheet::Icon::Warning))
            return;
        auto r = jobs::submitY2jbAutoBlock(m_ctx, consoleHost, consolePort, reg, backupDir);
        if (!r.ok()) {
            emit toastRequested(Tone::Err, FW_TR("Not started"), r.error().message);
            return;
        }
        m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
        emit toastRequested(Tone::Info, FW_TR("Auto-block queued"), FW_TR("%1 · %2 — reboot the PS5 when it is done").arg(reg, tid));
    });
    l->addWidget(block);

    // 4. Y2JB itself.
    auto* load = new Card(tab);
    load->setTitle(FW_TR("Put Y2JB on the console"), [](QWidget* p) {
        auto* b = new Badge(FW_TR("Step 4"), Tone::Neutral, p);
        b->setIconName({});
        return b;
    }(load));
    auto* lrow = new QWidget(load);
    auto* lh = hbox(lrow, space::sm);
    auto* how = caption(FW_TR("Upload download0.dat to the app's download folder. Autoloader updates: y2jb_update.zip on the USB root."), lrow);
    how->setWordWrap(true);
    lh->addWidget(how, 1);
    auto* open = button(FW_TR("Open the folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), lrow);
    lh->addWidget(open);
    load->body()->addWidget(lrow);
    connect(open, &QPushButton::clicked, this, [this] {
        const QString dir = ps5::remoteParent(ps5::y2jbDownload0Path(*ps5::y2jbRegionFromName(region())));
        if (!m_session->isConnected()) {
            emit toastRequested(Tone::Warn, FW_TR("Not connected"), FW_TR("Connect first, then open %1.").arg(dir));
            return;
        }
        showTool(QStringLiteral("ftp"));
        listRemote(dir);
    });
    l->addWidget(load);

    // Local files (rare).
    auto* local = new Disclosure(FW_TR("Patch local files"), FW_TR("For copies of param.json, appinfo.db and app.db on this PC"), tab);
    m_patchFolder = new PathField(PathField::Mode::Directory, local);
    local->body()->addWidget(field(FW_TR("Folder with the files"), m_patchFolder,
                                   FW_TR("A .bak copy is written next to each file first; every patch is read back."), local));
    auto* prow = new QWidget(local);
    auto* ph = hbox(prow, space::sm);
    ph->addStretch(1);
    m_patchLocal = button(FW_TR("Patch local files"), ButtonVariant::Secondary, QStringLiteral("edit"), prow);
    ph->addWidget(m_patchLocal);
    local->body()->addWidget(prow);
    connect(m_patchLocal, &QPushButton::clicked, this, [this] {
        const QString folder = m_patchFolder->path();
        if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
            m_patchFolder->setError(FW_TR("Choose the folder that holds the files."));
            return;
        }
        m_patchFolder->setError({});
        const auto reg = *ps5::y2jbRegionFromName(region());
        m_patchLocal->setEnabled(false);
        QPointer<Ps5ConnectPage> self(this);
        (void)QtConcurrent::run([self, folder, reg] {
            auto r = ps5::patchLocalFolder(folder, reg);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, r] {
                if (!self)
                    return;
                self->m_patchLocal->setEnabled(true);
                if (!r.ok()) {
                    Sheet::message(self->window(), FW_TR("Nothing was patched"), r.error().message, Sheet::Icon::Error);
                    return;
                }
                QStringList lines;
                for (const auto& f : r->files)
                    lines << QStringLiteral("%1 %2 — %3").arg(f.status, f.name, f.detail);
                Sheet::message(self->window(), r->allVerified() ? FW_TR("Local files patched") : FW_TR("Patched with problems"),
                               Sheet::monoLines(lines), r->allVerified() ? Sheet::Icon::Info : Sheet::Icon::Warning);
            }, Qt::QueuedConnection);
        });
    });
    l->addWidget(local);
    l->addStretch(1);
    return tab;
}

QString Ps5ConnectPage::region() const { return m_region->currentValue().toString(); }

// ---- misc ---------------------------------------------------------------------------------

bool Ps5ConnectPage::acceptsDrop(const QList<QUrl>& urls) const
{
    if (urls.isEmpty())
        return false;
    if (currentTool() == u"install")
        return urls.first().toLocalFile().endsWith(QLatin1String(".pkg"), Qt::CaseInsensitive);
    if (!jobs::isAvailable(m_ctx, ps5::jobkinds::kUpload))
        return false;
    // On the browser also while not connected: the drop is answered here ("Connect first"), not by another page.
    return m_session->isConnected() || currentTool() == u"ftp";
}

void Ps5ConnectPage::dropUrls(const QList<QUrl>& urls)
{
    QStringList paths;
    for (const auto& u : urls)
        if (!u.toLocalFile().isEmpty())
            paths << u.toLocalFile();
    if (currentTool() == u"install" && !paths.isEmpty() && paths.first().endsWith(QLatin1String(".pkg"), Qt::CaseInsensitive)) {
        m_installTab->setPackage(paths.first());
        return;
    }
    if (paths.isEmpty())
        return;
    if (!m_session->isConnected()) {
        emit toastRequested(Tone::Warn, FW_TR("Connect first"), FW_TR("Connect to the console, then drop the files on the list to upload them."));
        return;
    }
    // A drop anywhere on the page (not on the file list, which shows its target): say where it goes and ask.
    const QString target = m_remoteDir;
    const QString what = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : FW_TR("%1 items").arg(paths.size());
    if (!Sheet::confirm(window(), FW_TR("Upload to the console?"),
                        FW_TR("%1 will be uploaded to %2 on %3.").arg(Sheet::monoSpan(what), Sheet::monoSpan(target), Sheet::monoSpan(browserHost())),
                        FW_TR("Cancel"), FW_TR("Upload"), false, Sheet::Icon::Info))
        return;
    showTool(QStringLiteral("ftp"));
    uploadFiles(paths, target);
}

QString Ps5ConnectPage::unsavedChanges() const
{
    QStringList what;
    if (m_autoloader->hasUnsavedEdits())
        what << FW_TR("autoload.txt has edits that are not saved");
    if (m_smp->hasLoaded() && !m_smp->changes().isEmpty())
        what << FW_TR("the ShadowMount+ settings have changes that are not pushed to the console");
    return what.join(FW_TR(", and "));
}

QString Ps5ConnectPage::browserHost() const
{
    return m_session->isConnected() ? m_session->host() : host();
}

int Ps5ConnectPage::browserPort() const
{
    return m_session->isConnected() && m_session->port() > 0 ? int(m_session->port()) : ftpPort();
}

void Ps5ConnectPage::setDragHighlight(bool on)
{
    if (m_filesCard)
        m_filesCard->setDropHighlight(on && currentTool() == u"ftp" && m_session->isConnected());
}

void Ps5ConnectPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("tab"))) {
        // A stable key ("payloads", "klog", "fakelib"...) or, for older callers, a group index.
        const QVariant tab = args.value(QStringLiteral("tab"));
        bool isIndex = false;
        const int index = tab.toInt(&isIndex);
        if (isIndex && tab.typeId() != QMetaType::QString)
            m_groups->setCurrentIndex(std::clamp(index, 0, m_groups->count() - 1));
        else
            showTool(tab.toString());
    }
    if (args.contains(QStringLiteral("path")) && m_session->isConnected()) {
        showTool(QStringLiteral("ftp"));
        listRemote(args.value(QStringLiteral("path")).toString());
    }
    if (args.contains(QStringLiteral("upload"))) {
        showTool(QStringLiteral("ftp"));
        if (m_session->isConnected())
            uploadFiles({args.value(QStringLiteral("upload")).toString()});
        else
            emit toastRequested(Tone::Warn, FW_TR("Connect first"), FW_TR("Connect to the console, then drop or upload the file."));
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
        return FW_TR("No console address");
    QStringList parts;
    parts << (m_session->isConnected() ? FW_TR("Connected to %1 · FTP %2").arg(browserHost()).arg(browserPort()) : QStringLiteral("%1 · FTP %2").arg(host()).arg(ftpPort()));
    if (m_klogClient)
        parts << FW_TR("Kernel log: %1 lines").arg(formatCount(m_klog->rowCount()));
    return parts.join(QStringLiteral(" · "));
}

QStringList Ps5ConnectPage::demoStates() const
{
    return {QStringLiteral("ftp"),       QStringLiteral("manager"),     QStringLiteral("payloads"), QStringLiteral("klog"),
            QStringLiteral("smplog"),    QStringLiteral("crashdumps"),  QStringLiteral("mount"),    QStringLiteral("fakelib"),
            QStringLiteral("autoloader"), QStringLiteral("ampr"),       QStringLiteral("y2jb"),     QStringLiteral("install"),
            QStringLiteral("diagnostics"), QStringLiteral("find")};
}

void Ps5ConnectPage::applyDemoState(const QString& name)
{
    // The console card as it looks when connected (example values).
    auto exampleConsole = [this] {
        m_hero->setState(ConsoleHero::State::Connected);
        m_hero->setSubtitle(QStringLiteral("192.168.1.40 · FTP 2121 · ftpsrv v0.21.1 (example)"));
        m_hero->setChip(QStringLiteral("ftp"), Tone::Ok, QStringLiteral("ftpsrv v0.21.1"));
        m_hero->setChip(QStringLiteral("klog"), Tone::Ok, QStringLiteral("3232"));
        m_hero->setChip(QStringLiteral("payload"), Tone::Ok, QStringLiteral("9021 · sent"));
        m_hero->setChip(QStringLiteral("shadowmount"), Tone::Ok, QStringLiteral("1.7beta11"));
        m_hero->setChip(QStringLiteral("installer"), Tone::Neutral, QStringLiteral("12800"));
    };
    if (name == QLatin1String("find")) {
        m_demoSheet = m_find->openSpecimen();
        return;
    }
    if (name == QLatin1String("ftp")) {
        exampleConsole();
        m_specimenListing = true;
        RemoteListing listing;
        listing.path = QStringLiteral("/data/etaHEN/games");
        const QDateTime t = QDateTime::currentDateTime().addDays(-2);
        auto entry = [&](const QString& n, bool dir, qint64 size, int hoursAgo) {
            ps5::FtpEntry e;
            e.name = n;
            e.isDir = dir;
            e.size = size;
            e.modified = t.addSecs(-3600LL * hoursAgo);
            listing.entries.push_back(e);
        };
        entry(QStringLiteral("PPSA05678-app0"), true, -1, 3);
        entry(QStringLiteral("backports"), true, -1, 30);
        entry(QStringLiteral("PPSA01325 Astro Bot v01.003.exfat"), false, 41LL * 1024 * 1024 * 1024, 5);
        entry(QStringLiteral("PPSA01341 Demon's Souls v01.004.ffpkg"), false, 33LL * 1024 * 1024 * 1024, 26);
        entry(QStringLiteral("PPSA02929 Ratchet v01.001.exfat"), false, 37LL * 1024 * 1024 * 1024, 52);
        entry(QStringLiteral("PPSA01476 Returnal v01.006.ffpfsc"), false, 52LL * 1024 * 1024 * 1024, 80);
        entry(QStringLiteral("readme.txt"), false, 2048, 200);
        m_storage = {{QStringLiteral("/data"), QStringLiteral("/dev/ssd0.user"), QStringLiteral("ufs"), {}, 825LL << 30, 612LL << 30, false, false}};
        m_remoteArea->show();
        m_remoteEmpty->hide();
        m_history = {QStringLiteral("/data"), listing.path};
        m_historyIndex = 1;
        showListing(listing);
        m_remote->selectRow(2);
        m_btnUpload->setEnabled(true);
        m_btnUploadFolder->setEnabled(true);
        m_btnNewFolder->setEnabled(true);
        m_btnDownload->setEnabled(true);
        m_btnRename->setEnabled(true);
        m_btnDelete->setEnabled(true);
        m_btnUp->setEnabled(true);
        m_btnBack->setEnabled(true);
        m_btnRefresh->setEnabled(true);
        // Example transfers.
        std::vector<JobRecord> rows(3);
        rows[0].id = QStringLiteral("demo-1");
        rows[0].kind = ps5::jobkinds::kUpload;
        rows[0].title = QStringLiteral("Upload PPSA01342 Astro Bot v01.004.exfat to PS5");
        rows[0].state = JobState::Running;
        rows[0].progress.currentItem = QStringLiteral("PPSA01342 Astro Bot v01.004.exfat");
        rows[0].progress.total = 41LL * 1024 * 1024 * 1024;
        rows[0].progress.done = rows[0].progress.total * 62 / 100;
        rows[0].progress.overallFraction = 0.62;
        rows[0].progress.ratePerSec = 38.4 * 1024 * 1024;
        rows[0].progress.etaMs = 7 * 60 * 1000;
        rows[1].id = QStringLiteral("demo-2");
        rows[1].kind = ps5::jobkinds::kUpload;
        rows[1].title = QStringLiteral("Upload PPSA05678-app0 to PS5");
        rows[1].state = JobState::Failed;
        rows[1].progress.overallFraction = 0.18;
        rows[1].error = Error(ErrorCode::InsufficientSpace, QStringLiteral("Uploading /mnt/usb0/games/PPSA05678-app0/sce_sys/icon0.png: the console is out of space."));
        rows[2].id = QStringLiteral("demo-3");
        rows[2].kind = ps5::jobkinds::kDownload;
        rows[2].title = QStringLiteral("Download config.ini from PS5");
        rows[2].state = JobState::Succeeded;
        rows[2].progress.overallFraction = 1;
        JobOutcome o;
        o.summary = QStringLiteral("1 file(s) downloaded to D:\\PS5\\backup · size-checked");
        rows[2].outcome = o;
        m_transfers->setSpecimen(rows);
        showTool(QStringLiteral("ftp"));
        return;
    }
    showTool(name);
    if (name == QLatin1String("diagnostics"))
        m_diagnostics->showSpecimen();
    if (name == QLatin1String("payloads") && m_payloads.entries().empty()) {
        // Example rows (the user's own list is not touched).
        const QSignalBlocker block(m_payloadList);
        m_payloadList->clear();
        const std::tuple<const char*, const char*, const char*> rows[] = {
            {"elfldr", "elfldr.elf · 92.4 KB", "Sent 3 min ago"},
            {"kstuff-lite 1.11", "kstuff.elf · 214.0 KB", "Sent 3 min ago"},
            {"ShadowMount+ 1.7beta11", "shadowmountplus.elf · 1.1 MB", "Sent yesterday"},
            {"ftpsrv v0.21.1", "ftpsrv-ps5.elf · 168.2 KB", ""},
        };
        for (const auto& [n, f, s] : rows) {
            auto* it = new QListWidgetItem(QString::fromUtf8(n), m_payloadList);
            it->setData(PayloadDelegate::FileRole, QString::fromUtf8(f));
            it->setData(PayloadDelegate::SentRole, QString::fromUtf8(s));
        }
        m_payloadList->setCurrentRow(0);
        m_payloadArea->show();
        m_payloadEmpty->hide();
        m_payloadDetail->setEnabled(true);
        m_pName->setText(QStringLiteral("elfldr"));
        m_pFile->setText(QStringLiteral("D:\\PS5\\payloads\\elfldr.elf · 92.4 KB"));
        m_pNotes->setText(QStringLiteral("ps5-payload-dev v0.27: the LAN ELF loader (example)"));
        m_pNotes->show();
        m_pLastSent->setText(QStringLiteral("Last sent 3 min ago"));
        m_pBadge->set(FW_TR("Ready"), Tone::Ok);
        m_pSend->setEnabled(true);
        exampleConsole();
    }
    for (auto* tool : findChildren<ConsoleToolTab*>())
        if (tool->demoKey() == name)
            tool->applyDemo();
    if (name == QLatin1String("mount"))
        exampleConsole();
    if (name == QLatin1String("klog") && m_klog->rowCount() == 0) {
        // Specimen lines so the viewer's tags are visible in the evidence.
        for (const QString& t : {QStringLiteral("[KERNEL] sceKernelLoadStartModule: libSceNpTrophy2.sprx"), QStringLiteral("[SHUTDOWN] requested by STOP file /data/shadowmount/STOP"),
                                 QStringLiteral("[SHUTDOWN] cleanup complete for STOP file /data/shadowmount/STOP"),
                                 QStringLiteral("warning: free pages low"), QStringLiteral("panic: specimen line, not from a console")}) {
            ps5::KlogLine l;
            l.received = QDateTime::currentDateTime();
            l.text = t;
            l.tag = ps5::klogTag(t);
            m_klog->append(l);
        }
        exampleConsole();
    }
}

void Ps5ConnectPage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
    m_diagnostics->clearResult();
    m_klog->clear();
    klogFilterChanged();
    for (auto* tool : findChildren<ConsoleToolTab*>())
        tool->clearDemo();
    m_transfers->clearSpecimen();
    rebuildPayloads();
    if (m_specimenListing) {
        m_specimenListing = false;
        m_storage.clear();
        m_listing = {};
        m_remote->setRowCount(0);
        m_history.clear();
        m_historyIndex = -1;
        m_remoteFree->clear();
        m_remoteDir = m_ctx.settings().value(ps5::keys::kGamesFolder).toString().isEmpty() ? QStringLiteral("/") : m_ctx.settings().value(ps5::keys::kGamesFolder).toString();
        rebuildBreadcrumb();
    }
    applyConnectionState(m_session->state(), {});
    for (const char* k : {"klog", "shadowmount", "installer"})
        m_hero->setChip(QLatin1String(k), Tone::Neutral, QStringLiteral("—"));
    updatePayloadChip();
    showTool(QStringLiteral("ftp"));
}

} // namespace fw::pages
