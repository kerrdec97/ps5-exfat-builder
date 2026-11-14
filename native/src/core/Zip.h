#pragma once

// ZIP archives: streaming writer and reader shared by Backport (backport and
// originals ZIPs), the Language Stripper (backups) and the problem-report
// bundle.
//
// Writer: store or deflate (zlib), CRC-32, UTF-8 names, ZIP64 for files or
// archives over 4 GiB, written through a .part file and renamed on finish().
// Reader: store/deflate, ZIP64, CRC verification, extraction that refuses
// entries escaping the destination ("zip slip").

#include "core/Cancellation.h"
#include "core/FileUtil.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QDateTime>
#include <QFile>
#include <QString>

#include <memory>
#include <vector>

namespace fw::zip {

enum class Method : quint16 { Store = 0, Deflate = 8 };

struct Entry {
    QString name;          // '/' separators; directories end with '/'
    Method method = Method::Store;
    quint32 crc32 = 0;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
    quint64 localHeaderOffset = 0;
    QDateTime modified;
    bool encrypted = false; // password-protected: listed, but read() refuses it
    bool isDirectory() const { return name.endsWith(u'/'); }
};

class Writer {
public:
    Writer();
    ~Writer(); // abandons the .part file unless finish() succeeded
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // Opens `<path>.fw.part` (a PartFile). `replaceExisting` decides what finish() may do when
    // `path` exists (never silently overwrite an earlier archive by default).
    Status open(const QString& path, bool replaceExisting = false);

    Status addFile(const QString& archiveName, const QString& sourcePath, Method method,
                   const CancellationToken& token, ProgressReporter* progress = nullptr);
    Status addData(const QString& archiveName, const QByteArray& data, Method method = Method::Deflate,
                   const QDateTime& modified = QDateTime::currentDateTime());
    Status addDirectory(const QString& archiveName, const QDateTime& modified = QDateTime::currentDateTime());

    // Writes the central directory and flushes. With `verifyBeforeCommit`, the
    // finished .part archive is read back in full (CRC and size of every
    // entry) before it is renamed to its final name; a failure deletes it.
    Status finish(bool verifyBeforeCommit = false, const CancellationToken& token = {});

    const std::vector<Entry>& entries() const { return m_entries; }
    QString path() const { return m_path; }

    // Tests only: force ZIP64 records above this many bytes (default 0xFFFFFFFF).
    void setZip64ThresholdForTesting(quint64 bytes) { m_zip64Threshold = bytes; }

private:
    Status writeEntry(const QString& name, Method method, const QDateTime& modified, quint64 sizeHint,
                      const std::function<Result<qint64>(QByteArray& chunk)>& readChunk,
                      const CancellationToken& token, ProgressReporter* progress);
    Status writeRaw(const QByteArray& b);

    QString m_path;
    bool m_replace = false;
    std::unique_ptr<PartFile> m_part;
    QFile m_file;
    std::vector<Entry> m_entries;
    quint64 m_zip64Threshold = 0xFFFFFFFFull;
    bool m_finished = false;
};

class Reader {
public:
    Status open(const QString& path);
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry* find(const QString& name) const;

    // Streams one entry's data, checking its CRC at the end.
    Status read(const Entry& entry, const std::function<Status(const QByteArray&)>& sink,
                const CancellationToken& token, ProgressReporter* progress = nullptr);
    Result<QByteArray> readAll(const Entry& entry, qint64 maxBytes = 256LL * 1024 * 1024);

    // Re-reads every entry and checks CRCs and sizes (archive read-back check).
    Status verifyAll(const CancellationToken& token, ProgressReporter* progress = nullptr);

    // Extracts every entry under `destination` (created if needed) through
    // .part files. Refuses absolute names, drive letters and ".." segments;
    // a character the host cannot store in a name (a ':' in a folder zipped
    // on Linux) is replaced, see extractedPath().
    // With `onlyUnder` ("Folder/"), only the entries whose name starts with it
    // (compared without regard to case) are checked and extracted.
    Status extractAll(const QString& destination, const CancellationToken& token, ProgressReporter* progress = nullptr,
                      bool overwrite = false, const QString& onlyUnder = {});

    // The safe relative path for an entry name, or empty if it would escape
    // (also for any name with a ':').
    static QString safeRelativePath(const QString& entryName);
    // The relative path extractAll() writes an entry to: each segment gets the
    // name the host can store (fw::hostEntryName). Empty if the name would
    // escape or is the destination itself ("./").
    static QString extractedPath(const QString& entryName);

private:
    QString m_path;
    std::vector<Entry> m_entries;
};

quint32 crc32(const QByteArray& data, quint32 seed = 0);

// Unpacking a file of a long name: an extractor writes it through a PartFile
// for stagingPath(target) and then calls moveIntoPlace(). stagingPath() is
// `target` itself, or, when the PartFile's working name would be longer than
// the 255 characters NTFS and exFAT allow in a name, a free short name in the
// same folder (PartFile::partPathFor() keeps its own name within that limit,
// so a name a drive can hold is written as it is). moveIntoPlace() renames such a file to `target` (a no-op for
// `target` itself); when it fails, or `target` exists and may not be
// replaced, the staged file is removed.
QString stagingPath(const QString& target);
Status moveIntoPlace(const QString& staged, const QString& target, bool replaceExisting);

} // namespace fw::zip
