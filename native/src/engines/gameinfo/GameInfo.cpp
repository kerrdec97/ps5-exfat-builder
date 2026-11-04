#include "engines/gameinfo/GameInfo.h"

#include "engines/gameinfo/Naming.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <deque>

namespace fw::gameinfo {

namespace {

constexpr qint64 kMaxSmallText = 64 * 1024;

QFileInfoList sortedEntries(const QString& dir, QDir::Filters filters)
{
    return QDir(dir).entryInfoList(filters | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                   QDir::Name | QDir::IgnoreCase);
}

QFileInfoList sortedSubdirs(const QString& dir)
{
    return sortedEntries(dir, QDir::Dirs);
}

bool isLinkLike(const QFileInfo& fi)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return fi.isSymLink() || fi.isJunction();
#else
    return fi.isSymLink();
#endif
}

QByteArray readSmall(const QString& path, bool* ok)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    *ok = true;
    return f.read(kMaxSmallText);
}

// First line of a text file, like Python's readline().strip() / read().strip().split('\n')[0].
QString firstLine(const QByteArray& bytes)
{
    const QString text = QString::fromUtf8(bytes).trimmed();
    const qsizetype nl = text.indexOf(u'\n');
    return (nl < 0 ? text : text.left(nl)).trimmed();
}

const QStringList kNonGameDirs = {
    QStringLiteral("sce_sys"),     QStringLiteral("sce_module"), QStringLiteral("fakelib"),
    QStringLiteral("image"),       QStringLiteral("$recycle.bin"), QStringLiteral("system volume information"),
    QStringLiteral("__macosx"),
};

void decideKind(GameMetadata& m)
{
    if (m.paramSfo && m.platform == Platform::PS4) {
        const QString cat = m.paramSfo->string(QStringLiteral("CATEGORY")).trimmed().toLower();
        if (cat == u"gd" || cat == u"gde") {
            m.kind = ContentKind::Application;
            m.kindReason = QStringLiteral("param.sfo CATEGORY \"%1\"").arg(cat);
        } else if (cat == u"gp") {
            m.kind = ContentKind::Patch;
            m.kindReason = QStringLiteral("param.sfo CATEGORY \"gp\"");
        } else if (cat == u"ac") {
            m.kind = ContentKind::AddOn;
            m.kindReason = QStringLiteral("param.sfo CATEGORY \"ac\"");
        } else if (!cat.isEmpty()) {
            m.kindReason = QStringLiteral("param.sfo CATEGORY \"%1\" is not recognised").arg(cat);
        }
        return;
    }
    if (m.paramJson) {
        const auto& p = *m.paramJson;
        if (!m.hasEboot) {
            // An add-on with content carries its own sce_sys/param.json and no
            // executable (core/fpkg/dlc.py).
            m.kind = ContentKind::AddOn;
            m.kindReason = QStringLiteral("param.json without eboot.bin");
            return;
        }
        if (!p.applicationCategoryType || *p.applicationCategoryType == 0) {
            m.kind = ContentKind::Application;
            m.kindReason = p.applicationCategoryType ? QStringLiteral("applicationCategoryType 0 with eboot.bin")
                                                     : QStringLiteral("eboot.bin with param.json");
            return;
        }
        m.kindReason = QStringLiteral("applicationCategoryType %1 is not a game").arg(*p.applicationCategoryType);
        return;
    }
    if (m.hasEboot) {
        m.kind = ContentKind::Application;
        m.kindReason = QStringLiteral("eboot.bin");
    }
}

void decidePlatform(GameMetadata& m)
{
    // PS4 dumps never carry a param.json, so even a damaged one means PS5.
    if (m.paramJson || !m.paramJsonPath.isEmpty()) {
        m.platform = Platform::PS5;
        return;
    }
    const QString id = m.titleId.toUpper();
    if (id.startsWith(u"PPSA")) {
        m.platform = Platform::PS5;
        return;
    }
    if (id.startsWith(u"CUSA") || m.paramSfo) {
        m.platform = Platform::PS4;
        return;
    }
    if (m.hasEboot) {
        QFile f(QDir(m.folder).filePath(QStringLiteral("eboot.bin")));
        if (f.open(QIODevice::ReadOnly) && f.read(4) == QByteArray("\x54\x14\xF5\xEE", 4))
            m.platform = Platform::PS5; // retail PS5 SELF magic
    }
}

} // namespace

