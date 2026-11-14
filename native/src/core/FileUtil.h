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
// Sizes, rates and counts use the decimal and group separators of the
// interface language (German "48,2 GB", "18.442"), always in Latin digits: a
// language written with other digits (Arabic...) keeps the English form shown
// here. Log lines and reports get the same text as the window.
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
// Replaces the characters Windows forbids in file names by '_' and drops
// trailing dots and spaces. Reserved device names (CON, NUL...) are left to
// hostFileName() in PathSafety.
QString sanitizeFileName(const QString& name);
// `path` if nothing exists there (nor its .part or .fw.part), else
// "name (2).ext", "name (3).ext"...
QString uniquePath(const QString& path);

// ---- writes ------------------------------------------------------------------
// Writes via a temporary file and atomic rename (QSaveFile).
Status writeFileAtomic(const QString& path, const QByteArray& data);

// A file written as `<final>.fw.part` and renamed on commit(). If the object
// is destroyed without commit() the partial file is removed.
//
// That working name is one only this app uses: a file of that name when the
// object is made is a leftover of an interrupted run (a crash, a power cut)
// and is removed at once, so a retry starts clean and gets the space back
// (a job makes its PartFile before it checks the free space, so the check
// counts that space as free). An extractor whose source may hold a file
// called like another one's working name renames or refuses it first.
// A file called `<final>.part` may be the user's (a download, a game file of
// that name) or an older version's working file: it is never written over,
// renamed or removed, and the app log names it (one warning line), so an
// older version's leftover can be found and deleted. Callers open partPath()
// with QIODevice::NewOnly where they can: the name is free once the object
// exists, and a leftover that could not be removed then fails the open
// instead of being written into.
class PartFile {
public:
    explicit PartFile(QString finalPath);
    ~PartFile();
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;

    // The name a PartFile for `finalPath` works under: "<finalPath>.fw.part",
    // or, for a name with no room for that within the 255 characters NTFS
    // and exFAT allow, its start, "~" and a digest of the name, then
    // ".fw.part" (the same for the same name, every time).
    static QString partPathFor(const QString& finalPath);

    QString partPath() const { return m_part; }
    QString finalPath() const { return m_final; }
    // Renames the .part file over the final name. Fails (keeps the .part) if
    // the final file exists and `replaceExisting` is false. An existing file
    // is replaced in one step, so it is either still there or replaced, never
    // gone; a read-only one loses its read-only flag for that step and gets
    // it back if the replace fails. Nothing is set aside, and no other file
    // (an older version's "<final>.fw-old" among them) is ever brought back.
    Status commit(bool replaceExisting = false);
    void abandon(); // delete the .part now
    bool committed() const { return m_committed; }

private:
    QString m_final;
    QString m_part;
    bool m_committed = false;
};

// Gives a file or folder another name without ever copying it, retrying for a
// moment while a scanner still holds it. QFile::rename() (and QDir::rename()
// for a file) answer a rename to another drive, or of a file held open, by
// copying it: uninterruptibly, straight under the new name, and as much space
// again. Never replaces `to`; a change of case only ("game.exfat" →
// "Game.exfat") renames the file itself on Windows. False when the rename is
// not possible; `why` (optional) then holds the system's reason, for the log.
// `attempts` (Windows) is how often a held file is tried, 100 ms apart: about
// 1 s by default; fewer where the user waits for each refusal (the UI thread).
bool renameWithoutCopying(const QString& from, const QString& to, QString* why = nullptr, int attempts = 10);
// Renames the file `from` over the existing file `to` in one step, never
// copying: afterwards `to` is the old file or the new one, never missing.
// False when that is not possible (either file is held open, `to` is
// read-only or a folder); `why` (optional) then holds the system's reason.
bool replaceWithoutCopying(const QString& from, const QString& to, QString* why = nullptr);

// PartFile::commit() for a finished file whose name the caller chose: renames
// `partPath` to `finalPath` without ever copying it, replacing a file there in
// one step when `replaceExisting` is set (see PartFile::commit()). On failure
// `partPath` is left as it is, and when an old file could not be put back
// under its name the error says what it is called now.
Status commitFinishedFile(const QString& partPath, const QString& finalPath, bool replaceExisting);

