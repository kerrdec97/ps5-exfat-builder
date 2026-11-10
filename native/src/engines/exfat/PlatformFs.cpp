#include "engines/exfat/PlatformFs.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatFormat.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fw::exfat {

Status commitOrKeep(PartFile& part, bool replaceExisting, const QString& what)
{
    Status st = part.commit(replaceExisting);
    if (st.ok())
        return st;
    const QFileInfo fi(part.finalPath());
    const QString suffix = fi.suffix().isEmpty() ? QString() : u'.' + fi.suffix();
    const QString kept = uniquePath(fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(" (new)") + suffix));
    if (!QFileInfo::exists(part.partPath()) || !QFile::rename(part.partPath(), kept))
        return st; // nothing could be saved: the plain error stands
    Error e = st.error();
    e.detail = e.detail.isEmpty() ? e.message : e.message + u' ' + e.detail;
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

QString keepStagedFolder(const QString& stage, const QString& output)
{
    const QString kept = uniquePath(output + QStringLiteral(".new"));
    return QDir().rename(stage, kept) ? kept : stage;
}

bool limitsFilesTo4GiB(const QString& fileSystemName)
{
    const QString fs = fileSystemName.toLower();
    return fs == u"vfat" || fs == u"msdos" || fs.startsWith(u"fat");
}

bool tooLargeForDrive(const QString& folder, qint64 bytes)
{
    if (bytes < 4 * kGiB)
        return false;
    const auto ds = diskSpace(folder);
    return ds.ok() && limitsFilesTo4GiB(ds->fileSystem);
}

Error fileError(const QString& userMessage, const QFileDevice& file)
{
    bool full = file.error() == QFileDevice::ResourceError;
#ifdef Q_OS_WIN
    // Qt reports a failed WriteFile as a plain write error on Windows; the
    // system's own text for "disk full" tells the two apart in any language.
    if (!full && file.error() == QFileDevice::WriteError)
        full = file.errorString() == qt_error_string(ERROR_DISK_FULL)
               || file.errorString() == qt_error_string(ERROR_HANDLE_DISK_FULL);
#endif
    if (full)
        return Error(ErrorCode::InsufficientSpace,
                     QStringLiteral("The drive ran out of space. Free some space or choose another folder, then try again."),
                     userMessage + QStringLiteral(" ") + file.errorString());
    return Error(ErrorCode::IoError, userMessage, file.errorString());
}

Status syncFile(QFile& file, const QString& displayName)
{
    if (!file.flush())
        return fileError(QStringLiteral("Could not write %1.").arg(displayName), file);
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

#ifdef Q_OS_WIN
static FILETIME toFileTime(const QDateTime& t)
{
    // 100 ns ticks since 1601-01-01 UTC.
    const quint64 ticks = quint64(t.toMSecsSinceEpoch() + 11644473600000LL) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = DWORD(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = DWORD(ticks >> 32);
    return ft;
}
#endif

bool setPathTimes(const QString& path, const QDateTime& created, const QDateTime& modified, const QDateTime& accessed)
{
#ifdef Q_OS_WIN
    const QString lp = toLongPath(path);
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(lp.utf16()), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FILETIME c{}, a{}, m{};
    if (created.isValid())
        c = toFileTime(created);
    if (accessed.isValid())
        a = toFileTime(accessed);
    if (modified.isValid())
        m = toFileTime(modified);
    const BOOL ok = SetFileTime(h, created.isValid() ? &c : nullptr, accessed.isValid() ? &a : nullptr,
                                modified.isValid() ? &m : nullptr);
    CloseHandle(h);
    return ok != 0;
#else
    Q_UNUSED(created);
    struct timespec ts[2];
    const auto fill = [](struct timespec& t, const QDateTime& dt) {
        if (!dt.isValid()) {
            t.tv_sec = 0;
            t.tv_nsec = UTIME_OMIT;
            return;
        }
        const qint64 ms = dt.toMSecsSinceEpoch();
        qint64 sec = ms / 1000;
        qint64 rem = ms % 1000;
        if (rem < 0) {
            rem += 1000;
            --sec;
        }
        t.tv_sec = time_t(sec);
        t.tv_nsec = long(rem * 1000000);
    };
    fill(ts[0], accessed);
    fill(ts[1], modified);
    const QByteArray p = QFile::encodeName(path);
    return ::utimensat(AT_FDCWD, p.constData(), ts, 0) == 0;
#endif
}

bool applyAttributes(const QString& path, quint16 attributes, bool isDir)
{
#ifdef Q_OS_WIN
    const QString lp = toLongPath(path);
    DWORD a = attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    if (isDir)
        a |= FILE_ATTRIBUTE_DIRECTORY;
    if (a == 0)
        a = FILE_ATTRIBUTE_NORMAL;
    return SetFileAttributesW(reinterpret_cast<const wchar_t*>(lp.utf16()), a) != 0;
#else
    if (isDir || !(attributes & kAttrReadOnly))
        return true;
    QFile f(path);
    const auto perms = f.permissions() & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser | QFileDevice::WriteGroup
                                           | QFileDevice::WriteOther);
    return f.setPermissions(perms);
#endif
}

Result<std::unique_ptr<QFile>> openForRead(const QString& path, int retries, int waitMs, const CancellationToken& token)
{
    auto f = std::make_unique<QFile>(toLongPath(path));
    for (int attempt = 0;; ++attempt) {
        if (f->open(QIODevice::ReadOnly))
            return f;
        if (attempt >= retries || !QFileInfo::exists(path))
            break;
        if (!token.sleepFor(waitMs))
            return Error::cancelled();
    }
    if (!QFileInfo::exists(path))
        return Error(ErrorCode::NotFound, QStringLiteral("%1 was removed while the image was being built.").arg(QDir::toNativeSeparators(path)),
                     f->errorString());
    return Error(ErrorCode::IoError,
                 QStringLiteral("Could not open %1. Close any program using it (antivirus, Windows Search) and try again.")
                     .arg(QDir::toNativeSeparators(path)),
                 f->errorString());
}

} // namespace fw::exfat