QString platformName(Platform p)
{
    switch (p) {
    case Platform::PS4: return QStringLiteral("PS4");
    case Platform::PS5: return QStringLiteral("PS5");
    case Platform::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QString contentKindName(ContentKind k)
{
    switch (k) {
    case ContentKind::Application: return QStringLiteral("Game");
    case ContentKind::Patch: return QStringLiteral("Patch");
    case ContentKind::AddOn: return QStringLiteral("Add-on");
    case ContentKind::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QString findMetaFile(const QString& folder, const QStringList& names)
{
    const QDir base(folder);
    for (const QString& sub : {QString(), QStringLiteral("sce_sys")})
        for (const QString& name : names) {
            const QString p = sub.isEmpty() ? base.filePath(name) : base.filePath(sub + u'/' + name);
            if (QFileInfo(p).isFile())
                return QDir::cleanPath(p);
        }
    // Depth-first walk, depth <= 5, like os.walk() with pruning.
    QStringList lower;
    for (const QString& n : names)
        lower << n.toLower();
    struct Item {
        QString path;
        int depth;
    };
    std::vector<Item> stack{{QDir::cleanPath(folder), 0}};
    int visited = 0;
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (++visited > 20000)
            break;
        const QFileInfoList files = sortedEntries(it.path, QDir::Files);
        for (const QString& want : lower)
            for (const QFileInfo& fi : files)
                if (fi.fileName().toLower() == want)
                    return fi.filePath();
        if (it.depth >= 5)
            continue;
        const QFileInfoList dirs = sortedSubdirs(it.path);
        for (auto d = dirs.crbegin(); d != dirs.crend(); ++d)
            if (!isLinkLike(*d))
                stack.push_back({d->filePath(), it.depth + 1});
    }
    return {};
}

bool hasEbootBin(const QString& folder)
{
    return QFileInfo(QDir(folder).filePath(QStringLiteral("eboot.bin"))).isFile();
}

Result<GameMetadata> readGameMetadata(const QString& folderIn)
{
    const QString folder = QDir::cleanPath(QFileInfo(folderIn).absoluteFilePath());
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound,
                     QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(folderIn)));
    GameMetadata m;
    m.folder = folder;
    m.hasEboot = hasEbootBin(folder);

    QString title, titleId, version;

    // 1. param.sfo
    m.paramSfoPath = findMetaFile(folder, {QStringLiteral("param.sfo")});
    if (!m.paramSfoPath.isEmpty()) {
        auto sfo = readSfo(m.paramSfoPath);
        if (sfo.ok()) {
            m.paramSfo = sfo.value();
            const SfoFile& s = *m.paramSfo;
            title = s.string(QStringLiteral("TITLE"));
            if (title.isEmpty())
                title = s.string(QStringLiteral("TITLE_00"));
            if (const SfoEntry* e = s.find(QStringLiteral("TITLE_ID")); e && !e->isInteger())
                titleId = e->isString() ? e->text() : QString();
            version = s.string(QStringLiteral("VERSION"));
            if (version.isEmpty())
                version = s.string(QStringLiteral("APP_VER"));
            if (!title.isEmpty())
                m.titleSource = QStringLiteral("param.sfo");
            if (s.skippedEntries > 0)
                m.notes << QStringLiteral("param.sfo: %1 damaged entries were skipped.").arg(s.skippedEntries);
        } else {
            m.notes << QStringLiteral("param.sfo could not be read: %1").arg(sfo.error().toLogString());
        }
    }
    const QString sfoVersion = version;

    // 2. pfs-version.dat overrides the version.
    m.pfsVersionPath = findMetaFile(folder, {QStringLiteral("pfs-version.dat")});
    if (!m.pfsVersionPath.isEmpty()) {
        bool ok = false;
        const QString line = firstLine(readSmall(m.pfsVersionPath, &ok));
        if (ok && !line.isEmpty() && (line.front().isDigit() || line.front() == u'.'))
            version = line;
    }

    // 3. param.json (always parsed for the extra facts; used for the legacy
    //    fields only when the SFO gave neither title nor ID).
    m.paramJsonPath = findMetaFile(folder, {QStringLiteral("param.json")});
    if (!m.paramJsonPath.isEmpty()) {
        auto pj = readParamJson(m.paramJsonPath);
        if (pj.ok()) {
            m.paramJson = pj.value();
            if (m.paramJson->repaired)
                m.notes << QStringLiteral("param.json is not strict JSON; it was read leniently.");
        } else {
            m.notes << QStringLiteral("param.json could not be read: %1").arg(pj.error().toLogString());
        }
    }
    if (title.isEmpty() && titleId.isEmpty() && m.paramJson) {
        const ParamJson& p = *m.paramJson;
        if (!p.title.isEmpty()) {
            title = p.title;
            m.titleSource = QStringLiteral("param.json");
        }
        titleId = p.titleId;
        if (sfoVersion.isEmpty() && version.isEmpty())
            version = p.version;
    }

    // 4. nptitle.dat (title only).
    if (title.isEmpty()) {
        m.npTitlePath = findMetaFile(folder, {QStringLiteral("nptitle.dat")});
        if (!m.npTitlePath.isEmpty()) {
            bool ok = false;
            const QByteArray bytes = readSmall(m.npTitlePath, &ok);
            const qsizetype nl = bytes.indexOf('\n');
            if (ok && !bytes.left(nl < 0 ? bytes.size() : nl).contains('\0')) {
                title = firstLine(bytes);
                if (!title.isEmpty())
                    m.titleSource = QStringLiteral("nptitle.dat");
            } else if (ok) {
                m.notes << QStringLiteral("nptitle.dat is binary; it was not used as a title.");
            }
        }
    }

    // 5. normalise.
    m.rawTitle = title.trimmed();
    m.title = m.rawTitle.isEmpty() ? QString() : sanitizeTitle(m.rawTitle);
    m.titleId = titleId.trimmed();
    m.version = normalizeContentVersion(version);

    // Extra facts.
    if (m.paramJson) {
        m.contentId = m.paramJson->contentId;
        m.contentVersion = m.paramJson->contentVersion;
        m.sdkVersion = m.paramJson->sdkVersion;
        m.requiredSystemVersion = m.paramJson->requiredSystemVersion;
    }
    if (m.paramSfo) {
        if (m.contentId.isEmpty())
            m.contentId = m.paramSfo->string(QStringLiteral("CONTENT_ID")).trimmed();
        for (const char* key : {"SYSTEM_VER", "SDK_VER", "SYSTEM_VERSION"}) {
            if (m.sdkVersion.isValid())
                break;
            if (auto v = m.paramSfo->integer(QLatin1String(key)))
                m.sdkVersion = decodeSystemVersion(*v);
        }
        if (!m.requiredSystemVersion.isValid())
            if (auto v = m.paramSfo->integer(QStringLiteral("SYSTEM_VER")))
                m.requiredSystemVersion = decodeSystemVersion(*v);
    }
    decidePlatform(m);
    decideKind(m);
    return m;
}

QJsonObject GameMetadata::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("folder"), QDir::toNativeSeparators(folder));
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("titleId"), titleId);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("titleSource"), titleSource);
    o.insert(QStringLiteral("contentId"), contentId);
    o.insert(QStringLiteral("contentVersion"), contentVersion);
    o.insert(QStringLiteral("platform"), platformName(platform));
    o.insert(QStringLiteral("kind"), contentKindName(kind));
    o.insert(QStringLiteral("kindReason"), kindReason);
    o.insert(QStringLiteral("sdkVersion"), sdkVersion.toString());
    o.insert(QStringLiteral("requiredSystemVersion"), requiredSystemVersion.toString());
    o.insert(QStringLiteral("hasEboot"), hasEboot);
    if (paramJson)
        o.insert(QStringLiteral("paramJson"), paramJson->raw);
    if (paramSfo) {
        QJsonObject sfo;
        for (const auto& e : paramSfo->entries)
            sfo.insert(e.key, e.isInteger() ? QJsonValue(qint64(e.integer()))
                              : e.isString() ? QJsonValue(e.text())
                                             : QJsonValue(QString::fromLatin1(e.raw.toHex())));
        o.insert(QStringLiteral("paramSfo"), sfo);
    }
    if (!notes.isEmpty())
        o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(notes));
    return o;
}