// Gives a finished .part file its final name. When that fails (the file it
// replaces is mounted or open in another program) the finished work is not
// thrown away: it is moved to a free name next to it, "<name> (new).<ext>",
// and the returned error says so. `what` starts that message, for example
// "The image was built and verified". Such an error is never an IoError (it
// is PermissionDenied or AlreadyExists): a retry of the job would only make
// another full copy, so the queue does not retry it by itself.
Status commitOrKeep(PartFile& part, bool replaceExisting, const QString& what);

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

    // Every TempDir holds a small marker file with this name. A folder left
    // behind in a place the user chose (Settings > Storage > Temporary folder)
    // is only ever deleted when the marker proves the app created it. While
    // the TempDir exists the marker also names its process; a kept folder's
    // marker no longer does once the TempDir is gone.
    static QString ownerMarkerName();
    // True when `dir` is a real folder (not a link) that holds the marker.
    static bool isOwnedFolder(const QString& dir);
    // True when the marker in `dir` names a process that still runs: a job of
    // this or of another copy of the app (one that shares the temporary
    // folder) works in it, so no sweep may take it.
    static bool isInUse(const QString& dir);

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
// The same check for a job's temporary folder. The message of
// ensureFreeSpace() advises another output folder, which does not help when
// the temporary drive is the full one; this one names the setting that does.
Status ensureFreeTempSpace(const QString& path, qint64 requiredBytes, qint64 marginBytes = 256LL * 1024 * 1024);

// ---- volume checks ---------------------------------------------------------------
struct VolumeInfo {
    QString rootPath;
    QString fileSystem;   // "NTFS", "exFAT", "FAT32", "ext4"...
    bool isNetwork = false;
    bool isRemovable = false;
    bool isCloudSynced = false; // OneDrive, Google Drive, Dropbox, iCloud, Creative Cloud folders
};
VolumeInfo volumeInfo(const QString& anyPathOnVolume);

// True for the file systems that cannot hold a file of 4 GiB or more (FAT32
// and older), by the name diskSpace() and volumeInfo() report.
bool limitsFilesTo4GiB(const QString& fileSystemName);
// True when the drive holding `folder` is such a file system and `bytes`
// is over its limit: checked before hours of writing fail at 4 GiB.
bool tooLargeForDrive(const QString& folder, qint64 bytes);

// Checks that `outputFolder` can hold a file of `largestFileBytes`: refuses
// FAT/FAT32 for files of 4 GiB or more; returns warnings (shown, not fatal) for
// network drives and cloud-synced folders.
Result<QStringList> checkOutputVolume(const QString& outputFolder, qint64 largestFileBytes);

// ---- streaming I/O -------------------------------------------------------------
inline constexpr qint64 kIoChunk = 4 * 1024 * 1024;

// Copies src → dst through a PartFile; preserves modification time. Progress is
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
    // Relative paths (or simple `*` globs on the file name) to skip. A folder
    // that matches is skipped with everything in it.
    QStringList excludes;
};

Result<FileTree> scanTree(const QString& root, const CancellationToken& token, const ScanOptions& options = {});

// Removes a directory tree. Refuses a protected location (see below) and
// anything outside `mustBeInside` when it is given.
Status removeTreeSafely(const QString& path, const QString& mustBeInside = {});

// True for a folder that is never deleted as a whole, whatever a job was told
// its source is: a drive root, the user's home folder and the folders it lies
// in, the Desktop, Documents, Downloads, Pictures, Music and Videos folders,
// and on Windows the Windows, Program Files and ProgramData folders. Folders
// inside them are not protected.
bool isProtectedLocation(const QString& path);

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

// Ends writing to `file`: flushes Qt's buffer, sets the modification time
// when one is given and closes the file. QFile keeps small writes in memory,
// so a full or vanished drive often shows up only here and not in write():
// every writer must check this result before it commits its .part file.
Status finishWrittenFile(QFile& file, const QString& displayName, const QDateTime& modified = {});

} // namespace fw
