#pragma once

// Directory junctions (Windows) / symlinks (elsewhere).
//
// Used to give external tools an ASCII-only path to a folder whose real path
// contains characters the tool cannot handle (Sony Publishing Tools). A
// JunctionGuard removes the link — never the target's contents — when it goes
// out of scope.

#include "core/Result.h"

#include <QString>

namespace fw {

// Creates `link` (must not exist) pointing at the existing directory `target`.
// Windows: an NTFS junction (no administrator rights needed); elsewhere a
// directory symlink.
Status createDirectoryJunction(const QString& link, const QString& target);

// Removes the link itself without touching the target's files. Refuses to
// remove a real directory.
Status removeDirectoryJunction(const QString& link);

// True for junctions, symlinks and other reparse points.
bool isLinkOrReparsePoint(const QString& path);

class JunctionGuard {
public:
    JunctionGuard() = default;
    explicit JunctionGuard(QString link) : m_link(std::move(link)) {}
    JunctionGuard(JunctionGuard&& o) noexcept : m_link(std::move(o.m_link)) { o.m_link.clear(); }
    JunctionGuard& operator=(JunctionGuard&& o) noexcept
    {
        if (this != &o) {
            release();
            m_link = std::move(o.m_link);
            o.m_link.clear();
        }
        return *this;
    }
    JunctionGuard(const JunctionGuard&) = delete;
    JunctionGuard& operator=(const JunctionGuard&) = delete;
    ~JunctionGuard() { release(); }

    const QString& path() const { return m_link; }
    void release()
    {
        if (!m_link.isEmpty())
            (void)removeDirectoryJunction(m_link);
        m_link.clear();
    }

private:
    QString m_link;
};

} // namespace fw
