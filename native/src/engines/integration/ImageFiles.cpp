#include "engines/integration/ImageFiles.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatReader.h"
#include "core/FolderAccess.h"
#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsReader.h"
#include "engines/ufs2/Ufs2Reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace fw::integration {

namespace {

QString nativeName(const QString& path) { return QDir::toNativeSeparators(path); }

// ---- folder ---------------------------------------------------------------------------

class FolderTree final : public FileTree {
public:
    explicit FolderTree(const QString& root) : FileTree(TreeFormat::Folder, root) {}

    Status scan(const CancellationToken& token)
    {
        QSet<QString> ancestors;
        qint64 n = 0;
        FW_TRY(walk(m_root, QString(), ancestors, n, token));
        finish();
        return success();
    }

    Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) override
    {
        const TreeItem& it = m_items[size_t(index)];
        if (offset >= it.size || len <= 0)
            return qint64(0);
        if (m_openIndex != index) {
            m_file.close();
            m_file.setFileName(toLongPath(m_root + u'/' + it.path));
            if (!m_file.open(QIODevice::ReadOnly))
                return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(nativeName(m_root + u'/' + it.path)),
                             m_file.errorString());
            m_openIndex = index;
        }
        if (!m_file.seek(offset))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(nativeName(m_file.fileName())), m_file.errorString());
        len = std::min(len, it.size - offset);
        qint64 got = 0;
        while (got < len) {
            const qint64 r = m_file.read(buf + got, len - got);
            if (r < 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(nativeName(m_file.fileName())),
                             m_file.errorString());
            if (r == 0)
                break; // the file shrank since the scan
            got += r;
        }
        return got;
    }

private:
    Status walk(const QString& dir, const QString& rel, QSet<QString>& ancestors, qint64& n, const CancellationToken& token)
    {
        // The folder's identity: a junction's canonical path is its own, so it
        // would not show a junction that leads back into the folders above it.
        QString canonical = folderIdentity(dir);
        if (canonical.isEmpty())
            canonical = QFileInfo(dir).canonicalFilePath();
        if (ancestors.contains(canonical)) {
            m_warnings << QStringLiteral("Skipped %1: the link leads back into its own folder.").arg(rel);
            return success();
        }
        ancestors.insert(canonical);
        const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        if (entries.isEmpty())
            FW_TRY(ensureFolderListable(dir)); // empty, or not allowed to look?
        for (const QFileInfo& fi : entries) {
            if ((++n & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            const QString path = rel.isEmpty() ? fi.fileName() : rel + u'/' + fi.fileName();
            if (fi.isSymLink() && !fi.exists()) {
                m_warnings << QStringLiteral("Skipped %1: the link's target does not exist.").arg(path);
                continue;
            }
            TreeItem item;
            item.path = path;
            if (fi.isDir()) {
                item.isDir = true;
                m_items.push_back(item);
                FW_TRY(walk(fi.filePath(), path, ancestors, n, token));
            } else if (fi.isFile()) {
                item.size = fi.size();
                m_items.push_back(item);
            } else {
                m_warnings << QStringLiteral("Skipped %1: not a regular file.").arg(path);
            }
        }
        ancestors.remove(canonical);
        return success();
    }

    QFile m_file;
    int m_openIndex = -1;
};

// ---- exFAT ----------------------------------------------------------------------------

class ExfatTree final : public FileTree {
public:
    explicit ExfatTree(const QString& image) : FileTree(TreeFormat::Exfat, image) {}

    Status open(const CancellationToken& token)
    {
        FW_TRY_ASSIGN(m_reader, exfat::ExfatReader::open(m_root));
        return load(token);
    }
    // An exFAT image inside a container; `keep` owns what the source reads from.
    Status open(std::shared_ptr<RandomSource> source, std::shared_ptr<void> keep, const CancellationToken& token)
    {
        m_keep = std::move(keep);
        m_formatName = QStringLiteral("exFAT image (%1)").arg(source->describe());
        FW_TRY_ASSIGN(m_reader, exfat::ExfatReader::open(std::move(source)));
        return load(token);
    }

private:
    Status load(const CancellationToken& token)
    {
        FW_TRY_ASSIGN(m_tree, m_reader->readTree(token, false));
        if (!m_tree.problems.isEmpty())
            m_warnings << QStringLiteral("%1 is damaged (%2 problem%3), for example: %4")
                              .arg(QFileInfo(m_root).fileName())
                              .arg(m_tree.problemCount)
                              .arg(m_tree.problemCount == 1 ? QString() : QStringLiteral("s"))
                              .arg(m_tree.problems.first());
        m_items.reserve(m_tree.entries.size());
        for (size_t i = 1; i < m_tree.entries.size(); ++i) {
            const exfat::ImageEntry& e = m_tree.entries[i];
            TreeItem item;
            item.path = e.path;
            item.isDir = e.isDir;
            item.size = e.isDir ? 0 : e.size;
            item.handle = qint64(i);
            m_items.push_back(item);
        }
        m_stream = m_reader->openStream();
        FW_TRY(m_stream->open());
        finish();
        return success();
    }

public:
    Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) override
    {
        const TreeItem& it = m_items[size_t(index)];
        return m_stream->read(m_tree.entries[size_t(it.handle)], offset, buf, len);
    }

private:
    std::shared_ptr<void> m_keep;
    std::unique_ptr<exfat::ExfatReader> m_reader;
    exfat::ImageTree m_tree;
    std::unique_ptr<exfat::ImageFileStream> m_stream;
};

