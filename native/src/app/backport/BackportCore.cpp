#include "app/backport/BackportCore.h"

#include "app/backport/BackportHost.h"
#include "core/FileUtil.h"
#include "core/Tr.h"
#include "core/Zip.h"
#include "engines/backport/CanonicalTrio.h"
#include "engines/backport/GameMetadata.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SdkTable.h"
#include "engines/backport/SelfReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <functional>

namespace fw::bp {

namespace {

bool cancelled(const std::atomic_bool* c) { return c && c->load(); }

// `why` (optional) says why the file gave no object: empty for "there is no
// such file", otherwise the reason it could not be opened or parsed.
QJsonObject readJsonObject(const QString& path, bool* ok = nullptr, QString* why = nullptr)
{
    if (ok)
        *ok = false;
    if (why)
        why->clear();
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (why && f.exists())
            *why = f.errorString();
        return {};
    }
    QByteArray b = f.readAll();
    if (b.startsWith("\xEF\xBB\xBF"))
        b.remove(0, 3);
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(b, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        if (why)
            *why = pe.error != QJsonParseError::NoError ? pe.errorString() : QStringLiteral("it does not hold a JSON object");
        return {};
    }
    if (ok)
        *ok = true;
    return d.object();
}

// settings.json as an object. An absent file, and one with nothing in it, is
// Missing (there is nothing to lose); anything else that does not parse is
// Unreadable and must never be replaced.
QJsonObject readSettingsObject(SettingsFile* state, QString* error)
{
    const QString path = folders().settingsPath;
    bool ok = false;
    QString why;
    QJsonObject o = readJsonObject(path, &ok, &why);
    SettingsFile st = ok ? SettingsFile::Ok : SettingsFile::Missing;
    if (!ok && !why.isEmpty()) {
        st = SettingsFile::Unreadable;
        QFile f(toLongPath(path));
        if (f.open(QIODevice::ReadOnly) && f.readAll().trimmed().isEmpty()) {
            st = SettingsFile::Missing;
            why.clear();
        }
    }
    if (state)
        *state = st;
    if (error)
        *error = why;
    return o;
}

// Equal to or inside (both normalised).
bool within(const QString& child, const QString& parent)
{
    const QString c = normKey(child), p = normKey(parent);
    if (c.isEmpty() || p.isEmpty())
        return false;
    if (c == p)
        return true;
    const QString pp = p.endsWith(u'\\') || p.endsWith(u'/') ? p : p + QDir::separator();
    return c.startsWith(pp);
}

QString findOnDisk(const QString& dir, const QString& name)
{
    QDir d(dir);
    if (!d.exists())
        return {};
    for (const QString& e : d.entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (e.compare(name, Qt::CaseInsensitive) == 0)
            return d.filePath(e);
    return {};
}

QString versionText(const QJsonValue& v)
{
    const auto p = backport::parsePs5VersionField(v.toVariant());
    if (!p)
        return {};
    return QStringLiteral("%1.%2").arg(p->first).arg(p->second, 2, 10, QLatin1Char('0'));
}

QString ebootSdk(const QString& folder)
{
    QFile f(toLongPath(QDir(folder).filePath(QStringLiteral("eboot.bin"))));
    if (!f.open(QIODevice::ReadOnly) || f.size() > 512LL * 1024 * 1024)
        return {};
    QByteArray data = f.readAll();
    if (backport::selfio::hasSelfMagic(data)) {
        auto elf = backport::selfio::unwrap(data);
        if (!elf.ok())
            return {};
        data = elf.value();
    }
    quint32 ps5 = 0;
    if (!backport::elfSdkMajor(data, &ps5) || ps5 == 0)
        return {};
    return backport::sdkVersionText(ps5);
}

const QStringList& skipNames()
{
    static const QStringList n = {QStringLiteral("$recycle.bin"), QStringLiteral("system volume information"),
                                  QStringLiteral("windows"), QStringLiteral("program files"),
                                  QStringLiteral("program files (x86)"), QStringLiteral("programdata"),
                                  QStringLiteral("appdata"), QStringLiteral("__pycache__"), QStringLiteral(".git")};
    return n;
}

} // namespace

QString nativeFolder(const QString& path)
{
    const QString p = path.trimmed();
    return p.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::cleanPath(p));
}

