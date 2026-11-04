#include "connect/ps5/MountConfig.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/KlogClient.h"
#include "connect/ps5/RemotePath.h"

#include <QElapsedTimer>
#include <QRegularExpression>

#include <algorithm>
#include <memory>

namespace fw::ps5 {

namespace {

const QStringList& smpMultiKeys()
{
    static const QStringList k = {QStringLiteral("kstuff_no_pause"), QStringLiteral("kstuff_delay"),
                                  QStringLiteral("scanpath"), QStringLiteral("global_fakelib_exclude")};
    return k;
}

// Python int(v, 0): decimal (no leading zeros), 0x / 0o / 0b prefixes, optional sign.
bool pythonIntBase0(const QString& in, qint64* out)
{
    QString s = in.trimmed();
    s.remove(u'_');
    bool neg = false;
    if (s.startsWith(u'-') || s.startsWith(u'+')) {
        neg = s.startsWith(u'-');
        s.remove(0, 1);
    }
    bool ok = false;
    qint64 v = 0;
    const QString low = s.toLower();
    if (low.startsWith(QLatin1String("0x")))
        v = s.mid(2).toLongLong(&ok, 16);
    else if (low.startsWith(QLatin1String("0o")))
        v = s.mid(2).toLongLong(&ok, 8);
    else if (low.startsWith(QLatin1String("0b")))
        v = s.mid(2).toLongLong(&ok, 2);
    else if (!s.isEmpty() && (s == u"0" || !s.startsWith(u'0'))) {
        for (QChar c : s)
            if (!c.isDigit())
                return false;
        v = s.toLongLong(&ok, 10);
    }
    if (!ok)
        return false;
    *out = neg ? -v : v;
    return true;
}

// Python int(v) for the ShadowMount+ checks: decimal only, sign allowed.
bool pythonInt(const QString& in, qint64* out)
{
    QString s = in.trimmed();
    s.remove(u'_');
    static const QRegularExpression re(QStringLiteral("^[+-]?\\d+$"));
    if (!re.match(s).hasMatch())
        return false;
    bool ok = false;
    *out = s.toLongLong(&ok);
    return ok;
}

QStringList listLines(const MountConfigValues& v, const QString& key)
{
    QStringList out;
    for (const QString& raw : v.value(key).split(u'\n')) {
        const QString t = raw.trimmed();
        if (!t.isEmpty() && !t.startsWith(u'#'))
            out << t;
    }
    return out;
}

} // namespace

QString mountToolName(MountTool tool)
{
    return tool == MountTool::ShadowMountPlus ? QStringLiteral("ShadowMountPlus") : QStringLiteral("MicroMount");
}

QString mountRemoteDir(MountTool tool)
{
    return tool == MountTool::ShadowMountPlus ? QStringLiteral("/data/shadowmount") : QStringLiteral("/data/micromount");
}

QString mountConfigRemotePath(MountTool tool)
{
    return mountRemoteDir(tool) + QStringLiteral("/config.ini");
}

QString mountDebugLogRemotePath(MountTool tool)
{
    return mountRemoteDir(tool) + QStringLiteral("/debug.log");
}

MountConfigValues mountConfigDefaults(MountTool tool)
{
    if (tool == MountTool::ShadowMountPlus)
        return {
            {QStringLiteral("debug"), QStringLiteral("1")},
            {QStringLiteral("quiet_mode"), QStringLiteral("0")},
            {QStringLiteral("mount_read_only"), QStringLiteral("1")},
            {QStringLiteral("force_mount"), QStringLiteral("0")},
            {QStringLiteral("app_install_all"), QStringLiteral("0")},
            {QStringLiteral("scan_depth"), QStringLiteral("1")},
            {QStringLiteral("scan_interval_seconds"), QStringLiteral("60")},
            {QStringLiteral("stability_wait_seconds"), QStringLiteral("10")},
            {QStringLiteral("exfat_backend"), QStringLiteral("lvd")},
            {QStringLiteral("ufs_backend"), QStringLiteral("lvd")},
            {QStringLiteral("backport_fakelib"), QStringLiteral("1")},
            {QStringLiteral("global_fakelib"), QStringLiteral("1")},
            {QStringLiteral("global_fakelib_path"), QStringLiteral("/data/shadowmount/fakelib")},
            {QStringLiteral("global_fakelib_priority"), QStringLiteral("game")},
            {QStringLiteral("kstuff_game_auto_toggle"), QStringLiteral("1")},
            {QStringLiteral("kstuff_crash_detection"), QStringLiteral("1")},
            {QStringLiteral("kstuff_pause_delay_image_seconds"), QStringLiteral("25")},
            {QStringLiteral("kstuff_pause_delay_direct_seconds"), QStringLiteral("15")},
            {QStringLiteral("lvd_exfat_sector_size"), QStringLiteral("512")},
            {QStringLiteral("lvd_ufs_sector_size"), QStringLiteral("4096")},
            {QStringLiteral("lvd_pfs_sector_size"), QStringLiteral("32768")},
            {QStringLiteral("md_exfat_sector_size"), QStringLiteral("512")},
            {QStringLiteral("md_ufs_sector_size"), QStringLiteral("512")},
        };
    return {
        {QStringLiteral("target_directory"), QStringLiteral("/data/homebrew")},
        {QStringLiteral("scan_depth"), QStringLiteral("1")},
        {QStringLiteral("scan_interval_seconds"), QStringLiteral("30")},
        {QStringLiteral("debug"), QStringLiteral("1")},
        {QStringLiteral("lvd_image_type"), QStringLiteral("0")},
        {QStringLiteral("lvd_sector_size"), QStringLiteral("65536")},
        {QStringLiteral("lvd_secondary_unit"), QStringLiteral("65536")},
        {QStringLiteral("lvd_raw_flags"), QStringLiteral("0x9")},
        {QStringLiteral("pfs_fstype"), QStringLiteral("pfs")},
        {QStringLiteral("pfs_mkeymode"), QStringLiteral("AC")},
        {QStringLiteral("pfs_budgetid"), QStringLiteral("system")},
        {QStringLiteral("pfs_sigverify"), QStringLiteral("0")},
        {QStringLiteral("pfs_playgo"), QStringLiteral("0")},
        {QStringLiteral("pfs_disc"), QStringLiteral("0")},
        {QStringLiteral("pfs_use_ekpfs"), QStringLiteral("1")},
        {QStringLiteral("pfs_read_only"), QStringLiteral("1")},
        {QStringLiteral("pfs_force"), QStringLiteral("0")},
    };
}

QStringList mountConfigKeys(MountTool tool)
{
    QStringList keys = mountConfigDefaults(tool).keys();
    if (tool == MountTool::ShadowMountPlus)
        keys << smpMultiKeys() << QStringLiteral("per_image_rules");
    else
        keys << QStringLiteral("scanpath");
    keys.sort();
    return keys;
}

MountConfigValues parseMountConfig(MountTool tool, const QString& text)
{
    MountConfigValues out;
    QMap<QString, QStringList> multi;
    QStringList perImage;
    const QStringList multiKeys = tool == MountTool::ShadowMountPlus ? smpMultiKeys() : QStringList{QStringLiteral("scanpath")};
    for (const QString& raw : text.split(u'\n')) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';'))
            continue;
        const qsizetype eq = line.indexOf(u'=');
        if (eq < 0)
            continue;
        const QString k = line.left(eq).trimmed();
        const QString v = line.mid(eq + 1).trimmed();
        if (k.isEmpty())
            continue;
        if (multiKeys.contains(k)) {
            multi[k] << v;
            continue;
        }
        if (tool == MountTool::ShadowMountPlus
            && (k == u"image_ro" || k == u"image_rw" || k == u"image_sector")) {
            perImage << k + u'=' + v;
            continue;
        }
        if (tool == MountTool::MicroMount && k == u"scan_paths") { // comma/semicolon variant
            static const QRegularExpression sep(QStringLiteral("[,;]"));
            for (const QString& p : v.split(sep))
                if (!p.trimmed().isEmpty())
                    multi[QStringLiteral("scanpath")] << p.trimmed();
            continue;
        }
        out.insert(k, v);
    }
    for (auto it = multi.cbegin(); it != multi.cend(); ++it)
        if (!it.value().isEmpty())
            out.insert(it.key(), it.value().join(u'\n'));
    if (!perImage.isEmpty())
        out.insert(QStringLiteral("per_image_rules"), perImage.join(u'\n'));
    return out;
}

