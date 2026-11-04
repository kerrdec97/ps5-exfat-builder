// PFS image reader. Port of Lazy_MkPFS lazy_mkpfs/inspect.py (inspect_pfs_image
// and helpers) and compression.py (PFSC decoding). GPL-3.0-or-later.

#include "engines/pfs/PfsReader.h"

#include "core/FileUtil.h"
#include "engines/pfs/PfscCodec.h"

#include <QFileInfo>

#include <algorithm>
#include <set>

namespace fw::pfs {

namespace {

constexpr qint64 kStreamChunk = 4 * 1024 * 1024;
constexpr int kMaxDepth = 512;
constexpr int kMaxProblems = 50;

Error damaged(const QString& what, const QString& image, const QStringList& all = {})
{
    return Error(ErrorCode::CorruptData,
                 QStringLiteral("The image %1 is damaged: %2.").arg(QFileInfo(image).fileName(), what),
                 all.isEmpty() ? what : all.mid(0, kMaxProblems).join(QStringLiteral("; ")));
}

struct ParsedDirent {
    quint32 inode = 0;
    qint32 type = 0;
    QByteArray name;
};

// parse_image_dirents(strict=True)
std::vector<ParsedDirent> parseDirents(const QByteArray& blob, QStringList& errors, const QString& where)
{
    std::vector<ParsedDirent> out;
    qint64 off = 0;
    const char* b = blob.constData();
    while (off + 16 <= blob.size()) {
        const quint32 ino = getU32(b + off);
        const qint32 type = getI32(b + off + 4);
        const qint32 nameLen = getI32(b + off + 8);
        const qint32 entSize = getI32(b + off + 12);
        if (ino == 0 && type == 0 && nameLen == 0 && entSize == 0)
            break;
        if (entSize < 17 || entSize % 8 != 0) {
            errors << QStringLiteral("%1: invalid entry size %2 at offset %3").arg(where).arg(entSize).arg(off);
            break;
        }
        if (nameLen < 0 || nameLen > entSize - 16) {
            errors << QStringLiteral("%1: invalid name length %2 at offset %3").arg(where).arg(nameLen).arg(off);
            break;
        }
        if (off + entSize > blob.size()) {
            errors << QStringLiteral("%1: entry at offset %2 runs past its block").arg(where).arg(off);
            break;
        }
        ParsedDirent d;
        d.inode = ino;
        d.type = type;
        d.name = blob.mid(off + 16, nameLen);
        for (char c : d.name)
            if (quint8(c) > 0x7F) {
                errors << QStringLiteral("%1: non-ASCII name at offset %2").arg(where).arg(off);
                break;
            }
        out.push_back(std::move(d));
        off += entSize;
    }
    return out;
}

class SubSource final : public ByteSource {
public:
    SubSource(std::shared_ptr<ByteSource> parent, qint64 base, qint64 size, QString name)
        : m_parent(std::move(parent)), m_base(base), m_size(size), m_name(std::move(name))
    {
    }
    qint64 size() const override { return m_size; }
    qint64 presentSize() const override { return std::clamp<qint64>(m_parent->presentSize() - m_base, 0, m_size); }
    Status readAt(qint64 offset, char* dst, qint64 len) override
    {
        if (offset < 0 || len < 0 || offset + len > m_size)
            return Error(ErrorCode::CorruptData, QStringLiteral("Read past the end of %1.").arg(m_name));
        return m_parent->readAt(m_base + offset, dst, len);
    }
    QString describe() const override { return m_name; }

private:
    std::shared_ptr<ByteSource> m_parent;
    qint64 m_base, m_size;
    QString m_name;
};

// The image's bytes as the header describes them (a short tail reads as zeros).
class ImageView final : public ByteSource {
public:
    explicit ImageView(const PfsImage* img) : m_img(img) {}
    qint64 size() const override { return m_img->header().ndblock * qint64(m_img->header().blockSize); }
    qint64 presentSize() const override { return m_img->presentBytes(); }
    Status readAt(qint64 offset, char* dst, qint64 len) override { return m_img->readBytes(offset, dst, len); }
    QString describe() const override { return m_img->describe(); }

private:
    const PfsImage* m_img;
};

class PfscByteSource final : public ByteSource {
public:
    PfscByteSource(std::unique_ptr<PfscReader> r, QString name) : m_reader(std::move(r)), m_name(std::move(name)) {}
    qint64 size() const override { return m_reader->logicalSize(); }
    Status readAt(qint64 offset, char* dst, qint64 len) override
    {
        if (offset < 0 || len < 0 || offset + len > size())
            return Error(ErrorCode::CorruptData, QStringLiteral("Read past the end of %1.").arg(m_name));
        while (len > 0) {
            const qint64 blk = offset / kPfscBlock;
            if (blk != m_cached) {
                m_cache.resize(qsizetype(kPfscBlock));
                FW_TRY(m_reader->decodeBlock(blk, m_cache.data()));
                m_cached = blk;
            }
            const qint64 within = offset - blk * kPfscBlock;
            const qint64 take = std::min(len, kPfscBlock - within);
            std::memcpy(dst, m_cache.constData() + within, size_t(take));
            dst += take;
            offset += take;
            len -= take;
        }
        return success();
    }
    QString describe() const override { return m_name; }

private:
    std::unique_ptr<PfscReader> m_reader;
    QString m_name;
    QByteArray m_cache;
    qint64 m_cached = -1;
};

} // namespace

// ---- FileByteSource -------------------------------------------------------------------

FileByteSource::FileByteSource(QString path) : m_path(std::move(path)), m_file(toLongPath(m_path)) {}

Status FileByteSource::open()
{
    if (!m_file.open(QIODevice::ReadOnly))
        return Error(QFileInfo::exists(m_path) ? ErrorCode::IoError : ErrorCode::NotFound,
                     QStringLiteral("Could not open %1.").arg(QFileInfo(m_path).fileName()), m_file.errorString());
    m_size = m_file.size();
    return success();
}

Status FileByteSource::readAt(qint64 offset, char* dst, qint64 len)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (offset < 0 || len < 0 || offset + len > m_size)
        return Error(ErrorCode::CorruptData, QStringLiteral("Read past the end of %1.").arg(QFileInfo(m_path).fileName()));
    if (!m_file.seek(offset))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                     m_file.errorString());
    qint64 got = 0;
    while (got < len) {
        const qint64 r = m_file.read(dst + got, len - got);
        if (r <= 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                         m_file.errorString());
        got += r;
    }
    return success();
}

