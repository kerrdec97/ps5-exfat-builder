#include "core/FileUtil.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace fw {

// ---- formatting --------------------------------------------------------------

namespace {
// The interface language's way of writing numbers ("441,1 GB" and "18.442" in
// German), in Latin digits only. English, the pseudo-languages and a language
// whose numbers are written in other digits (Arabic, Persian, Bengali...) get
// none: they keep the English form. Sizes and rates have no group separator,
// as in English ("1023.9 KB"); counts have one ("18,442").
struct NumberFormat {
    QLocale plain;   // sizes and rates
    QLocale grouped; // counts
};

const NumberFormat* uiNumbers()
{
    thread_local QString code = QStringLiteral("en");
    thread_local std::optional<NumberFormat> format;
    const QString current = i18n::currentCode();
    if (current != code) {
        code = current;
        format.reset();
        const QLocale locale(current);
        if (!current.startsWith(QLatin1String("qps")) && locale.language() != QLocale::C
            && locale.language() != QLocale::English && locale.zeroDigit() == QLatin1String("0")) {
            NumberFormat f{locale, locale};
            f.plain.setNumberOptions(QLocale::OmitGroupSeparator);
            f.grouped.setNumberOptions(QLocale::DefaultNumberOptions);
            format = f;
        }
    }
    return format ? &*format : nullptr;
}

// `v` with `decimals` decimals, no group separator.
QString localNumber(double v, int decimals)
{
    if (const NumberFormat* f = uiNumbers())
        return f->plain.toString(v, 'f', decimals);
    return QString::number(v, 'f', decimals);
}
} // namespace

QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        bytes = 0;
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QStringLiteral("%1 B").arg(bytes);
    // The unit is chosen from the rounded value: 1,048,575 bytes is "1.0 MB",
    // not "1024.0 KB".
    if (u < 5 && v >= 1023.95) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(localNumber(v, 1), QLatin1String(units[u]));
}

QString formatRate(double bytesPerSec)
{
    if (bytesPerSec <= 0)
        return QStringLiteral("—");
    // As above: what rounds to 1024 of a unit is 1 of the next one.
    const double kb = bytesPerSec / 1024.0;
    if (kb < 0.5)
        return QStringLiteral("<1 KB/s"); // would print as "0 KB/s"
    if (kb < 1023.5)
        return QStringLiteral("%1 KB/s").arg(localNumber(kb, 0));
    const double mb = kb / 1024.0;
    if (mb < 1023.5)
        return QStringLiteral("%1 MB/s").arg(localNumber(mb, 0));
    return QStringLiteral("%1 GB/s").arg(localNumber(mb / 1024.0, 1));
}

QString formatTimeLeft(qint64 ms)
{
    if (ms < 0)
        return QString();
    const qint64 s = ms / 1000;
    if (s < 60)
        return FW_TR("Less than a minute left");
    const qint64 m = (s + 30) / 60;
    if (m < 60)
        return m == 1 ? FW_TR("About 1 min left") : FW_TR("About %1 min left").arg(m);
    const qint64 h = m / 60, rm = m % 60;
    if (rm == 0)
        return FW_TR("About %1 h left").arg(h);
    return FW_TR("About %1 h %2 min left").arg(h).arg(rm);
}

QString formatCount(qint64 n)
{
    if (const NumberFormat* f = uiNumbers())
        return f->grouped.toString(n);
    static const QLocale english(QLocale::English, QLocale::UnitedStates);
    return english.toString(n);
}

// ---- paths -------------------------------------------------------------------

QString toLongPath(const QString& path)
{
#ifdef Q_OS_WIN
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.size() < 248 || native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
#else
    return path;
#endif
}

bool isSameOrInside(const QString& child, const QString& parent)
{
    QString c = QDir::cleanPath(QFileInfo(child).absoluteFilePath());
    QString p = QDir::cleanPath(QFileInfo(parent).absoluteFilePath());
#ifdef Q_OS_WIN
    c = c.toLower();
    p = p.toLower();
#endif
    if (c == p)
        return true;
    if (!p.endsWith(u'/'))
        p += u'/';
    return c.startsWith(p);
}

QString sanitizeFileName(const QString& name)
{
    // Almost every name needs nothing: told by one scan, which costs a small
    // part of what the expression below does (every segment of every name
    // in an archive's list comes here).
    const auto forbidden = [](QChar ch) {
        const char16_t c = ch.unicode();
        return c < 0x20 || c == u'<' || c == u'>' || c == u':' || c == u'"' || c == u'/' || c == u'\\' || c == u'|'
            || c == u'?' || c == u'*';
    };
    if (!name.isEmpty() && !name.endsWith(u'.') && !name.endsWith(u' ') && std::none_of(name.cbegin(), name.cend(), forbidden))
        return name;
    static const QRegularExpression bad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    QString out = name;
    out.replace(bad, QStringLiteral("_"));
    while (out.endsWith(u'.') || out.endsWith(u' '))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("_") : out;
}