QString renderMountConfig(MountTool tool, const MountConfigValues& data)
{
    QStringList lines;
    auto put = [&](const char* key) {
        const QString v = data.value(QLatin1String(key));
        if (!v.isEmpty())
            lines << QStringLiteral("%1=%2").arg(QLatin1String(key), v);
    };
    auto putList = [&](const char* key) {
        for (const QString& t : listLines(data, QLatin1String(key)))
            lines << QStringLiteral("%1=%2").arg(QLatin1String(key), t);
    };
    if (tool == MountTool::ShadowMountPlus) {
        lines << QStringLiteral("# Generated by exFAT Image Builder") << QStringLiteral("# ShadowMountPlus runtime config")
              << QStringLiteral("# Edit and re-push from the GUI.") << QString();
        lines << QString::fromUtf8("# ── kstuff ─────────────────────────────────────");
        for (const char* k : {"kstuff_game_auto_toggle", "kstuff_crash_detection", "kstuff_pause_delay_image_seconds",
                              "kstuff_pause_delay_direct_seconds"})
            put(k);
        putList("kstuff_no_pause");
        putList("kstuff_delay");
        lines << QString();
        lines << QString::fromUtf8("# ── mount ──────────────────────────────────────");
        for (const char* k : {"mount_read_only", "force_mount", "app_install_all", "exfat_backend", "ufs_backend"})
            put(k);
        lines << QString();
        lines << QString::fromUtf8("# ── scanning ───────────────────────────────────");
        for (const char* k : {"scan_depth", "scan_interval_seconds", "stability_wait_seconds"})
            put(k);
        putList("scanpath");
        lines << QString();
        lines << QString::fromUtf8("# ── fakelib ────────────────────────────────────");
        for (const char* k : {"backport_fakelib", "global_fakelib", "global_fakelib_path", "global_fakelib_priority"})
            put(k);
        putList("global_fakelib_exclude");
        lines << QString();
        const QString rules = data.value(QStringLiteral("per_image_rules")).trimmed();
        if (!rules.isEmpty()) {
            lines << QString::fromUtf8("# ── per-image overrides ────────────────────");
            for (const QString& raw : rules.split(u'\n')) {
                const QString t = raw.trimmed();
                if (!t.isEmpty() && !t.startsWith(u'#') && t.contains(u'='))
                    lines << t;
            }
            lines << QString();
        }
        lines << QString::fromUtf8("# ── advanced ───────────────────────────────────");
        for (const char* k : {"debug", "quiet_mode", "lvd_exfat_sector_size", "lvd_ufs_sector_size", "lvd_pfs_sector_size",
                              "md_exfat_sector_size", "md_ufs_sector_size"})
            put(k);
    } else {
        lines << QStringLiteral("# MicroMount runtime config") << QStringLiteral("# Generated by exFAT Image Builder")
              << QStringLiteral("# Place as: /data/micromount/config.ini") << QString();
        lines << QString::fromUtf8("# ── core ───────────────────────────────────────");
        for (const char* k : {"target_directory", "scan_depth", "scan_interval_seconds", "debug"})
            put(k);
        putList("scanpath");
        lines << QString();
        lines << QString::fromUtf8("# ── LVD mount profile ──────────────────────────");
        for (const char* k : {"lvd_image_type", "lvd_sector_size", "lvd_secondary_unit", "lvd_raw_flags"})
            put(k);
        lines << QString();
        lines << QString::fromUtf8("# ── PFS mount profile ──────────────────────────");
        for (const char* k : {"pfs_fstype", "pfs_mkeymode", "pfs_budgetid", "pfs_sigverify", "pfs_playgo", "pfs_disc",
                              "pfs_use_ekpfs", "pfs_read_only", "pfs_force"})
            put(k);
    }
    return lines.join(u'\n') + u'\n';
}