QString normKey(const QString& path)
{
    if (path.isEmpty())
        return {};
    QFileInfo fi(path);
    QString p = fi.canonicalFilePath();
    if (p.isEmpty())
        p = QDir::cleanPath(fi.absoluteFilePath());
    p = QDir::toNativeSeparators(p);
#ifdef Q_OS_WIN
    p = p.toLower();
#endif
    while (p.size() > 3 && (p.endsWith(u'\\') || p.endsWith(u'/')))
        p.chop(1);
    return p;
}

// ---- discovery -----------------------------------------------------------------------

QStringList discoverGames(const QStringList& roots, int depth, const QStringList& excludes, const std::atomic_bool* cancel)
{
    QStringList ex;
    for (const QString& e : excludes)
        if (!e.trimmed().isEmpty())
            ex << normKey(e);
    auto excluded = [&](const QString& path) {
        const QString k = normKey(path);
        for (const QString& e : ex) {
            if (k == e)
                return true;
            const QString pref = e.endsWith(QDir::separator()) ? e : e + QDir::separator();
            if (k.startsWith(pref))
                return true;
        }
        return false;
    };
    QStringList found;
    QSet<QString> seen, visited;
    std::function<void(const QString&, int)> walk = [&](const QString& dir, int level) {
        if (cancelled(cancel))
            return;
        const QString key = normKey(dir);
        if (visited.contains(key))
            return; // junction loop
        visited.insert(key);
        if (excluded(dir))
            return;
        const QFileInfo eboot(QDir(dir).filePath(QStringLiteral("eboot.bin")));
        if (eboot.isFile()) {
            if (!seen.contains(key)) {
                seen.insert(key);
                found << QDir::toNativeSeparators(QDir(dir).absolutePath());
            }
            return; // a game folder is not searched further
        }
        if (level >= depth)
            return;
        const QFileInfoList subs = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& s : subs) {
            const QString n = s.fileName().toLower();
            if (n.startsWith(u'$') || skipNames().contains(n))
                continue;
            walk(s.absoluteFilePath(), level + 1);
        }
    };
    for (const QString& r : roots)
        if (QFileInfo(r).isDir())
            walk(QFileInfo(r).absoluteFilePath(), 0);
    std::sort(found.begin(), found.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
    return found;
}

bool plausibleVersion(const QString& v)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)\\.(\\d\\d)$"));
    const auto m = re.match(v);
    if (!m.hasMatch())
        return false;
    const int n = m.captured(1).toInt();
    return n >= 1 && n <= 30;
}

std::pair<QString, QString> nameFromFolder(const QString& name)
{
    static const QRegularExpression verRe(QStringLiteral("(?<!\\d)(\\d{2}\\.\\d{3}\\.\\d{3})(?!\\d)"));
    static const QRegularExpression junk1(QStringLiteral("(?i)_?backup_\\d{8}_\\d{6}|PPSA\\d{5}|CUSA\\d{5}|-?app0|-?patch0|\\[[^\\]]*\\]"));
    static const QRegularExpression emptyParens(QStringLiteral("\\(\\s*\\)"));
    static const QRegularExpression junk2(QStringLiteral("(?i)\\b(?:v|ver)?\\d{2}\\.\\d{3}(?:\\.\\d{3})?\\b|\\bFW\\s*\\d\\.?xx\\b|\\b\\d\\.?xx\\b"));
    static const QRegularExpression collapse(QStringLiteral("[_\\s\\-\\.]+"));
    QString version;
    const auto vm = verRe.match(name);
    if (vm.hasMatch())
        version = vm.captured(1);
    QString t = name;
    if (!version.isEmpty())
        t.remove(version);
    t.remove(junk1);
    t.remove(emptyParens);
    t.replace(u'_', u' ');
    t.remove(junk2);
    t.remove(emptyParens);
    t.replace(collapse, QStringLiteral(" "));
    static const QString strip = QStringLiteral(" -.()");
    while (!t.isEmpty() && strip.contains(t.front()))
        t.remove(0, 1);
    while (!t.isEmpty() && strip.contains(t.back()))
        t.chop(1);
    return {t, version};
}

Game readGame(const QString& folder) { return readGame(folder, readLedger()); }