QString uniquePath(const QString& path)
{
    const auto taken = [](const QString& p) {
        return QFileInfo::exists(p) || QFileInfo::exists(p + QStringLiteral(".part")) || QFileInfo::exists(PartFile::partPathFor(p));
    };
    if (!taken(path))
        return path;
    const QFileInfo fi(path);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    for (int i = 2; i < 10000; ++i) {
        // One call: chained arg() would read a "%2" inside the name as a placeholder.
        const QString candidate = QDir(dir).filePath(QStringLiteral("%1 (%2)%3").arg(base, QString::number(i), suffix));
        if (!taken(candidate))
            return candidate;
    }
    return path;
}

// ---- writes ------------------------------------------------------------------

Status writeFileAtomic(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()),
                     f.errorString());
    if (f.write(data) != data.size()) {
        f.cancelWriting();
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()),
                     f.errorString());
    }
    if (!f.commit())
        return Error(ErrorCode::IoError, QStringLiteral("Could not save %1.").arg(QFileInfo(path).fileName()),
                     f.errorString());
    return success();
}

namespace {
bool existsOrLink(const QString& path)
{
    const QFileInfo fi(path);
    return fi.exists() || fi.isSymLink();
}
} // namespace

QString PartFile::partPathFor(const QString& finalPath)
{
    static const QString suffix = QStringLiteral(".fw.part");
    // NTFS and exFAT allow 255 characters in a name. A name with no room for
    // the suffix keeps its start and a digest of the whole name instead: the
    // working name stays this file's own, so the next run finds its leftover.
    constexpr qsizetype kMaxName = 255;
    const qsizetype slash = std::max(finalPath.lastIndexOf(u'/'), finalPath.lastIndexOf(u'\\'));
    const QStringView name = QStringView(finalPath).sliced(slash + 1);
    if (name.size() + suffix.size() <= kMaxName)
        return finalPath + suffix;
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    qsizetype keep = kMaxName - suffix.size() - 1 - digest.size();
    if (name.at(keep - 1).isHighSurrogate())
        --keep; // never half a character
    return finalPath.left(slash + 1) + name.first(keep) + u'~' + digest + suffix;
}

PartFile::PartFile(QString finalPath) : m_final(std::move(finalPath)), m_part(partPathFor(m_final))
{
    if (existsOrLink(m_part))
        QFile::remove(m_part); // ours by its name: left by an interrupted run
    // Not known to be this app's: left alone (see the header), and named in
    // the log, so a leftover of an older version can be found and deleted.
    const QString plain = m_final + QStringLiteral(".part");
    if (!existsOrLink(plain))
        return;
    const QString theirs = QDir::toNativeSeparators(plain);
    Logger::instance().log(LogLevel::Warning, QStringLiteral("files"),
                           QStringLiteral("%1 was left as it is: it may be another program's file or a leftover of an "
                                          "older version (delete it if you do not need it). Writing %2 instead.")
                               .arg(theirs, QDir::toNativeSeparators(m_part)),
                           {}, {{QStringLiteral("file"), theirs}});
}

PartFile::~PartFile()
{
    if (!m_committed)
        QFile::remove(partPath());
}

namespace {
// Renames a file within its folder. QFile::rename() answers a refused rename
// (another program holds the file) by copying the whole file: minutes for an
// image, not cancellable, and as much disk space again, only to fail anyway.
// This never copies. A scanner or indexer that still has the file open gets a
// moment to let go first: `attempts` tries, 100 ms apart.
bool renameWithoutCopy(const QString& from, const QString& to, QString& why, int attempts = 10)
{
#ifdef Q_OS_WIN
    const std::wstring source = toLongPath(from).toStdWString(), target = toLongPath(to).toStdWString();
    DWORD err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < std::max(1, attempts); ++attempt) {
        if (attempt > 0)
            Sleep(100);
        if (MoveFileExW(source.c_str(), target.c_str(), 0)) // no MOVEFILE_COPY_ALLOWED, no replacing
            return true;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)
            break;
    }
    why = QStringLiteral("MoveFileEx failed (%1): %2 -> %3").arg(QString::number(err), from, to);
    return false;
#else
    Q_UNUSED(attempts);
    if (!QFileInfo::exists(to) && ::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    why = QStringLiteral("rename failed: %1 -> %2").arg(from, to);
    return false;
#endif
}

