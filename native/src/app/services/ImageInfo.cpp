#include "app/services/ImageInfo.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/gameinfo/ParamJson.h"
#include "engines/gameinfo/Sfo.h"
#include "engines/integration/ImageFiles.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <climits>
#include <mutex>

namespace fw::ui {

namespace {

std::mutex g_cacheMutex;
QHash<QString, ImageSummary>& cache()
{
    static QHash<QString, ImageSummary> c;
    return c;
}

QString cacheKey(const QFileInfo& fi)
{
    return QStringLiteral("%1|%2|%3").arg(QDir::cleanPath(fi.absoluteFilePath())).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
}

// The shortest path in the tree that ends with `suffix` (case-insensitive):
// "sce_sys/param.json" at the root, or one folder down in a folder container.
int findMeta(const integration::FileTree& tree, const QString& suffix)
{
    int best = tree.find(suffix);
    if (best >= 0)
        return best;
    int bestLen = INT_MAX;
    const auto& items = tree.items();
    for (int i = 0; i < int(items.size()); ++i) {
        const auto& it = items[size_t(i)];
        if (it.isDir || !it.path.endsWith(suffix, Qt::CaseInsensitive))
            continue;
        if (it.path.size() > suffix.size() && it.path.at(it.path.size() - suffix.size() - 1) != u'/')
            continue;
        if (it.path.count(u'/') > 3 || it.path.size() >= bestLen)
            continue;
        best = i;
        bestLen = int(it.path.size());
    }
    return best;
}

QByteArray readWhole(integration::FileTree& tree, int index, qint64 limit)
{
    const qint64 size = tree.items()[size_t(index)].size;
    if (size <= 0 || size > limit)
        return {};
    QByteArray data(qsizetype(size), '\0');
    auto r = tree.read(index, 0, data.data(), size);
    if (!r.ok())
        return {};
    data.truncate(qsizetype(r.value()));
    return data;
}

QString cachedCover(const QString& image, const QByteArray& png)
{
    if (png.isEmpty())
        return {};
    const QString dir = QDir(Paths::cacheDir()).filePath(QStringLiteral("covers"));
    if (!QDir().mkpath(dir))
        return {};
    const QByteArray hash = QCryptographicHash::hash(image.toUtf8() + png.left(4096), QCryptographicHash::Sha1).toHex().left(20);
    const QString file = QDir(dir).filePath(QStringLiteral("image-%1.png").arg(QString::fromLatin1(hash)));
    if (QFileInfo(file).size() == png.size())
        return file;
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly) || f.write(png) != png.size())
        return {};
    return file;
}

QString sfoVersion(const gameinfo::SfoFile& sfo)
{
    QString v = sfo.string(QStringLiteral("APP_VER"));
    if (v.isEmpty())
        v = sfo.string(QStringLiteral("VERSION"));
    return v;
}

} // namespace

// ---- names ----------------------------------------------------------------------------

QString imageFormatOf(const QString& path, bool withPackages)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"exfat" || ext == u"ffpkg" || ext == u"ffpfs" || ext == u"ffpfsc")
        return ext;
    if (withPackages && ext == u"pkg")
        return ext;
    return {};
}

QString imageFormatName(const QString& format)
{
    if (format == u"exfat")
        return QStringLiteral("exFAT");
    if (format == u"ffpkg")
        return QStringLiteral("FFPKG");
    if (format == u"ffpfs")
        return QStringLiteral("FFPFS");
    if (format == u"ffpfsc")
        return QStringLiteral("FFPFSC");
    if (format == u"pkg")
        return QStringLiteral("FPKG");
    if (format == u"folder")
        return QStringLiteral("Folder");
    return format.toUpper();
}

QString imageFormatIcon(const QString& format)
{
    if (format == u"exfat")
        return QStringLiteral("drive");
    if (format == u"ffpkg")
        return QStringLiteral("package");
    if (format == u"ffpfs")
        return QStringLiteral("layers");
    if (format == u"ffpfsc")
        return QStringLiteral("compress");
    if (format == u"pkg")
        return QStringLiteral("package_check");
    return QStringLiteral("folder");
}

