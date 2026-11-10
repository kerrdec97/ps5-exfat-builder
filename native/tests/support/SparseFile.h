#pragma once

// Sparse files for tests that need sizes a test must not really write: a
// file over 4 GiB to exercise 64-bit offsets, or one larger than the free
// space. Header-only, so a test executable needs nothing more to link.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QPair>
#include <QString>

#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace fwtest {

// The bytes `path` takes on disk (less than its size for a sparse or
// compressed file), -1 when that cannot be read.
inline qint64 bytesOnDisk(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(native.c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
        return -1;
    return (qint64(high) << 32) | low;
#else
    struct stat sb;
    if (::stat(QFile::encodeName(path).constData(), &sb) != 0)
        return -1;
    return qint64(sb.st_blocks) * 512;
#endif
}

// A sparse file of `size` bytes holding `marks` (offset, bytes) and zeros
// elsewhere. Returns the bytes it takes on disk, -1 when it could not be
// made. A file system without sparse files makes a real file of that size
// (or fails for want of room), so callers compare the result with what they
// can afford and skip when it is too much.
//
// On Windows the sparse attribute stays with the file when it is later
// opened for writing and truncated, so making an empty sparse file (size 0)
// first keeps the holes of whatever is then written to that path
// unallocated.
inline qint64 makeSparseFile(const QString& path, qint64 size, const QList<QPair<qint64, QByteArray>>& marks = {})
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    HANDLE h = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    LARGE_INTEGER li;
    li.QuadPart = size;
    ok = ok && SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    for (const auto& m : marks) {
        li.QuadPart = m.first;
        DWORD written = 0;
        ok = ok && SetFilePointerEx(h, li, nullptr, FILE_BEGIN)
             && WriteFile(h, m.second.constData(), DWORD(m.second.size()), &written, nullptr) && written == DWORD(m.second.size());
    }
    CloseHandle(h);
    if (!ok)
        return -1;
#else
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || !f.resize(size))
        return -1;
    for (const auto& m : marks)
        if (!f.seek(m.first) || f.write(m.second) != m.second.size())
            return -1;
    f.close();
#endif
    return bytesOnDisk(path);
}

} // namespace fwtest