// ---- FFPKG (UFS2) -----------------------------------------------------------------------

class Ufs2FileTree final : public FileTree {
public:
    explicit Ufs2FileTree(const QString& image) : FileTree(TreeFormat::Ffpkg, image) {}

    Status open(const CancellationToken& token)
    {
        FW_TRY_ASSIGN(m_reader, ufs2::Ufs2Reader::open(m_root));
        return load(token);
    }
    Status open(std::shared_ptr<RandomSource> source, std::shared_ptr<void> keep, const CancellationToken& token)
    {
        m_keep = std::move(keep);
        m_formatName = QStringLiteral("FFPKG (UFS2) image (%1)").arg(source->describe());
        FW_TRY_ASSIGN(m_reader, ufs2::Ufs2Reader::open(std::move(source)));
        return load(token);
    }

    Status load(const CancellationToken& token)
    {
        QStringList skipped;
        FW_TRY_ASSIGN(ufs2::SourceTree tree, m_reader->loadTree(token, &skipped));
        for (const QString& s : skipped)
            m_warnings << QStringLiteral("Skipped %1: a link or special file.").arg(s);
        m_items.reserve(tree.nodes.size());
        for (size_t i = 1; i < tree.nodes.size(); ++i) {
            const ufs2::TreeNode& n = tree.nodes[i];
            if (n.removed)
                continue;
            TreeItem item;
            item.path = tree.relativePath(int(i));
            item.isDir = n.isDir();
            item.size = n.isDir() ? 0 : n.size;
            item.handle = qint64(n.imageIno);
            m_items.push_back(item);
        }
        finish();
        return success();
    }

    Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) override
    {
        const TreeItem& it = m_items[size_t(index)];
        if (offset >= it.size || len <= 0)
            return qint64(0);
        if (m_extentsIndex != index) {
            const auto ino = quint32(it.handle);
            FW_TRY_ASSIGN(ufs2::Inode in, m_reader->readInode(ino));
            FW_TRY_ASSIGN(m_extents, m_reader->extents(in, ino));
            m_extentsIndex = index;
        }
        len = std::min(len, it.size - offset);
        // The extents cover [0, size) in order; find the one holding `offset`.
        auto at = std::upper_bound(m_extents.begin(), m_extents.end(), offset,
                                   [](qint64 off, const ufs2::FileExtent& e) { return off < e.fileOffset; });
        if (at == m_extents.begin())
            return Error(ErrorCode::CorruptData, QStringLiteral("The image is damaged: %1 has no data at the start.").arg(it.path));
        --at;
        qint64 done = 0;
        while (done < len && at != m_extents.end()) {
            const qint64 pos = offset + done;
            const qint64 within = pos - at->fileOffset;
            if (within >= at->length) {
                ++at;
                continue;
            }
            const qint64 n = std::min(len - done, at->length - within);
            if (at->imageOffset < 0)
                std::memset(buf + done, 0, size_t(n));
            else
                FW_TRY(m_reader->readAt(at->imageOffset + within, buf + done, n));
            done += n;
            ++at;
        }
        if (done < len)
            return Error(ErrorCode::CorruptData, QStringLiteral("The image is damaged: %1 is shorter than its size.").arg(it.path));
        return done;
    }