// ---- PfscReader ----------------------------------------------------------------------

struct PfscReader::InflaterHolder {
    Inflater inflater;
};

Result<std::unique_ptr<PfscReader>> PfscReader::open(std::shared_ptr<ByteSource> source, qint64 base, qint64 stored,
                                                     qint64 logical)
{
    const QString name = source->describe();
    // Only bytes really in the image count: a short image reads as zeros
    // after presentSize(), and a crafted header must not make us allocate or
    // decode a table that is not there.
    const qint64 present = std::min(source->presentSize(), source->size());
    char head[kPfscHeaderSize];
    if (stored < kPfscHeaderSize)
        return damaged(QStringLiteral("a compressed file is too small for its PFSC header"), name);
    if (base < 0 || base + kPfscHeaderSize > present)
        return damaged(QStringLiteral("a compressed file starts past the end of the image"), name);
    FW_TRY(source->readAt(base, head, kPfscHeaderSize));
    PfscHeader h;
    const QString why = parsePfscHeader(head, kPfscHeaderSize, &h);
    if (!why.isEmpty())
        return damaged(why, name);
    if (h.dataOffset > stored)
        return damaged(QStringLiteral("PFSC data offset exceeds the stored payload"), name);
    const qint64 tableBytes = (h.blockCount + 1) * 8;
    if (h.offsetsOffset + tableBytes > h.dataOffset || h.offsetsOffset + tableBytes > stored)
        return damaged(QStringLiteral("PFSC block offset table is truncated"), name);
    if (logical > h.logicalSize)
        return damaged(QStringLiteral("PFSC logical size is smaller than the file size"), name);
    if (base + h.offsetsOffset + tableBytes > present)
        return damaged(QStringLiteral("the PFSC block offset table of a compressed file lies past the end of the image"),
                       name);
    // The table is read in bounded chunks straight into the offsets (it is
    // known to be in the image, so its size is bounded by the image's).
    std::unique_ptr<PfscReader> r(new PfscReader());
    r->m_offsets.resize(size_t(h.blockCount + 1));
    {
        constexpr qint64 kChunkEntries = 64 * 1024;
        QByteArray chunk(qsizetype(std::min<qint64>(kChunkEntries, h.blockCount + 1) * 8), Qt::Uninitialized);
        for (qint64 first = 0; first <= h.blockCount; first += kChunkEntries) {
            const qint64 n = std::min<qint64>(kChunkEntries, h.blockCount + 1 - first);
            FW_TRY(source->readAt(base + h.offsetsOffset + first * 8, chunk.data(), n * 8));
            for (qint64 i = 0; i < n; ++i)
                r->m_offsets[size_t(first + i)] = qint64(getU64(chunk.constData() + i * 8));
        }
    }
    if (r->m_offsets.front() != h.dataOffset)
        return damaged(QStringLiteral("PFSC block offsets do not start at the data offset"), name);
    if (r->m_offsets.back() > stored)
        return damaged(QStringLiteral("PFSC block offsets exceed the stored payload"), name);
    if (base + r->m_offsets.back() > present)
        return damaged(QStringLiteral("the compressed blocks of a file lie past the end of the image"), name);
    for (size_t i = 1; i < r->m_offsets.size(); ++i)
        if (r->m_offsets[i] < r->m_offsets[i - 1])
            return damaged(QStringLiteral("PFSC block offsets are not in order"), name);
    r->m_source = std::move(source);
    r->m_base = base;
    r->m_logical = logical;
    r->m_blockCount = h.blockCount;
    r->m_inflater = std::make_shared<InflaterHolder>();
    return r;
}

