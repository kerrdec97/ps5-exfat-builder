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

Error fileError(const QString& userMessage, const QFileDevice& file)
{
    if (file.error() == QFileDevice::ResourceError)
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
    return Error(ErrorCode::IoError,
                 QStringLiteral("Could not open %1. Close any program using it (antivirus, Windows Search) and try again.")
                     .arg(QDir::toNativeSeparators(path)),
                 f->errorString());
}

} // namespace fw::exfat