Game readGame(const QString& folder, const QJsonArray& ledger)
{
    Game g;
    g.path = QDir::toNativeSeparators(folder);
    g.key = normKey(folder);
    try {
        const QDir dir(folder);
        const QString paramPath = dir.filePath(QStringLiteral("sce_sys/param.json"));
        bool ok = false;
        const QJsonObject param = readJsonObject(paramPath, &ok);
        const QString icon = dir.filePath(QStringLiteral("sce_sys/icon0.png"));
        if (QFileInfo(icon).isFile())
            g.icon = icon;
        if (ok) {
            g.titleId = param.value(QStringLiteral("titleId")).toString().trimmed().toUpper();
            g.version = param.value(QStringLiteral("contentVersion")).toString().trimmed();
            const QJsonObject loc = param.value(QStringLiteral("localizedParameters")).toObject();
            QString lang = loc.value(QStringLiteral("defaultLanguage")).toString();
            if (lang.isEmpty())
                lang = QStringLiteral("en-US");
            g.title = loc.value(lang).toObject().value(QStringLiteral("titleName")).toString().trimmed();
            if (g.title.isEmpty())
                g.title = loc.value(QStringLiteral("en-US")).toObject().value(QStringLiteral("titleName")).toString().trimmed();
            const QString sdk = versionText(param.value(QStringLiteral("sdkVersion")));
            const QString fw = versionText(param.value(QStringLiteral("requiredSystemSoftwareVersion")));
            if (!sdk.isEmpty()) {
                if (plausibleVersion(sdk))
                    g.sdk = sdk;
                else
                    g.notes << FW_TR("param.json sdkVersion reads as %1, which is not an SDK; using eboot.bin").arg(sdk);
            }
            if (!fw.isEmpty()) {
                if (plausibleVersion(fw))
                    g.fw = fw;
                else
                    g.notes << FW_TR("param.json requiredSystemSoftwareVersion reads as %1, which is not a firmware").arg(fw);
            }
        }
        if (g.sdk.isEmpty()) {
            const QString s = ebootSdk(folder);
            if (plausibleVersion(s)) {
                g.sdk = s;
                g.notes << FW_TR("SDK read from eboot.bin (param.json has none or an invalid value)");
            }
        }
        const QString folderName = QFileInfo(folder).fileName();
        if (g.titleId.isEmpty()) {
            static const QRegularExpression ppsa(QStringLiteral("(?i)PPSA\\d{5}"));
            const auto m = ppsa.match(folderName);
            if (m.hasMatch())
                g.titleId = m.captured(0).toUpper();
        }
        if (g.title.isEmpty() || g.version.isEmpty()) {
            const auto [t, v] = nameFromFolder(folderName);
            bool used = false;
            if (g.title.isEmpty() && !t.isEmpty()) {
                g.title = t;
                used = true;
            }
            if (g.version.isEmpty() && !v.isEmpty()) {
                g.version = v;
                used = true;
            }
            if (used)
                g.notes << FW_TR("Title and version read from the folder name (param.json has none)");
        }
        // Already backported: any canonical trio member, by size and SHA-256.
        const QString fakelib = dir.filePath(QStringLiteral("fakelib"));
        int hits = 0, present = 0;
        for (const auto& lib : backport::kCanonicalTrio) {
            const QString p = findOnDisk(fakelib, QString::fromLatin1(lib.name));
            if (p.isEmpty())
                continue;
            ++present;
            const QFileInfo fi(p);
            if (fi.size() != lib.size)
                continue;
            auto h = sha256File(p, {});
            if (h.ok() && h.value().compare(QLatin1String(lib.sha256), Qt::CaseInsensitive) == 0)
                ++hits;
        }
        if (hits > 0) {
            g.alreadyBackported = true;
            g.notes << FW_TR("%1 canonical backport fakelib(s) already present in this folder").arg(hits);
        } else if (present == 3) {
            g.notes << FW_TR("fakelib holds another tool's AGC / AGCDriver / PSML: the build replaces them with this "
                                      "program's set and the originals backup keeps the dump's copies");
        }
        g.appliedRecord = activeLedgerRecord(folder, ledger);
    } catch (const std::exception& e) {
        g.error = FW_TR("Error: %1").arg(QString::fromLocal8Bit(e.what()));
    }
    return g;
}

qint64 executableBytes(const QString& folder, const std::atomic_bool* cancel)
{
    static const QStringList suffixes = {QString(), QStringLiteral("bin"), QStringLiteral("prx"), QStringLiteral("sprx"),
                                         QStringLiteral("elf"), QStringLiteral("self"), QStringLiteral("so")};
    qint64 total = 0;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelled(cancel))
            break;
        const QFileInfo fi(it.next());
        const QString name = fi.fileName();
        if (name.compare(QLatin1String("param.json"), Qt::CaseInsensitive) == 0) {
            total += fi.size();
            continue;
        }
        if (!suffixes.contains(fi.suffix().toLower()))
            continue;
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray head = f.read(4);
        if (head == QByteArray("\x7f" "ELF", 4) || head == QByteArray("\x4f\x15\x3d\x1d", 4) || head == QByteArray("\x54\x14\xf5\xee", 4))
            total += fi.size();
    }
    return total;
}

