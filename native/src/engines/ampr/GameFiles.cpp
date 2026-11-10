#include "engines/ampr/GameFiles.h"

#include "engines/ampr/Lz4Pack.h"
#include "engines/pfs/FolderAccess.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace fw::ampr {

namespace {

class FolderGame final : public GameFiles {
public:
    explicit FolderGame(QString root) : m_root(std::move(root)) {}

    Status scan(const CancellationToken& token)
    {
        QSet<QString> ancestors;
        qint64 n = 0;
        FW_TRY(walk(m_root, QString(), ancestors, n, token));
        std::sort(m_items.begin(), m_items.end(), [](const GameItem& a, const GameItem& b) { return a.path < b.path; });
        for (int i = 0; i < int(m_items.size()); ++i)
            m_index.insert(m_items[size_t(i)].path, i);
        return success();
    }

    QString describe() const override { return QStringLiteral("folder"); }
    const std::vector<GameItem>& items() const override { return m_items; }
    int find(const QString& path) const override { return m_index.value(path, -1); }
    QStringList warnings() const override { return m_warnings; }
    Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) override
    {
        if (index < 0 || size_t(index) >= m_items.size() || m_items[size_t(index)].isDir)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("No such file."));
        if (m_open != index) {
            m_file.close();
            m_file.setFileName(QDir(m_root).filePath(m_items[size_t(index)].path));
            if (!m_file.open(QIODevice::ReadOnly)) {
                m_open = -1;
                return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(m_file.fileName())),
                             m_file.errorString());
            }
            m_open = index;
        }
        if (!m_file.seek(offset))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(m_file.fileName())));
        qint64 done = 0;
        while (done < len) {
            const qint64 n = m_file.read(dst + done, len - done);
            if (n < 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(m_file.fileName())),
                             m_file.errorString());
            if (n == 0)
                break;
            done += n;
        }
        return done;
    }

private:
    // Symbolic links and junctions are followed, as the image builds' own
    // scans do, so a dump spread over two drives packs whole; a link that
    // leads back into its own folder or to nothing is left out with a
    // warning. A .lnk shortcut is an ordinary game file.
    Status walk(const QString& dir, const QString& rel, QSet<QString>& ancestors, qint64& n, const CancellationToken& token)
    {
        const QString identity = pfs::folderIdentity(dir);
        ancestors.insert(identity);
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        if (entries.isEmpty())
            FW_TRY(pfs::ensureFolderListable(dir)); // empty, or not allowed to look?
        for (const QFileInfo& fi : entries) {
            if ((++n & 1023) == 0 && !token.checkpoint())
                return Error::cancelled();
            const QString path = rel.isEmpty() ? fi.fileName() : rel + u'/' + fi.fileName();
            if ((fi.isSymbolicLink() || fi.isJunction()) && !fi.exists()) {
                m_warnings << QStringLiteral("Skipped %1: the link's target does not exist.").arg(path);
                continue;
            }
            GameItem item;
            item.path = path;
            item.isDir = fi.isDir();
            if (item.isDir && (fi.isSymbolicLink() || fi.isJunction())) {
                const QString target = pfs::folderIdentity(fi.absoluteFilePath());
                if (!target.isEmpty() && ancestors.contains(target)) {
                    m_warnings << QStringLiteral("Skipped %1: the link leads back into its own folder.").arg(path);
                    continue;
                }
            }
            item.size = item.isDir ? 0 : fi.size();
            item.mtime = fi.lastModified(QTimeZone::UTC).toSecsSinceEpoch();
            m_items.push_back(item);
            if (item.isDir)
                FW_TRY(walk(fi.absoluteFilePath(), path, ancestors, n, token));
        }
        ancestors.remove(identity);
        return success();
    }

    QString m_root;
    std::vector<GameItem> m_items;
    QHash<QString, int> m_index;
    QStringList m_warnings;
    QFile m_file;
    int m_open = -1;
};

class ItemAccess final : public RandomAccess {
public:
    ItemAccess(std::shared_ptr<GameFiles> files, int index) : m_files(std::move(files)), m_index(index) {}
    qint64 size() const override { return m_files->items()[size_t(m_index)].size; }
    Status readAt(qint64 offset, char* dst, qint64 len) override
    {
        qint64 done = 0;
        while (done < len) {
            auto n = m_files->read(m_index, offset + done, dst + done, len - done);
            if (!n.ok())
                return n.takeError();
            if (*n <= 0)
                return Error(ErrorCode::CorruptData,
                             QStringLiteral("%1 ends early.").arg(m_files->items()[size_t(m_index)].path));
            done += *n;
        }
        return success();
    }

private:
    std::shared_ptr<GameFiles> m_files;
    int m_index;
};

