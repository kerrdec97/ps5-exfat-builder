#include "engines/fpkg/SourceStager.h"

#include <QDir>
#include <QFileInfo>

#include <deque>
#include <map>
#include <mutex>

namespace fw::fpkg {

namespace {
std::mutex& stagerMutex()
{
    static std::mutex m;
    return m;
}
std::map<QString, std::shared_ptr<SourceStager>>& stagers()
{
    static std::map<QString, std::shared_ptr<SourceStager>> s;
    return s;
}
} // namespace

void registerSourceStager(std::shared_ptr<SourceStager> stager)
{
    if (!stager)
        return;
    std::lock_guard lock(stagerMutex());
    for (const QString& ext : stager->extensions())
        stagers()[ext.toLower()] = stager;
}

void unregisterSourceStagers()
{
    std::lock_guard lock(stagerMutex());
    stagers().clear();
}

std::shared_ptr<SourceStager> sourceStagerFor(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    std::lock_guard lock(stagerMutex());
    const auto it = stagers().find(ext);
    return it == stagers().end() ? nullptr : it->second;
}

QStringList imageSourceExtensions()
{
    return {QStringLiteral("exfat"), QStringLiteral("ffpfsc"), QStringLiteral("ffpkg")};
}

QString findAppRoot(const QString& dir, int maxDepth)
{
    std::deque<std::pair<QString, int>> queue{{dir, 0}};
    while (!queue.empty()) {
        auto [cur, depth] = queue.front();
        queue.pop_front();
        const QDir d(cur);
        const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name | QDir::IgnoreCase);
        for (const QString& s : subs)
            if (s.compare(QStringLiteral("sce_sys"), Qt::CaseInsensitive) == 0)
                return QFileInfo(cur).absoluteFilePath();
        if (depth >= maxDepth)
            continue;
        for (const QString& s : subs) {
            const QFileInfo fi(d.filePath(s));
            if (fi.isSymbolicLink() || fi.isJunction())
                continue;
            queue.emplace_back(fi.absoluteFilePath(), depth + 1);
        }
    }
    return {};
}

} // namespace fw::fpkg