private:
    std::shared_ptr<void> m_keep;
    std::shared_ptr<ufs2::Ufs2Reader> m_reader;
    std::vector<ufs2::FileExtent> m_extents;
    int m_extentsIndex = -1;
};

// ---- PFS / FFPFSC -----------------------------------------------------------------------

class PfsFileTree final : public FileTree {
public:
    explicit PfsFileTree(const QString& image) : FileTree(TreeFormat::Pfs, image) {}

    Status open(const CancellationToken& token)
    {
        m_formatName = QFileInfo(m_root).suffix().toLower() == u"ffpfs" ? QStringLiteral("FFPFS image")
                                                                       : QStringLiteral("FFPFSC image");
        m_file = std::make_shared<pfs::FileByteSource>(m_root);
        FW_TRY(m_file->open());
        FW_TRY_ASSIGN(m_outer, pfs::PfsImage::open(m_file));
        m_warnings << m_outer->warnings();
        if (!token.checkpoint())
            return Error::cancelled();
        if (looksNested(*m_outer)) {
            FW_TRY_ASSIGN(auto src, m_outer->openFile(m_outer->inodes()[m_outer->files().front().inode]));
            FW_TRY_ASSIGN(m_inner, pfs::PfsImage::open(src));
            m_warnings << m_inner->warnings();
        }
        const pfs::PfsImage& img = target();
        const qint64 bs = img.header().blockSize;
        m_items.reserve(img.dirs().size() + img.files().size());
        for (const pfs::PfsEntry& d : img.dirs()) {
            TreeItem item;
            item.path = QString::fromUtf8(d.path);
            item.isDir = true;
            m_items.push_back(item);
        }
        for (const pfs::PfsEntry& f : img.files()) {
            const pfs::Inode& ino = img.inodes()[f.inode];
            TreeItem item;
            item.path = QString::fromUtf8(f.path);
            item.size = ino.logicalSize();
            item.padBlock = ino.isCompressed() ? bs : 0;
            item.handle = qint64(f.inode);
            m_items.push_back(item);
        }
        finish();
        return success();
    }

    Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) override
    {
        const TreeItem& it = m_items[size_t(index)];
        if (offset >= it.size || len <= 0)
            return qint64(0);
        if (m_sourceIndex != index) {
            m_source.reset();
            FW_TRY_ASSIGN(m_source, target().openFile(target().inodes()[size_t(it.handle)]));
            m_sourceIndex = index;
        }
        len = std::min(len, it.size - offset);
        FW_TRY(m_source->readAt(offset, buf, len));
        return len;
    }

    // A container whose only file is an .exfat / .ffpkg / .ufs2 image (a direct
    // conversion, or ps5-dump-forge's .ffpfsc with an inner image): that file
    // as a random-access source, with its format ("exfat" or "ffpkg").
    Result<std::shared_ptr<RandomSource>> wrappedImage(QString* format, QString* innerName = nullptr)
    {
        const pfs::PfsImage& img = target();
        if (img.files().size() != 1 || !img.dirs().empty())
            return std::shared_ptr<RandomSource>();
        const pfs::PfsEntry& e = img.files().front();
        const QString name = QString::fromUtf8(e.path);
        const QString low = name.toLower();
        if (low.endsWith(QLatin1String(".exfat")))
            *format = QStringLiteral("exfat");
        else if (low.endsWith(QLatin1String(".ffpkg")) || low.endsWith(QLatin1String(".ufs2")))
            *format = QStringLiteral("ffpkg");
        else
            return std::shared_ptr<RandomSource>();
        FW_TRY_ASSIGN(auto src, img.openFile(img.inodes()[e.inode]));
        if (innerName)
            *innerName = name;
        return std::shared_ptr<RandomSource>(
            std::make_shared<PfsFileSource>(std::move(src), QStringLiteral("%1 inside %2").arg(name, QFileInfo(m_root).fileName())));
    }

