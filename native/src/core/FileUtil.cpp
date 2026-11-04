#include "core/FileUtil.h"

#include "core/Paths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fw {

// ---- formatting --------------------------------------------------------------

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
    return QStringLiteral("%1 %2").arg(QString::number(v, 'f', 1), QLatin1String(units[u]));
}

QString formatRate(double bytesPerSec)
{
    if (bytesPerSec <= 0)
        return QStringLiteral("—");
    const qint64 b = qint64(bytesPerSec);
    if (b < 1024 * 1024)
        return QStringLiteral("%1 KB/s").arg(QString::number(bytesPerSec / 1024.0, 'f', 0));
    if (b < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB/s").arg(QString::number(bytesPerSec / (1024.0 * 1024.0), 'f', 0));
    return QStringLiteral("%1 GB/s").arg(QString::number(bytesPerSec / (1024.0 * 1024.0 * 1024.0), 'f', 1));
}

QString formatTimeLeft(qint64 ms)
{
    if (ms < 0)
        return QString();
    const qint64 s = ms / 1000;
    if (s < 60)
        return QStringLiteral("Less than a minute left");
    const qint64 m = (s + 30) / 60;
    if (m < 60)
        return m == 1 ? QStringLiteral("About 1 min left") : QStringLiteral("About %1 min left").arg(m);
    const qint64 h = m / 60, rm = m % 60;
    if (rm == 0)
        return QStringLiteral("About %1 h left").arg(h);
    return QStringLiteral("About %1 h %2 min left").arg(h).arg(rm);
}

QString formatCount(qint64 n)
{
    QLocale c(QLocale::English, QLocale::UnitedStates);
    return c.toString(n);
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
    static const QRegularExpression bad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    QString out = name;
    out.replace(bad, QStringLiteral("_"));
    while (out.endsWith(u'.') || out.endsWith(u' '))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("_") : out;
}

QString uniquePath(const QString& path)
{
    const auto taken = [](const QString& p) { return QFileInfo::exists(p) || QFileInfo::exists(p + QStringLiteral(".part")); };
    if (!taken(path))
        return path;
    const QFileInfo fi(path);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(suffix));
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

PartFile::PartFile(QString finalPath) : m_final(std::move(finalPath)) {}

PartFile::~PartFile()
{
    if (!m_committed)
        QFile::remove(partPath());
}

Status PartFile::commit(bool replaceExisting)
{
    if (m_committed)
        return success();
    if (QFileInfo::exists(m_final)) {
        if (!replaceExisting)
            return Error(ErrorCode::AlreadyExists,
                         QStringLiteral("%1 already exists. Choose another name or output folder.")
                             .arg(QFileInfo(m_final).fileName()));
        // Set the old file aside first so a failed rename can be undone (audit M2).
        // A name nobody uses, so a user's own "<name>.old" is never touched.
        const QString aside = uniquePath(m_final + QStringLiteral(".fw-old"));
        if (!QFile::rename(m_final, aside))
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("Could not replace %1. Close any program using it and try again.")
                             .arg(QFileInfo(m_final).fileName()));
        if (!QFile::rename(partPath(), m_final)) {
            QFile::rename(aside, m_final);
            return Error(ErrorCode::IoError, QStringLiteral("Could not rename the finished file."),
                         partPath() + QStringLiteral(" -> ") + m_final);
        }
        QFile::remove(aside);
    } else if (!QFile::rename(partPath(), m_final)) {
        return Error(ErrorCode::IoError, QStringLiteral("Could not rename the finished file."),
                     partPath() + QStringLiteral(" -> ") + m_final);
    }
    m_committed = true;
    return success();
}

void PartFile::abandon()
{
    QFile::remove(partPath());
}

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
            return;
        }
    }
}

