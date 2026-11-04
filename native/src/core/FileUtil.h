#pragma once

// File-system helpers shared by every engine: atomic writes, .part files,
// streaming copy/hash with progress and cancellation, free-space checks,
// directory scans and the user-facing number formats from the design system.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QString>

#include <memory>
#include <vector>

namespace fw {

// ---- formatting (design system: "48.2 GB", "418 MB/s", "About 1 min left") ----
QString formatBytes(qint64 bytes);
QString formatRate(double bytesPerSec);
QString formatTimeLeft(qint64 ms);
QString formatCount(qint64 n); // "18,442"

// ---- paths -------------------------------------------------------------------
// Windows: prefixes `\\?\` for paths longer than MAX_PATH so deep game trees
// work; elsewhere returns the native path unchanged.
QString toLongPath(const QString& path);
// True if `child` is `parent` or inside it (case-insensitive on Windows).
bool isSameOrInside(const QString& child, const QString& parent);
// Removes characters Windows forbids in file names.
QString sanitizeFileName(const QString& name);
// `path` if nothing exists there (nor its .part), else "name (2).ext", "name (3).ext"...
QString uniquePath(const QString& path);

// ---- writes ------------------------------------------------------------------
// Writes via a temporary file and atomic rename (QSaveFile).
Status writeFileAtomic(const QString& path, const QByteArray& data);

// A file written as `<final>.part` and renamed on commit(). If the object is
// destroyed without commit() the partial file is removed.
class PartFile {
public:
    explicit PartFile(QString finalPath);
    ~PartFile();
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;

    QString partPath() const { return m_final + QStringLiteral(".part"); }
    QString finalPath() const { return m_final; }
    // Renames the .part file over the final name. Fails (keeps the .part) if
    // the final file exists and `replaceExisting` is false.
    Status commit(bool replaceExisting = false);
    void abandon(); // delete the .part now
    bool committed() const { return m_committed; }

private:
    QString m_final;
    bool m_committed = false;
};

// A private temporary directory, removed recursively on destruction unless
// keep() was called. Created under Paths::tempRoot() by default.
class TempDir {
public:
    explicit TempDir(const QString& prefix = QStringLiteral("job"), const QString& parentDir = {});
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    bool isValid() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }
    QString filePath(const QString& name) const;
    void keep() { m_keep = true; }

private:
    QString m_path;
    bool m_keep = false;
};

// ---- disk space --------------------------------------------------------------
struct DiskSpace {
    qint64 available = 0;
    qint64 total = 0;
    QString rootPath;   // e.g. "D:/"
    QString fileSystem; // e.g. "NTFS"
};
Result<DiskSpace> diskSpace(const QString& anyPathOnVolume);
// Fails with InsufficientSpace and a user-facing message if fewer than
// `requiredBytes + marginBytes` are free on the volume holding `path`.
Status ensureFreeSpace(const QString& path, qint64 requiredBytes, qint64 marginBytes = 256LL * 1024 * 1024);

// ---- volume checks ---------------------------------------------------------------
struct VolumeInfo {
    QString rootPath;
    QString fileSystem;   // "NTFS", "exFAT", "FAT32", "ext4"...
    bool isNetwork = false;
    bool isRemovable = false;
    bool isCloudSynced = false; // OneDrive, Google Drive, Dropbox, iCloud, Creative Cloud folders
};
VolumeInfo volumeInfo(const QString& anyPathOnVolume);

// Checks that `outputFolder` can hold a file of `largestFileBytes`: refuses
// FAT/FAT32 for files of 4 GiB or more; returns warnings (shown, not fatal) for
// network drives and cloud-synced folders.
Result<QStringList> checkOutputVolume(const QString& outputFolder, qint64 largestFileBytes);

// ---- streaming I/O -------------------------------------------------------------
inline constexpr qint64 kIoChunk = 4 * 1024 * 1024;

// Copies src → dst through dst.part; preserves modification time. Progress is
// advanced by bytes copied. `durable` flushes the copy to the drive
// (flushToDisk) before it gets its name, for files that replace a user's
// file (backport Apply and rollback).
Status copyFileStreaming(const QString& src, const QString& dst, const CancellationToken& token,
                         ProgressReporter* progress = nullptr, bool replaceExisting = false, bool durable = false);

// SHA-256 of a file (hex, lowercase).
Result<QString> sha256File(const QString& path, const CancellationToken& token, ProgressReporter* progress = nullptr);
QString sha256Hex(const QByteArray& data);

// Byte-for-byte comparison of two files.
Result<bool> filesEqual(const QString& a, const QString& b, const CancellationToken& token,
                        ProgressReporter* progress = nullptr);

// ---- directory scans ------------------------------------------------------------
struct FileEntry {
    QString relativePath; // '/' separators, relative to the scan root
    qint64 size = 0;
    bool isDir = false;
    QDateTime modified;
};

struct FileTree {
    QString root;
    std::vector<FileEntry> entries; // directories before their children, sorted
    qint64 totalBytes = 0;
    qint64 fileCount = 0;
    qint64 dirCount = 0;
};

struct ScanOptions {
    bool includeHidden = true;
    bool followSymlinks = false;
    // Relative paths (or simple `*` globs on the file name) to skip.
    QStringList excludes;
};

Result<FileTree> scanTree(const QString& root, const CancellationToken& token, const ScanOptions& options = {});

// Removes a directory tree. Refuses to touch drive roots, the user's home
// folder and anything outside `mustBeInside` when it is given.
Status removeTreeSafely(const QString& path, const QString& mustBeInside = {});

// Deletes a folder and everything in it like QDir::removeRecursively(), but
// never descends into a junction or symbolic link: the link itself is removed
// and what it points at is left alone (Qt follows NTFS junctions).
bool removeTreeNoFollow(const QString& dir);

// True for symbolic links and NTFS junctions (mount points); false for other
// reparse points such as cloud placeholders and for .lnk shortcuts.
bool isJunctionOrSymlink(const QString& path);

// Flushes Qt's buffer and the operating system's cache of an open file to the
// drive (FlushFileBuffers / fsync), so a verified image survives a power cut
// before it replaces anything.
Status flushToDisk(QFile& file, const QString& displayName);

} // namespace fw
