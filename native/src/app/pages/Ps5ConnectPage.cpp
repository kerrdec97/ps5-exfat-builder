#include "app/pages/Ps5ConnectPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QIntValidator>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTimer>

namespace fw::pages {

using namespace ui;
using namespace theme;

Ps5ConnectPage::Ps5ConnectPage(AppContext& ctx, JobQueueModel& queueModel, PayloadStore& payloads, QWidget* parent)
    : Page(ctx, QStringLiteral("ps5"), QStringLiteral("PS5 connect"),
           QStringLiteral("Browse the console over FTP, send payloads and follow the kernel log."), parent),
      m_payloads(payloads)
{
    Q_UNUSED(queueModel);
    m_host = textInput(QStringLiteral("192.168.1.40"), true, this);
    m_host->setFixedWidth(160);
    m_host->setAccessibleName(QStringLiteral("Console IP"));
    m_ftpPort = textInput(QStringLiteral("2121"), true, this);
    m_ftpPort->setFixedWidth(72);
    m_ftpPort->setValidator(new QIntValidator(1, 65535, m_ftpPort));
    m_ftpPort->setAccessibleName(QStringLiteral("FTP port"));
    m_ftpPort->setToolTip(QStringLiteral("FTP port (ftpsrv 2121, etaHEN 1337)"));
    m_payloadPort = textInput(QStringLiteral("9021"), true, this);
    m_payloadPort->setFixedWidth(72);
    m_payloadPort->setValidator(new QIntValidator(1, 65535, m_payloadPort));
    m_payloadPort->setAccessibleName(QStringLiteral("Payload port"));
    m_payloadPort->setToolTip(QStringLiteral("ELF loader port (9021, or 9090 for older loaders)"));
    m_test = button(QStringLiteral("Test connection"), ButtonVariant::Secondary, QStringLiteral("plug"), this);
    header()->addAction(m_host);
    header()->addAction(m_ftpPort);
    header()->addAction(m_payloadPort);
    header()->addAction(m_test);
    connect(m_test, &QPushButton::clicked, this, &Ps5ConnectPage::probe);
    bindSettingKeys();

    auto* statusRow = new QWidget(this);
    auto* sh = hbox(statusRow, space::sm);
    m_badge = new Badge(QStringLiteral("Not tested"), Tone::Neutral, statusRow);
    m_badge->setIconName(QStringLiteral("plug"));
    sh->addWidget(m_badge);
    sh->addWidget(caption(QStringLiteral("The test opens a TCP connection to the FTP port. Nothing is sent."), statusRow));
    sh->addStretch(1);
    content()->addWidget(statusRow);

    m_tabs = new QTabBar(this);
    m_tabs->addTab(QStringLiteral("FTP browser"));
    m_tabs->addTab(QStringLiteral("Payloads"));
    m_tabs->addTab(QStringLiteral("Kernel log"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setFocusPolicy(Qt::TabFocus);
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

    // ---- FTP tab
    auto* ftp = new QWidget(m_stack);
    auto* fl = vbox(ftp, space::lg);
    auto* ftpCard = new Card(ftp, Card::Variant::Table);
    auto* fh = ftpCard->header();
    fh->addWidget(heading(QStringLiteral("Console"), TextStyle::Subtitle, ftpCard));
    fh->addStretch(1);
    m_ftpUnavailable = new UnavailableState(QStringLiteral("The FTP browser"), QStringLiteral("ps5.ftp.list"), ftpCard);
    ftpCard->body()->addWidget(m_ftpUnavailable);
    fl->addWidget(ftpCard);
    auto* upload = new Card(ftp);
    upload->setTitle(QStringLiteral("Send a file to the console"));
    m_uploadLocal = new PathField(PathField::Mode::OpenFile, upload);
    m_uploadLocal->setFilter(QStringLiteral("Images and packages (*.exfat *.ffpkg *.ffpfsc *.pkg *.zip);;All files (*)"));
    m_uploadLocal->setDialogTitle(QStringLiteral("Choose the file to send"));
    upload->body()->addWidget(field(QStringLiteral("Local file"), m_uploadLocal, QStringLiteral("Drop a file anywhere on this page."), upload));
    m_uploadRemote = textInput(QStringLiteral("/data/etaHEN/games/"), true, upload);
    m_uploadRemote->setText(QStringLiteral("/data/etaHEN/games/"));
    upload->body()->addWidget(field(QStringLiteral("Remote folder"), m_uploadRemote, QStringLiteral("Created on the console if it does not exist."), upload));
    auto* ub = new QWidget(upload);
    auto* ubl = hbox(ub, space::sm);
    ubl->addStretch(1);
    m_uploadButton = button(QStringLiteral("Send to PS5"), ButtonVariant::Primary, QStringLiteral("upload"), ub);
    ubl->addWidget(m_uploadButton);
    upload->body()->addWidget(ub);
    connect(m_uploadButton, &QPushButton::clicked, this, &Ps5ConnectPage::uploadFile);
    fl->addWidget(upload);
    fl->addStretch(1);
    m_stack->addWidget(ftp);

    // ---- Payloads tab
    auto* pay = new QWidget(m_stack);
    auto* pl = new TwoColumnLayout(pay);
    pl->setLeftMax(100000);
    auto* listCard = new Card(pay, Card::Variant::Table);
    auto* lh = listCard->header();
    lh->addWidget(heading(QStringLiteral("Payloads"), TextStyle::Subtitle, listCard));
    lh->addStretch(1);
    auto* add = button(QStringLiteral("Add payload…"), ButtonVariant::Secondary, QStringLiteral("plus"), listCard);
    lh->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { addOrEditPayload(-1); });
    m_payloadList = new QListWidget(listCard);
    m_payloadList->setFocusPolicy(Qt::StrongFocus);
    m_payloadList->setMinimumHeight(6 * size::row);
    m_payloadList->setSpacing(0);
    listCard->body()->addWidget(m_payloadList);
    m_payloadEmpty = new EmptyState(QStringLiteral("rocket"), QStringLiteral("No payloads yet"),
                                    QStringLiteral("Add an .elf or .bin payload. It is sent to the console's ELF loader port."), listCard);
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
    m_stack->addWidget(pay);
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
    connect(&m_payloads, &PayloadStore::changed, this, &Ps5ConnectPage::rebuildPayloads);

    // ---- Kernel log tab
    auto* klog = new Card(m_stack);
    klog->body()->addWidget(new UnavailableState(QStringLiteral("The kernel log viewer"), QStringLiteral("ps5.klog.stream"), klog));
    m_stack->addWidget(klog);

    rebuildPayloads();
    m_uploadButton->setEnabled(jobs::isAvailable(m_ctx, QStringLiteral("ps5.ftp.upload")));
    m_uploadButton->setToolTip(m_uploadButton->isEnabled() ? QString() : QStringLiteral("Not available in this build"));
    m_ftpUnavailable->setVisible(true);
}

void Ps5ConnectPage::bindSettingKeys()
{
    // Bind to the PS5 connection settings when the connect module declares them.
    for (const SettingDef* d : m_ctx.schema().inSection(QStringLiteral("PS5 connection"))) {
        const QString k = d->key.toLower();
        if (m_hostKey.isEmpty() && (k.endsWith(QLatin1String(".host")) || k.endsWith(QLatin1String(".ip")) || k.endsWith(QLatin1String("address"))))
            m_hostKey = d->key;
        else if (m_ftpPortKey.isEmpty() && k.contains(QLatin1String("ftp")) && k.contains(QLatin1String("port")))
            m_ftpPortKey = d->key;
        else if (m_payloadPortKey.isEmpty() && k.contains(QLatin1String("payload")) && k.contains(QLatin1String("port")))
            m_payloadPortKey = d->key;
    }
    auto bind = [this](QLineEdit* e, const QString& key) {
        if (key.isEmpty())
            return;
        e->setText(m_ctx.settings().value(key).toString());
        connect(e, &QLineEdit::editingFinished, this, [this, e, key] { (void)m_ctx.settings().setValue(key, e->text().trimmed()); });
    };
    bind(m_host, m_hostKey);
    bind(m_ftpPort, m_ftpPortKey);
    bind(m_payloadPort, m_payloadPortKey);
}

QString Ps5ConnectPage::host() const { return m_host->text().trimmed(); }
int Ps5ConnectPage::ftpPort() const { return m_ftpPort->text().trimmed().isEmpty() ? 2121 : m_ftpPort->text().toInt(); }
int Ps5ConnectPage::payloadPort() const { return m_payloadPort->text().trimmed().isEmpty() ? 9021 : m_payloadPort->text().toInt(); }

void Ps5ConnectPage::probe()
{
    const QString h = host();
    if (h.isEmpty()) {
        setInputError(m_host, true);
        setReachable(false, QStringLiteral("Enter the console's IP address first."));
        return;
    }
    setInputError(m_host, false);
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
    }
    m_socket = new QTcpSocket(this);
    m_badge->set(QStringLiteral("Testing…"), Tone::Info);
    m_badge->setIconName(QStringLiteral("loader"));
    m_test->setEnabled(false);
    connect(m_socket, &QTcpSocket::connected, this, [this, h] {
        m_socket->abort();
        setReachable(true, QStringLiteral("FTP port %1 answers on %2").arg(ftpPort()).arg(h));
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this, [this, h](QAbstractSocket::SocketError) {
        setReachable(false, QStringLiteral("%1 did not answer on port %2. Check that the console is on, jailbroken and on the same network.").arg(h).arg(ftpPort()));
    });
    QTimer::singleShot(3000, m_socket, [this] {
        if (m_socket && m_socket->state() != QAbstractSocket::ConnectedState && m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->abort();
    });
    m_socket->connectToHost(h, quint16(ftpPort()));
}

void Ps5ConnectPage::setReachable(bool on, const QString& detail)
{
    m_reachable = on;
    m_probeDetail = detail;
    m_test->setEnabled(true);
    m_badge->set(on ? QStringLiteral("Reachable · %1").arg(host()) : QStringLiteral("Not reachable"), on ? Tone::Ok : Tone::Err);
    m_badge->setToolTip(detail);
    emit reachabilityChanged(on, host());
    emit statusContextChanged();
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
    const auto e = m_payloads.entries()[size_t(index)];
    auto r = jobs::submitPayload(m_ctx, host(), payloadPort(), e.path);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Payload not sent"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    m_payloads.markSent(e.path);
    emit toastRequested(Tone::Info, QStringLiteral("Sending payload"), QStringLiteral("%1 → %2:%3").arg(e.name, host()).arg(payloadPort()));
}

void Ps5ConnectPage::uploadFile()
{
    if (m_uploadLocal->path().isEmpty()) {
        m_uploadLocal->setError(QStringLiteral("Choose a file to send."));
        return;
    }
    m_uploadLocal->setError({});
    if (host().isEmpty()) {
        setInputError(m_host, true);
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address in the header first."));
        return;
    }
    auto r = jobs::submitFtpUpload(m_ctx, host(), ftpPort(), m_uploadLocal->path(), m_uploadRemote->text().trimmed());
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Upload not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, QStringLiteral("Upload queued"), QFileInfo(m_uploadLocal->path()).fileName());
}

bool Ps5ConnectPage::acceptsDrop(const QList<QUrl>& urls) const
{
    return !urls.isEmpty() && QFileInfo(urls.first().toLocalFile()).isFile();
}

void Ps5ConnectPage::dropUrls(const QList<QUrl>& urls)
{
    if (!acceptsDrop(urls))
        return;
    m_tabs->setCurrentIndex(0);
    m_uploadLocal->setPath(urls.first().toLocalFile());
}

void Ps5ConnectPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("tab")))
        m_tabs->setCurrentIndex(args.value(QStringLiteral("tab")).toInt());
    if (args.contains(QStringLiteral("upload")))
        m_uploadLocal->setPath(args.value(QStringLiteral("upload")).toString());
}

QString Ps5ConnectPage::statusContext() const
{
    if (host().isEmpty())
        return QStringLiteral("No console address");
    return m_reachable ? QStringLiteral("Reachable: %1 · FTP %2 · payload %3").arg(host()).arg(ftpPort()).arg(payloadPort())
                       : QStringLiteral("%1 · FTP %2 · payload %3").arg(host()).arg(ftpPort()).arg(payloadPort());
}

} // namespace fw::pages
