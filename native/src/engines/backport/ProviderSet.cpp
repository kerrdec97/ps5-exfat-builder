#include "engines/backport/ProviderSet.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/Common.h"
#include "engines/backport/ExecutableScanner.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace fw::backport {

bool isProviderLibraryName(const QString& fileName)
{
    static const QStringList names = {QStringLiteral("libsceampr.sprx"), QStringLiteral("libsceampr.prx"),
                                      QStringLiteral("libsceplaygo.sprx"), QStringLiteral("libsceplaygo.prx")};
    return names.contains(fileName.toLower());
}

QString defaultProviderPinsPath()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("backport/provider-pins.json"));
}

QStringList providerProblems(const QString& dir, const QString& game, FakelibSource* out, QStringList* ignored,
                             const CancellationToken& token)
{
    const QFileInfo di(dir);
    if (dir.trimmed().isEmpty() || !di.isDir())
        return {QStringLiteral("The AMPR/PlayGo folder does not exist: %1").arg(QDir::toNativeSeparators(dir))};
    if (!game.isEmpty() && (isSameOrInside(dir, game) || isSameOrInside(game, dir)))
        return {QStringLiteral("The AMPR/PlayGo folder must be outside the game folder.")};

    const QString root = QDir::cleanPath(di.absoluteFilePath());
    QStringList problems;
    QStringList other;
    FakelibSource set;
    set.dir = root;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    QStringList found;
    while (it.hasNext())
        found << it.next();
    found.sort();
    const QDir base(root);
    for (const QString& path : std::as_const(found)) {
        if (!token.checkpoint())
            return {QStringLiteral("Cancelled.")};
        const QString rel = base.relativeFilePath(path);
        const QStringList parts = rel.split(u'/');
        bool inMetadataDir = false;
        for (int i = 0; i + 1 < parts.size(); ++i)
            if (isOsMetadataDir(parts[i]))
                inMetadataDir = true;
        const QString name = parts.back();
        const QString low = name.toLower();
        if (inMetadataDir || isOsMetadataFile(name))
            continue;
        if (!isProviderLibraryName(name)) {
            if (low.endsWith(QStringLiteral(".sprx")) || low.endsWith(QStringLiteral(".prx")))
                other << rel;
            continue;
        }
        if (parts.size() > 1) {
            problems << QStringLiteral("%1 is in a sub-folder; the AMPR/PlayGo libraries must sit directly in the "
                                       "chosen folder.")
                            .arg(QDir::toNativeSeparators(rel));
            continue;
        }
        const qint64 size = QFileInfo(path).size();
        if (size < kMinFakelibBytes) {
            problems << QStringLiteral("%1 is only %2 bytes: a placeholder or an incomplete download, not a library.")
                            .arg(name)
                            .arg(size);
            continue;
        }
        auto head = readHead(path, 4);
        if (!head.ok() || !hasExecutableMagic(head.value())) {
            problems << QStringLiteral("%1 is not an ELF/SELF library.").arg(name);
            continue;
        }
        for (const FakelibLib& l : set.libs)
            if (QFileInfo(l.name).completeBaseName().compare(QFileInfo(name).completeBaseName(), Qt::CaseInsensitive) == 0)
                problems << QStringLiteral("%1 and %2 are both in the folder; keep only one of them.").arg(l.name, name);
        auto h = sha256File(path, token);
        if (!h.ok()) {
            problems << QStringLiteral("%1 cannot be read: %2").arg(name, h.error().message);
            continue;
        }
        set.libs.push_back({name, QFileInfo(path).absoluteFilePath(), size, h.value()});
    }
    if (set.libs.empty() && problems.isEmpty())
        problems << QStringLiteral("The folder contains neither libSceAmpr.sprx nor libScePlayGo.sprx.");
    if (ignored)
        *ignored = other;
    if (problems.isEmpty() && out) {
        std::sort(set.libs.begin(), set.libs.end(), [](const FakelibLib& a, const FakelibLib& b) { return a.name < b.name; });
        set.ignored = other;
        *out = set;
    }
    return problems;
}

namespace {

QJsonObject readPinsFile(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

QString shortHash(const QString& h)
{
    return h.left(8) + QStringLiteral("…");
}

} // namespace

std::optional<ProviderPins> loadProviderPins(const QString& pinsPath, const QString& folder)
{
    const QJsonObject doc = readPinsFile(pinsPath);
    for (const QJsonValue& v : doc.value(QStringLiteral("folders")).toArray()) {
        const QJsonObject o = v.toObject();
        if (!samePath(QDir::fromNativeSeparators(o.value(QStringLiteral("folder")).toString()), folder))
            continue;
        ProviderPins p;
        p.folder = folder;
        p.pinnedAt = o.value(QStringLiteral("pinned_at")).toString();
        const QJsonObject files = o.value(QStringLiteral("files")).toObject();
        for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
            const QJsonObject f = it.value().toObject();
            p.files.insert(it.key(), {f.value(QStringLiteral("sha256")).toString().toLower(),
                                      qint64(f.value(QStringLiteral("size")).toDouble())});
        }
        return p;
    }
    return std::nullopt;
}