Status PfscReader::decodeBlock(qint64 index, char* out)
{
    if (index < 0 || index >= m_blockCount)
        return damaged(QStringLiteral("PFSC block %1 is out of range").arg(index), m_source->describe());
    const qint64 start = m_offsets[size_t(index)];
    const qint64 len = m_offsets[size_t(index + 1)] - start;
    if (len > kPfscBlock)
        return damaged(QStringLiteral("PFSC block %1 is larger than 64 KiB").arg(index), m_source->describe());
    if (len == kPfscBlock)
        return m_source->readAt(m_base + start, out, len);
    m_stored.resize(qsizetype(std::max<qint64>(len, 1)));
    FW_TRY(m_source->readAt(m_base + start, m_stored.data(), len));
    const QString why = m_inflater->inflater.inflateBlock(m_stored.constData(), len, out);
    if (!why.isEmpty())
        return damaged(QStringLiteral("PFSC block %1: %2").arg(index).arg(why), m_source->describe());
    return success();
}

// ---- PfsImage -------------------------------------------------------------------------

const PfsEntry* PfsImage::findFile(const QByteArray& path) const
{
    auto it = m_fileIndex.find(path);
    return it == m_fileIndex.end() ? nullptr : &m_files[it->second];
}

Result<std::unique_ptr<PfsImage>> PfsImage::open(std::shared_ptr<ByteSource> source)
{
    std::unique_ptr<PfsImage> img(new PfsImage());
    img->m_source = std::move(source);
    FW_TRY(img->parse());
    return img;
}

Status PfsImage::readBytes(qint64 offset, char* dst, qint64 len) const
{
    const qint64 fsize = m_source->size();
    const qint64 logicalEnd = m_header.ndblock * qint64(m_header.blockSize);
    if (offset < 0 || len < 0 || offset + len > std::max(fsize, logicalEnd))
        return damaged(QStringLiteral("data points past the end of the image"), describe());
    if (offset + len <= fsize)
        return m_source->readAt(offset, dst, len);
    // Within ndblock but past the end of a short image: zeros.
    const qint64 avail = std::max<qint64>(0, fsize - offset);
    if (avail > 0)
        FW_TRY(m_source->readAt(offset, dst, avail));
    std::memset(dst + avail, 0, size_t(len - avail));
    return success();
}

qint64 PfsImage::presentBytes() const
{
    return std::min(std::min(m_source->presentSize(), m_source->size()),
                    m_header.ndblock * qint64(m_header.blockSize));
}