private:
    // One file of a PFS image as a RandomSource (PFSC blocks decoded on demand).
    class PfsFileSource final : public RandomSource {
    public:
        PfsFileSource(std::shared_ptr<pfs::ByteSource> src, QString name) : m_src(std::move(src)), m_name(std::move(name)) {}
        qint64 size() const override { return m_src->size(); }
        Status readAt(qint64 offset, char* dst, qint64 len) override
        {
            std::lock_guard lock(m_mutex);
            return m_src->readAt(offset, dst, len);
        }
        QString describe() const override { return m_name; }

    private:
        std::shared_ptr<pfs::ByteSource> m_src;
        QString m_name;
        std::mutex m_mutex;
    };

    const pfs::PfsImage& target() const { return m_inner ? *m_inner : *m_outer; }

    // A container that holds exactly one nested PFS image (PfsVerify's rule:
    // the name, or the PFS magic at the start of the only file).
    static bool looksNested(const pfs::PfsImage& outer)
    {
        if (outer.files().size() != 1 || !outer.dirs().empty())
            return false;
        const pfs::PfsEntry& e = outer.files().front();
        const QByteArray low = pfs::asciiLower(e.path);
        if (low == pfs::kNestedImageName || low.endsWith(".ffpfs"))
            return true;
        auto src = outer.openFile(outer.inodes()[e.inode]);
        if (!src.ok() || (*src)->size() < 0x400)
            return false;
        char head[16];
        if (!(*src)->readAt(0, head, 16).ok())
            return false;
        return pfs::getI64(head + 8) == pfs::kPfsMagic;
    }

    // Declaration order matters: each one refers to the ones above it.
    std::shared_ptr<pfs::FileByteSource> m_file;
    std::unique_ptr<pfs::PfsImage> m_outer;
    std::unique_ptr<pfs::PfsImage> m_inner;
    std::shared_ptr<pfs::ByteSource> m_source;
    int m_sourceIndex = -1;
};

QString suffixOf(const QString& path) { return QFileInfo(path).suffix().toLower(); }

} // namespace

// ---- FileTree --------------------------------------------------------------------------

QString FileTree::describe() const
{
    if (!m_formatName.isEmpty())
        return m_formatName;
    switch (m_format) {
    case TreeFormat::Folder: return QStringLiteral("folder");
    case TreeFormat::Exfat: return QStringLiteral("exFAT image");
    case TreeFormat::Ffpkg: return QStringLiteral("FFPKG (UFS2) image");
    case TreeFormat::Pfs: return QStringLiteral("PFS image");
    }
    return {};
}

int FileTree::find(const QString& path) const
{
    QString p = path;
    while (p.startsWith(u'/'))
        p.remove(0, 1);
    return m_index.value(p, -1);
}

void FileTree::finish()
{
    std::sort(m_items.begin(), m_items.end(), [](const TreeItem& a, const TreeItem& b) { return a.path < b.path; });
    m_index.clear();
    m_index.reserve(qsizetype(m_items.size()));
    m_files = m_dirs = m_bytes = 0;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const TreeItem& it = m_items[i];
        m_index.insert(it.path, int(i));
        if (it.isDir) {
            ++m_dirs;
        } else {
            ++m_files;
            m_bytes += it.size;
        }
    }
}