RootSearch findGameRoots(const QString& folder, const CancellationToken& token, int maxDepth, int maxDirs)
{
    RootSearch out;
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return out;
    const QString base = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    if (hasEbootBin(base)) {
        out.root = base;
        out.candidates << base;
        out.scanned = 1;
        return out;
    }
    std::deque<std::pair<QString, int>> queue{{base, 0}};
    QSet<QString> seen;
    int scanned = 0;
    while (!queue.empty()) {
        if (token.isCancelled()) {
            out.cancelled = true;
            break;
        }
        auto [path, depth] = queue.front();
        queue.pop_front();
        if (++scanned > maxDirs) {
            out.truncated = true;
            break;
        }
        if (depth > 0 && hasEbootBin(path)) {
            if (!seen.contains(path)) {
                seen.insert(path);
                out.candidates << path;
            }
            continue;
        }
        if (depth >= maxDepth) {
            out.depthCapped = true;
            continue;
        }
        for (const QFileInfo& sub : sortedSubdirs(path)) {
            const QString nm = sub.fileName().toLower();
            if (nm.startsWith(u'.') || kNonGameDirs.contains(nm))
                continue;
            queue.emplace_back(sub.filePath(), depth + 1);
        }
    }
    out.scanned = qMin(scanned, maxDirs);
    if (out.candidates.size() == 1)
        out.root = out.candidates.front();
    return out;
}