Status PfsImage::readMetadata(qint64 offset, char* dst, qint64 len, const QString& what) const
{
    if (offset < 0 || len < 0 || offset + len > presentBytes())
        return damaged(QStringLiteral("%1 lies past the end of the image").arg(what), describe());
    return readBytes(offset, dst, len);
}

Result<QByteArray> PfsImage::readPayload(const Inode& ino) const
{
    if (ino.blocks == 0)
        return QByteArray();
    const qint64 stored = ino.storedSize();
    const qint64 bs = m_header.blockSize;
    if (stored < 0 || stored > qint64(ino.blocks) * bs)
        return damaged(QStringLiteral("inode %1 has an invalid size").arg(ino.number), describe());
    if (stored > 256LL * 1024 * 1024)
        return damaged(QStringLiteral("inode %1 metadata is implausibly large").arg(ino.number), describe());
    const QString what = QStringLiteral("the metadata of inode %1").arg(ino.number);
    if (ino.db[0] < 0 || ino.db[0] * bs + stored > presentBytes()) // checked before allocating
        return damaged(QStringLiteral("%1 lies past the end of the image").arg(what), describe());
    QByteArray data(qsizetype(stored), Qt::Uninitialized);
    FW_TRY(readMetadata(ino.db[0] * bs, data.data(), stored, what));
    return data;
}

