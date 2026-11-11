#pragma once

// 7z and RAR archives as build sources (GitHub #37), under ArchiveSource.
//
// The data is decoded by libarchive (7z, RAR 4, RAR 5) with liblzma, zlib and
// zstd; this file adds what a build source needs around it:
//
//  * a listing that never unpacks anything. 7z keeps its list of files in one
//    place and RAR 4 lets the reader jump from header to header; a solid RAR 5
//    archive can only be walked by libarchive by decoding every file, so RAR 5
//    headers are read here directly (they are small, checksummed records);
//  * limits a crafted archive cannot get past: the size of a 7z header before
//    it is decoded, the number of entries, one allocation of the library
//    (cmake/libarchive/fw_libarchive_config.h), and a budget for anything a
//    listing or a look at one file may read;
//  * one sequential pass that unpacks a folder of the archive, with every
//    name checked before the first file is written and every checksum
//    verified while its file is read.
//
// Not read: password-protected archives, solid RAR 4 archives, RAR 5 archives
// with a dictionary over 64 MiB, 7z files split into .001 parts.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::archive {

enum class ArchiveKind { Unknown, Zip, SevenZip, Rar4, Rar5 };

// What the file is by its first bytes; the suffix is not trusted.
ArchiveKind sniffArchive(const QString& path);

struct ArchiveEntry {
    enum class Type {
        File,
        Directory,
        Link,    // symbolic or hard link: never created
        FileCopy // RAR 5 "identical file stored once": refers to another entry
    };
    QString name;        // '/' separators, no "./" in front, no '/' at the end
    Type type = Type::File;
    qint64 size = 0;     // files: the size the archive lists
    bool encrypted = false;
};

struct ArchiveListing {
    QString path;                 // the file the user chose
    ArchiveKind kind = ArchiveKind::Unknown;
    QStringList volumes;          // every file of the archive, in order
    bool solid = false;           // RAR 5: files share one compressed stream
    qint64 largestDictionary = 0; // RAR 5: bytes
    std::vector<ArchiveEntry> entries;
};

// Reads the list of files of a 7z or RAR archive (all its parts for a
// multi-part RAR, which must be given by its first part). Nothing is unpacked.
Result<ArchiveListing> listArchive(const QString& path, const CancellationToken& token = {});

// What reading one file out of the middle of an archive may cost: in a solid
// archive everything stored before it has to be decoded first.
struct ReadBudget {
    qint64 archiveBytes = -1; // bytes of the archive that may be read; -1: no limit
    int milliseconds = -1;    // -1: no limit
};

// The contents of one file (`name` as listed, compared without regard to
// case). `withinBudget` is false, and the result empty, when the budget ran
// out first; that is not an error.
Result<QByteArray> readArchiveFile(const ArchiveListing& listing, const QString& name, qint64 maxSize,
                                   const ReadBudget& budget, bool* withinBudget,
                                   const CancellationToken& token = {});

// Refuses what extractArchive() must not write, before anything is written:
// names that would leave the destination, two names the destination cannot
// tell apart, and what libarchive cannot decode (RAR 5 dictionaries over
// 64 MiB, files stored as a copy of another). Only the entries under
// `onlyUnder` ("Folder/", compared without regard to case; empty: all) count.
Status checkExtractable(const ArchiveListing& listing, const QString& onlyUnder = {});

struct ExtractResult {
    qint64 files = 0;
    qint64 bytes = 0;
    qint64 linksSkipped = 0; // symbolic and hard links, which are never created
};

// Unpacks the entries under `onlyUnder` into `destination` (created if
// needed) in one pass through the archive, files through .part files, with
// their modification times. Fails on the first entry that is damaged, is
// larger than listed or was not listed.
Result<ExtractResult> extractArchive(const ArchiveListing& listing, const QString& destination,
                                     const QString& onlyUnder, const CancellationToken& token,
                                     ProgressReporter* progress = nullptr);

} // namespace fw::archive
