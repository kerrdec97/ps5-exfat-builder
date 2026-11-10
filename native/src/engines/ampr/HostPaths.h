#pragma once

// Where the stored paths of a game go on this computer.
//
// The paths of a GameFiles come from a pack index or an image and are
// untrusted: "../x", "C:/x" or "a//b" must never let a job write outside its
// output folder. HostPaths turns each stored path into a path that provably
// lies inside one root, the same way the per-format extractors do
// (core/PathSafety.h):
//
//   * a path with an empty, "." or ".." component, a backslash, a NUL or a
//     drive letter is refused (the job fails, nothing is written for it);
//   * every other component gets the name this drive can store
//     (hostEntryName); two names that would be the same file here get
//     " (2)", " (3)"... and both kinds of change are listed in renamed().
//     Folders whose names differ only in case stay one folder where the
//     drive compares names that way;
//   * the result goes through childPathInside().
//
// Add folders and files in any order; a path always maps to the same place.

#include "core/Result.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace fw::ampr {

// "" when `relPath` ('/'-separated, relative) is made of plain names; else
// what is wrong with it, for a message ("a \"..\" folder name").
QString storedPathProblem(const QString& relPath);

class HostPaths {
public:
    explicit HostPaths(QString root);

    const QString& root() const { return m_root; }
    // The full path for the stored `relPath` (a file unless `isDir`).
    Result<QString> add(const QString& relPath, bool isDir = false);
    // The path below the root that add() chose ("" when it was never added).
    QString relative(const QString& relPath) const { return m_rel.value(relPath); }
    // "stored path → name on this drive" for every name that changed.
    const QStringList& renamed() const { return m_renamed; }

private:
    QString m_root;
    struct Taken {
        QString name; // as created on this drive
        bool isDir = false;
    };
    QHash<QString, QString> m_rel;                 // stored path -> host path below the root
    QHash<QString, QHash<QString, Taken>> m_taken; // host folder -> name key -> entry
    QStringList m_renamed;
};

// The warnings a job shows for renamed entries (at most 20 lines and a count).
QStringList renamedWarnings(const QStringList& renamed);

} // namespace fw::ampr