QString ProviderPinCheck::describe() const
{
    QStringList parts;
    if (!changed.isEmpty())
        parts << QStringLiteral("changed: %1").arg(changed.join(QStringLiteral(", ")));
    if (!added.isEmpty())
        parts << QStringLiteral("not pinned: %1").arg(added.join(QStringLiteral(", ")));
    if (!missing.isEmpty())
        parts << QStringLiteral("pinned but missing: %1").arg(missing.join(QStringLiteral(", ")));
    return parts.join(QStringLiteral("; "));
}

ProviderPinCheck checkProviderPins(const FakelibSource& providers, const QString& pinsPath)
{
    ProviderPinCheck c;
    const auto pins = loadProviderPins(pinsPath, providers.dir);
    if (!pins)
        return c;
    c.pinned = true;
    c.pinnedAt = pins->pinnedAt;
    QStringList matched;
    for (const FakelibLib& l : providers.libs) {
        QString key;
        for (auto it = pins->files.cbegin(); it != pins->files.cend(); ++it)
            if (it.key().compare(l.name, Qt::CaseInsensitive) == 0)
                key = it.key();
        if (key.isEmpty()) {
            c.added << l.name;
            continue;
        }
        matched << key;
        const ProviderPin& p = pins->files.value(key);
        if (p.sha256 != l.sha256.toLower() || p.size != l.size)
            c.changed << QStringLiteral("%1 (pinned %2, now %3)").arg(l.name, shortHash(p.sha256), shortHash(l.sha256));
    }
    for (auto it = pins->files.cbegin(); it != pins->files.cend(); ++it)
        if (!matched.contains(it.key()))
            c.missing << it.key();
    return c;
}

Status pinProviders(const FakelibSource& providers, const QString& pinsPath)
{
    QJsonObject doc = readPinsFile(pinsPath);
    QJsonArray kept;
    for (const QJsonValue& v : doc.value(QStringLiteral("folders")).toArray())
        if (!samePath(QDir::fromNativeSeparators(v.toObject().value(QStringLiteral("folder")).toString()), providers.dir))
            kept.append(v);
    QJsonObject entry;
    entry.insert(QStringLiteral("folder"), QDir::toNativeSeparators(providers.dir));
    entry.insert(QStringLiteral("pinned_at"), QDateTime::currentDateTime().toString(Qt::ISODate));
    QJsonObject files;
    for (const FakelibLib& l : providers.libs) {
        QJsonObject f;
        f.insert(QStringLiteral("sha256"), l.sha256.toLower());
        f.insert(QStringLiteral("size"), double(l.size));
        files.insert(l.name, f);
    }
    entry.insert(QStringLiteral("files"), files);
    kept.append(entry);
    doc.insert(QStringLiteral("version"), 1);
    doc.insert(QStringLiteral("folders"), kept);
    if (!QDir().mkpath(QFileInfo(pinsPath).absolutePath()))
        return makeError(Code::Unreadable, QStringLiteral("Could not create the folder for %1.").arg(QDir::toNativeSeparators(pinsPath)));
    if (Status st = writeFileAtomic(pinsPath, QJsonDocument(doc).toJson(QJsonDocument::Indented)); !st.ok())
        return makeError(Code::Unreadable,
                         QStringLiteral("The AMPR/PlayGo pins could not be saved: %1").arg(st.error().message),
                         st.error().detail);
    return success();
}

FakelibSource withProviders(const std::optional<FakelibSource>& base, const FakelibSource& providers)
{
    FakelibSource out = base ? *base : FakelibSource{};
    if (!base)
        out.dir = providers.dir;
    for (const FakelibLib& p : providers.libs) {
        bool replaced = false;
        for (FakelibLib& l : out.libs) {
            if (l.name.compare(p.name, Qt::CaseInsensitive) == 0) {
                l.path = p.path;
                l.size = p.size;
                l.sha256 = p.sha256;
                replaced = true;
            }
        }
        if (!replaced)
            out.libs.push_back(p);
    }
    std::sort(out.libs.begin(), out.libs.end(), [](const FakelibLib& a, const FakelibLib& b) { return a.name < b.name; });
    return out;
}

} // namespace fw::backport