// Renames `from` over the existing file `to` in one step (NTFS and POSIX
// renames replace atomically): afterwards `to` is the old file or the new one,
// never missing, whatever happens in between. Never copies; retries like
// renameWithoutCopy().
bool replaceWithoutCopy(const QString& from, const QString& to, QString& why)
{
#ifdef Q_OS_WIN
    const std::wstring source = toLongPath(from).toStdWString(), target = toLongPath(to).toStdWString();
    DWORD err = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0)
            Sleep(100);
        if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED && err != ERROR_LOCK_VIOLATION)
            break;
    }
    why = QStringLiteral("MoveFileEx (replace) failed (%1): %2 -> %3").arg(QString::number(err), from, to);
    return false;
#else
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    why = QStringLiteral("rename failed: %1 -> %2").arg(from, to);
    return false;
#endif
}

// True when another program holds `path` so that it cannot be renamed or
// deleted (opened without delete sharing). Tells which of the two files of a
// refused replace is in use.
bool heldOpen(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring p = toLongPath(path).toStdWString();
    HANDLE h = CreateFileW(p.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
#else
    Q_UNUSED(path);
    return false;
#endif
}
} // namespace

bool renameWithoutCopying(const QString& from, const QString& to, QString* why, int attempts)
{
    QString reason;
#ifdef Q_OS_WIN
    // "game.exfat" → "Game.exfat": the name of `to` is the file itself on a
    // case-insensitive folder. MoveFileEx without MOVEFILE_REPLACE_EXISTING
    // still refuses a different file of that name (a case-sensitive folder).
    const QString a = QDir::cleanPath(QFileInfo(from).absoluteFilePath()), b = QDir::cleanPath(QFileInfo(to).absoluteFilePath());
    const bool caseOnly = a != b && a.compare(b, Qt::CaseInsensitive) == 0;
#else
    const bool caseOnly = false;
#endif
    const bool ok = (caseOnly || !existsOrLink(to)) && renameWithoutCopy(from, to, reason, attempts);
    if (why)
        *why = ok ? QString() : reason.isEmpty() ? QStringLiteral("%1 already exists").arg(to) : reason;
    return ok;
}

bool replaceWithoutCopying(const QString& from, const QString& to, QString* why)
{
    QString reason;
    const bool ok = replaceWithoutCopy(from, to, reason);
    if (why)
        *why = ok ? QString() : reason;
    return ok;
}

Status commitFinishedFile(const QString& partPath, const QString& finalPath, bool replaceExisting)
{
    QString why;
    const QString name = QFileInfo(finalPath).fileName();
    const auto renameFailed = [&] {
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not give %1 its final name. Close any program that has it or its .part file "
                                    "open and try again.")
                         .arg(name),
                     why);
    };
    const auto replaceFailed = [&] {
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("Could not replace %1. Close any program using it and try again.").arg(name), why);
    };
    const QFileInfo existing(finalPath);
    if (existing.exists() || existing.isSymLink()) {
        if (!replaceExisting)
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Choose another name or folder.").arg(name));
        if (existing.isDir() && !existing.isSymLink())
            return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 is a folder. Choose another name or folder.").arg(name));
        // A read-only file is not replaced by a rename: the flag goes for
        // the replace and comes back if it fails. Nothing is set aside under
        // another name (a power cut in between would leave the name empty).
        const bool readOnly = !existing.isWritable() && !existing.isSymLink();
        const QFileDevice::Permissions before = existing.permissions();
        if (readOnly)
            QFile::setPermissions(finalPath, before | QFileDevice::WriteOwner | QFileDevice::WriteUser);
        // One step: a power cut cannot leave the name empty (audit M2).
        if (!replaceWithoutCopy(partPath, finalPath, why)) {
            if (readOnly)
                QFile::setPermissions(finalPath, before);
            return heldOpen(partPath) && !heldOpen(finalPath) ? renameFailed() : replaceFailed();
        }
    } else if (!renameWithoutCopy(partPath, finalPath, why)) {
        return renameFailed();
    }
    return success();
}

Status PartFile::commit(bool replaceExisting)
{
    if (m_committed)
        return success();
    FW_TRY(commitFinishedFile(partPath(), m_final, replaceExisting));
    m_committed = true;
    return success();
}

void PartFile::abandon()
{
    QFile::remove(partPath());
}