// ---- ledger --------------------------------------------------------------------------

namespace {
QJsonArray ledgerRows()
{
    return readJsonObject(folders().ledgerPath).value(QStringLiteral("applied")).toArray();
}
QString journalState(const QString& journal, bool* readable)
{
    bool ok = false;
    const QJsonObject j = readJsonObject(journal, &ok);
    if (readable)
        *readable = ok;
    return j.value(QStringLiteral("state")).toString();
}
} // namespace

QJsonArray readLedger() { return ledgerRows(); }

QJsonObject activeLedgerRecord(const QString& gameDir) { return activeLedgerRecord(gameDir, ledgerRows()); }

QJsonObject activeLedgerRecord(const QString& gameDir, const QJsonArray& ledger)
{
    const QString key = normKey(gameDir);
    std::vector<QJsonObject> rows;
    for (const QJsonValue& v : ledger) {
        const QJsonObject r = v.toObject();
        if (normKey(r.value(QStringLiteral("game_dir")).toString()) != key)
            continue;
        const QString st = r.value(QStringLiteral("state")).toString();
        if (st != QLatin1String("APPLIED") && st != QLatin1String("IN_PROGRESS"))
            continue;
        bool readable = false;
        const QString js = journalState(r.value(QStringLiteral("journal")).toString(), &readable);
        if (readable && js != QLatin1String("APPLIED") && js != QLatin1String("IN_PROGRESS"))
            continue;
        rows.push_back(r);
    }
    if (rows.empty())
        return {};
    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) {
        return a.value(QStringLiteral("started_at")).toString() > b.value(QStringLiteral("started_at")).toString();
    });
    return rows.front();
}

std::vector<QJsonObject> unfinishedApplies(int quietSeconds)
{
    std::vector<QJsonObject> out;
    const QDateTime now = QDateTime::currentDateTime();
    for (const QJsonValue& v : ledgerRows()) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("state")).toString() != QLatin1String("IN_PROGRESS"))
            continue;
        const QString journal = r.value(QStringLiteral("journal")).toString();
        if (journalState(journal, nullptr) != QLatin1String("IN_PROGRESS"))
            continue;
        // A journal that was written a moment ago belongs to an apply that is
        // most likely still running (in the standalone PS5 Backport GUI).
        const QDateTime written = QFileInfo(journal).lastModified();
        if (quietSeconds > 0 && written.isValid() && written.secsTo(now) < quietSeconds)
            continue;
        out.push_back(r);
    }
    return out;
}

std::vector<QJsonObject> interruptedApplies()
{
    // The ledger cannot tell an interrupted apply from one in progress in
    // another program, so nothing is offered while that program runs.
    if (foreignEngineRunning())
        return {};
    return unfinishedApplies(kApplyQuietSeconds);
}

// ---- settings ------------------------------------------------------------------------

Settings loadSettings(SettingsFile* state, QString* error)
{
    Settings s;
    const QJsonObject o = readSettingsObject(state, error);
    for (const QJsonValue& v : o.value(QStringLiteral("scan_roots")).toArray())
        if (v.isString() && !v.toString().trimmed().isEmpty())
            s.scanRoots << QDir::toNativeSeparators(v.toString());
    if (o.value(QStringLiteral("output_dir")).isString())
        s.outputDir = QDir::toNativeSeparators(QDir::cleanPath(o.value(QStringLiteral("output_dir")).toString()));
    if (o.value(QStringLiteral("backup_dir")).isString())
        s.backupDir = QDir::toNativeSeparators(QDir::cleanPath(o.value(QStringLiteral("backup_dir")).toString()));
    if (s.outputDir == QLatin1String("."))
        s.outputDir.clear();
    if (s.backupDir == QLatin1String("."))
        s.backupDir.clear();
    if (o.value(QStringLiteral("scan_depth")).isDouble())
        s.scanDepth = std::clamp(o.value(QStringLiteral("scan_depth")).toInt(6), 1, 12);
    return s;
}