Result<QByteArray> readWhole(GameFiles& files, int index, qint64 maxBytes)
{
    const qint64 size = files.items()[size_t(index)].size;
    if (size > maxBytes)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is too large to be a pack index.").arg(files.items()[size_t(index)].path));
    QByteArray out(qsizetype(size), '\0');
    qint64 done = 0;
    while (done < size) {
        auto n = files.read(index, done, out.data() + done, size - done);
        if (!n.ok())
            return n.takeError();
        if (*n <= 0)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 ends early.").arg(files.items()[size_t(index)].path));
        done += *n;
    }
    return out;
}

} // namespace

Result<std::shared_ptr<GameFiles>> openFolderGame(const QString& folder, const CancellationToken& token)
{
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 is not a folder.").arg(QDir::toNativeSeparators(folder)));
    auto g = std::make_shared<FolderGame>(QFileInfo(folder).absoluteFilePath());
    FW_TRY(g->scan(token));
    return std::shared_ptr<GameFiles>(g);
}

std::shared_ptr<RandomAccess> itemAccess(std::shared_ptr<GameFiles> files, int index)
{
    return std::make_shared<ItemAccess>(std::move(files), index);
}

QString packKindName(PackKind kind)
{
    switch (kind) {
    case PackKind::Zstd: return QStringLiteral("zstd");
    case PackKind::Lz4: return QStringLiteral("LZ4");
    case PackKind::None: break;
    }
    return {};
}

PackKind detectPackKind(const GameFiles& files)
{
    const int z = files.find(kZstdIndexName);
    if (z >= 0 && !files.items()[size_t(z)].isDir)
        return PackKind::Zstd;
    const int l = files.find(kLz4IndexName);
    if (l >= 0 && !files.items()[size_t(l)].isDir)
        return PackKind::Lz4;
    return PackKind::None;
}

// ---- UnpackedGame ------------------------------------------------------------------------

class UnpackedGame::Decoder {
public:
    virtual ~Decoder() = default;
    virtual Result<qint64> read(int file, qint64 offset, char* dst, qint64 len) = 0;
};

namespace {

class ZstdDecoder final : public UnpackedGame::Decoder {
public:
    explicit ZstdDecoder(std::unique_ptr<ZstdPackReader> r) : m_r(std::move(r)) {}
    Result<qint64> read(int file, qint64 offset, char* dst, qint64 len) override { return m_r->read(file, offset, dst, len); }

private:
    std::unique_ptr<ZstdPackReader> m_r;
};

class Lz4Decoder final : public UnpackedGame::Decoder {
public:
    explicit Lz4Decoder(std::unique_ptr<Lz4PackReader> r) : m_r(std::move(r)) {}
    Result<qint64> read(int file, qint64 offset, char* dst, qint64 len) override { return m_r->read(file, offset, dst, len); }

private:
    std::unique_ptr<Lz4PackReader> m_r;
};

QString parentOf(const QString& path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash < 0 ? QString() : path.left(slash);
}

} // namespace

QStringList UnpackedGame::warnings() const { return m_base->warnings(); }

QString UnpackedGame::describe() const
{
    return QStringLiteral("%1 (%2 packs unpacked)").arg(m_base->describe(), packKindName(m_kind));
}

