#include "app/services/SetupChecklist.h"
#include "app/i18n/I18n.h"

#include "connect/ps5/FtpDiagnostics.h"
#include "connect/ps5/Module.h"
#include "core/FileUtil.h"
#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace fw::ui {

namespace {

const QString kDiagnoseKind = QStringLiteral("ps5.ftp.diagnose");

int portOf(const SettingsValues& s, const QString& key, int fallback)
{
    const qint64 v = s.integer(key);
    return v >= 1 && v <= 65535 ? int(v) : fallback;
}

QString settingsPage() { return QStringLiteral("settings"); }

QVariantMap settingsArgs(const QString& key)
{
    return {{QStringLiteral("section"), QStringLiteral("PS5 connection")}, {QStringLiteral("key"), key}};
}

QString ago(const QDateTime& t)
{
    const qint64 s = t.secsTo(QDateTime::currentDateTime());
    if (s < 60)
        return tr("just now");
    if (s < 3600)
        return tr("%1 min ago").arg(s / 60);
    if (s < 86400)
        return tr("%1 h ago").arg(s / 3600);
    return t.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

bool FtpCheckRecord::isFor(const QString& h, int p) const
{
    return host.compare(h.trimmed(), Qt::CaseInsensitive) == 0 && port == p;
}

QString lastFtpCheckPath()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("ps5-last-check.json"));
}

std::optional<FtpCheckRecord> loadLastFtpCheck()
{
    QFile f(lastFtpCheckPath());
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return std::nullopt;
    FtpCheckRecord r;
    r.host = o.value(QStringLiteral("host")).toString();
    r.port = o.value(QStringLiteral("port")).toInt();
    r.reachable = o.value(QStringLiteral("reachable")).toBool();
    r.loggedIn = o.value(QStringLiteral("loggedIn")).toBool();
    r.when = QDateTime::fromString(o.value(QStringLiteral("when")).toString(), Qt::ISODate);
    r.detail = o.value(QStringLiteral("detail")).toString();
    if (r.host.isEmpty())
        return std::nullopt;
    return r;
}

bool saveLastFtpCheck(const FtpCheckRecord& r)
{
    const QJsonObject o{{QStringLiteral("host"), r.host},
                        {QStringLiteral("port"), r.port},
                        {QStringLiteral("reachable"), r.reachable},
                        {QStringLiteral("loggedIn"), r.loggedIn},
                        {QStringLiteral("when"), r.when.toString(Qt::ISODate)},
                        {QStringLiteral("detail"), r.detail}};
    QDir().mkpath(Paths::dataDir());
    return writeFileAtomic(lastFtpCheckPath(), QJsonDocument(o).toJson()).ok();
}

std::optional<FtpCheckRecord> ftpCheckFromJob(const JobRecord& rec)
{
    if (rec.kind != kDiagnoseKind || !rec.outcome || rec.outcome->details.isEmpty())
        return std::nullopt;
    const ps5::FtpDiagnosis d = ps5::FtpDiagnosis::fromJson(rec.outcome->details);
    if (d.host.isEmpty())
        return std::nullopt;
    FtpCheckRecord r;
    r.host = d.host;
    r.port = d.port;
    r.reachable = d.reachable;
    r.loggedIn = d.loggedIn;
    r.when = rec.finished.isValid() ? rec.finished : QDateTime::currentDateTime();
    r.detail = d.error;
    return r;
}

ChecklistInputs checklistInputs(const SettingsValues& s)
{
    ChecklistInputs in;
    in.consoleIp = s.string(ps5::keys::kConsoleIp).trimmed();
    in.ftpPort = portOf(s, ps5::keys::kFtpPort, 2121);
    in.klogPort = portOf(s, ps5::keys::kKlogPort, 3232);
    in.payloadPort = portOf(s, ps5::keys::kPayloadPort, 9090);
    return in;
}

