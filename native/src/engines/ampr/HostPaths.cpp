#include "engines/ampr/HostPaths.h"

#include "core/PathSafety.h"

#include <QDir>

namespace fw::ampr {

namespace {

QString componentProblem(const QString& part)
{
    if (part.isEmpty())
        return QStringLiteral("an empty folder name");
    if (part == u"." || part == u"..")
        return QStringLiteral("a \".\" or \"..\" folder name");
    if (part.contains(u'\\'))
        return QStringLiteral("a backslash");
    if (part.contains(QChar(u'\0')))
        return QStringLiteral("a NUL character");
    if (part.size() >= 2 && part[1] == u':' && ((part[0] >= u'A' && part[0] <= u'Z') || (part[0] >= u'a' && part[0] <= u'z')))
        return QStringLiteral("a drive letter");
    return {};
}

} // namespace

QString storedPathProblem(const QString& relPath)
{
    if (relPath.isEmpty())
        return QStringLiteral("an empty path");
    for (const QString& part : relPath.split(u'/'))
        if (const QString why = componentProblem(part); !why.isEmpty())
            return why;
    return {};
}

HostPaths::HostPaths(QString root) : m_root(std::move(root)) {}

Result<QString> HostPaths::add(const QString& relPath, bool isDir)
{
    if (const QString why = storedPathProblem(relPath); !why.isEmpty())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("The game lists the unsafe path \"%1\" (%2). Nothing was written for it.").arg(relPath, why));
    const QStringList parts = relPath.split(u'/');
    QString stored, host;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const QString& part = parts[i];
        const bool dir = isDir || i + 1 < parts.size();
        stored = stored.isEmpty() ? part : stored + u'/' + part;
        if (const auto known = m_rel.constFind(stored); known != m_rel.constEnd()) {
            host = *known;
            continue;
        }
        const QString mapped = hostEntryName(part);
        QString local = mapped;
        QHash<QString, Taken>& used = m_taken[hostNameKey(host)];
        const auto same = used.constFind(hostNameKey(local));
        if (same != used.constEnd() && dir && same->isDir) {
            local = same->name; // one folder on this drive
        } else {
            for (int k = 2; used.contains(hostNameKey(local)); ++k)
                local = QStringLiteral("%1 (%2)").arg(mapped).arg(k);
            used.insert(hostNameKey(local), {local, dir});
            if (local != part)
                m_renamed << QStringLiteral("%1 → %2").arg(stored, local);
        }
        host = host.isEmpty() ? local : host + u'/' + local;
        m_rel.insert(stored, host);
    }
    return childPathInside(m_root, host);
}

QStringList renamedWarnings(const QStringList& renamed)
{
    QStringList out;
    for (int i = 0; i < renamed.size() && i < 20; ++i)
        out << QStringLiteral("Renamed for this drive: %1").arg(renamed[i]);
    if (renamed.size() > 20)
        out << QStringLiteral("%1 more names were changed.").arg(renamed.size() - 20);
    return out;
}

} // namespace fw::ampr
