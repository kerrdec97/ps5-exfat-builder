#include "engines/fpkg/AsciiAliases.h"

#include "core/FileUtil.h"
#include "core/FsLinks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace fw::fpkg {

bool isAsciiPath(const QString& path)
{
    for (const QChar c : path)
        if (c.unicode() > 0x7F)
            return false;
    return true;
}

bool processIsRunning(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

AsciiAliases::AsciiAliases(QStringList baseCandidates, QStringList forbidden, Log log)
    : m_bases(std::move(baseCandidates)), m_forbidden(std::move(forbidden)), m_log(std::move(log))
{
}

AsciiAliases::~AsciiAliases()
{
    releaseAll();
}

void AsciiAliases::releaseAll()
{
    for (auto it = m_links.crbegin(); it != m_links.crend(); ++it) {
        const Status st = removeDirectoryJunction(*it);
        if (!st.ok())
            m_log.warn(QStringLiteral("Could not remove the folder link %1: %2").arg(QDir::toNativeSeparators(*it), st.error().message));
    }
    // The (possibly empty) alias folder stays: another job may be using it.
    m_links.clear();
    m_map.clear();
}

QStringList AsciiAliases::links() const
{
    return m_links;
}

Result<QString> AsciiAliases::map(const QString& dir, const QString& tag)
{
    const QString real = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    for (const auto& [r, t] : m_map)
        if (r == real)
            return t;
    if (isAsciiPath(real))
        return real;
    if (!QFileInfo(real).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(real)));

    QStringList tried;
    for (const QString& candidate : std::as_const(m_bases)) {
        if (candidate.isEmpty() || !isAsciiPath(candidate) || !QFileInfo(candidate).isDir())
            continue;
        if (std::any_of(m_forbidden.begin(), m_forbidden.end(),
                        [&](const QString& f) { return !f.isEmpty() && isSameOrInside(candidate, f); }))
            continue;
        const QString base = QDir(candidate).filePath(aliasFolderName());
        if (!QDir().mkpath(base))
            continue;
        sweepStale(base);
        const QString name = QStringLiteral("a%1-%2-%3")
                                 .arg(QCoreApplication::applicationPid(), 0, 16)
                                 .arg(QRandomGenerator::global()->generate() & 0xFFFFFF, 6, 16, QLatin1Char('0'))
                                 .arg(tag);
        const QString link = QDir(base).filePath(name);
        const Status st = createDirectoryJunction(link, real);
        if (!st.ok()) {
            tried << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(base), st.error().toLogString());
            continue;
        }
        // Only use the link when the target can be listed through it.
        QDir through(link);
        if (!through.exists() || through.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size()
                                     != QDir(real).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size()) {
            (void)removeDirectoryJunction(link);
            tried << QStringLiteral("%1: the folder could not be read through the link").arg(QDir::toNativeSeparators(link));
            continue;
        }
        m_links << link;
        m_map.emplace_back(real, link);
        std::sort(m_map.begin(), m_map.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        m_log.info(QStringLiteral("Using the ASCII link %1 for %2").arg(QDir::toNativeSeparators(link), QDir::toNativeSeparators(real)));
        return link;
    }
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("The path %1 contains characters the Publishing Tools reject, and no ASCII link to it "
                                "could be made. Move or rename the folder to a plain ASCII path, or choose an NTFS "
                                "temporary folder with an ASCII path.").arg(QDir::toNativeSeparators(real)),
                 tried.join(QStringLiteral("; ")));
}

QString AsciiAliases::toToolPath(const QString& realPath) const
{
    const QString real = QDir::cleanPath(QFileInfo(realPath).absoluteFilePath());
    for (const auto& [r, t] : m_map) {
        if (real == r)
            return QDir::toNativeSeparators(t);
        if (isSameOrInside(real, r))
            return QDir::toNativeSeparators(t + real.mid(r.size()));
    }
    return QDir::toNativeSeparators(real);
}

int AsciiAliases::sweepStale(const QString& base)
{
    static const QRegularExpression re(QStringLiteral("^a([0-9a-f]+)-[0-9a-f]{6}-"));
    int removed = 0;
    const QFileInfoList entries =
        QDir(base).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& fi : entries) {
        const auto m = re.match(fi.fileName());
        if (!m.hasMatch())
            continue;
        bool ok = false;
        const qint64 pid = m.captured(1).toLongLong(&ok, 16);
        if (!ok || processIsRunning(pid))
            continue;
        if (!isLinkOrReparsePoint(fi.absoluteFilePath()))
            continue; // never delete real folders
        if (removeDirectoryJunction(fi.absoluteFilePath()).ok())
            ++removed;
    }
    return removed;
}

} // namespace fw::fpkg