bool saveSettings(const Settings& s, int keys, QString* error)
{
    QString err;
    if (!ensureFolders(&err)) {
        if (error)
            *error = err;
        return false;
    }
    // The file is shared with the standalone program: start from what it holds
    // right now and change only the asked keys, so nothing that program wrote
    // since (or a key this app does not know) is lost.
    SettingsFile state = SettingsFile::Missing;
    QJsonObject o = readSettingsObject(&state, &err);
    if (state == SettingsFile::Unreadable) {
        if (error)
            *error = FW_TR("%1 cannot be read (%2); it was left as it is")
                         .arg(QDir::toNativeSeparators(folders().settingsPath), err);
        return false;
    }
    if (keys & ScanRootsKey) {
        QJsonArray roots;
        for (const QString& r : s.scanRoots)
            roots.append(QDir::toNativeSeparators(r));
        o.insert(QStringLiteral("scan_roots"), roots);
    }
    if (keys & OutputDirKey)
        o.insert(QStringLiteral("output_dir"), nativeFolder(s.outputDir));
    if (keys & BackupDirKey)
        o.insert(QStringLiteral("backup_dir"), nativeFolder(s.backupDir));
    if (keys & ScanDepthKey)
        o.insert(QStringLiteral("scan_depth"), std::clamp(s.scanDepth, 1, 12));
    for (const auto& [k, v] : std::initializer_list<std::pair<const char*, QJsonValue>>{
             {"scan_roots", QJsonArray()}, {"output_dir", QString()}, {"backup_dir", QString()}, {"scan_depth", 6},
             {"klog_ip", QString()}, {"klog_port", 3232}, {"klog_reconnect_ms", 1000}, {"klog_max_lines", 5000}, {"geometry", QString()}})
        if (!o.contains(QLatin1String(k)))
            o.insert(QLatin1String(k), v);
    const auto st = writeFileAtomic(folders().settingsPath, QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!st.ok() && error)
        *error = st.error().message;
    return st.ok();
}

bool saveSettings(const Settings& s) { return saveSettings(s, AllSettingsKeys, nullptr); }

// ---- preflight -----------------------------------------------------------------------

namespace {
qint64 freeBytes(QString p)
{
    QFileInfo fi(p);
    while (!fi.exists()) {
        const QString parent = fi.absolutePath();
        if (parent == fi.absoluteFilePath() || parent.isEmpty())
            return 0;
        fi = QFileInfo(parent);
    }
    return QStorageInfo(fi.absoluteFilePath()).bytesAvailable();
}
QString gb(qint64 b) { return QString::number(double(b) / 1e9, 'f', 1); }
} // namespace