QList<ChecklistItem> setupChecklist(const ChecklistInputs& in)
{
    QList<ChecklistItem> out;
    const bool ip = !in.consoleIp.isEmpty();

    ChecklistItem a; // the address fields and Find my PS5 are in the PS5 connect header
    a.id = QStringLiteral("ip");
    a.done = ip;
    a.label = ip ? tr("PS5 IP configured") : tr("PS5 IP not configured");
    a.detail = ip ? in.consoleIp : tr("Type it on PS5 connect, or let Find my PS5 search the network.");
    a.fixText = ip ? QString() : tr("Find my PS5");
    a.fixPage = QStringLiteral("ps5");
    out << a;

    ChecklistItem f;
    f.id = QStringLiteral("ftp");
    f.fixPage = QStringLiteral("ps5");
    f.fixArgs = {{QStringLiteral("tab"), QStringLiteral("diagnostics")}}; // Ps5ConnectPage tab key
    const QString where = QStringLiteral("%1:%2").arg(in.consoleIp).arg(in.ftpPort);
    if (!ip) {
        f.label = tr("FTP not checked");
        f.detail = tr("Set the console's address first.");
    } else if (in.browserConnected) {
        f.done = true;
        f.label = tr("FTP reachable");
        f.detail = tr("FTP browser connected to %1").arg(where);
    } else if (in.lastCheck && in.lastCheck->isFor(in.consoleIp, in.ftpPort)) {
        f.done = in.lastCheck->passed();
        f.label = f.done ? tr("FTP reachable") : tr("FTP not reachable");
        f.detail = f.done ? tr("Diagnostics reached %1 %2").arg(where, ago(in.lastCheck->when))
                          : QStringLiteral("%1 (%2)").arg(in.lastCheck->detail.isEmpty() ? tr("The last check failed") : in.lastCheck->detail,
                                                         ago(in.lastCheck->when));
        f.fixText = f.done ? QString() : tr("Run diagnostics");
    } else {
        f.label = tr("FTP not checked");
        f.detail = tr("Run the diagnostics on PS5 connect to check %1.").arg(where);
        f.fixText = tr("Run diagnostics");
    }
    out << f;

    ChecklistItem k;
    k.id = QStringLiteral("klog");
    k.fixPage = settingsPage();
    k.fixArgs = settingsArgs(ps5::keys::kKlogPort);
    const bool klogClash = in.klogPort == in.ftpPort;
    k.done = ip && !klogClash;
    k.label = k.done ? tr("Kernel log configured") : tr("Kernel log not configured");
    k.detail = !ip ? tr("Set the console's address first.")
             : klogClash ? tr("The kernel log port (%1) is the FTP port.").arg(in.klogPort)
                         : tr("klogsrv at %1:%2").arg(in.consoleIp).arg(in.klogPort);
    k.fixText = ip && klogClash ? tr("Change port") : QString();
    out << k;

    ChecklistItem p;
    p.id = QStringLiteral("payload");
    p.fixPage = settingsPage();
    p.fixArgs = settingsArgs(ps5::keys::kPayloadPort);
    const bool payloadClash = in.payloadPort == in.ftpPort || in.payloadPort == in.klogPort;
    p.done = ip && !payloadClash;
    p.label = p.done ? tr("Payload port configured") : tr("Payload port not configured");
    p.detail = !ip ? tr("Set the console's address first.")
             : payloadClash ? tr("The payload port (%1) is also the FTP or kernel log port.").arg(in.payloadPort)
                            : tr("Payload loader at %1:%2").arg(in.consoleIp).arg(in.payloadPort);
    p.fixText = ip && payloadClash ? tr("Change port") : QString();
    out << p;

    ChecklistItem l;
    l.id = QStringLiteral("payloads");
    l.done = in.payloadsReady > 0;
    l.label = l.done ? tr("Payloads added") : tr("No payloads added");
    l.detail = l.done ? tr("%1 payload file%2 ready to send").arg(in.payloadsReady).arg(in.payloadsReady == 1 ? QString() : tr("s"))
                      : tr("Add your .elf / .bin payloads on PS5 connect › Payloads.");
    l.fixText = l.done ? QString() : tr("Add payloads");
    l.fixPage = QStringLiteral("ps5");
    l.fixArgs = {{QStringLiteral("tab"), QStringLiteral("payloads")}};
    out << l;
    return out;
}

int completedCount(const QList<ChecklistItem>& items)
{
    int n = 0;
    for (const auto& i : items)
        n += i.done ? 1 : 0;
    return n;
}

} // namespace fw::ui