bool isTreeImage(const QString& path)
{
    const QString ext = suffixOf(path);
    return ext == u"exfat" || ext == u"ffpkg" || ext == u"ffpfs" || ext == u"ffpfsc";
}

Result<std::unique_ptr<FileTree>> openFileTree(const QString& path, const CancellationToken& token)
{
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(nativeName(path)));
    const QString abs = QDir::cleanPath(fi.absoluteFilePath());
    if (fi.isDir()) {
        auto t = std::make_unique<FolderTree>(abs);
        FW_TRY(t->scan(token));
        return std::unique_ptr<FileTree>(std::move(t));
    }
    const QString ext = suffixOf(abs);
    if (ext == u"exfat") {
        auto t = std::make_unique<ExfatTree>(abs);
        FW_TRY(t->open(token));
        return std::unique_ptr<FileTree>(std::move(t));
    }
    if (ext == u"ffpkg") {
        auto t = std::make_unique<Ufs2FileTree>(abs);
        FW_TRY(t->open(token));
        return std::unique_ptr<FileTree>(std::move(t));
    }
    if (ext == u"ffpfs" || ext == u"ffpfsc") {
        auto t = std::make_unique<PfsFileTree>(abs);
        FW_TRY(t->open(token));
        // A container around one exFAT or UFS2 image: open through to that image
        // (the container tree stays alive as the source's owner).
        QString inner, innerName;
        FW_TRY_ASSIGN(std::shared_ptr<RandomSource> wrapped, t->wrappedImage(&inner, &innerName));
        if (!wrapped)
            return std::unique_ptr<FileTree>(std::move(t));
        std::shared_ptr<PfsFileTree> keep(std::move(t));
        if (inner == u"exfat") {
            auto x = std::make_unique<ExfatTree>(abs);
            FW_TRY(x->open(wrapped, keep, token));
            x->setInnerImage(innerName);
            return std::unique_ptr<FileTree>(std::move(x));
        }
        auto u = std::make_unique<Ufs2FileTree>(abs);
        FW_TRY(u->open(wrapped, keep, token));
        u->setInnerImage(innerName);
        return std::unique_ptr<FileTree>(std::move(u));
    }
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("%1 is not an .exfat, .ffpkg, .ffpfs or .ffpfsc image or a folder.").arg(nativeName(path)));
}

Result<FileSlice> readSlice(FileTree& tree, const QString& path, qint64 offset, qint64 length)
{
    const int index = tree.find(path);
    QString shown = path;
    if (!shown.startsWith(u'/'))
        shown.prepend(u'/');
    if (index < 0)
        return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in %2.").arg(shown, QFileInfo(tree.rootPath()).fileName()));
    const TreeItem& it = tree.items()[size_t(index)];
    if (it.isDir)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is a folder.").arg(shown));
    if (offset < 0 || length < 0)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid range for %1.").arg(shown));
    FileSlice out;
    out.path = it.path;
    out.fileSize = it.size;
    out.offset = std::min(offset, it.size);
    const qint64 want = std::min(length, it.size - out.offset);
    if (want <= 0)
        return out;
    out.data.resize(qsizetype(want));
    qint64 got = 0;
    while (got < want) {
        FW_TRY_ASSIGN(const qint64 n, tree.read(index, out.offset + got, out.data.data() + got, want - got));
        if (n <= 0)
            break;
        got += n;
    }
    out.data.truncate(qsizetype(got));
    return out;
}

Result<FileSlice> readFileSlice(const QString& imageOrFolder, const QString& path, qint64 offset, qint64 length,
                                const CancellationToken& token)
{
    FW_TRY_ASSIGN(auto tree, openFileTree(imageOrFolder, token));
    return readSlice(*tree, path, offset, length);
}

} // namespace fw::integration