QString findLibraryGameRoot(const QString& wrapper)
{
    if (hasEbootBin(wrapper))
        return QDir::cleanPath(wrapper);
    const QFileInfoList children = sortedSubdirs(wrapper);
    for (const QFileInfo& c : children)
        if (hasEbootBin(c.filePath()))
            return c.filePath();
    for (const QFileInfo& c : children)
        for (const QFileInfo& g : sortedSubdirs(c.filePath()))
            if (hasEbootBin(g.filePath()))
                return g.filePath();
    return {};
}

QString findRenameGameRoot(const QString& path)
{
    if (hasEbootBin(path))
        return QDir::cleanPath(path);
    for (const QFileInfo& c : sortedSubdirs(path))
        if (hasEbootBin(c.filePath()))
            return c.filePath();
    if (!findTitleIdIn(QFileInfo(path).fileName()).isEmpty())
        return QDir::cleanPath(path);
    return {};
}

qint64 dumpSize(const QString& folder, const CancellationToken& token)
{
    qint64 total = 0;
    qint64 n = 0;
    std::vector<QString> stack{folder};
    while (!stack.empty()) {
        const QString cur = stack.back();
        stack.pop_back();
        const QFileInfoList entries = QDir(cur).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
        for (const QFileInfo& fi : entries) {
            if ((++n & 511) == 0 && token.isCancelled())
                return -1;
            if (isLinkLike(fi))
                continue;
            if (fi.isDir())
                stack.push_back(fi.filePath());
            else if (fi.isFile())
                total += fi.size();
        }
    }
    return total;
}

QString findCoverSource(const QString& folderIn)
{
    const QString folder = QDir::cleanPath(folderIn);
    const QDir base(folder);
    for (const QString& rel : {QStringLiteral("sce_sys/icon0.png"), QStringLiteral("icon0.png")}) {
        const QString p = base.filePath(rel);
        if (QFileInfo(p).isFile())
            return p;
    }
    struct Item {
        QString path;
        int depth;
    };
    std::vector<Item> stack{{folder, 0}};
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        for (const QFileInfo& fi : sortedEntries(it.path, QDir::Files))
            if (fi.fileName().compare(QLatin1String("icon0.png"), Qt::CaseInsensitive) == 0)
                return fi.filePath();
        if (it.depth >= 4)
            continue;
        const QFileInfoList dirs = sortedSubdirs(it.path);
        for (auto d = dirs.crbegin(); d != dirs.crend(); ++d) {
            const QString nm = d->fileName().toLower();
            if (nm == u"decrypted" || nm == u"fakelib" || isLinkLike(*d))
                continue;
            stack.push_back({d->filePath(), it.depth + 1});
        }
    }
    return {};
}

} // namespace fw::gameinfo