QStringList validateMountConfig(MountTool tool, const MountConfigValues& data)
{
    QStringList problems;
    if (tool == MountTool::ShadowMountPlus) {
        for (const char* k : {"kstuff_pause_delay_image_seconds", "kstuff_pause_delay_direct_seconds",
                              "scan_interval_seconds", "stability_wait_seconds"}) {
            const QString v = data.value(QLatin1String(k)).trimmed();
            if (v.isEmpty())
                continue;
            qint64 n = 0;
            if (!pythonInt(v, &n) || n < 0 || n > 3600)
                problems << QStringLiteral("%1 must be a whole number in 0–3600. Got: %2").arg(QLatin1String(k), v);
        }
        for (const QString& t : listLines(data, QStringLiteral("kstuff_delay"))) {
            const qsizetype colon = t.indexOf(u':');
            if (colon < 0) {
                problems << QStringLiteral("Expected TITLEID:SECONDS, got: %1").arg(t);
                continue;
            }
            const QString sec = t.mid(colon + 1);
            qint64 n = 0;
            if (!pythonInt(sec, &n) || n < 0 || n > 3600)
                problems << QStringLiteral("Seconds must be 0–3600, got: %1 in rule %2").arg(sec, t);
        }
    } else {
        for (const char* k : {"scan_interval_seconds", "lvd_sector_size", "lvd_secondary_unit", "lvd_image_type"}) {
            const QString v = data.value(QLatin1String(k)).trimmed();
            if (v.isEmpty())
                continue;
            qint64 n = 0;
            if (!pythonIntBase0(v, &n) || n < 0)
                problems << QStringLiteral("%1 must be a non-negative integer. Got: %2").arg(QLatin1String(k), v);
        }
    }
    return problems;
}