Status commitOrKeep(PartFile& part, bool replaceExisting, const QString& what)
{
    Status st = part.commit(replaceExisting);
    if (st.ok())
        return st;
    const QFileInfo fi(part.finalPath());
    const QString suffix = fi.suffix().isEmpty() ? QString() : u'.' + fi.suffix();
    const QString kept = uniquePath(fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(" (new)") + suffix));
    QString ignored;
    if (!QFileInfo::exists(part.partPath()) || !renameWithoutCopy(part.partPath(), kept, ignored))
        return st; // nothing could be saved: the plain error stands
    Error e = st.error();
    e.detail = e.detail.isEmpty() ? e.message : e.message + u' ' + e.detail;
    // The finished work is saved: running the job again would only make
    // another full copy of it, so the queue must not retry this by itself.
    if (e.code == ErrorCode::IoError)
        e.code = ErrorCode::PermissionDenied;
    const QString name = QDir::toNativeSeparators(part.finalPath());
    const QString keptName = QDir::toNativeSeparators(kept);
    if (e.code == ErrorCode::AlreadyExists)
        e.message = QStringLiteral("%1, but %2 already exists. It was saved as %3.").arg(what, name, keptName);
    else if (fi.exists())
        e.message = QStringLiteral("%1, but could not replace %2 because that file is in use. It was saved as %3. "
                                   "Close the program that uses the old file, then rename the new one.")
                        .arg(what, name, keptName);
    else
        e.message = QStringLiteral("%1, but could not be named %2. It was saved as %3.").arg(what, name, keptName);
    return e;
}

namespace {
constexpr char kOwnerMarkerMagic[] = "fw-temp 1\n"; // first line of TempDir's marker file

// When this process started (Windows: its creation time in 100 ns units), or
// 0 when that cannot be told. With the process id it names the owner of a
// temporary folder, also after the id has gone to a later process.
qint64 ownStartTime()
{
#ifdef Q_OS_WIN
    static const qint64 started = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            return qint64(0);
        return qint64((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    }();
    return started;
#else
    return 0;
#endif
}

// The marker's text: the magic line, the owner while a job works in the
// folder ("owner <process id> <start time>"), and a line for whoever finds it.
QByteArray ownerMarkerText(bool inUse)
{
    QByteArray text = kOwnerMarkerMagic;
    if (inUse)
        text += "owner " + QByteArray::number(QCoreApplication::applicationPid()) + ' ' + QByteArray::number(ownStartTime()) + '\n';
    text += "Temporary folder of exFAT Image Builder. It is removed when its job ends.\n";
    return text;
}

// True while process `pid` runs and, when `started` is known, is the process
// that started then rather than a later one that was given the same id.
bool processStillRuns(qint64 pid, qint64 started)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    // Every process of this user, elevated or not, lets itself be opened for
    // limited queries (Task Manager relies on it). One that refuses is not a
    // copy of the app the user started: with the start time on record, the id
    // has gone to another account's process (a service, say), and the folder's
    // owner has ended. Only without a start time does a refusal keep it.
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED && started <= 0;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool timed = GetProcessTimes(h, &created, &exited, &kernel, &user);
    CloseHandle(h);
    if (!running)
        return false;
    return started <= 0 || !timed || qint64((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime) == started;
#else
    Q_UNUSED(started);
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}
} // namespace

TempDir::TempDir(const QString& prefix, const QString& parentDir)
{
    const QString parent = parentDir.isEmpty() ? Paths::tempRoot() : parentDir;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString name = QStringLiteral("fw-%1-%2-%3")
                                 .arg(sanitizeFileName(prefix))
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")))
                                 .arg(QRandomGenerator::global()->generate() & 0xFFFFFF, 6, 16, QLatin1Char('0'));
        const QString p = QDir(parent).filePath(name);
        if (!QFileInfo::exists(p) && QDir().mkpath(p)) {
            m_path = p;
            // Without the marker the folder still works; it is then never
            // swept from a folder the user chose. It names this process: a
            // sweep (of another copy of the app, too) leaves the folder while
            // this process runs.
            QFile marker(QDir(p).filePath(ownerMarkerName()));
            if (marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                marker.write(ownerMarkerText(true));
                marker.close();
            }
            return;
        }
    }
}

QString TempDir::ownerMarkerName()
{
    return QStringLiteral(".fw-temp");
}

bool TempDir::isOwnedFolder(const QString& dir)
{
    const QFileInfo folder(dir);
    if (!folder.isDir() || isJunctionOrSymlink(dir))
        return false;
    const QString markerPath = QDir(dir).filePath(ownerMarkerName());
    if (!QFileInfo(markerPath).isFile() || isJunctionOrSymlink(markerPath))
        return false;
    QFile marker(markerPath);
    return marker.open(QIODevice::ReadOnly) && marker.read(sizeof(kOwnerMarkerMagic) - 1) == kOwnerMarkerMagic;
}

