#include "engines/pfs/FolderAccess.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace fw::pfs {

Status ensureFolderListable(const QString& dir)
{
    QString detail;
#ifdef Q_OS_WIN
    const QString pattern = toLongPath(dir) + QStringLiteral("\\*");
    WIN32_FIND_DATAW data;
    const HANDLE h = FindFirstFileExW(reinterpret_cast<const wchar_t*>(pattern.utf16()), FindExInfoBasic, &data,
                                      FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return success();
    }
    const DWORD err = GetLastError();
    // The root of an empty drive has no "." entry to find.
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES)
        return success();
    detail = QStringLiteral("Windows error %1").arg(err);
#else
    if (DIR* d = ::opendir(QFile::encodeName(dir).constData())) {
        ::closedir(d);
        return success();
    }
    detail = QString::fromLocal8Bit(std::strerror(errno));
#endif
    return Error(ErrorCode::PermissionDenied,
                 QStringLiteral("The folder %1 cannot be read. Check its permissions.").arg(QDir::toNativeSeparators(dir)), detail);
}

QString folderIdentity(const QString& dir)
{
#ifdef Q_OS_WIN
    const QString path = toLongPath(dir);
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info;
    const bool ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok)
        return {};
    return QStringLiteral("%1:%2:%3").arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh).arg(info.nFileIndexLow);
#else
    struct stat st;
    if (::stat(QFile::encodeName(dir).constData(), &st) != 0)
        return {};
    return QStringLiteral("%1:%2").arg(qulonglong(st.st_dev)).arg(qulonglong(st.st_ino));
#endif
}

} // namespace fw::pfs
