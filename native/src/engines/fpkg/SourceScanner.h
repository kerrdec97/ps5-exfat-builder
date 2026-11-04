#pragma once

// Walks a dump folder (or an update folder laid over its base) and decides
// what goes into the package, in the order the Publishing Tools will lay it
// out.
//
// Order: recursive; inside every folder the entries (files and folders
// together) are sorted by their case-folded name, then by code point;
// sub-folders appear at their sorted position.
//
// Overlay: `roots[0]` is the source; further roots (an update's base) fill in
// paths the source does not have. Paths are matched case-insensitively. For a
// path present in several roots the data comes from the first root and the
// spelling from the last one (the base's spelling is kept for replaced
// files).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/fpkg/DumpRules.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::fpkg {

struct ScanOptions {
    LeftoverOptions leftovers;
    // Root folders ignored in every root but the first (payload folders of
    // an earlier package build).
    QStringList ignoredBaseRootDirs = {QStringLiteral("playgo-languages"), QStringLiteral("playgo-chunks")};
    int checkpointEvery = 2000;
};

struct ScannedFile {
    QString rel;      // package path ('/' separators)
    QString absPath;  // real path of the data
    qint64 size = 0;
    int root = 0;     // which root supplied the data
};

struct ExcludedEntry {
    QString rel;
    QString reason;
    bool isDir = false;
};

struct BadPath {
    QString rel;
    QString reason;
};

struct ScanResult {
    std::vector<ScannedFile> files;       // walk order
    std::vector<ExcludedEntry> excluded;  // with reasons (01-create-gp5.log)
    QStringList emptyDirs;                // empty or junk-only folders outside sce_sys (walk order)
    std::vector<BadPath> badPaths;        // mapped paths failing the Publishing Tools path rule
    std::vector<std::pair<QString, QString>> sceSysDds; // (rel, abs) of sce_sys/*.dds, walk order
    QStringList ps4SelfMagic;             // executables starting with the PS4 SELF signature
    qint64 totalBytes = 0;
    qint64 keystoneSize = -1;             // -1: no sce_sys/keystone
    qint64 entriesSeen = 0;
    int filesFromRoot[4] = {0, 0, 0, 0};

    // Every walked folder (walk order, package spelling); lower-case package
    // path of every walked folder, and of every folder that holds at least
    // one mapped file somewhere below it.
    QStringList dirs;
    QSet<QString> dirsLower;
    QSet<QString> dirsWithFilesLower;
    QHash<QString, int> fileIndexLower;   // lower-case rel -> index in files

    const ScannedFile* findFile(const QString& rel) const;
    bool hasFileUnder(const QString& relDir) const { return dirsWithFilesLower.contains(relDir.toLower()); }
    bool hasDir(const QString& relDir) const { return dirsLower.contains(relDir.toLower()); }
};

// Errors: a root that is not a readable folder (NotFound / IoError), a link
// or junction anywhere in the walked tree (Unsupported), two packaged entries
// of one root whose names differ only in letter case, such as "Data/" next to
// "data/" or "Readme.TXT" next to "readme.txt" (Unsupported, naming both: the
// package file system is case-insensitive, so one would be lost),
// cancellation.
Result<ScanResult> scanSource(const QStringList& roots, const ScanOptions& options, const CancellationToken& token);

// Finds `rel` below `root` matching each component case-insensitively (an
// exact match is preferred). Returns the real path or "".
QString findPathCaseInsensitive(const QString& root, const QString& rel, bool wantDir);

// First of `roots` holding the file `rel` (case-insensitive), or "".
QString findInRoots(const QStringList& roots, const QString& rel);

// PS4 SELF signature 4F 15 3D 1D.
bool startsWithPs4SelfMagic(const QString& path);
bool isExecutableName(const QString& name);

} // namespace fw::fpkg