bool TempDir::isInUse(const QString& dir)
{
    QFile marker(QDir(dir).filePath(ownerMarkerName()));
    if (!marker.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = marker.read(512);
    marker.close();
    if (!head.startsWith(kOwnerMarkerMagic))
        return false;
    for (const QByteArray& line : head.split('\n'))
        if (line.startsWith("owner ")) {
            const QList<QByteArray> owner = line.mid(6).simplified().split(' ');
            return processStillRuns(owner.value(0).toLongLong(), owner.value(1).toLongLong());
        }
    return false; // no owner: a kept folder, or one of 5.0.2 and earlier
}

TempDir::~TempDir()
{
    if (m_path.isEmpty())
        return;
    if (!m_keep) {
        removeTreeNoFollow(m_path); // never through a junction left inside
        return;
    }
    // Kept: no job works in it any more, so a later Clear may take it.
    if (isOwnedFolder(m_path)) {
        QFile marker(QDir(m_path).filePath(ownerMarkerName()));
        if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
            marker.write(ownerMarkerText(false));
    }
}

QString TempDir::filePath(const QString& name) const
{
    return QDir(m_path).filePath(name);
}

// ---- disk space --------------------------------------------------------------

Result<DiskSpace> diskSpace(const QString& anyPathOnVolume)
{
    QString probe = QFileInfo(anyPathOnVolume).absoluteFilePath();
    while (!probe.isEmpty() && !QFileInfo::exists(probe)) {
        const QString up = QFileInfo(probe).absolutePath();
        if (up == probe)
            break;
        probe = up;
    }
    QStorageInfo si(probe);
    if (!si.isValid() || !si.isReady())
        return Error(ErrorCode::NotFound, QStringLiteral("The drive for %1 is not available.").arg(anyPathOnVolume));
    DiskSpace d;
    d.available = si.bytesAvailable();
    d.total = si.bytesTotal();
    d.rootPath = si.rootPath();
    d.fileSystem = QString::fromLatin1(si.fileSystemType());
    return d;
}

namespace {
Status ensureFreeSpaceOr(const QString& path, qint64 requiredBytes, qint64 marginBytes, const QString& remedy);
} // namespace

Status ensureFreeSpace(const QString& path, qint64 requiredBytes, qint64 marginBytes)
{
    return ensureFreeSpaceOr(path, requiredBytes, marginBytes, QStringLiteral("choose another output folder"));
}

Status ensureFreeTempSpace(const QString& path, qint64 requiredBytes, qint64 marginBytes)
{
    return ensureFreeSpaceOr(path, requiredBytes, marginBytes,
                             QStringLiteral("choose another temporary folder in Settings > Storage"));
}

namespace {
Status ensureFreeSpaceOr(const QString& path, qint64 requiredBytes, qint64 marginBytes, const QString& remedy)
{
    auto ds = diskSpace(path);
    if (!ds.ok())
        return ds.takeError();
    // Sizes read from an archive or an image can be absurd: no overflow, and a
    // negative size never turns the check off.
    const qint64 required = std::max<qint64>(0, requiredBytes), margin = std::max<qint64>(0, marginBytes);
    const qint64 need = required > std::numeric_limits<qint64>::max() - margin ? std::numeric_limits<qint64>::max()
                                                                                : required + margin;
    if (ds->available >= need)
        return success();
    QString drive = QDir::toNativeSeparators(ds->rootPath);
    if (drive.endsWith(u'\\') && drive.size() > 1)
        drive.chop(1);
    return Error(ErrorCode::InsufficientSpace,
                 QStringLiteral("Not enough space on %1. Free %2 or %3.")
                     .arg(drive, formatBytes(need - ds->available), remedy),
                 QStringLiteral("need %1 bytes, available %2").arg(need).arg(ds->available));
}
} // namespace

// ---- volume checks ---------------------------------------------------------------

VolumeInfo volumeInfo(const QString& anyPathOnVolume)
{
    VolumeInfo v;
    QString probe = QFileInfo(anyPathOnVolume).absoluteFilePath();
    while (!probe.isEmpty() && !QFileInfo::exists(probe)) {
        const QString up = QFileInfo(probe).absolutePath();
        if (up == probe)
            break;
        probe = up;
    }
    const QStorageInfo si(probe);
    v.rootPath = si.rootPath();
    v.fileSystem = QString::fromLatin1(si.fileSystemType());
#ifdef Q_OS_WIN
    const QString root = QDir::toNativeSeparators(si.rootPath());
    if (root.startsWith(QStringLiteral("\\\\"))) {
        v.isNetwork = true;
    } else if (!root.isEmpty()) {
        const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
        v.isNetwork = type == DRIVE_REMOTE;
        v.isRemovable = type == DRIVE_REMOVABLE;
    }
#else
    const QString fs = v.fileSystem.toLower();
    v.isNetwork = fs.startsWith(QStringLiteral("nfs")) || fs == u"cifs" || fs == u"smb3" || fs.startsWith(QStringLiteral("fuse.sshfs"));
#endif
    static const QStringList cloud = {QStringLiteral("onedrive"), QStringLiteral("google drive"), QStringLiteral("googledrive"),
                                      QStringLiteral("dropbox"), QStringLiteral("iclouddrive"), QStringLiteral("icloud drive"),
                                      QStringLiteral("creative cloud")};
    const QString lower = QFileInfo(anyPathOnVolume).absoluteFilePath().toLower();
    v.isCloudSynced = std::any_of(cloud.begin(), cloud.end(), [&](const QString& c) { return lower.contains(c); });
    return v;
}

namespace {
constexpr qint64 k4GiB = 4LL * 1024 * 1024 * 1024; // FAT32 holds files up to 4 GiB - 1
}

bool limitsFilesTo4GiB(const QString& fileSystemName)
{
    const QString fs = fileSystemName.toLower();
    return fs == u"vfat" || fs == u"msdos" || fs.startsWith(u"fat");
}

bool tooLargeForDrive(const QString& folder, qint64 bytes)
{
    if (bytes < k4GiB)
        return false;
    const auto ds = diskSpace(folder);
    return ds.ok() && limitsFilesTo4GiB(ds->fileSystem);
}

Result<QStringList> checkOutputVolume(const QString& outputFolder, qint64 largestFileBytes)
{
    const VolumeInfo v = volumeInfo(outputFolder);
    const QString fs = v.fileSystem.toUpper();
    QString drive = QDir::toNativeSeparators(v.rootPath);
    if (drive.endsWith(u'\\') && drive.size() > 1)
        drive.chop(1);
    if (limitsFilesTo4GiB(fs) && largestFileBytes >= k4GiB)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 is formatted as %2, which cannot hold files of 4 GB or more. Choose an NTFS or exFAT drive.")
                         .arg(drive, fs == u"FAT32" ? QStringLiteral("FAT32") : QStringLiteral("FAT")),
                     QStringLiteral("largest file %1 bytes").arg(largestFileBytes));
    QStringList warnings;
    if (v.isNetwork)
        warnings << QStringLiteral("%1 is a network drive. Building there is much slower and a dropped connection fails the job.").arg(drive);
    if (v.isCloudSynced)
        warnings << QStringLiteral("The output folder is synced to the cloud. Pause syncing while building, or the sync client may lock or upload half-written files.");
    return warnings;
}

