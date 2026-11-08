#include "app/pages/ps5/DiagnosticsTab.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"

#include <QGridLayout>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

const QString kKind = QStringLiteral("ps5.ftp.diagnose");

QString mbps(double v)
{
    return v < 0 ? QStringLiteral("—") : QStringLiteral("%1 MB/s").arg(v, 0, 'f', v < 10 ? 1 : 0);
}

// The legacy _ftpdiag_evidence_tags: short tags for the classifier's reasons.
QString evidenceTags(const QStringList& evidence)
{
    QStringList tags;
    for (const QString& e : evidence) {
        const QString low = e.toLower();
        QString tag;
        if (low.startsWith(QLatin1String("no ")))
            tag = QStringLiteral("no zftpd markers");
        else if (low.contains(QLatin1String("xcrypt")))
            tag = QStringLiteral("XCRYPT");
        else if (low.contains(QLatin1String("copy")))
            tag = QStringLiteral("server-side copy");
        else if (low.contains(QLatin1String("mlsd")))
            tag = QStringLiteral("feature set");
        else if (low.contains(QLatin1String("banner")))
            tag = QStringLiteral("banner");
        else
            tag = e.section(u'(', 0, 0).trimmed().left(18);
        if (!tag.isEmpty() && !tags.contains(tag))
            tags << tag;
    }
    return tags.join(QStringLiteral(" · "));
}

} // namespace

DiagnosticsTab::DiagnosticsTab(AppContext& ctx, Source source, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_src(std::move(source))
{
    auto* l = vbox(this, space::lg);

    // ---- run
    auto* card = new Card(this);
    m_verdict = new Badge(QStringLiteral("Not run yet"), Tone::Neutral, card);
    m_verdict->setIconName({});
    card->setTitle(QStringLiteral("Connection diagnostics"), m_verdict);
    auto* intro = caption(QStringLiteral("Checks the console's FTP server step by step; the speed test also moves 16 MiB each way."), card);
    intro->setToolTip(QStringLiteral("The connection and its round trip, the greeting and login, the advertised features, passive and "
                                     "binary mode, and whether the folder below can be written (a 28-byte probe file is written and "
                                     "removed). The speed test also sends 16 MiB, reads it back and deletes it."));
    intro->setWordWrap(true);
    card->body()->addWidget(intro);
    m_folder = textInput(QStringLiteral("/data/etaHEN/games"), true, card);
    m_folder->setAccessibleName(QStringLiteral("Folder on the PS5"));
    card->body()->addWidget(field(QStringLiteral("Folder on the PS5"), m_folder,
                                  QStringLiteral("Follows the FTP browser's open folder until you type here."), card));
    auto* row = new QWidget(card);
    auto* rh = hbox(row, space::sm);
    m_status = caption(QStringLiteral("Not run yet."), row);
    m_status->setWordWrap(true);
    rh->addWidget(m_status, 1);
    m_cancel = button(QStringLiteral("Cancel"), ButtonVariant::Secondary, QStringLiteral("stop"), row);
    m_speed = button(QStringLiteral("Run speed test"), ButtonVariant::Secondary, QStringLiteral("gauge"), row);
    m_run = button(QStringLiteral("Run diagnostics"), ButtonVariant::Primary, QStringLiteral("scan"), row);
    rh->addWidget(m_cancel);
    rh->addWidget(m_speed);
    rh->addWidget(m_run);
    card->body()->addWidget(row);
    l->addWidget(card);

    // ---- result cells
    m_results = new Card(this);
    m_results->setTitle(QStringLiteral("Results"));
    m_problem = new InfoBar(Tone::Err, m_results);
    m_problem->hide();
    m_results->body()->addWidget(m_problem);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(space::xxl);
    grid->setVerticalSpacing(space::lg);
    struct Spec {
        const char* key;
        const char* title;
        bool badge;
    };
    const Spec specs[] = {{"reachable", "Reachable", true},      {"rtt", "Round trip", false},
                          {"login", "Login", true},              {"destination", "Destination", false},
                          {"passive", "Passive mode", true},     {"binary", "Binary mode (TYPE I)", true},
                          {"writable", "Folder writable", true}, {"server", "Server", false}};
    int i = 0;
    for (const Spec& s : specs) {
        const bool text = qstrcmp(s.key, "destination") == 0 || qstrcmp(s.key, "server") == 0; // words, not numbers
        grid->addWidget(makeCell(QString::fromLatin1(s.key), QString::fromLatin1(s.title), s.badge, m_results,
                                 text ? TextStyle::Body : TextStyle::Mono),
                        i / 4, i % 4);
        ++i;
    }
    for (int c = 0; c < 4; ++c)
        grid->setColumnStretch(c, 1);
    m_results->body()->addLayout(grid);
    m_results->body()->addWidget(hairline(m_results));
    auto* bannerBox = new QWidget(m_results);
    auto* bl = vbox(bannerBox, 2);
    bl->addWidget(caption(QStringLiteral("Greeting"), bannerBox));
    m_banner = label(QStringLiteral("—"), TextStyle::Mono, color::fg_1, bannerBox);
    m_banner->setWordWrap(true);
    m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bl->addWidget(m_banner);
    m_results->body()->addWidget(bannerBox);
    auto* featBox = new QWidget(m_results);
    auto* fl = vbox(featBox, space::xs);
    fl->addWidget(caption(QStringLiteral("Features (FEAT)"), featBox));
    m_featRow = new QWidget(featBox);
    m_feat = new FlowLayout(m_featRow, space::xs, space::xs);
    fl->addWidget(m_featRow);
    m_results->body()->addWidget(featBox);
    l->addWidget(m_results);

    // ---- speed
    m_speedCard = new Card(this);
    m_speedCard->setTitle(QStringLiteral("Speed test"));
    auto* sg = new QGridLayout;
    sg->setHorizontalSpacing(space::xxl);
    sg->addWidget(makeCell(QStringLiteral("upload"), QStringLiteral("Upload"), false, m_speedCard, TextStyle::Stat), 0, 0);
    sg->addWidget(makeCell(QStringLiteral("download"), QStringLiteral("Download"), false, m_speedCard, TextStyle::Stat), 0, 1);
    sg->addWidget(makeCell(QStringLiteral("readback"), QStringLiteral("Read back"), true, m_speedCard), 0, 2);
    for (int c = 0; c < 4; ++c)
        sg->setColumnStretch(c, 1);
    m_speedCard->body()->addLayout(sg);
    m_speedCaption = caption({}, m_speedCard);
    m_speedCaption->setWordWrap(true);
    m_speedCard->body()->addWidget(m_speedCaption);
    m_leftover = new InfoBar(Tone::Warn, m_speedCard);
    m_leftover->hide();
    m_speedCard->body()->addWidget(m_leftover);
    l->addWidget(m_speedCard);
    l->addStretch(1);

    connect(m_run, &QPushButton::clicked, this, [this] { run(false); });
    connect(m_speed, &QPushButton::clicked, this, [this] { run(true); });
    connect(m_cancel, &QPushButton::clicked, this, &DiagnosticsTab::cancel);
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, &DiagnosticsTab::onJobChanged);
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, &DiagnosticsTab::onJobFinished);
    clearResult();
    setRunning(false);
    prefillFolder();
}

