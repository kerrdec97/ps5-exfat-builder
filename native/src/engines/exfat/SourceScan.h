#pragma once

// Scans the folder that goes into an image: names, sizes, attributes and
// timestamps, with the build's exclusion rules applied. The verifier scans
// the source with the same rules, so an excluded file is never reported as
// "missing from the image".

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QStringList>

#include <vector>

namespace fw::exfat {

struct ScanRules {
    // Glob patterns, case-insensitive:
    //   "Thumbs.db", "*.exfat"  - file names at any depth
    //   "sce_sys/*.bak"         - a pattern with '/' matches the relative path
    //   "decrypted/"            - a trailing '/' matches folders instead of files
    QStringList excludePatterns;
    // Skip files carrying the hidden attribute (Windows) or starting with a
    // dot (elsewhere), like the legacy robocopy /XA:H. Folders are kept.
    bool excludeHidden = false;
};

class ExcludeMatcher {
public:
    explicit ExcludeMatcher(const ScanRules& rules);
    bool excludesFile(const QString& name, const QString& relativePath, bool hidden) const;
    bool excludesDir(const QString& name, const QString& relativePath) const;
    bool isEmpty() const { return m_fileName.empty() && m_filePath.empty() && m_dirName.empty() && m_dirPath.empty() && !m_hidden; }

private:
    std::vector<QRegularExpression> m_fileName, m_filePath, m_dirName, m_dirPath;
    bool m_hidden = false;
};

struct SourceNode {
    QString name;          // as on disk (UTF-16)
    QString relativePath;  // '/' separators; empty for the root
    QString absolutePath;
    bool isDir = false;
    qint64 size = 0;
    quint16 attributes = 0; // exFAT attribute bits
    QDateTime created, modified, accessed;
    int parent = -1;
    std::vector<int> children; // sorted by up-cased name
    int imageEntry = -1;       // edits: data comes from this entry of the original image
};

struct SourceTree {
    QString root;
    std::vector<SourceNode> nodes; // nodes[0] is the root folder
    qint64 totalBytes = 0;
    qint64 fileCount = 0;
    qint64 dirCount = 0;          // folders below the root
    qint64 excludedCount = 0;     // files and folders skipped by the rules
    QStringList excludedSamples;  // the first few, for the log
    QStringList warnings;
};

Result<SourceTree> scanSource(const QString& root, const ScanRules& rules, const CancellationToken& token,
                              ProgressReporter* progress = nullptr);

// Attributes and times of one file or folder (used by the scanner and tests).
quint16 sourceAttributes(const QString& absolutePath, bool isDir, bool writable);

} // namespace fw::exfat