// ---- streaming I/O -------------------------------------------------------------

Status copyFileStreaming(const QString& src, const QString& dst, const CancellationToken& token,
                         ProgressReporter* progress, bool replaceExisting, bool durable)
{
    QFile in(toLongPath(src));
    if (!in.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QFileInfo(src).fileName()),
                     in.errorString());
    QDir().mkpath(QFileInfo(dst).absolutePath());
    PartFile part(dst);
    QFile out(toLongPath(part.partPath()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QFileInfo(dst).fileName()),
                     out.errorString());
    QByteArray buf;
    buf.resize(int(kIoChunk));
    while (true) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 n = in.read(buf.data(), buf.size());
        if (n < 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(src).fileName()),
                         in.errorString());
        if (n == 0)
            break;
        if (out.write(buf.constData(), n) != n)
            return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QFileInfo(dst).fileName()),
                         out.errorString());
        if (progress)
            progress->advance(n);
    }
    if (durable) {
        if (Status st = flushToDisk(out, QFileInfo(dst).fileName()); !st.ok())
            return st;
    }
    FW_TRY(finishWrittenFile(out, QFileInfo(dst).fileName(), QFileInfo(src).lastModified()));
    return part.commit(replaceExisting);
}

Result<QString> sha256File(const QString& path, const CancellationToken& token, ProgressReporter* progress)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName()),
                     f.errorString());
    QCryptographicHash h(QCryptographicHash::Sha256);
    QByteArray buf;
    buf.resize(int(kIoChunk));
    while (true) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(path).fileName()),
                         f.errorString());
        if (n == 0)
            break;
        h.addData(QByteArrayView(buf.constData(), n));
        if (progress)
            progress->advance(n);
    }
    return QString::fromLatin1(h.result().toHex());
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