Preflight preflight(const QString& output, const QString& backup, const QStringList& games, const std::atomic_bool* cancel)
{
    Preflight r;
    qint64 est = 0, largest = 0;
    for (const QString& g : games) {
        const qint64 b = executableBytes(g, cancel);
        est += b;
        largest = std::max(largest, b);
    }
    const qint64 needOut = qint64(double(est) * 2.2) + 50LL * 1024 * 1024;
    const qint64 needBak = qint64(double(est) * 1.1) + 20LL * 1024 * 1024;
    const QString home = engineHome();
    // One whole sentence per folder and problem (the texts are shown and translated).
    struct Dest {
        QString dir;
        qint64 need;
        QString notSet, notFull, insideGame, gameInside, insideEngine, cannotCreate, cannotWrite, noSpace, lowSpace;
    };
    const Dest dests[] = {
        {output, needOut, FW_TR("Backport output folder is not set."), FW_TR("Backport output folder must be a full path: %1"),
         FW_TR("The backport output folder is inside the game folder %1."),
         FW_TR("The game folder %1 is inside the backport output folder."),
         FW_TR("The backport output folder is inside the program's engine folder."),
         FW_TR("Cannot write to the backport output folder %1: the folder cannot be created"),
         FW_TR("Cannot write to the backport output folder %1: %2"),
         FW_TR("Not enough free space for the backport output folder: about %1 GB needed, %2 GB free."),
         FW_TR("Free space on the backport output drive is low (%1 GB).")},
        {backup, needBak, FW_TR("Originals backup folder is not set."), FW_TR("Originals backup folder must be a full path: %1"),
         FW_TR("The originals backup folder is inside the game folder %1."),
         FW_TR("The game folder %1 is inside the originals backup folder."),
         FW_TR("The originals backup folder is inside the program's engine folder."),
         FW_TR("Cannot write to the originals backup folder %1: the folder cannot be created"),
         FW_TR("Cannot write to the originals backup folder %1: %2"),
         FW_TR("Not enough free space for the originals backup folder: about %1 GB needed, %2 GB free."),
         FW_TR("Free space on the originals backup drive is low (%1 GB).")},
    };
    for (const Dest& t : dests) {
        if (t.dir.isEmpty())
            r.errors << t.notSet;
        else if (QDir::isRelativePath(t.dir))
            r.errors << t.notFull.arg(t.dir);
    }
    if (!r.errors.isEmpty())
        return r;
    if (within(output, backup) || within(backup, output))
        r.errors << FW_TR("The backport output folder and the originals backup folder must be separate (not the same "
                          "folder and not inside each other).");
    for (const QString& g : games) {
        for (const Dest& t : dests) {
            if (within(t.dir, g))
                r.errors << t.insideGame.arg(g);
            if (within(g, t.dir))
                r.errors << t.gameInside.arg(g);
        }
    }
    for (const Dest& t : dests)
        if (within(t.dir, home))
            r.errors << t.insideEngine;
    if (!r.errors.isEmpty())
        return r;
    for (const Dest& t : dests) {
        const QString& d = t.dir;
        if (!QDir().mkpath(d)) {
            r.errors << t.cannotCreate.arg(d);
            continue;
        }
        const QString probe = QDir(d).filePath(QStringLiteral(".d97_write_test_%1").arg(QCoreApplication::applicationPid()));
        QFile pf(probe);
        if (!pf.open(QIODevice::WriteOnly) || pf.write("ok") != 2) {
            r.errors << t.cannotWrite.arg(d, pf.errorString());
            pf.close();
            QFile::remove(probe); // it may have been created before the write failed
            continue;
        }
        pf.close();
        QFile::remove(probe);
        const qint64 free = freeBytes(d);
        if (free < t.need)
            r.errors << t.noSpace.arg(gb(t.need), gb(free));
        else if (free < t.need * 2)
            r.warnings << t.lowSpace.arg(gb(free));
    }
    const qint64 engineFree = freeBytes(home);
    if (engineFree > 0 && engineFree < largest * 6 + 500LL * 1024 * 1024) {
        const QString root = QStorageInfo(QFileInfo(home).exists() ? home : QFileInfo(home).absolutePath()).rootPath();
        QString drive = QDir::toNativeSeparators(root);
        while (drive.endsWith(u'\\'))
            drive.chop(1);
        r.errors << FW_TR("Not enough free space for the backport engine's working files on %1 (%2 GB free).")
                        .arg(drive.isEmpty() ? QDir::toNativeSeparators(home) : drive, gb(engineFree));
    }
    for (const QString& g : games)
        if (!QFileInfo(QDir(g).filePath(QStringLiteral("eboot.bin"))).isFile())
            r.errors << FW_TR("%1 is no longer a game folder (no eboot.bin).").arg(g);
    return r;
}

// ---- AMPR / PlayGo -------------------------------------------------------------------

const std::vector<Variant>& variants()
{
    static const std::vector<Variant> v = {
        {QStringLiteral("82484486"), QStringLiteral("ampr"), FW_TR_NOOP("Known-good AMPR (82484486)"),
         FW_TR_NOOP("The AMPR the backport engine adds to Unity games that have none (BLUE REFLECTION, Trails through "
                        "Daybreak II and others confirmed on base 4.50). Try it first for Unity games."),
         QStringLiteral("experimental_outputs/known_good_fakelibs/libSceAmpr.sprx (pinned control)"),
         FW_TR_NOOP("unknown (community fakelib)"), 218678},
        {QStringLiteral("0aadd31d"), QStringLiteral("ampr"), QStringLiteral("Unreal Engine AMPR (0aadd31d)"),
         FW_TR_NOOP("The AMPR shipped with South of Midnight V3 and Borderlands 4 V2, both confirmed on base 4.50. Try it "
                        "for Unreal Engine games that load no content or play no audio (apr_resolve_vnode error 25 in the klog)."),
         QStringLiteral("FW 4xx PPSA29642 South of Midnight (01.001.000) backport files.zip"),
         FW_TR_NOOP("unknown (community fakelib)"), 303878},
        {QStringLiteral("2feef6a2"), QStringLiteral("ampr"), QStringLiteral("drakmor ampr_emu 0.4.2.1 AMPR (2feef6a2)"),
         FW_TR_NOOP("drakmor's AMPR emulator, release 0.4.2.1 (folder variant). Fixed DAVE THE DIVER's start-up crash on "
                        "base 4.50 together with its PlayGo (below). Try it when a game stops right after launch with "
                        "0xa0020104 and the dump brought its own fakelib folder."),
         QStringLiteral("https://github.com/drakmor/ampr_emu (release 0.4.2.1)"),
         FW_TR_NOOP("GPL-3.0 (drakmor/ampr_emu); redistributed unchanged"), 325296},
        {QStringLiteral("a0f20f40"), QStringLiteral("playgo"), FW_TR_NOOP("Known-good PlayGo (a0f20f40)"),
         FW_TR_NOOP("The PlayGo stub used in eight delivered backports (Unity and others; Street Fighter 6 Year 4 DLC on "
                        "Pro 13.20 among them). Try it first."),
         QStringLiteral("experimental_outputs/known_good_fakelibs/libScePlayGo.sprx (pinned control)"),
         FW_TR_NOOP("unknown (community fakelib)"), 11590},
        {QStringLiteral("f5fdb0ed"), QStringLiteral("playgo"), QStringLiteral("drakmor ampr_emu 0.4.2.1 PlayGo (f5fdb0ed)"),
         FW_TR_NOOP("The PlayGo that ships with drakmor's ampr_emu 0.4.2.1. Use it together with that AMPR (DAVE THE "
                        "DIVER, base 4.50)."),
         QStringLiteral("https://github.com/drakmor/ampr_emu (release 0.4.2.1)"),
         FW_TR_NOOP("GPL-3.0 (drakmor/ampr_emu); redistributed unchanged"), 18150},
    };
    return v;
}