QWidget* DiagnosticsTab::makeCell(const QString& key, const QString& title, bool badge, QWidget* parent, TextStyle valueStyle)
{
    Cell c;
    c.box = new QWidget(parent);
    auto* v = vbox(c.box, space::xs);
    v->addWidget(caption(title, c.box));
    if (badge) {
        auto* line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        c.badge = new Badge(QStringLiteral("—"), Tone::Neutral, c.box);
        line->addWidget(c.badge);
        line->addStretch(1);
        v->addLayout(line);
    } else {
        c.value = label(QStringLiteral("—"), valueStyle, color::fg_0, c.box);
        c.value->setWordWrap(true);
        v->addWidget(c.value);
    }
    c.detail = caption({}, c.box);
    c.detail->setWordWrap(true);
    c.detail->hide();
    v->addWidget(c.detail);
    v->addStretch(1);
    m_cells.insert(key, c);
    return c.box;
}

void DiagnosticsTab::setCell(const QString& key, const QString& text, Tone tone, const QString& detail)
{
    const auto it = m_cells.constFind(key);
    if (it == m_cells.cend())
        return;
    if (it->badge) {
        it->badge->set(text, tone);
        it->badge->setIconName(tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone));
    } else {
        it->value->setText(text);
    }
    it->detail->setText(detail);
    it->detail->setVisible(!detail.isEmpty());
}

void DiagnosticsTab::setStatus(const QString& text, Tone tone)
{
    m_status->setText(text);
    setTextColor(m_status, tone == Tone::Err ? color::danger_hi : tone == Tone::Warn ? color::warn_hi : tone == Tone::Ok ? color::success_hi : color::fg_3);
}