Result<bool> filesEqual(const QString& a, const QString& b, const CancellationToken& token, ProgressReporter* progress)
{
    QFile fa(toLongPath(a)), fb(toLongPath(b));
    if (!fa.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QFileInfo(a).fileName()),
                     a + QStringLiteral(": ") + fa.errorString());
    if (!fb.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QFileInfo(b).fileName()),
                     b + QStringLiteral(": ") + fb.errorString());
    if (fa.size() != fb.size())
        return false;
    QByteArray ba, bb;
    ba.resize(int(kIoChunk));
    bb.resize(int(kIoChunk));
    while (true) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 na = fa.read(ba.data(), ba.size());
        const qint64 nb = fb.read(bb.data(), bb.size());
        if (na < 0 || nb < 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read while comparing files."));
        if (na != nb || std::memcmp(ba.constData(), bb.constData(), size_t(na)) != 0)
            return false;
        if (na == 0)
            return true;
        if (progress)
            progress->advance(na);
    }
}

// ---- directory scans ------------------------------------------------------------

namespace {
bool matchesExclude(const QString& rel, const QStringList& excludes)
{
    const QString name = rel.section(u'/', -1);
    for (const QString& ex : excludes) {
        if (ex.contains(u'*') || ex.contains(u'?')) {
            const auto re = QRegularExpression::fromWildcard(ex, Qt::CaseInsensitive);
            if (re.match(name).hasMatch() || re.match(rel).hasMatch())
                return true;
        } else if (rel.compare(ex, Qt::CaseInsensitive) == 0
                   || rel.startsWith(ex + u'/', Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
} // namespace

bool isJunctionOrSymlink(const QString& path)
{
    const QFileInfo fi(path);
    if (fi.isSymbolicLink())
        return true;
#ifdef Q_OS_WIN
    // Junctions and symbolic links only: cloud placeholders (OneDrive) and
    // deduplicated files are reparse points too, but they are ordinary data.
    WIN32_FIND_DATAW fd;
    const QString native = QDir::toNativeSeparators(toLongPath(path));
    HANDLE h = FindFirstFileW(reinterpret_cast<LPCWSTR>(native.utf16()), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        && (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK);
#else
    return false;
#endif
}

Result<FileTree> scanTree(const QString& root, const CancellationToken& token, const ScanOptions& options)
{
    QFileInfo rootInfo(root);
    if (!rootInfo.exists() || !rootInfo.isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(root)));

    FileTree tree;
    tree.root = rootInfo.absoluteFilePath();
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (options.includeHidden)
        filters |= QDir::Hidden;

    // One listing per folder, and every decision from the listing's own data
    // (no further query per entry). A symbolic link or junction is skipped
    // before anything below it is read (QDirIterator's own recursion walks
    // into junctions), so a link to a large tree, or to one of its own
    // parents, costs nothing. Windows shortcuts (.lnk) are ordinary files.
    // An excluded folder is skipped with everything in it.
    struct Folder {
        QString path;
        QString relPrefix; // "" for the root, else "a/b/"
    };
    std::vector<Folder> pending{{tree.root, QString()}};
    // followSymlinks: each linked folder is entered once (a link may point
    // back at a folder that holds it). Qt's canonical path does not resolve
    // junctions, so the link's own target is what identifies the folder.
    const auto identity = [](const QString& path) {
        const QString real = QFileInfo(path).canonicalFilePath();
#ifdef Q_OS_WIN
        return real.toLower();
#else
        return real;
#endif
    };
    QSet<QString> followed;
    if (options.followSymlinks)
        followed.insert(identity(tree.root));
    qint64 n = 0;
    while (!pending.empty()) {
        const Folder folder = std::move(pending.back());
        pending.pop_back();
        QDirIterator it(folder.path, filters);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            if ((++n & 0x3FF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const bool link = fi.isSymbolicLink() || fi.isJunction();
            if (link && !options.followSymlinks)
                continue;
            const QString rel = folder.relPrefix + fi.fileName();
            if (!options.excludes.isEmpty() && matchesExclude(rel, options.excludes))
                continue;
            FileEntry e;
            e.relativePath = rel;
            e.isDir = fi.isDir();
            e.size = e.isDir ? 0 : fi.size();
            e.modified = fi.lastModified();
            if (e.isDir) {
                ++tree.dirCount;
                bool enter = true;
                if (link) {
                    const QString real = identity(fi.isJunction() ? fi.junctionTarget() : fi.symLinkTarget());
                    enter = !real.isEmpty() && !followed.contains(real);
                    followed.insert(real);
                }
                if (enter)
                    pending.push_back({fi.absoluteFilePath(), rel + u'/'});
            } else {
                ++tree.fileCount;
                tree.totalBytes += e.size;
            }
            tree.entries.push_back(std::move(e));
        }
    }
    std::sort(tree.entries.begin(), tree.entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath.compare(b.relativePath, Qt::CaseSensitive) < 0;
    });
    return tree;
}

bool removeTreeNoFollow(const QString& dir)
{
    if (isJunctionOrSymlink(dir))
        return QFile::remove(dir) || QDir().rmdir(dir); // the link only
    bool ok = true;
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString p = it.next();
        const QFileInfo fi = it.fileInfo();
        if (isJunctionOrSymlink(p)) {
            if (!QFile::remove(p) && !QDir().rmdir(p))
                ok = false;
        } else if (fi.isDir()) {
            if (!removeTreeNoFollow(p))
                ok = false;
        } else if (!QFile::remove(p)) {
            QFile::setPermissions(p, fi.permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
            if (!QFile::remove(p))
                ok = false;
        }
    }
    if (QDir().rmdir(dir))
        return ok;
    // A read-only folder (Explorer sets the attribute on customised folders;
    // extraction keeps it): Windows refuses to remove it until it is cleared.
    QFile::setPermissions(dir, QFileInfo(dir).permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
    return QDir().rmdir(dir) && ok;
}

Status flushToDisk(QFile& file, const QString& displayName)
{
    if (!file.flush())
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(displayName), file.errorString());
#ifdef Q_OS_WIN
    const intptr_t h = _get_osfhandle(file.handle());
    if (h == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(h)))
        return Error(ErrorCode::IoError, QStringLiteral("Could not finish writing %1 to the drive.").arg(displayName),
                     QStringLiteral("FlushFileBuffers failed (%1)").arg(GetLastError()));
#else
    if (::fsync(file.handle()) != 0)
        return Error(ErrorCode::IoError, QStringLiteral("Could not finish writing %1 to the drive.").arg(displayName),
                     QStringLiteral("fsync failed"));
#endif
    return success();
}

Status finishWrittenFile(QFile& file, const QString& displayName, const QDateTime& modified)
{
    const auto failed = [&] {
        const QString why = file.errorString();
        file.close();
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(displayName), why);
    };
    if (!file.flush())
        return failed();
    if (modified.isValid())
        file.setFileTime(modified, QFileDevice::FileModificationTime); // must come after the last write
    file.close(); // keeps the error of a failed close, clears every earlier one
    if (file.error() != QFileDevice::NoError)
        return failed();
    return success();
}

bool isProtectedLocation(const QString& path)
{
    const QString abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (QDir(abs).isRoot() || abs.size() <= 3)
        return true;
    // The home folder and everything it lies in ("C:/Users").
    if (isSameOrInside(QDir::homePath(), abs))
        return true;
    QStringList folders;
    for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
                                QStandardPaths::DownloadLocation, QStandardPaths::PicturesLocation,
                                QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation})
        folders << QStandardPaths::writableLocation(location);
#ifdef Q_OS_WIN
    for (const char* variable : {"SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData"})
        folders << qEnvironmentVariable(variable);
#endif
    return std::any_of(folders.cbegin(), folders.cend(), [&](const QString& folder) {
        return !folder.isEmpty() && isSameOrInside(abs, folder) && isSameOrInside(folder, abs);
    });
}