QString shortLabel(const QString& label, bool userFile, const QString& sha8)
{
    if (userFile)
        return QStringLiteral("file %1").arg(sha8);
    QString s = label;
    const int paren = s.indexOf(QStringLiteral(" ("));
    QString hash;
    if (paren >= 0) {
        hash = s.mid(paren + 2).chopped(s.endsWith(u')') ? 1 : 0);
        s = s.left(paren);
    }
    s.replace(QStringLiteral("Known-good AMPR"), QStringLiteral("known-good"));
    s.replace(QStringLiteral("Known-good PlayGo"), QStringLiteral("known-good"));
    s.remove(QStringLiteral(" AMPR"));
    s.remove(QStringLiteral(" PlayGo"));
    s.replace(QStringLiteral("Unreal Engine"), QStringLiteral("UE"));
    if (s.contains(QStringLiteral("known-good")) || s.startsWith(QStringLiteral("UE")))
        s += u' ' + hash;
    return s.trimmed();
}

UserLibrary checkUserLibrary(const QString& path, const QString& kind)
{
    UserLibrary u;
    const QString name = QFileInfo(path).fileName();
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        u.error = FW_TR("%1: cannot be read (%2)").arg(name, f.errorString());
        return u;
    }
    // Every known AMPR / PlayGo library is well below 1 MiB; anything far
    // larger is not one, and is not read into memory to find that out.
    if (f.size() > 32LL * 1024 * 1024) {
        u.error = FW_TR("%1 is not a signed PS5 library (.sprx)").arg(name);
        return u;
    }
    const QByteArray data = f.readAll();
    if (data.startsWith(QByteArray("\x7f" "ELF", 4))) {
        u.error = FW_TR("%1 is a plain ELF, not a signed library. A decoded copy (for example one downloaded from a "
                                 "console over FTP) does not load on the console. Use the signed .sprx from the library's own release.")
                      .arg(name);
        return u;
    }
    if (!data.startsWith(QByteArray("\x4f\x15\x3d\x1d", 4)) && !data.startsWith(QByteArray("\x54\x14\xf5\xee", 4))) {
        u.error = FW_TR("%1 is not a signed PS5 library (.sprx)").arg(name);
        return u;
    }
    if (data.size() < 64) {
        u.error = FW_TR("%1 is too small to be a library").arg(name);
        return u;
    }
    const QString sha = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    u.sha8 = sha.left(8);
    for (const Variant& v : variants()) {
        if (v.id == u.sha8 && v.size == data.size()) {
            if (v.kind != kind) {
                const QString theirs = v.kind == QLatin1String("ampr") ? QStringLiteral("AMPR") : QStringLiteral("PlayGo");
                const QString want = kind == QLatin1String("ampr") ? QStringLiteral("AMPR") : QStringLiteral("PlayGo");
                u.error = FW_TR("%1 is the bundled %2 library %3, not a %4 library").arg(name, theirs, i18n::translate(v.label), want);
                return u;
            }
            u.ok = true;
            u.label = i18n::translate(v.label);
            return u;
        }
    }
    u.ok = true;
    u.label = FW_TR("your file %1 (%2)").arg(name, u.sha8);
    return u;
}