Result<QString> loadMountConfig(FtpClient& c, MountTool tool, const CancellationToken& token)
{
    auto bytes = c.download(mountConfigRemotePath(tool), token);
    if (!bytes.ok()) {
        if (bytes.error().code == ErrorCode::NotFound)
            return Error(ErrorCode::NotFound,
                         QStringLiteral("%1 does not exist on the PS5. This is normal if %2 has never been run. Edit "
                                        "the settings and push them to create it.")
                             .arg(mountConfigRemotePath(tool), mountToolName(tool)),
                         bytes.error().detail);
        return bytes.takeError();
    }
    return QString::fromUtf8(bytes.value());
}

Result<QString> fetchMountDebugLog(FtpClient& c, MountTool tool, const CancellationToken& token)
{
    FW_TRY_ASSIGN(QByteArray bytes, c.download(mountDebugLogRemotePath(tool), token));
    return QString::fromUtf8(bytes);
}

Result<MountPushResult> pushMountConfig(FtpClient& c, MountTool tool, const QString& text, const QString& backupDir,
                                        const CancellationToken& token)
{
    MountPushResult out;
    const QString remote = mountConfigRemotePath(tool);
    (void)c.mkdirs(mountRemoteDir(tool)); // best effort, like the legacy MKD
    FW_TRY_ASSIGN(out.backupPath, backupRemoteFile(c, remote, backupDir, token));
    const QByteArray bytes = text.toUtf8();
    const Status swap = c.uploadReplace(remote, bytes, QStringLiteral(".new"), token);
    if (swap.ok()) {
        out.swapped = true;
        return out;
    }
    if (swap.error().code != ErrorCode::Unsupported || !c.isConnected())
        return swap.error();
    // Rename unsupported: direct STOR (legacy fallback); the backup is local.
    FW_TRY(c.upload(remote, bytes, token));
    (void)c.dele(remote + QStringLiteral(".new"));
    return out;
}

// ---- Safe to unplug -----------------------------------------------------------------------

namespace {

qint64 monotonicMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

} // namespace

StopConfirmTracker::StopConfirmTracker(int settleMs) : m_settleMs(std::max(0, settleMs)), m_clockStart(monotonicMs()) {}

bool StopConfirmTracker::feed(const QString& line)
{
    return feed(line, monotonicMs() - m_clockStart);
}

bool StopConfirmTracker::poll()
{
    return poll(monotonicMs() - m_clockStart);
}