TempDir::~TempDir()
{
    if (!m_keep && !m_path.isEmpty())
        QDir(m_path).removeRecursively();
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

Status ensureFreeSpace(const QString& path, qint64 requiredBytes, qint64 marginBytes)
{
    auto ds = diskSpace(path);
    if (!ds.ok())
        return ds.takeError();
    const qint64 need = requiredBytes + marginBytes;
    if (ds->available >= need)
        return success();
    QString drive = QDir::toNativeSeparators(ds->rootPath);
    if (drive.endsWith(u'\\') && drive.size() > 1)
        drive.chop(1);
    return Error(ErrorCode::InsufficientSpace,
                 QStringLiteral("Not enough space on %1. Free %2 or choose another output folder.")
                     .arg(drive, formatBytes(need - ds->available)),
                 QStringLiteral("need %1 bytes, available %2").arg(need).arg(ds->available));
}

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

Result<QStringList> checkOutputVolume(const QString& outputFolder, qint64 largestFileBytes)
{
    const VolumeInfo v = volumeInfo(outputFolder);
    const QString fs = v.fileSystem.toUpper();
    QString drive = QDir::toNativeSeparators(v.rootPath);
    if (drive.endsWith(u'\\') && drive.size() > 1)
        drive.chop(1);
    constexpr qint64 k4GiB = 4LL * 1024 * 1024 * 1024;
    if ((fs == u"FAT32" || fs == u"FAT" || fs == u"VFAT" || fs == u"MSDOS") && largestFileBytes >= k4GiB - 1)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 is formatted as FAT32, which cannot hold files of 4 GB or more. Choose an NTFS or exFAT drive.").arg(drive),
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
                         ProgressReporter* progress, bool replaceExisting)
{
    QFile in(toLongPath(src));
    if (!in.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QFileInfo(src).fileName()),
                     in.errorString());
    QDir().mkpath(QFileInfo(dst).absolutePath());
    PartFile part(dst);
    QFile out(toLongPath(part.partPath()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
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
    if (!out.flush())
        return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QFileInfo(dst).fileName()),
                     out.errorString());
    out.setFileTime(QFileInfo(src).lastModified(), QFileDevice::FileModificationTime);
    out.close();
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
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(a), fa.errorString());
    if (!fb.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(b), fb.errorString());
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
    QDirIterator::IteratorFlags flags = QDirIterator::Subdirectories;
    if (options.followSymlinks)
        flags |= QDirIterator::FollowSymlinks;

    QDirIterator it(tree.root, filters, flags);
    const QDir base(tree.root);
    qint64 n = 0;
    while (it.hasNext()) {
        it.next();
        if ((++n & 0x3FF) == 0 && !token.checkpoint())
            return Error::cancelled();
        const QFileInfo fi = it.fileInfo();
        if (fi.isSymLink() && !options.followSymlinks)
            continue;
        const QString rel = base.relativeFilePath(fi.absoluteFilePath());
        if (!options.excludes.isEmpty() && matchesExclude(rel, options.excludes))
            continue;
        FileEntry e;
        e.relativePath = rel;
        e.isDir = fi.isDir();
        e.size = e.isDir ? 0 : fi.size();
        e.modified = fi.lastModified();
        if (e.isDir)
            ++tree.dirCount;
        else {
            ++tree.fileCount;
            tree.totalBytes += e.size;
        }
        tree.entries.push_back(std::move(e));
    }
    // Excluded directories: drop their children too.
    if (!options.excludes.isEmpty()) {
        // matchesExclude already handles "dir/" prefixes for literal excludes.
    }
    std::sort(tree.entries.begin(), tree.entries.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.relativePath.compare(b.relativePath, Qt::CaseSensitive) < 0;
    });
    return tree;
}

Status removeTreeSafely(const QString& path, const QString& mustBeInside)
{
    const QFileInfo fi(path);
    if (!fi.exists())
        return success();
    const QString abs = QDir::cleanPath(fi.absoluteFilePath());
    const QString home = QDir::cleanPath(QDir::homePath());
    const bool isRoot = QDir(abs).isRoot() || abs.size() <= 3;
    if (isRoot || abs.compare(home, Qt::CaseInsensitive) == 0)
        return Error(ErrorCode::PermissionDenied, QStringLiteral("Refused to delete %1.").arg(QDir::toNativeSeparators(abs)));
    if (!mustBeInside.isEmpty() && !isSameOrInside(abs, mustBeInside))
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("Refused to delete %1: it is outside %2.")
                         .arg(QDir::toNativeSeparators(abs), QDir::toNativeSeparators(mustBeInside)));
    if (fi.isDir()) {
        if (!QDir(abs).removeRecursively())
            return Error(ErrorCode::IoError, QStringLiteral("Some files in %1 could not be deleted.").arg(QDir::toNativeSeparators(abs)));
    } else if (!QFile::remove(abs)) {
        return Error(ErrorCode::IoError, QStringLiteral("Could not delete %1.").arg(QDir::toNativeSeparators(abs)));
    }
    return success();
}

} // namespace fw