void DiagnosticsTab::setRunning(bool running)
{
    const bool available = jobs::isAvailable(m_ctx, kKind);
    m_run->setEnabled(!running && available);
    m_speed->setEnabled(!running && available);
    m_run->setToolTip(available ? QString() : QStringLiteral("Not available in this build"));
    m_cancel->setVisible(running);
    m_folder->setEnabled(!running);
    if (running) {
        m_verdict->set(QStringLiteral("Running"), Tone::Info);
        m_verdict->setIconName(QStringLiteral("loader"));
    }
}

void DiagnosticsTab::prefillFolder()
{
    if (m_folder->isModified() && !m_folder->text().trimmed().isEmpty())
        return;
    QString folder = m_src.folder ? m_src.folder() : QString();
    if (folder.trimmed().isEmpty())
        folder = m_ctx.settings().value(ps5::keys::kGamesFolder).toString();
    m_folder->setText(folder); // keeps isModified() false
}

void DiagnosticsTab::showEvent(QShowEvent* e)
{
    prefillFolder();
    QWidget::showEvent(e);
}

void DiagnosticsTab::run(bool speedTest)
{
    if (isRunning())
        return;
    const QString host = m_src.host ? m_src.host().trimmed() : QString();
    if (host.isEmpty()) {
        setStatus(QStringLiteral("Enter the console's IP address at the top of the page first, or use Find my PS5."), Tone::Err);
        return;
    }
    const int port = m_src.port ? m_src.port() : 0;
    auto r = jobs::submitFtpDiagnose(m_ctx, host, port, m_folder->text(), speedTest);
    if (!r.ok()) {
        setStatus(r.error().message, Tone::Err);
        return;
    }
    m_jobId = r.value();
    m_ctx.queue().holdQueue(false);
    setRunning(true);
    setStatus(QStringLiteral("%1 queued for %2:%3…").arg(speedTest ? QStringLiteral("Speed test") : QStringLiteral("Diagnostics"), host).arg(port));
}

void DiagnosticsTab::cancel()
{
    if (!m_jobId.isEmpty())
        m_ctx.queue().cancel(m_jobId);
}

void DiagnosticsTab::onJobChanged(const QString& id)
{
    if (id != m_jobId)
        return;
    const auto rec = m_ctx.queue().record(id);
    if (!rec)
        return;
    if (rec->state == JobState::Queued) {
        setStatus(QStringLiteral("Waiting for the jobs ahead of it in the queue…"));
    } else if (rec->state == JobState::Running) {
        QString text = rec->progress.currentItem.isEmpty() ? rec->progress.phase : rec->progress.currentItem;
        if (rec->progress.total > 0 && !rec->progress.indeterminate)
            text += QStringLiteral(" · %1").arg(thinPercent(rec->progress.phaseFraction()));
        setStatus(text.isEmpty() ? QStringLiteral("Running…") : text + QStringLiteral("…"));
    }
}

void DiagnosticsTab::onJobFinished(const QString& id)
{
    const auto rec = m_ctx.queue().record(id);
    if (!rec || rec->kind != kKind)
        return;
    if (id == m_jobId) {
        m_jobId.clear();
        setRunning(false);
    }
    const QString when = rec->finished.isValid() ? rec->finished.toLocalTime().toString(QStringLiteral("HH:mm")) : QString();
    if (rec->outcome && !rec->outcome->details.isEmpty()) {
        const ps5::FtpDiagnosis d = ps5::FtpDiagnosis::fromJson(rec->outcome->details);
        QString verdict;
        Tone tone = Tone::Ok;
        switch (rec->outcome->verification) {
        case Verification::Passed: verdict = QStringLiteral("All checks passed"); break;
        case Verification::Failed:
            tone = Tone::Err;
            verdict = !d.reachable ? QStringLiteral("Not reachable")
                    : !d.usable()  ? QStringLiteral("FTP server not usable")
                                   : QStringLiteral("Data read back differs");
            break;
        default:
            tone = Tone::Warn;
            verdict = QStringLiteral("Some checks failed");
            break;
        }
        const QString summary = rec->outcome->summary.contains(d.host)
                                    ? rec->outcome->summary
                                    : QStringLiteral("%1:%2 · %3").arg(d.host).arg(d.port).arg(rec->outcome->summary);
        showResult(d, verdict, tone, QStringLiteral("Finished %1 · %2").arg(when, summary));
        return;
    }
    if (rec->state == JobState::Cancelled) {
        m_verdict->set(QStringLiteral("Cancelled"), Tone::Neutral);
        m_verdict->setIconName({});
        setStatus(QStringLiteral("Cancelled at %1. Any file the checks wrote was removed; the log says otherwise.").arg(when));
        return;
    }
    m_verdict->set(QStringLiteral("Failed"), Tone::Err);
    m_verdict->setIconName(defaultBadgeIcon(Tone::Err));
    setStatus(rec->error ? rec->error->message : QStringLiteral("The diagnostics failed. See the log."), Tone::Err);
}