// ---- support ZIP ---------------------------------------------------------------------

namespace {
QByteArray redact(QByteArray text)
{
    const QString home = QDir::homePath();
    if (home.size() < 4)
        return text;
    QString s = QString::fromUtf8(text);
    const QString native = QDir::toNativeSeparators(home);
    QString jsonEscaped = native;
    jsonEscaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    for (const QString& v : {jsonEscaped, native, QDir::fromNativeSeparators(home)})
        s.replace(v, QStringLiteral("~"), Qt::CaseInsensitive);
    return s.toUtf8();
}
bool isTextFile(const QString& name)
{
    const QString suf = QFileInfo(name).suffix().toLower();
    return suf.isEmpty() || suf == QLatin1String("json") || suf == QLatin1String("log") || suf == QLatin1String("txt")
        || suf == QLatin1String("md") || suf == QLatin1String("csv") || suf == QLatin1String("py");
}
} // namespace

QString makeSupportZip(const QString& note, const QString& engineRevision, const QStringList& hostLogFiles)
{
    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desktop.isEmpty())
        desktop = QDir::homePath();
    const QString path = QDir(desktop).filePath(
        QStringLiteral("PS5BackportGUI_logs_%1.zip").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    zip::Writer w;
    auto st = w.open(path, false);
    if (!st.ok())
        return u'!' + st.error().message;
    const Folders f = folders();
    QJsonObject sys{{QStringLiteral("app"), QStringLiteral("exFAT Image Builder (Backport tab)")},
                    {QStringLiteral("engine"), QStringLiteral("PS5 Backport GUI %1").arg(QLatin1String(kEngineVersion))},
                    {QStringLiteral("backend"), engineRevision},
                    {QStringLiteral("platform"), QSysInfo::prettyProductName()},
                    {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                    {QStringLiteral("engine_home"), QStringLiteral("~redacted~")},
                    {QStringLiteral("note"), note}};
    (void)w.addData(QStringLiteral("SYSTEM.json"), QJsonDocument(sys).toJson());
    QJsonObject settings = readJsonObject(f.settingsPath);
    settings.remove(QStringLiteral("klog_ip"));
    (void)w.addData(QStringLiteral("config/settings.json"), redact(QJsonDocument(settings).toJson()));
    if (QFileInfo(f.ledgerPath).isFile()) {
        QFile lf(f.ledgerPath);
        if (lf.open(QIODevice::ReadOnly))
            (void)w.addData(QStringLiteral("config/applied_backports.json"), redact(lf.readAll()));
    }
    auto addFile = [&](const QString& archiveName, const QString& file) {
        const QFileInfo fi(file);
        if (!fi.isFile() || fi.size() > 64LL * 1024 * 1024)
            return;
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly))
            return;
        QByteArray b = in.readAll();
        if (isTextFile(fi.fileName()))
            b = redact(b);
        (void)w.addData(archiveName, b, zip::Method::Deflate, fi.lastModified());
    };
    for (const QFileInfo& fi : QDir(f.logDir).entryInfoList({QStringLiteral("*.log")}, QDir::Files))
        addFile(QStringLiteral("logs/%1").arg(fi.fileName()), fi.absoluteFilePath());
    QFileInfoList runs = QDir(f.runsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    if (runs.size() > 60)
        runs = runs.mid(0, 60);
    for (const QFileInfo& r : runs) {
        QDirIterator it(r.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            addFile(QStringLiteral("logs/runs/%1/%2").arg(r.fileName(), QDir(r.absoluteFilePath()).relativeFilePath(p)), p);
        }
    }
    for (const QString& hl : hostLogFiles)
        addFile(QStringLiteral("host/%1").arg(QFileInfo(hl).fileName()), hl);
    st = w.finish(true);
    if (!st.ok())
        return u'!' + st.error().message;
    return path;
}

void appendAppLog(const QString& level, const QString& message)
{
    const Folders f = folders();
    QDir().mkpath(f.logDir);
    QFile lf(QDir(f.logDir).filePath(QStringLiteral("app_%1.log").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")))));
    // The standalone program appends to the same file: one unbuffered write
    // per line keeps the lines of the two whole.
    if (!lf.open(QIODevice::Append | QIODevice::Text | QIODevice::Unbuffered))
        return;
    const QString line = QStringLiteral("%1 [%2] %3\n").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), level, message);
    lf.write(line.toUtf8());
}

} // namespace fw::bp