Status removeTreeSafely(const QString& path, const QString& mustBeInside)
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return success();
    const QString abs = QDir::cleanPath(fi.absoluteFilePath());
    if (isProtectedLocation(abs))
        return Error(ErrorCode::PermissionDenied, QStringLiteral("Refused to delete %1.").arg(QDir::toNativeSeparators(abs)));
    if (!mustBeInside.isEmpty() && !isSameOrInside(abs, mustBeInside))
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("Refused to delete %1: it is outside %2.")
                         .arg(QDir::toNativeSeparators(abs), QDir::toNativeSeparators(mustBeInside)));
    if (fi.isDir() && (fi.isSymLink() || isJunctionOrSymlink(abs))) {
        // A link to a folder: remove the link, never what it points at.
        if (!QFile::remove(abs) && !QDir().rmdir(abs))
            return Error(ErrorCode::IoError, QStringLiteral("Could not delete %1.").arg(QDir::toNativeSeparators(abs)));
    } else if (fi.isDir()) {
        if (!removeTreeNoFollow(abs))
            return Error(ErrorCode::IoError, QStringLiteral("Some files in %1 could not be deleted.").arg(QDir::toNativeSeparators(abs)));
    } else if (!QFile::remove(abs)) {
        return Error(ErrorCode::IoError, QStringLiteral("Could not delete %1.").arg(QDir::toNativeSeparators(abs)));
    }
    return success();
}

} // namespace fw