void DiagnosticsTab::clearResult()
{
    m_verdict->set(QStringLiteral("Not run yet"), Tone::Neutral);
    m_verdict->setIconName({});
    m_problem->hide();
    for (auto it = m_cells.cbegin(); it != m_cells.cend(); ++it)
        setCell(it.key(), QStringLiteral("—"));
    m_banner->setText(QStringLiteral("—"));
    while (QLayoutItem* item = m_feat->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_speedCaption->setText(QStringLiteral("Not run yet. Run speed test sends %1 to the folder above, reads it back and deletes it.")
                                .arg(formatBytes(ps5::kSpeedTestBytes)));
    m_leftover->hide();
    setStatus(QStringLiteral("Not run yet."));
}

void DiagnosticsTab::showResult(const ps5::FtpDiagnosis& d, const QString& verdict, Tone tone, const QString& status)
{
    m_verdict->set(verdict, tone);
    m_verdict->setIconName(defaultBadgeIcon(tone));
    setStatus(status, tone == Tone::Ok ? Tone::Neutral : tone);
    // The problem that stopped the checks, or the notes.
    if (!d.error.isEmpty() || !d.notes.isEmpty()) {
        m_problem->setTone(d.usable() ? Tone::Warn : Tone::Err);
        m_problem->setMessage(!d.error.isEmpty() ? d.error : d.notes.join(u' '));
        m_problem->show();
    } else {
        m_problem->hide();
    }
    const QString where = QStringLiteral("%1:%2").arg(d.host).arg(d.port);
    setCell(QStringLiteral("reachable"), d.reachable ? QStringLiteral("Yes") : QStringLiteral("No"), d.reachable ? Tone::Ok : Tone::Err, where);
    setCell(QStringLiteral("rtt"), d.rttMs >= 0 ? QStringLiteral("%1 ms").arg(d.rttMs) : QStringLiteral("—"));
    if (!d.reachable)
        setCell(QStringLiteral("login"), QStringLiteral("—"));
    else if (d.loggedIn)
        setCell(QStringLiteral("login"), QStringLiteral("Accepted"), Tone::Ok);
    else
        setCell(QStringLiteral("login"), d.greeted ? QStringLiteral("Refused") : QStringLiteral("No greeting"), Tone::Err);
    setCell(QStringLiteral("destination"), d.destination.isEmpty() ? QStringLiteral("—") : d.destination, Tone::Neutral, d.folder);
    if (!d.passive)
        setCell(QStringLiteral("passive"), QStringLiteral("—"));
    else
        setCell(QStringLiteral("passive"), *d.passive ? d.passiveMode : QStringLiteral("Refused"), *d.passive ? Tone::Ok : Tone::Err,
                *d.passive ? QString() : QStringLiteral("PASV and EPSV refused"));
    if (!d.binary)
        setCell(QStringLiteral("binary"), QStringLiteral("—"));
    else
        setCell(QStringLiteral("binary"), *d.binary ? QStringLiteral("Accepted") : QStringLiteral("Refused"), *d.binary ? Tone::Ok : Tone::Err);
    if (!d.writable)
        setCell(QStringLiteral("writable"), QStringLiteral("—"));
    else if (*d.writable && d.probeCleaned)
        setCell(QStringLiteral("writable"), QStringLiteral("Yes"), Tone::Ok, QStringLiteral("Probe written and removed"));
    else if (*d.writable)
        setCell(QStringLiteral("writable"), QStringLiteral("Yes · probe left"), Tone::Warn, d.writableDetail);
    else
        setCell(QStringLiteral("writable"), QStringLiteral("No"), Tone::Err, d.writableDetail);
    if (d.greeted) {
        const QString label = d.server.label == QLatin1String("zftpd") ? QStringLiteral("zftpd (%1)").arg(d.server.confidence) : d.server.label;
        setCell(QStringLiteral("server"), label, Tone::Neutral, QStringLiteral("Evidence: %1").arg(evidenceTags(d.server.evidence)));
    } else {
        setCell(QStringLiteral("server"), QStringLiteral("—"));
    }
    m_banner->setText(!d.greeted ? QStringLiteral("—") : d.banner.isEmpty() ? QStringLiteral("(empty greeting)") : d.banner);

    while (QLayoutItem* item = m_feat->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (d.loggedIn && !d.featOk) {
        m_feat->addWidget(caption(QStringLiteral("The server does not answer FEAT."), m_featRow));
    } else if (d.loggedIn) {
        const std::pair<const char*, bool> feats[] = {{"MLSD", d.features.mlsd}, {"REST", d.features.rest},   {"SIZE", d.features.size},
                                                      {"MDTM", d.features.mdtm}, {"UTF8", d.features.utf8},   {"XCRYPT", d.features.xcrypt},
                                                      {"Server-side copy", d.features.copy}};
        for (const auto& [name, on] : feats) {
            auto* b = new Badge(QString::fromLatin1(name), on ? Tone::Ok : Tone::Neutral, m_featRow);
            b->setIconName(on ? QStringLiteral("check") : QStringLiteral("minus"));
            b->setToolTip(on ? QStringLiteral("Advertised") : QStringLiteral("Not advertised"));
            m_feat->addWidget(b);
        }
    }
    m_featRow->updateGeometry();

    // Speed test.
    const ps5::SpeedTestResult& s = d.speed;
    m_leftover->hide();
    if (s.ran) {
        setCell(QStringLiteral("upload"), mbps(s.uploadMBps));
        setCell(QStringLiteral("download"), mbps(s.downloadMBps));
        if (!s.readBackMatches)
            setCell(QStringLiteral("readback"), QStringLiteral("—"));
        else
            setCell(QStringLiteral("readback"), *s.readBackMatches ? QStringLiteral("Identical") : QStringLiteral("Differs"),
                    *s.readBackMatches ? Tone::Ok : Tone::Err);
        m_speedCaption->setText(!s.error.isEmpty()
                                    ? QStringLiteral("The speed test did not finish: %1").arg(s.error)
                                    : QStringLiteral("%1 each way through %2. Measured this run on this network; not a guarantee.")
                                          .arg(formatBytes(s.bytes), s.path));
        if (!s.cleaned) {
            m_leftover->setMessage(QStringLiteral("The speed-test file %1 could not be removed from the PS5. Delete it in the FTP browser.").arg(s.path));
            m_leftover->show();
        }
    } else {
        for (const QString& k : {QStringLiteral("upload"), QStringLiteral("download"), QStringLiteral("readback")})
            setCell(k, QStringLiteral("—"));
        m_speedCaption->setText(!s.error.isEmpty() ? s.error
                                                   : QStringLiteral("Not run. Run speed test sends %1 to the folder above, reads it back and deletes it.")
                                                         .arg(formatBytes(ps5::kSpeedTestBytes)));
    }
}

void DiagnosticsTab::showSpecimen()
{
    ps5::FtpDiagnosis d;
    d.host = QStringLiteral("192.168.1.42");
    d.port = 2121;
    d.folder = QStringLiteral("/mnt/usb0/games");
    d.reachable = true;
    d.rttMs = 4;
    d.greeted = true;
    d.banner = QStringLiteral("FTP server ready (specimen, not from a console)");
    d.loggedIn = true;
    d.featOk = true;
    d.features.mlsd = d.features.rest = d.features.size = d.features.mdtm = d.features.utf8 = true;
    d.passive = true;
    d.passiveMode = QStringLiteral("PASV");
    d.binary = true;
    d.writable = true;
    d.probePath = d.folder + QStringLiteral("/.exfatbuilder_wtest_1762257600.tmp");
    d.destination = ps5::classifyDestination(d.folder).label;
    d.server = ps5::classifyFtpServer(d.features, d.banner);
    d.speed.ran = true;
    d.speed.bytes = ps5::kSpeedTestBytes;
    d.speed.uploadMBps = 38.4;
    d.speed.downloadMBps = 61.0;
    d.speed.readBackMatches = true;
    d.speed.path = d.folder + QStringLiteral("/.exfatbuilder_speedtest_1762257600.bin");
    showResult(d, QStringLiteral("All checks passed"), Tone::Ok, QStringLiteral("Specimen result for the screenshot, not from a console."));
}

QString DiagnosticsTab::statusText() const { return m_status->text(); }
QString DiagnosticsTab::verdictText() const { return m_verdict->text(); }
QString DiagnosticsTab::bannerText() const { return m_banner->text(); }

QString DiagnosticsTab::cellText(const QString& key) const
{
    const auto it = m_cells.constFind(key);
    if (it == m_cells.cend())
        return {};
    return it->badge ? it->badge->text() : it->value->text();
}

Tone DiagnosticsTab::cellTone(const QString& key) const
{
    const auto it = m_cells.constFind(key);
    return it != m_cells.cend() && it->badge ? it->badge->tone() : Tone::Neutral;
}

} // namespace fw::pages