Status PfsImage::parse()
{
    const QString name = describe();
    // Bytes really stored: a nested image inside a short outer image reads
    // partly as zeros, which must not pass for its metadata.
    const qint64 fsize = std::min(m_source->presentSize(), m_source->size());
    if (fsize < 0x400)
        return damaged(QStringLiteral("the file is too small to be a PFS image"), name);
    QByteArray hdr(0x400, Qt::Uninitialized);
    FW_TRY(m_source->readAt(0, hdr.data(), 0x400));
    const char* h = hdr.constData();
    PfsHeaderInfo& H = m_header;
    H.version = getI64(h + 0x00);
    H.magic = getI64(h + 0x08);
    H.readonly = quint8(h[0x1A]);
    H.mode = getU16(h + 0x1C);
    H.blockSize = int(getU32(h + 0x20));
    H.nblock = getI64(h + 0x28);
    H.inodeCount = getI64(h + 0x30);
    H.ndblock = getI64(h + 0x38);
    H.inodeBlockCount = getI64(h + 0x40);
    if (H.magic != kPfsMagic)
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("%1 is not a PFS image.").arg(QFileInfo(name).fileName()),
                     QStringLiteral("magic %1").arg(H.magic));
    if (H.mode & kModeEncrypted)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 is an encrypted PFS image. Encrypted images are not supported.")
                         .arg(QFileInfo(name).fileName()));
    if (H.mode & kModeSigned)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 is a signed PFS image. Signed images are not supported.")
                         .arg(QFileInfo(name).fileName()));
    const qint64 bs = H.blockSize;
    if (bs < 0x400 || bs > 0x100000 || (bs & (bs - 1)) != 0)
        return damaged(QStringLiteral("invalid block size %1").arg(bs), name);
    if (H.readonly != 1)
        m_warnings << QStringLiteral("header read-only byte is %1, expected 1").arg(H.readonly);
    if (H.version != kVersionPs4 && H.version != kVersionPs5)
        m_warnings << QStringLiteral("unknown PFS version %1").arg(H.version);
    const int isz = kInodeD32Size;
    const qint64 perBlock = bs / isz;
    if (perBlock <= 0 || H.inodeBlockCount < 1 || H.inodeCount < 3 || H.inodeCount > H.inodeBlockCount * perBlock
        || H.ndblock <= 1 + H.inodeBlockCount || H.ndblock > (qint64(1) << 40) / bs || H.inodeCount > 50'000'000)
        return damaged(QStringLiteral("the header's block or inode counts are invalid"), name);
    if ((1 + H.inodeBlockCount) * bs > fsize)
        return damaged(QStringLiteral("the inode table is cut off"), name);
    if (H.ndblock * bs > fsize) {
        m_missingTail = H.ndblock * bs - fsize;
        m_warnings << QStringLiteral("the image is %1 bytes shorter than its header says (the missing tail is "
                                     "read as zeros)")
                          .arg(m_missingTail);
    } else if (fsize > H.ndblock * bs) {
        m_warnings << QStringLiteral("%1 bytes of extra data after the last block").arg(fsize - H.ndblock * bs);
    }

    // ---- inode table ----
    m_inodes.reserve(size_t(H.inodeCount));
    QByteArray block(qsizetype(bs), Qt::Uninitialized);
    for (qint64 b = 0; b < H.inodeBlockCount && qint64(m_inodes.size()) < H.inodeCount; ++b) {
        FW_TRY(readMetadata((1 + b) * bs, block.data(), bs, QStringLiteral("the inode table")));
        for (qint64 i = 0; i < perBlock && qint64(m_inodes.size()) < H.inodeCount; ++i)
            m_inodes.push_back(parseInode(block.constData() + i * isz, quint32(m_inodes.size()), false, 32));
    }

    // ---- validate_inode_layout ----
    {
        struct Range {
            qint64 start, end;
            quint32 ino;
        };
        std::vector<Range> ranges;
        for (const Inode& ino : m_inodes) {
            // Every byte of an inode lives in its blocks: a size the blocks
            // cannot hold (e.g. data but no blocks at all) is damage, not an
            // empty file.
            const qint64 capacity = qint64(ino.blocks) * bs;
            if (ino.size < 0 || ino.sizeCompressed < 0) {
                problem(QStringLiteral("inode %1 has a negative size").arg(ino.number));
                continue;
            }
            if (ino.blocks == 0 && (ino.logicalSize() > 0 || ino.storedSize() > 0)) {
                problem(QStringLiteral("inode %1 holds %2 bytes but has no blocks")
                            .arg(ino.number)
                            .arg(std::max(ino.logicalSize(), ino.storedSize())));
                continue;
            }
            if (ino.storedSize() > capacity || (!ino.isCompressed() && ino.logicalSize() > capacity))
                problem(QStringLiteral("inode %1 is larger than its blocks").arg(ino.number));
            if (ino.blocks == 0)
                continue;
            const qint64 start = ino.db[0];
            const qint64 end = start + qint64(ino.blocks) - 1;
            if (start < 0) {
                problem(QStringLiteral("inode %1 has a negative first block").arg(ino.number));
                continue;
            }
            if (end >= H.ndblock)
                problem(QStringLiteral("inode %1 blocks [%2,%3] exceed the image (%4 blocks)")
                            .arg(ino.number)
                            .arg(start)
                            .arg(end)
                            .arg(H.ndblock));
            ranges.push_back({start, end, ino.number});
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
            return a.start != b.start ? a.start < b.start : a.ino < b.ino;
        });
        for (size_t i = 1; i < ranges.size(); ++i)
            if (ranges[i].start <= ranges[i - 1].end)
                problem(QStringLiteral("inodes %1 and %2 overlap").arg(ranges[i - 1].ino).arg(ranges[i].ino));
    }
    if (!m_problems.isEmpty())
        return damaged(m_problems.front(), name, m_problems);

    // ---- super root, flat_path_table, collision_resolver ----
    QByteArray superBlob(qsizetype(bs), Qt::Uninitialized);
    FW_TRY(readMetadata((1 + H.inodeBlockCount) * bs, superBlob.data(), bs, QStringLiteral("the super root")));
    const auto superEntries = parseDirents(superBlob, m_problems, QStringLiteral("super root"));
    qint64 fptIno = -1, colIno = -1, urootIno = -1;
    for (const auto& e : superEntries) {
        if (e.name == "flat_path_table")
            fptIno = e.inode;
        else if (e.name == "collision_resolver")
            colIno = e.inode;
        else if (e.name == "uroot")
            urootIno = e.inode;
    }
    if (fptIno < 0)
        problem(QStringLiteral("the super root has no flat_path_table"));
    if (urootIno < 0 || urootIno >= qint64(m_inodes.size()))
        problem(QStringLiteral("the super root has no valid uroot"));
    if (!m_problems.isEmpty())
        return damaged(m_problems.front(), name, m_problems);
    std::set<qint64> special{0, urootIno};
    if (fptIno >= 0)
        special.insert(fptIno);
    if (colIno >= 0)
        special.insert(colIno);

    std::map<quint32, quint32> fpt;
    std::map<quint32, std::vector<ParsedDirent>> collisions;
    if (fptIno >= 0 && fptIno < qint64(m_inodes.size())) {
        FW_TRY_ASSIGN(const QByteArray fptBlob, readPayload(m_inodes[size_t(fptIno)]));
        if (fptBlob.size() % 8 != 0)
            problem(QStringLiteral("flat_path_table size is not a multiple of 8"));
        for (qint64 i = 0; i + 8 <= fptBlob.size(); i += 8) {
            const quint32 hsh = getU32(fptBlob.constData() + i);
            if (fpt.count(hsh))
                problem(QStringLiteral("flat_path_table repeats hash 0x%1").arg(hsh, 8, 16, QLatin1Char('0')));
            fpt[hsh] = getU32(fptBlob.constData() + i + 4);
        }
        const bool anyCollision = std::any_of(fpt.begin(), fpt.end(), [](const auto& kv) { return kv.second & 0x80000000u; });
        if (anyCollision) {
            if (colIno < 0 || colIno >= qint64(m_inodes.size())) {
                problem(QStringLiteral("flat_path_table refers to a missing collision_resolver"));
            } else {
                FW_TRY_ASSIGN(const QByteArray cblob, readPayload(m_inodes[size_t(colIno)]));
                for (const auto& [hsh, v] : fpt) {
                    if (!(v & 0x80000000u))
                        continue;
                    const qint64 off = v & 0x7FFFFFFF;
                    if (off >= cblob.size()) {
                        problem(QStringLiteral("collision_resolver offset out of range"));
                        continue;
                    }
                    collisions[hsh] = parseDirents(cblob.mid(off), m_problems, QStringLiteral("collision_resolver"));
                }
            }
        }
    }

    // ---- tree (build_tree_from_uroot) ----
    std::map<QByteArray, quint32> files;
    std::map<QByteArray, quint32> dirs{{QByteArray(), quint32(urootIno)}};
    std::set<quint32> visited;
    std::map<quint32, QByteArray> dirPath{{quint32(urootIno), QByteArray()}};
    std::function<Status(quint32, const QByteArray&, quint32, std::set<quint32>&, int)> walk;
    walk = [&](quint32 dirIno, const QByteArray& rel, quint32 parentIno, std::set<quint32>& ancestors,
               int depth) -> Status {
        if (!visited.insert(dirIno).second)
            return success();
        const QString where = rel.isEmpty() ? QStringLiteral("/") : QString::fromLatin1(rel);
        if (depth > kMaxDepth) {
            problem(QStringLiteral("directory %1 is nested too deeply").arg(where));
            return success();
        }
        if (dirIno >= m_inodes.size()) {
            problem(QStringLiteral("directory inode %1 is out of range").arg(dirIno));
            return success();
        }
        const Inode& ino = m_inodes[dirIno];
        if (!ino.isDir()) {
            problem(QStringLiteral("inode %1 is used as a directory but is not one").arg(dirIno));
            return success();
        }
        FW_TRY_ASSIGN(const QByteArray payload, readPayload(ino));
        const auto entries = parseDirents(payload, m_problems, where);
        int dots = 0, dotdots = 0;
        for (const auto& e : entries) {
            if (e.name == ".") {
                ++dots;
                if (e.inode != dirIno || e.type != kDirentDot)
                    problem(QStringLiteral("directory %1 has a wrong '.' entry").arg(where));
            } else if (e.name == "..") {
                ++dotdots;
                const quint32 expected = rel.isEmpty() ? dirIno : parentIno;
                if (e.inode != expected || e.type != kDirentDotDot)
                    problem(QStringLiteral("directory %1 has a wrong '..' entry").arg(where));
            }
        }
        if (dots != 1 || dotdots != 1)
            problem(QStringLiteral("directory %1 must have exactly one '.' and one '..'").arg(where));
        std::set<QByteArray> names;
        ancestors.insert(dirIno);
        for (const auto& e : entries) {
            if (e.name == "." || e.name == "..")
                continue;
            if (!names.insert(e.name).second) {
                problem(QStringLiteral("directory %1 lists %2 twice").arg(where, QString::fromLatin1(e.name)));
                continue;
            }
            // A name is one path component: a separator, NUL or a dot entry
            // in the wrong place could point outside an extraction folder.
            // (Extra "." / ".." entries are also caught by the count above.)
            if (e.name.isEmpty() || e.name == "." || e.name == ".." || e.name.contains('/') || e.name.contains('\\')
                || e.name.contains('\0')) {
                problem(QStringLiteral("directory %1 has an invalid name").arg(where));
                continue;
            }
            const QByteArray child = rel.isEmpty() ? e.name : rel + '/' + e.name;
            if (e.inode >= m_inodes.size()) {
                problem(QStringLiteral("%1 refers to a missing inode").arg(QString::fromLatin1(child)));
                continue;
            }
            const Inode& ci = m_inodes[e.inode];
            if (e.type == kDirentDirectory) {
                if (!ci.isDir()) {
                    problem(QStringLiteral("%1 is listed as a folder but is not one").arg(QString::fromLatin1(child)));
                    continue;
                }
                if (ancestors.count(e.inode)) {
                    problem(QStringLiteral("folder cycle at %1").arg(QString::fromLatin1(child)));
                    continue;
                }
                auto prev = dirPath.find(e.inode);
                if (prev != dirPath.end() && prev->second != child) {
                    problem(QStringLiteral("folder inode %1 is reachable from two paths").arg(e.inode));
                    continue;
                }
                dirPath[e.inode] = child;
                dirs[child] = e.inode;
                FW_TRY(walk(e.inode, child, dirIno, ancestors, depth + 1));
            } else if (e.type == kDirentFile) {
                if (!ci.isFile()) {
                    problem(QStringLiteral("%1 is listed as a file but is not one").arg(QString::fromLatin1(child)));
                    continue;
                }
                files[child] = e.inode;
            } else {
                problem(QStringLiteral("directory %1 has an entry of unknown type %2").arg(where).arg(e.type));
            }
        }
        ancestors.erase(dirIno);
        return success();
    };
    std::set<quint32> ancestors;
    FW_TRY(walk(quint32(urootIno), QByteArray(), quint32(urootIno), ancestors, 0));

    // ---- validate_fpt_maps ----
    {
        struct Expected {
            QByteArray full;
            bool isDir;
            quint32 ino;
        };
        std::map<quint32, std::vector<Expected>> expected;
        const bool ci = H.caseInsensitive();
        for (const auto& [rel, ino] : dirs)
            if (!rel.isEmpty())
                expected[fptHash('/' + rel, ci)].push_back({'/' + rel, true, ino});
        for (const auto& [rel, ino] : files)
            expected[fptHash('/' + rel, ci)].push_back({'/' + rel, false, ino});
        for (const auto& [hsh, list] : expected) {
            auto it = fpt.find(hsh);
            if (it == fpt.end()) {
                problem(QStringLiteral("flat_path_table is missing %1").arg(QString::fromLatin1(list.front().full)));
                continue;
            }
            const quint32 v = it->second;
            if (list.size() == 1) {
                const Expected& e = list.front();
                if ((v & 0x80000000u) || bool(v & 0x20000000u) != e.isDir || (v & 0x1FFFFFFFu) != e.ino)
                    problem(QStringLiteral("flat_path_table entry for %1 is wrong").arg(QString::fromLatin1(e.full)));
            } else {
                if (!(v & 0x80000000u)) {
                    problem(QStringLiteral("flat_path_table hash 0x%1 should use the collision resolver")
                                .arg(hsh, 8, 16, QLatin1Char('0')));
                    continue;
                }
                const auto& actual = collisions[hsh];
                for (const Expected& e : list) {
                    const bool found = std::any_of(actual.begin(), actual.end(), [&](const ParsedDirent& d) {
                        return d.name == e.full && d.inode == e.ino && (d.type == kDirentDirectory) == e.isDir;
                    });
                    if (!found)
                        problem(QStringLiteral("collision_resolver is missing %1").arg(QString::fromLatin1(e.full)));
                }
            }
        }
        for (const auto& [hsh, v] : fpt)
            if (!expected.count(hsh))
                problem(QStringLiteral("flat_path_table has an unexpected hash 0x%1").arg(hsh, 8, 16, QLatin1Char('0')));
    }

    // ---- orphans ----
    {
        std::set<qint64> reachable(special.begin(), special.end());
        for (const auto& kv : files)
            reachable.insert(kv.second);
        for (const auto& kv : dirs)
            reachable.insert(kv.second);
        QStringList orphans;
        for (const Inode& ino : m_inodes)
            if (!reachable.count(ino.number))
                orphans << QString::number(ino.number);
        if (!orphans.isEmpty())
            problem(QStringLiteral("inodes not reachable from the tree: %1").arg(orphans.mid(0, 20).join(u',')));
    }
    if (!m_problems.isEmpty())
        return damaged(m_problems.front(), name, m_problems);

    for (const auto& [rel, ino] : files) {
        m_fileIndex.emplace(rel, m_files.size());
        m_files.push_back({rel, false, ino});
    }
    for (const auto& [rel, ino] : dirs)
        if (!rel.isEmpty())
            m_dirs.push_back({rel, true, ino});
    return success();
}