QString ImageSummary::formatName() const { return imageFormatName(format); }

QString ImageSummary::displayTitle() const
{
    return title.isEmpty() ? QFileInfo(path).completeBaseName() : title;
}

QString ImageSummary::metaLine() const
{
    QStringList parts{formatName()};
    if (bytes > 0)
        parts << formatBytes(bytes);
    if (!titleId.isEmpty())
        parts << titleId;
    if (!version.isEmpty())
        parts << QStringLiteral("v%1").arg(version);
    if (files >= 0)
        parts << QStringLiteral("%1 file%2").arg(formatCount(files), files == 1 ? QString() : QStringLiteral("s"));
    return parts.join(QStringLiteral(" · "));
}

void guessFromFileName(ImageSummary& s)
{
    const QString base = QFileInfo(s.inner.isEmpty() ? s.path : s.inner).completeBaseName();
    static const QRegularExpression id(QStringLiteral("\\b([A-Z]{4})[-_ ]?([0-9]{5})\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ver(QStringLiteral("(?:\\bv|\\(|_v|\\s)([0-9]{2}\\.[0-9]{2,3}(?:\\.[0-9]{3})?)\\)?"), QRegularExpression::CaseInsensitiveOption);
    QString rest = base;
    if (s.titleId.isEmpty()) {
        const auto m = id.match(base);
        if (m.hasMatch()) {
            s.titleId = (m.captured(1) + m.captured(2)).toUpper();
            rest.remove(m.capturedStart(0), m.capturedLength(0));
        }
    }
    if (s.version.isEmpty()) {
        const auto m = ver.match(rest);
        if (m.hasMatch()) {
            s.version = m.captured(1);
            rest.remove(m.capturedStart(0), m.capturedLength(0));
        }
    }
    if (s.title.isEmpty()) {
        rest.replace(u'_', u' ');
        rest.remove(QRegularExpression(QStringLiteral("[\\(\\)\\[\\]]")));
        rest = rest.simplified();
        if (rest.startsWith(u'-'))
            rest = rest.mid(1).trimmed();
        if (rest.size() >= 2 && rest != base)
            s.title = rest;
    }
}

// ---- reading --------------------------------------------------------------------------

ImageSummary readImageSummary(const QString& path, bool readInside, const CancellationToken& token)
{
    ImageSummary s;
    s.path = path;
    s.format = imageFormatOf(path, true);
    const QFileInfo fi(path);
    s.exists = fi.isFile();
    if (!s.exists) {
        s.error = QStringLiteral("This file does not exist.");
        guessFromFileName(s);
        return s;
    }
    s.bytes = fi.size();
    s.modified = fi.lastModified();
    const QString key = cacheKey(fi);
    if (readInside) {
        std::lock_guard lock(g_cacheMutex);
        if (auto it = cache().constFind(key); it != cache().constEnd())
            return it.value();
    }
    if (readInside && s.format != u"pkg" && !s.format.isEmpty()) {
        auto tree = integration::openFileTree(path, token);
        if (!tree.ok()) {
            s.error = tree.error().message;
        } else {
            integration::FileTree& t = *tree.value();
            s.files = t.fileCount();
            s.dirs = t.dirCount();
            s.contentBytes = t.totalBytes();
            if (t.fileCount() == 1 && t.items().size() == 1 && !imageFormatOf(t.items().front().path).isEmpty())
                s.inner = t.items().front().path;
            if (!t.innerImage().isEmpty())
                s.inner = t.innerImage(); // opened straight through to it
            if (t.find(QStringLiteral("ampr_zstd.index")) >= 0)
                s.packs = QStringLiteral("zstd");
            else if (t.find(QStringLiteral("ampr_assets.index")) >= 0)
                s.packs = QStringLiteral("LZ4");
            if (const int pj = findMeta(t, QStringLiteral("sce_sys/param.json")); pj >= 0) {
                auto p = gameinfo::parseParamJson(readWhole(t, pj, 4 * 1024 * 1024));
                if (p.ok()) {
                    s.title = p->title;
                    s.titleId = p->titleId;
                    s.version = p->contentVersion.isEmpty() ? p->version : p->contentVersion;
                    s.contentId = p->contentId;
                    s.fromImage = !s.titleId.isEmpty() || !s.title.isEmpty();
                }
            }
            if (!s.fromImage) {
                if (const int sf = findMeta(t, QStringLiteral("sce_sys/param.sfo")); sf >= 0) {
                    auto sfo = gameinfo::parseSfo(readWhole(t, sf, 1024 * 1024));
                    if (sfo.ok()) {
                        s.title = sfo->string(QStringLiteral("TITLE"));
                        s.titleId = sfo->string(QStringLiteral("TITLE_ID"));
                        s.version = sfoVersion(sfo.value());
                        s.contentId = sfo->string(QStringLiteral("CONTENT_ID"));
                        s.fromImage = !s.titleId.isEmpty() || !s.title.isEmpty();
                    }
                }
            }
            if (const int ic = findMeta(t, QStringLiteral("sce_sys/icon0.png")); ic >= 0)
                s.coverPath = cachedCover(path, readWhole(t, ic, 8 * 1024 * 1024));
        }
    }
    guessFromFileName(s);
    if (readInside && !token.isCancelled()) {
        std::lock_guard lock(g_cacheMutex);
        cache().insert(key, s);
    }
    return s;
}

QStringList findImageFiles(const QStringList& folders, const CancellationToken& token)
{
    QStringList out;
    QSet<QString> seen;
    const auto add = [&](const QFileInfo& fi) {
        const QString p = QDir::cleanPath(fi.absoluteFilePath());
        if (!seen.contains(p.toLower())) {
            seen.insert(p.toLower());
            out << p;
        }
    };
    const QStringList filters = {QStringLiteral("*.exfat"), QStringLiteral("*.ffpkg"), QStringLiteral("*.ffpfs"), QStringLiteral("*.ffpfsc")};
    for (const QString& folder : folders) {
        if (token.isCancelled())
            break;
        const QDir d(folder);
        if (!d.exists())
            continue;
        for (const QFileInfo& fi : d.entryInfoList(filters, QDir::Files))
            add(fi);
        for (const QFileInfo& sub : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (token.isCancelled())
                break;
            for (const QFileInfo& fi : QDir(sub.absoluteFilePath()).entryInfoList(filters, QDir::Files))
                add(fi);
        }
    }
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

// ---- async ----------------------------------------------------------------------------

ImageSummaryReader::ImageSummaryReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ImageSummary>();
}

void ImageSummaryReader::read(const QString& path)
{
    const quint64 gen = ++m_generation;
    m_busy = true;
    QPointer<ImageSummaryReader> self(this);
    (void)QtConcurrent::run([self, path, gen] {
        ImageSummary s = readImageSummary(path, true);
        QMetaObject::invokeMethod(self.data(), [self, s, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->ready(s);
        }, Qt::QueuedConnection);
    });
}

ImageScanner::ImageScanner(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ImageSummary>();
}

ImageScanner::~ImageScanner()
{
    if (m_cancel)
        m_cancel->cancel();
}

void ImageScanner::scan(const QStringList& folders)
{
    if (m_cancel)
        m_cancel->cancel();
    m_cancel = std::make_unique<CancellationSource>();
    const CancellationToken token = m_cancel->token();
    const quint64 gen = ++m_generation;
    m_busy = true;
    QPointer<ImageScanner> self(this);
    (void)QtConcurrent::run([self, folders, gen, token] {
        QList<ImageSummary> out;
        for (const QString& f : findImageFiles(folders, token)) {
            if (token.isCancelled())
                return;
            out << readImageSummary(f, true, token);
        }
        QMetaObject::invokeMethod(self.data(), [self, out, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->finished(out);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
