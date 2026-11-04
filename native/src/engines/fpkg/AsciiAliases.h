#pragma once

// ASCII aliases for folders whose real path the Publishing Tools reject.
//
// A root (source, output, base folder, base package folder, the SDK's temp
// folder) whose path has any character above 0x7F gets a link with a plain
// ASCII path: an NTFS junction on Windows, a symlink elsewhere (core/FsLinks).
// The link lives in an ASCII, writable base folder (system temp, the job's
// temp folder, %ProgramData%, the output drive's root) and never inside a
// source folder. A link is only used once its target can be listed through
// it. Links are removed when the object is destroyed; links left by a
// process that no longer runs are removed by sweepStale(). Removing a link
// never touches the folder it points to. Paths are mapped once at the roots.

#include "core/Log.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace fw::fpkg {

bool isAsciiPath(const QString& path);

class AsciiAliases {
public:
    // `baseCandidates`: folders that may hold the links, in order of
    // preference (non-ASCII or unwritable ones are skipped). `forbidden`:
    // links are never created inside these folders (the sources).
    AsciiAliases(QStringList baseCandidates, QStringList forbidden, Log log = {});
    ~AsciiAliases();
    AsciiAliases(const AsciiAliases&) = delete;
    AsciiAliases& operator=(const AsciiAliases&) = delete;

    // An ASCII path for the folder `dir`: `dir` itself when it is already
    // ASCII, else a new link. Unsupported when no link can be made.
    Result<QString> map(const QString& dir, const QString& tag);

    // Real absolute path under a mapped root -> the path to give the tool
    // (native separators). Paths outside every mapped root are returned as is.
    QString toToolPath(const QString& realPath) const;

    QStringList links() const;
    void releaseAll();

    // Removes links of processes that are no longer running from the alias
    // folder under `base`. Returns how many were removed.
    static int sweepStale(const QString& base);
    static QString aliasFolderName() { return QStringLiteral("fw-fpkg-aliases"); }

private:
    QStringList m_bases;
    QStringList m_forbidden;
    Log m_log;
    std::vector<std::pair<QString, QString>> m_map; // real root -> tool root
    QStringList m_links;
};

bool processIsRunning(qint64 pid);

} // namespace fw::fpkg