Status PfsImage::readFile(const Inode& ino, const Sink& sink, const CancellationToken& token) const
{
    const qint64 logical = ino.logicalSize();
    if (logical <= 0)
        return success();
    if (ino.blocks == 0)
        return damaged(QStringLiteral("inode %1 holds %2 bytes but has no blocks").arg(ino.number).arg(logical), describe());
    const qint64 bs = m_header.blockSize;
    const qint64 base = ino.db[0] * bs;
    if (!ino.isCompressed()) {
        if (qint64(ino.blocks) * bs < logical)
            return damaged(QStringLiteral("inode %1 is larger than its blocks").arg(ino.number), describe());
        QByteArray buf(qsizetype(std::min(kStreamChunk, logical)), Qt::Uninitialized);
        qint64 off = 0;
        while (off < logical) {
            if (!token.checkpoint())
                return Error::cancelled();
            const qint64 take = std::min<qint64>(buf.size(), logical - off);
            FW_TRY(readBytes(base + off, buf.data(), take));
            FW_TRY(sink(buf.constData(), take));
            off += take;
        }
        return success();
    }
    const qint64 stored = ino.storedSize();
    if (stored > qint64(ino.blocks) * bs)
        return damaged(QStringLiteral("inode %1 is larger than its blocks").arg(ino.number), describe());
    FW_TRY_ASSIGN(auto reader, PfscReader::open(std::make_shared<ImageView>(this), base, stored, logical));
    const qint64 batch = 64;
    QByteArray out(qsizetype(batch * kPfscBlock), Qt::Uninitialized);
    qint64 emitted = 0;
    for (qint64 b = 0; b < reader->blockCount() && emitted < logical;) {
        if (!token.checkpoint())
            return Error::cancelled();
        qint64 n = 0;
        while (n < batch && b + n < reader->blockCount() && emitted + n * kPfscBlock < logical) {
            FW_TRY(reader->decodeBlock(b + n, out.data() + n * kPfscBlock));
            ++n;
        }
        const qint64 take = std::min(n * kPfscBlock, logical - emitted);
        FW_TRY(sink(out.constData(), take));
        emitted += take;
        b += n;
    }
    if (emitted != logical)
        return damaged(QStringLiteral("a compressed file decoded to the wrong size"), describe());
    return success();
}

Result<std::shared_ptr<ByteSource>> PfsImage::openFile(const Inode& ino) const
{
    const qint64 logical = ino.logicalSize();
    const qint64 bs = m_header.blockSize;
    const QString name = describe() + QStringLiteral(" (inode %1)").arg(ino.number);
    const qint64 capacity = qint64(ino.blocks) * bs;
    if (logical < 0 || ino.storedSize() < 0 || ino.storedSize() > capacity
        || (!ino.isCompressed() && logical > capacity))
        return damaged(QStringLiteral("inode %1 is larger than its blocks").arg(ino.number), describe());
    auto view = std::make_shared<ImageView>(this);
    if (!ino.isCompressed())
        return std::shared_ptr<ByteSource>(std::make_shared<SubSource>(view, ino.db[0] * bs, logical, name));
    FW_TRY_ASSIGN(auto reader, PfscReader::open(view, ino.db[0] * bs, ino.storedSize(), logical));
    return std::shared_ptr<ByteSource>(std::make_shared<PfscByteSource>(std::move(reader), name));
}

} // namespace fw::pfs
