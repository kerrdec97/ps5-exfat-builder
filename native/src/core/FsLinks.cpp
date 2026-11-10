#include "core/FsLinks.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstddef>
#include <cstring>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>

#include <vector>
#endif

namespace fw {

#ifdef Q_OS_WIN
namespace {
// REPARSE_DATA_BUFFER (mount point variant) is only declared in ntifs.h.
struct MountPointReparseBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
};
constexpr size_t kHeaderSize = offsetof(MountPointReparseBuffer, SubstituteNameOffset); // tag+len+reserved

QString lastErrorText()
{
    wchar_t* buf = nullptr;
    const DWORD err = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString s = buf ? QString::fromWCharArray(buf).trimmed() : QString();
    if (buf)
        LocalFree(buf);
    return QStringLiteral("%1 (error %2)").arg(s).arg(err);
}
} // namespace
#endif

bool isLinkOrReparsePoint(const QString& path)
{
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(toLongPath(path)).utf16()));
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}

Status createHardLink(const QString& link, const QString& target)
{
    if (QFileInfo::exists(link))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(link)));
#ifdef Q_OS_WIN
    const std::wstring l = toLongPath(QDir::toNativeSeparators(QFileInfo(link).absoluteFilePath())).toStdWString();
    const std::wstring t = toLongPath(QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath())).toStdWString();
    if (!CreateHardLinkW(l.c_str(), t.c_str(), nullptr))
        return Error(ErrorCode::IoError, QStringLiteral("Could not link %1.").arg(QDir::toNativeSeparators(link)),
                     QStringLiteral("CreateHardLinkW error %1").arg(GetLastError()));
    return success();
#else
    if (::link(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) != 0)
        return Error(ErrorCode::IoError, QStringLiteral("Could not link %1.").arg(link));
    return success();
#endif
}

Status createDirectoryJunction(const QString& link, const QString& target)
{
    const QFileInfo t(target);
    if (!t.isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(target)));
    if (QFileInfo::exists(link) || isLinkOrReparsePoint(link))
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists.").arg(QDir::toNativeSeparators(link)));
    QDir().mkpath(QFileInfo(link).absolutePath());

#ifdef Q_OS_WIN
    const QString nativeLink = QDir::toNativeSeparators(QFileInfo(link).absoluteFilePath());
    // Cleaned: the target is stored as given, and a ".." in it would not resolve.
    QString nativeTarget = QDir::toNativeSeparators(QDir::cleanPath(t.absoluteFilePath()));
    if (!nativeTarget.endsWith(u'\\'))
        nativeTarget += u'\\';
    const std::wstring subst = (QStringLiteral("\\??\\") + nativeTarget).toStdWString();
    const std::wstring print = nativeTarget.toStdWString();

    if (!CreateDirectoryW(nativeLink.toStdWString().c_str(), nullptr))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create a link folder."), lastErrorText());

    HANDLE h = CreateFileW(nativeLink.toStdWString().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const QString why = lastErrorText();
        RemoveDirectoryW(nativeLink.toStdWString().c_str());
        return Error(ErrorCode::IoError, QStringLiteral("Could not open the link folder."), why);
    }

    const USHORT substBytes = USHORT(subst.size() * sizeof(WCHAR));
    const USHORT printBytes = USHORT(print.size() * sizeof(WCHAR));
    const size_t pathBytes = size_t(substBytes) + sizeof(WCHAR) + size_t(printBytes) + sizeof(WCHAR);
    const size_t total = offsetof(MountPointReparseBuffer, PathBuffer) + pathBytes;
    std::vector<char> raw(total, 0);
    auto* rb = reinterpret_cast<MountPointReparseBuffer*>(raw.data());
    rb->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rb->ReparseDataLength = USHORT(total - kHeaderSize);
    rb->SubstituteNameOffset = 0;
    rb->SubstituteNameLength = substBytes;
    rb->PrintNameOffset = USHORT(substBytes + sizeof(WCHAR));
    rb->PrintNameLength = printBytes;
    std::memcpy(rb->PathBuffer, subst.c_str(), substBytes);
    std::memcpy(reinterpret_cast<char*>(rb->PathBuffer) + substBytes + sizeof(WCHAR), print.c_str(), printBytes);

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, raw.data(), DWORD(total), nullptr, 0, &returned, nullptr);
    const QString why = ok ? QString() : lastErrorText();
    CloseHandle(h);
    if (!ok) {
        RemoveDirectoryW(nativeLink.toStdWString().c_str());
        return Error(ErrorCode::IoError, QStringLiteral("Could not create a folder junction."), why);
    }
    // NTFS stores any target without looking at it. One a junction cannot
    // reach (a network share, a mapped network drive) would only fail later,
    // inside the tool that is given the link: open the folder through it now.
    const HANDLE through = CreateFileW(nativeLink.toStdWString().c_str(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (through != INVALID_HANDLE_VALUE) {
        CloseHandle(through);
    } else if (GetLastError() != ERROR_ACCESS_DENIED) { // denied: the target is there, only not ours to list
        const QString unreachable = lastErrorText();
        RemoveDirectoryW(nativeLink.toStdWString().c_str());
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("A folder junction cannot point at %1.").arg(QDir::toNativeSeparators(target)), unreachable);
    }
    return success();
#else
    if (!QFile::link(t.absoluteFilePath(), link))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create a folder link."));
    return success();
#endif
}

Status removeDirectoryJunction(const QString& link)
{
    if (!isLinkOrReparsePoint(link)) {
        if (!QFileInfo::exists(link))
            return success();
        return Error(ErrorCode::PermissionDenied,
                     QStringLiteral("%1 is a real folder, not a link; it was left in place.").arg(QDir::toNativeSeparators(link)));
    }
#ifdef Q_OS_WIN
    // RemoveDirectory on a junction removes the junction only.
    if (!RemoveDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(link).utf16())))
        return Error(ErrorCode::IoError, QStringLiteral("Could not remove the folder junction."), lastErrorText());
#else
    if (!QFile::remove(link))
        return Error(ErrorCode::IoError, QStringLiteral("Could not remove the folder link."));
#endif
    return success();
}

} // namespace fw