bool StopConfirmTracker::feed(const QString& line, qint64 nowMs)
{
    if (m_confirmed)
        return true;
    const QString low = line.toLower();
    if (low.contains(QLatin1String("shadowmountplus.elf calls exit"))) {
        m_exit = true;
        m_confirmed = true;
        return true;
    }
    if (low.contains(QLatin1String("[shutdown] stop requested")))
        m_ack = true;
    if (m_ack && low.contains(QLatin1String("unmount start")))
        ++m_started;
    if (m_ack && low.contains(QLatin1String("unmount complete")))
        ++m_completed;
    const bool balanced = m_ack && m_started > 0 && m_completed >= m_started;
    if (!balanced)
        m_balancedSince = -1;
    else if (m_balancedSince < 0)
        m_balancedSince = nowMs;
    return poll(nowMs);
}

bool StopConfirmTracker::poll(qint64 nowMs)
{
    if (!m_confirmed && m_balancedSince >= 0 && nowMs - m_balancedSince >= m_settleMs)
        m_confirmed = true;
    return m_confirmed;
}

QString stopOutcomeMessage(StopOutcome outcome)
{
    switch (outcome) {
    case StopOutcome::Confirmed:
        return QStringLiteral("ShadowMountPlus cleanup confirmed — safe to unplug. Re-send the payload after plugging "
                              "the drive back in to resume auto-mounting.");
    case StopOutcome::NoKlog:
        return QStringLiteral("STOP sent — cleanup NOT confirmed (the kernel log could not be reached). Do not unplug "
                              "yet; wait, or check the kernel log.");
    case StopOutcome::KlogLost:
        return QStringLiteral("STOP sent — cleanup NOT confirmed (the kernel log connection dropped). Do not unplug yet.");
    case StopOutcome::Timeout:
        return QStringLiteral("STOP sent — cleanup NOT confirmed in time. Do not unplug yet.");
    }
    return {};
}

Result<StopResult> stopShadowMount(const ConsoleEndpoint& ep, const CancellationToken& token,
                                   const std::function<void(const QString&)>& status, int confirmTimeoutMs,
                                   int klogConnectTimeoutMs, int settleMs)
{
    auto say = [&](const QString& s) {
        if (status)
            status(s);
    };
    // 1. klog first, so no cleanup line races past us.
    say(QStringLiteral("Connecting to klog (%1:%2)...").arg(ep.host).arg(ep.klogPort));
    std::unique_ptr<KlogSession> klog;
    if (auto k = KlogSession::open(ep.host, ep.klogPort, klogConnectTimeoutMs, token); k.ok())
        klog = std::move(k.value());
    else if (k.error().isCancelled())
        return Error::cancelled();

    // 2. The STOP sentinel, always.
    say(QStringLiteral("Sending STOP signal to PS5..."));
    {
        FtpClient c(ftpOptionsFor(ep));
        FW_TRY(c.open(ep.host, ep.ftpPort, token));
        (void)c.mkdirs(mountRemoteDir(MountTool::ShadowMountPlus)); // best effort
        FW_TRY(c.upload(mountRemoteDir(MountTool::ShadowMountPlus) + QStringLiteral("/STOP"), QByteArray(), token));
        c.quit();
    }
    StopResult res;
    if (!klog) {
        res.outcome = StopOutcome::NoKlog;
        return res;
    }
    // 3. Watch for the confirmation. Lines are read in short slices so the
    //    settle period can complete while klog is quiet.
    say(QStringLiteral("Waiting for ShadowMountPlus confirmation..."));
    StopConfirmTracker tracker(settleMs);
    QElapsedTimer clock;
    clock.start();
    res.outcome = StopOutcome::Timeout;
    while (true) {
        if (tracker.poll(clock.elapsed())) {
            res.outcome = StopOutcome::Confirmed;
            break;
        }
        const qint64 left = confirmTimeoutMs - clock.elapsed();
        if (left <= 0)
            break; // Timeout
        const auto r = klog->read(int(std::min<qint64>(left, 200)), token);
        if (r.cancelled)
            return Error::cancelled();
        bool done = false;
        for (const QString& line : r.lines) {
            res.klogLines << line;
            if (tracker.feed(line, clock.elapsed())) {
                done = true;
                break;
            }
        }
        if (done) {
            res.outcome = StopOutcome::Confirmed;
            break;
        }
        if (r.closed) {
            res.outcome = StopOutcome::KlogLost;
            break;
        }
    }
    res.unmountsStarted = tracker.unmountsStarted();
    res.unmountsCompleted = tracker.unmountsCompleted();
    return res;
}

} // namespace fw::ps5
