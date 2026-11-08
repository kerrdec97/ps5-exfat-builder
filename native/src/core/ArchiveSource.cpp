#include "core/ArchiveSource.h"

#include "core/FileUtil.h"
#include "core/Zip.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>

#include <algorithm>

namespace fw::archive {

namespace {

QString nativePath(const QString& p) { return QDir::toNativeSeparators(p); }

// "a/b/eboot.bin" -> "a/b/"; "eboot.bin" -> "".
QString folderOf(const QString& entryName)
{
    const int slash = int(entryName.lastIndexOf(u'/'));
    return slash < 0 ? QString() : entryName.left(slash + 1);
}

} // namespace

bool isZipSource(const QString& path)
{
    const QFileInfo fi(path);
    return fi.isFile() && fi.suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0;
}

Result<ZipGame> inspectZip(const QString& zipPath)
{
    zip::Reader r;
    FW_TRY(r.open(zipPath));
    // Every folder that holds eboot.bin or sce_sys/param.json is a game root.
    QMap<QString, int> roots; // prefix (lower case) -> bit 1 eboot, bit 2 param
    QMap<QString, QString> spelling;
    const zip::Entry* paramEntry = nullptr;
    for (const zip::Entry& e : r.entries()) {
        if (e.isDirectory())
            continue;
        const QString lower = e.name.toLower();
        if (lower == QLatin1String("eboot.bin") || lower.endsWith(QLatin1String("/eboot.bin"))) {
            const QString prefix = folderOf(e.name);
            roots[prefix.toLower()] |= 1;
            spelling.insert(prefix.toLower(), prefix);
        } else if (lower == QLatin1String("sce_sys/param.json") || lower.endsWith(QLatin1String("/sce_sys/param.json"))) {
            const QString prefix = e.name.left(e.name.size() - int(QStringLiteral("sce_sys/param.json").size()));
            roots[prefix.toLower()] |= 2;
            spelling.insert(prefix.toLower(), prefix);
        }
    }
    if (roots.isEmpty())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 holds no game: there is no eboot.bin or sce_sys/param.json in it.")
                                              .arg(QFileInfo(zipPath).fileName()));
    // Nested folders of one game (e.g. sce_module/.../eboot.bin) are not
    // separate games: keep only roots that are not inside another root.
    QStringList keys = roots.keys();
    std::sort(keys.begin(), keys.end(), [](const QString& a, const QString& b) { return a.size() < b.size(); });
    QStringList top;
    for (const QString& k : keys) {
        bool nested = false;
        for (const QString& t : top)
            if (k.startsWith(t)) {
                nested = true;
                break;
            }
        if (!nested)
            top << k;
    }
    ZipGame g;
    g.rootPrefix = spelling.value(top.first());
    for (int i = 1; i < top.size(); ++i) {
        QString name = spelling.value(top[i]);
        if (name.endsWith(u'/'))
            name.chop(1);
        g.otherGames << (name.isEmpty() ? QStringLiteral("(top level)") : name);
    }
    const QString rootLower = g.rootPrefix.toLower();
    g.hasEboot = roots.value(rootLower) & 1;
    g.hasParam = roots.value(rootLower) & 2;
    for (const zip::Entry& e : r.entries()) {
        if (e.isDirectory() || !e.name.toLower().startsWith(rootLower))
            continue;
        g.bytes += qint64(e.uncompressedSize);
        ++g.files;
        if (e.name.compare(g.rootPrefix + QStringLiteral("sce_sys/param.json"), Qt::CaseInsensitive) == 0)
            paramEntry = &e;
    }
    if (paramEntry && paramEntry->uncompressedSize < 16ull * 1024 * 1024) {
        auto data = r.readAll(*paramEntry);
        if (data.ok())
            g.paramJson = data.value();
    }
    return g;
}

Status checkZipSource(const QString& zipPath)
{
    auto g = inspectZip(zipPath);
    if (!g.ok())
        return g.error();
    if (!g->otherGames.isEmpty())
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("%1 holds more than one game (%2 and %3). Zip one game per archive, or unpack it and "
                                    "choose the game's folder.")
                         .arg(QFileInfo(zipPath).fileName(), g->rootPrefix.isEmpty() ? QStringLiteral("(top level)") : g->rootPrefix,
                              g->otherGames.join(QStringLiteral(", "))));
    return success();
}

Result<QString> unpackZipSource(const QString& zipPath, const QString& tempDir, const CancellationToken& token,
                                ProgressReporter* progress)
{
    FW_TRY(checkZipSource(zipPath));
    FW_TRY_ASSIGN(ZipGame g, inspectZip(zipPath));
    if (tempDir.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No temporary folder to unpack %1 into.").arg(nativePath(zipPath)));
    const QString dest = QDir(tempDir).filePath(QStringLiteral("zip-source"));
    QDir().mkpath(dest);
    FW_TRY(ensureFreeSpace(dest, g.bytes));
    zip::Reader r;
    FW_TRY(r.open(zipPath));
    if (progress) {
        progress->begin(QStringLiteral("Unpacking ZIP"), g.bytes, g.files);
        progress->setCurrentItem(QStringLiteral("Unpacking %1").arg(QFileInfo(zipPath).fileName()));
    }
    FW_TRY(r.extractAll(dest, token, progress, true));
    const QString root = QDir::cleanPath(QDir(dest).filePath(g.rootPrefix));
    if (!QFileInfo(root).isDir())
        return Error(ErrorCode::CorruptData, QStringLiteral("The game folder was not found after unpacking %1.").arg(nativePath(zipPath)));
    return root;
}

} // namespace fw::archive