Result<std::shared_ptr<UnpackedGame>> UnpackedGame::open(std::shared_ptr<GameFiles> base, const CancellationToken& token)
{
    std::shared_ptr<UnpackedGame> g(new UnpackedGame);
    g->m_base = base;
    g->m_kind = detectPackKind(*base);
    if (g->m_kind == PackKind::None)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The game has no zstd or LZ4 packs."));

    // The packed files, as (path, size) in the decoder's file order.
    std::vector<std::pair<QString, qint64>> packed;
    QSet<QString> packFiles; // left out of the result
    qint64 indexMtime = 0;
    if (g->m_kind == PackKind::Zstd) {
        const int ix = base->find(kZstdIndexName);
        indexMtime = base->items()[size_t(ix)].mtime;
        // About 100 bytes per packed file: 256 MiB is far beyond any game and
        // keeps a file that only claims to be an index from costing 2 GiB.
        FW_TRY_ASSIGN(QByteArray bytes, readWhole(*base, ix, 256LL * 1024 * 1024));
        auto opener = [base](quint32 v) -> Result<std::shared_ptr<RandomAccess>> {
            const int i = base->find(zstdVolumeName(int(v)));
            if (i < 0 || base->items()[size_t(i)].isDir)
                return Error(ErrorCode::NotFound, QStringLiteral("%1 is missing from the packed game.").arg(zstdVolumeName(int(v))));
            return itemAccess(base, i);
        };
        FW_TRY_ASSIGN(auto reader, ZstdPackReader::open(bytes, opener));
        for (const auto& f : reader->index().files)
            packed.emplace_back(QString::fromUtf8(f.path), qint64(f.size));
        packFiles.insert(kZstdIndexName);
        for (quint32 v = 0; v < reader->index().volumeSizes.size(); ++v)
            packFiles.insert(zstdVolumeName(int(v)));
        g->m_decoder = std::make_shared<ZstdDecoder>(std::move(reader));
    } else {
        FW_TRY_ASSIGN(auto reader, Lz4PackReader::open(base, token));
        indexMtime = base->items()[size_t(base->find(kLz4IndexName))].mtime;
        for (int i = 0; i < reader->fileCount(); ++i)
            packed.emplace_back(reader->path(i), reader->size(i));
        for (const QString& p : reader->packFiles())
            packFiles.insert(p);
        g->m_decoder = std::make_shared<Lz4Decoder>(std::move(reader));
    }
    if (!token.checkpoint())
        return Error::cancelled();

    // Base items minus the pack files, then the packed files and their folders.
    // A file that is both loose and packed is served from the packs (the copy
    // the runtime would serve); the loose copy is left out.
    QSet<QString> packedFolded;
    for (const auto& p : packed)
        packedFolded.insert(p.first.toLower());
    for (int i = 0; i < int(base->items().size()); ++i) {
        const GameItem& it = base->items()[size_t(i)];
        if (!it.isDir && (packFiles.contains(it.path) || packedFolded.contains(it.path.toLower()))) {
            g->m_dropped << it.path;
            continue;
        }
        g->m_index.insert(it.path, int(g->m_items.size()));
        g->m_items.push_back(it);
        g->m_sources.push_back({false, i});
    }
    for (int i = 0; i < int(packed.size()); ++i) {
        const QString& path = packed[size_t(i)].first;
        if (g->m_index.contains(path) && !g->m_items[size_t(g->m_index.value(path))].isDir)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is listed twice in the packs.").arg(path));
        if (g->m_index.contains(path))
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is both a folder and a packed file.").arg(path));
        QString dir = parentOf(path);
        for (; !dir.isEmpty() && !g->m_index.contains(dir); dir = parentOf(dir)) {
            GameItem d;
            d.path = dir;
            d.isDir = true;
            d.mtime = indexMtime;
            g->m_index.insert(dir, int(g->m_items.size()));
            g->m_items.push_back(d);
            g->m_sources.push_back({false, -1});
        }
        if (!dir.isEmpty() && !g->m_items[size_t(g->m_index.value(dir))].isDir)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is both a file and a folder of packed files.").arg(dir));
        GameItem f;
        f.path = path;
        f.size = packed[size_t(i)].second;
        f.mtime = indexMtime;
        g->m_index.insert(path, int(g->m_items.size()));
        g->m_items.push_back(f);
        g->m_sources.push_back({true, i});
        ++g->m_packedFiles;
        g->m_packedBytes += f.size;
    }
    // Sorted by path like every other tree.
    std::vector<int> order(g->m_items.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return g->m_items[size_t(a)].path < g->m_items[size_t(b)].path; });
    std::vector<GameItem> items;
    std::vector<Source> sources;
    items.reserve(order.size());
    sources.reserve(order.size());
    g->m_index.clear();
    for (int i : order) {
        g->m_index.insert(g->m_items[size_t(i)].path, int(items.size()));
        items.push_back(g->m_items[size_t(i)]);
        sources.push_back(g->m_sources[size_t(i)]);
    }
    g->m_items = std::move(items);
    g->m_sources = std::move(sources);
    return g;
}

Result<qint64> UnpackedGame::read(int index, qint64 offset, char* dst, qint64 len)
{
    if (index < 0 || size_t(index) >= m_items.size() || m_items[size_t(index)].isDir)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No such file."));
    const Source& s = m_sources[size_t(index)];
    if (s.packed)
        return m_decoder->read(s.index, offset, dst, len);
    return m_base->read(s.index, offset, dst, len);
}

Result<std::shared_ptr<GameFiles>> logicalGame(std::shared_ptr<GameFiles> files, const CancellationToken& token)
{
    if (detectPackKind(*files) == PackKind::None)
        return files;
    FW_TRY_ASSIGN(auto u, UnpackedGame::open(std::move(files), token));
    return std::shared_ptr<GameFiles>(u);
}

} // namespace fw::ampr
