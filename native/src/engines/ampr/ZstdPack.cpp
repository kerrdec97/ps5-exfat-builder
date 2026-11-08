#include "engines/ampr/ZstdPack.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace fw::ampr {

namespace {

void put16(char* p, quint16 v)
{
    p[0] = char(v & 0xFF);
    p[1] = char(v >> 8);
}
void put32(char* p, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = char((v >> (8 * i)) & 0xFF);
}
void put64(char* p, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = char((v >> (8 * i)) & 0xFF);
}
quint16 get16(const char* p) { return quint16(quint8(p[0]) | (quint16(quint8(p[1])) << 8)); }
quint32 get32(const char* p)
{
    quint32 v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | quint8(p[i]);
    return v;
}
quint64 get64(const char* p)
{
    quint64 v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | quint8(p[i]);
    return v;
}

Error corrupt(const QString& what)
{
    return Error(ErrorCode::CorruptData, QStringLiteral("The zstd pack index is damaged: %1.").arg(what));
}

bool recordLess(const ZstdFileRecord& a, const ZstdFileRecord& b)
{
    if (a.hash != b.hash)
        return a.hash < b.hash;
    return std::lexicographical_compare(a.path.begin(), a.path.end(), b.path.begin(), b.path.end(),
                                        [](char x, char y) { return quint8(x) < quint8(y); });
}

qint64 chunkLogicalBytes(quint64 fileSize, quint32 chunkInFile)
{
    const quint64 start = quint64(chunkInFile) << kZstdChunkShift;
    return qint64(std::min<quint64>(quint64(kZstdChunkBytes), fileSize - start));
}

} // namespace

QString zstdVolumeName(int volumeId)
{
    return QStringLiteral("ampr_zstd-%1.pak").arg(volumeId, 3, 10, QLatin1Char('0'));
}

bool isZstdPackFileName(const QString& name)
{
    static const QRegularExpression vol(QStringLiteral("^ampr_zstd-[0-9]{3,}\\.pak$"));
    return name == kZstdIndexName || vol.match(name).hasMatch();
}

quint64 fnv1a64(const QByteArray& bytes)
{
    quint64 h = 14695981039346656037ULL;
    for (const char c : bytes) {
        h ^= quint64(quint8(c));
        h *= 1099511628211ULL;
    }
    return h;
}

quint32 crc32(const char* data, qint64 length, quint32 seed)
{
    uLong c = seed;
    while (length > 0) {
        const uInt n = uInt(std::min<qint64>(length, 1 << 30));
        c = ::crc32(c, reinterpret_cast<const Bytef*>(data), n);
        data += n;
        length -= n;
    }
    return quint32(c);
}

quint64 ZstdChunk::encode() const
{
    return quint64(offset) | (quint64(volume & 0xFFF) << 32) | (quint64((stored - 1) & 0x3FFFF) << 44)
           | (quint64(codec & 0x3) << 62);
}

ZstdChunk ZstdChunk::decode(quint64 v)
{
    ZstdChunk c;
    c.offset = quint32(v & 0xFFFFFFFFULL);
    c.volume = quint32((v >> 32) & 0xFFF);
    c.stored = quint32(((v >> 44) & 0x3FFFF) + 1);
    c.codec = quint8((v >> 62) & 0x3);
    return c;
}

int ZstdIndex::find(const QByteArray& path) const
{
    ZstdFileRecord key;
    key.path = path;
    key.hash = fnv1a64(path);
    const auto it = std::lower_bound(files.begin(), files.end(), key, recordLess);
    if (it == files.end() || it->hash != key.hash || it->path != path)
        return -1;
    return int(it - files.begin());
}

QString zstdPathProblem(const QByteArray& path)
{
    if (path.isEmpty())
        return QStringLiteral("an empty path");
    if (path.size() > 65535)
        return QStringLiteral("a path longer than 65,535 bytes");
    if (path.contains('\\') || path.contains('\0'))
        return QStringLiteral("a backslash or NUL in the path");
    for (const QByteArray& part : path.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return QStringLiteral("an empty, \".\" or \"..\" folder name");
    return {};
}

QByteArray encodeZstdIndex(const ZstdIndex& ix)
{
    const quint64 fc = ix.files.size();
    const quint64 cc = ix.chunks.size();
    const quint64 vc = ix.volumeSizes.size();
    quint64 stringsLen = 0;
    for (const auto& f : ix.files)
        stringsLen += quint64(f.path.size());
    const quint64 filesOff = kZstdIndexHeaderBytes;
    const quint64 chunksOff = filesOff + fc * kZstdFileRecordBytes;
    const quint64 volumesOff = chunksOff + cc * 8;
    const quint64 stringsOff = volumesOff + vc * 8;
    QByteArray out(qsizetype(stringsOff + stringsLen), '\0');
    char* h = out.data();
    std::memcpy(h, kZstdIndexMagic, 8);
    put32(h + 8, 1);
    put32(h + 12, kZstdIndexHeaderBytes);
    h[16] = char(kZstdChunkShift);
    h[17] = char(ix.level);
    put32(h + 20, quint32(vc));
    std::memcpy(h + 24, ix.archiveId.data(), 16);
    put64(h + 40, fc);
    put64(h + 48, cc);
    put64(h + 56, filesOff);
    put64(h + 64, chunksOff);
    put64(h + 72, volumesOff);
    put64(h + 80, stringsOff);
    put64(h + 88, stringsLen);
    put64(h + 96, ix.totalLogical);
    quint64 pathOff = 0;
    char* rec = h + filesOff;
    for (const auto& f : ix.files) {
        put64(rec + 0, f.hash);
        put64(rec + 8, f.size);
        put32(rec + 16, f.firstChunk);
        put32(rec + 20, quint32(pathOff));
        put16(rec + 24, quint16(f.path.size()));
        put16(rec + 26, f.flags);
        std::memcpy(h + stringsOff + pathOff, f.path.constData(), size_t(f.path.size()));
        pathOff += quint64(f.path.size());
        rec += kZstdFileRecordBytes;
    }
    for (quint64 i = 0; i < cc; ++i)
        put64(h + chunksOff + i * 8, ix.chunks[size_t(i)]);
    for (quint64 i = 0; i < vc; ++i)
        put64(h + volumesOff + i * 8, ix.volumeSizes[size_t(i)]);
    put32(h + 112, crc32(h + kZstdIndexHeaderBytes, qint64(out.size()) - kZstdIndexHeaderBytes));
    char head[kZstdIndexHeaderBytes];
    std::memcpy(head, h, kZstdIndexHeaderBytes);
    std::memset(head + 112, 0, 8);
    put32(h + 116, crc32(head, kZstdIndexHeaderBytes));
    return out;
}

Result<ZstdIndex> decodeZstdIndex(const QByteArray& bytes)
{
    if (bytes.size() < kZstdIndexHeaderBytes || std::memcmp(bytes.constData(), kZstdIndexMagic, 8) != 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("ampr_zstd.index is not a zstd pack index."));
    const char* h = bytes.constData();
    if (get32(h + 8) != 1 || get32(h + 12) != kZstdIndexHeaderBytes)
        return Error(ErrorCode::Unsupported, QStringLiteral("ampr_zstd.index has a version this app does not read."));
    if (quint8(h[16]) != kZstdChunkShift)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("ampr_zstd.index uses %1-byte chunks; the runtime mounts only 256 KiB chunks (repack the game).")
                         .arg(qint64(1) << std::min(40, int(quint8(h[16])))));
    ZstdIndex ix;
    ix.level = quint8(h[17]);
    if (ix.level < kZstdMinLevel || ix.level > kZstdMaxLevel)
        return corrupt(QStringLiteral("zstd level %1").arg(ix.level));
    {
        char head[kZstdIndexHeaderBytes];
        std::memcpy(head, h, kZstdIndexHeaderBytes);
        std::memset(head + 112, 0, 8);
        if (crc32(head, kZstdIndexHeaderBytes) != get32(h + 116))
            return corrupt(QStringLiteral("header checksum"));
    }
    const quint64 vc = get32(h + 20);
    std::memcpy(ix.archiveId.data(), h + 24, 16);
    const quint64 fc = get64(h + 40), cc = get64(h + 48);
    const quint64 filesOff = get64(h + 56), chunksOff = get64(h + 64), volumesOff = get64(h + 72);
    const quint64 stringsOff = get64(h + 80), stringsLen = get64(h + 88);
    ix.totalLogical = get64(h + 96);
    const quint64 size = quint64(bytes.size());
    if (vc > quint64(kZstdMaxVolumes) || (fc > 0 && vc == 0) || cc > 0xFFFFFFFFULL)
        return corrupt(QStringLiteral("counts"));
    if (fc > size / kZstdFileRecordBytes || cc > size / 8 || stringsOff > size || stringsLen > size)
        return corrupt(QStringLiteral("table sizes"));
    if (filesOff != kZstdIndexHeaderBytes || chunksOff != filesOff + fc * kZstdFileRecordBytes
        || volumesOff != chunksOff + cc * 8 || stringsOff != volumesOff + vc * 8 || stringsOff + stringsLen != size)
        return corrupt(QStringLiteral("table offsets"));
    if (crc32(h + kZstdIndexHeaderBytes, qint64(size) - kZstdIndexHeaderBytes) != get32(h + 112))
        return corrupt(QStringLiteral("table checksum"));

    ix.volumeSizes.resize(size_t(vc));
    for (quint64 i = 0; i < vc; ++i) {
        const quint64 v = get64(h + volumesOff + i * 8);
        if (v < quint64(kZstdVolumeHeaderBytes) || v > quint64(kZstdVolumeLimit))
            return corrupt(QStringLiteral("size of volume %1").arg(i));
        ix.volumeSizes[size_t(i)] = v;
    }
    ix.files.resize(size_t(fc));
    quint64 expectChunk = 0, expectPath = 0, logical = 0;
    for (quint64 i = 0; i < fc; ++i) {
        const char* r = h + filesOff + i * kZstdFileRecordBytes;
        ZstdFileRecord& f = ix.files[size_t(i)];
        f.hash = get64(r);
        f.size = get64(r + 8);
        if (f.size > ZstdFileRecord::kMaxSize)
            return corrupt(QStringLiteral("size of record %1").arg(i));
        f.firstChunk = get32(r + 16);
        const quint32 pathOff = get32(r + 20);
        const quint16 pathLen = get16(r + 24);
        f.flags = get16(r + 26);
        if (f.flags & ~quint16(1))
            return corrupt(QStringLiteral("flags of record %1").arg(i));
        if (pathOff != expectPath || quint64(pathOff) + pathLen > stringsLen)
            return corrupt(QStringLiteral("path of record %1").arg(i));
        f.path = QByteArray(h + stringsOff + pathOff, pathLen);
        expectPath += pathLen;
        if (!zstdPathProblem(f.path).isEmpty() || fnv1a64(f.path) != f.hash)
            return corrupt(QStringLiteral("path \"%1\"").arg(QString::fromUtf8(f.path)));
        if (f.firstChunk != expectChunk)
            return corrupt(QStringLiteral("chunk numbering at \"%1\"").arg(QString::fromUtf8(f.path)));
        expectChunk += f.chunkCount();
        logical += f.size;
        if (i > 0 && !recordLess(ix.files[size_t(i - 1)], f))
            return corrupt(QStringLiteral("record order at \"%1\"").arg(QString::fromUtf8(f.path)));
    }
    if (expectPath != stringsLen || expectChunk != cc || logical != ix.totalLogical)
        return corrupt(QStringLiteral("totals"));
    ix.chunks.resize(size_t(cc));
    for (quint64 i = 0; i < cc; ++i)
        ix.chunks[size_t(i)] = get64(h + chunksOff + i * 8);
    for (const auto& f : ix.files) {
        for (quint32 k = 0; k < f.chunkCount(); ++k) {
            const ZstdChunk c = ZstdChunk::decode(ix.chunks[size_t(f.firstChunk + k)]);
            const qint64 logicalBytes = chunkLogicalBytes(f.size, k);
            const bool ok = c.codec <= 1 && c.volume < vc && c.offset >= quint32(kZstdVolumeHeaderBytes)
                            && quint64(c.offset) + c.stored <= ix.volumeSizes[c.volume]
                            && (c.codec == 0 ? qint64(c.stored) == logicalBytes : qint64(c.stored) < logicalBytes)
                            && !(f.flags & 1 && c.codec != 0);
            if (!ok)
                return corrupt(QStringLiteral("chunk %1 of \"%2\"").arg(k).arg(QString::fromUtf8(f.path)));
        }
    }
    return ix;
}

QByteArray encodeZstdVolumeHeader(quint32 volumeId, quint32 volumeCount, const std::array<char, 16>& archiveId,
                                  quint64 payloadBytes)
{
    QByteArray h(kZstdVolumeHeaderBytes, '\0');
    char* p = h.data();
    std::memcpy(p, kZstdVolumeMagic, 8);
    put32(p + 8, 1);
    put32(p + 12, kZstdVolumeHeaderBytes);
    put32(p + 16, volumeId);
    put32(p + 20, volumeCount);
    std::memcpy(p + 24, archiveId.data(), 16);
    put64(p + 40, payloadBytes);
    put32(p + 60, crc32(p, kZstdVolumeHeaderBytes));
    return h;
}

Status checkZstdVolumeHeader(const QByteArray& header, quint32 volumeId, const ZstdIndex& index)
{
    const QString name = zstdVolumeName(int(volumeId));
    if (header.size() != kZstdVolumeHeaderBytes || std::memcmp(header.constData(), kZstdVolumeMagic, 8) != 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is not a zstd pack volume.").arg(name));
    const char* p = header.constData();
    QByteArray zeroed = header;
    std::memset(zeroed.data() + 60, 0, 4);
    if (crc32(zeroed.constData(), kZstdVolumeHeaderBytes) != get32(p + 60))
        return Error(ErrorCode::CorruptData, QStringLiteral("The header of %1 is damaged.").arg(name));
    if (get32(p + 8) != 1 || get32(p + 12) != kZstdVolumeHeaderBytes || get32(p + 16) != volumeId
        || get32(p + 20) != index.volumeSizes.size() || std::memcmp(p + 24, index.archiveId.data(), 16) != 0
        || get64(p + 40) + kZstdVolumeHeaderBytes != index.volumeSizes[volumeId])
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("%1 does not belong to this ampr_zstd.index (another pack, or a damaged copy).").arg(name));
    return success();
}

// ---- writing -------------------------------------------------------------------------

namespace {

struct Slot {
    std::vector<char> in;
    std::vector<char> out;
    qint64 inLen = 0;
    qint64 outLen = 0;
    quint8 codec = 0;
    quint32 crc = 0;
    int file = -1;
    QString error;
};

// Persistent compression workers, each with its own contexts. The main thread
// hands over a batch and waits for it; while the workers compress one batch
// the main thread reads the next.
class CompressPool {
public:
    CompressPool(int threads, int level, bool roundTrip) : m_level(level), m_roundTrip(roundTrip), m_threadCount(threads)
    {
        for (int i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { work(); });
    }
    ~CompressPool()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads)
            t.join();
    }
    void start(Slot* batch, int count)
    {
        std::lock_guard lock(m_mutex);
        m_slots = batch;
        m_count = count;
        m_next = 0;
        m_active = m_threadCount;
        ++m_generation;
        m_cv.notify_all();
    }
    void wait()
    {
        std::unique_lock lock(m_mutex);
        // Every worker has left this batch (so none can touch the next one's counter).
        m_doneCv.wait(lock, [&] { return m_active == 0; });
    }

private:
    void work()
    {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        ZSTD_DCtx* d = ZSTD_createDCtx();
        ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, m_level);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, kZstdChunkShift);
        ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 1);
        std::vector<char> check(static_cast<size_t>(kZstdChunkBytes), 0);
        quint64 seen = 0;
        for (;;) {
            Slot* batch = nullptr;
            int count = 0;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || m_generation != seen; });
                if (m_stop)
                    break;
                seen = m_generation;
                batch = m_slots;
                count = m_count;
            }
            for (;;) {
                const int i = m_next.fetch_add(1);
                if (i >= count)
                    break;
                compress(c, d, check, batch[i]);
            }
            {
                std::lock_guard lock(m_mutex);
                if (--m_active == 0)
                    m_doneCv.notify_all();
            }
        }
        ZSTD_freeCCtx(c);
        ZSTD_freeDCtx(d);
    }
    void compress(ZSTD_CCtx* c, ZSTD_DCtx* d, std::vector<char>& check, Slot& s)
    {
        s.error.clear();
        const size_t r = ZSTD_compress2(c, s.out.data(), s.out.size(), s.in.data(), size_t(s.inLen));
        if (!ZSTD_isError(r) && qint64(r) < s.inLen) {
            if (m_roundTrip) {
                const size_t back = ZSTD_decompressDCtx(d, check.data(), check.size(), s.out.data(), r);
                if (ZSTD_isError(back) || qint64(back) != s.inLen || std::memcmp(check.data(), s.in.data(), size_t(s.inLen)) != 0) {
                    s.error = QStringLiteral("a compressed chunk did not decode back to its data");
                    return;
                }
            }
            s.codec = 1;
            s.outLen = qint64(r);
            s.crc = crc32(s.out.data(), s.outLen);
        } else {
            s.codec = 0;
            s.outLen = s.inLen;
            s.crc = crc32(s.in.data(), s.inLen);
        }
    }

    const int m_level;
    const bool m_roundTrip;
    const int m_threadCount;
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv, m_doneCv;
    Slot* m_slots = nullptr;
    int m_count = 0;
    std::atomic<int> m_next{0};
    int m_active = 0;
    quint64 m_generation = 0;
    bool m_stop = false;
};

class VolumeWriter {
public:
    VolumeWriter(QString dir, qint64 limit) : m_dir(std::move(dir)), m_limit(limit) {}
    ~VolumeWriter() { m_file.close(); }

    Status put(const char* data, qint64 len, ZstdChunk& c)
    {
        if (!m_file.isOpen() || m_pos + len > m_limit) {
            FW_TRY(close());
            if (int(m_sizes.size()) >= kZstdMaxVolumes)
                return Error(ErrorCode::Unsupported, QStringLiteral("The game needs more than 4,096 pack volumes."));
            const QString name = zstdVolumeName(int(m_sizes.size()));
            m_file.setFileName(QDir(m_dir).filePath(name));
            if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(m_file.fileName())),
                             m_file.errorString());
            m_names << name;
            const QByteArray zero(kZstdVolumeHeaderBytes, '\0');
            FW_TRY(write(zero.constData(), zero.size()));
            m_pos = kZstdVolumeHeaderBytes;
        }
        c.volume = quint32(m_names.size() - 1);
        c.offset = quint32(m_pos);
        FW_TRY(write(data, len));
        m_pos += len;
        return success();
    }
    Status close()
    {
        if (!m_file.isOpen())
            return success();
        if (!m_file.flush())
            return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(m_file.fileName()), m_file.errorString());
        m_file.close();
        m_sizes.push_back(quint64(m_pos));
        return success();
    }
    // Writes every volume header now that the count is known.
    Status writeHeaders(const std::array<char, 16>& archiveId)
    {
        for (size_t i = 0; i < m_sizes.size(); ++i) {
            QFile f(QDir(m_dir).filePath(m_names[qsizetype(i)]));
            if (!f.open(QIODevice::ReadWrite))
                return Error(ErrorCode::IoError, QStringLiteral("Could not reopen %1.").arg(f.fileName()), f.errorString());
            const QByteArray h = encodeZstdVolumeHeader(quint32(i), quint32(m_sizes.size()), archiveId,
                                                        m_sizes[i] - kZstdVolumeHeaderBytes);
            if (!f.seek(0) || f.write(h) != h.size() || !f.flush())
                return Error(ErrorCode::IoError, QStringLiteral("Could not write the header of %1.").arg(f.fileName()), f.errorString());
        }
        return success();
    }
    const std::vector<quint64>& sizes() const { return m_sizes; }
    const QStringList& names() const { return m_names; }
    void removeAll()
    {
        m_file.close();
        for (const QString& n : m_names)
            QFile::remove(QDir(m_dir).filePath(n));
    }

private:
    Status write(const char* p, qint64 n)
    {
        while (n > 0) {
            const qint64 w = m_file.write(p, n);
            if (w <= 0)
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(m_file.fileName())),
                             m_file.errorString());
            p += w;
            n -= w;
        }
        return success();
    }

    QString m_dir;
    qint64 m_limit;
    QFile m_file;
    qint64 m_pos = 0;
    std::vector<quint64> m_sizes;
    QStringList m_names;
};

} // namespace

Result<ZstdPackResult> writeZstdPack(std::vector<PackInput> files, const ZstdPackOptions& o, const QString& outDir,
                                     const CancellationToken& token, ProgressReporter* progress)
{
    QElapsedTimer timer;
    timer.start();
    if (o.level < kZstdMinLevel || o.level > kZstdMaxLevel)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The zstd level must be 1 to 22."));
    if (o.volumeLimit < kZstdVolumeHeaderBytes + kZstdChunkBytes || o.volumeLimit > kZstdVolumeLimit)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid pack volume size."));

    ZstdPackResult res;
    ZstdIndex& ix = res.index;
    ix.level = quint8(o.level);
    ix.archiveId = o.archiveId;
    if (std::all_of(ix.archiveId.begin(), ix.archiveId.end(), [](char c) { return c == 0; })) {
        quint32 words[4];
        QRandomGenerator::system()->fillRange(words, 4);
        std::memcpy(ix.archiveId.data(), words, 16);
    }

    // Records in (hash, path) order; reject bad and duplicate paths and a file
    // that is also a folder of another.
    std::vector<int> order(files.size());
    ix.files.resize(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        const QString problem = zstdPathProblem(files[i].path);
        if (!problem.isEmpty())
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("%1 cannot go into a pack (%2).").arg(QString::fromUtf8(files[i].path), problem));
        if (files[i].size < 0 || quint64(files[i].size) > ZstdFileRecord::kMaxSize)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("Invalid size for %1.").arg(QString::fromUtf8(files[i].path)));
        ix.files[i].path = files[i].path;
        ix.files[i].hash = fnv1a64(files[i].path);
        ix.files[i].size = quint64(files[i].size);
        order[i] = int(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) { return recordLess(ix.files[size_t(a)], ix.files[size_t(b)]); });
    {
        std::vector<QByteArray> sorted;
        sorted.reserve(files.size());
        for (const auto& f : files)
            sorted.push_back(f.path);
        std::sort(sorted.begin(), sorted.end());
        for (size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i] == sorted[i - 1])
                return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is listed twice.").arg(QString::fromUtf8(sorted[i])));
            if (sorted[i].startsWith(sorted[i - 1] + '/'))
                return Error(ErrorCode::InvalidArgument,
                             QStringLiteral("%1 is both a file and a folder.").arg(QString::fromUtf8(sorted[i - 1])));
        }
    }
    {
        std::vector<ZstdFileRecord> recs;
        std::vector<PackInput> ins;
        recs.reserve(files.size());
        ins.reserve(files.size());
        for (int i : order) {
            recs.push_back(std::move(ix.files[size_t(i)]));
            ins.push_back(std::move(files[size_t(i)]));
        }
        ix.files = std::move(recs);
        files = std::move(ins);
    }
    quint64 chunkTotal = 0;
    for (auto& f : ix.files) {
        if (chunkTotal + f.chunkCount() > 0xFFFFFFFFULL)
            return Error(ErrorCode::Unsupported, QStringLiteral("The game has too many chunks for one pack."));
        f.firstChunk = quint32(chunkTotal);
        chunkTotal += f.chunkCount();
        ix.totalLogical += f.size;
    }
    ix.chunks.assign(size_t(chunkTotal), 0);
    res.chunkCrcs.assign(size_t(chunkTotal), 0);

    int threads = o.threads > 0 ? o.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    threads = std::clamp(threads, 1, 64);
    res.threads = threads;
    const int batchChunks = std::max(8, threads * 4);
    const size_t outCap = ZSTD_compressBound(size_t(kZstdChunkBytes));
    std::vector<Slot> batches[2];
    for (auto& b : batches) {
        b.resize(size_t(batchChunks));
        for (auto& s : b) {
            s.in.resize(size_t(kZstdChunkBytes));
            s.out.resize(outCap);
        }
    }
    if (progress && o.setProgressTotal)
        progress->setTotal(qint64(ix.totalLogical));

    VolumeWriter vw(outDir, o.volumeLimit);
    bool committed = false;
    struct Cleanup {
        VolumeWriter& w;
        bool& committed;
        ~Cleanup()
        {
            if (!committed)
                w.removeAll();
        }
    } cleanup{vw, committed};

    CompressPool pool(threads, o.level, o.roundTripCheck);

    // Reading state: the file being read and how far.
    size_t readFile = 0;
    qint64 readPos = 0;
    std::unique_ptr<SequentialReader> reader;
    quint32 nextChunk = 0; // chunk record filled next when writing

    auto fill = [&](std::vector<Slot>& b, int& count) -> Status {
        count = 0;
        while (count < batchChunks && readFile < files.size()) {
            const qint64 size = files[readFile].size;
            if (size == 0) {
                ++readFile;
                continue;
            }
            if (!reader) {
                if (!files[readFile].open)
                    return Error(ErrorCode::Internal, QStringLiteral("No reader for %1.").arg(QString::fromUtf8(files[readFile].path)));
                auto r = files[readFile].open();
                if (!r.ok())
                    return r.takeError();
                reader = std::move(r).value();
                readPos = 0;
                if (progress)
                    progress->setCurrentItem(QString::fromUtf8(files[readFile].path));
            }
            Slot& s = b[size_t(count)];
            const qint64 want = std::min(kZstdChunkBytes, size - readPos);
            qint64 got = 0;
            while (got < want) {
                auto n = reader->read(s.in.data() + got, want - got);
                if (!n.ok())
                    return n.takeError();
                if (*n == 0)
                    return Error(ErrorCode::IoError, QStringLiteral("%1 is shorter than when the game was scanned. Close programs that "
                                                                    "change it and try again.").arg(QString::fromUtf8(files[readFile].path)));
                got += *n;
            }
            s.inLen = want;
            s.file = int(readFile);
            readPos += want;
            ++count;
            if (readPos == size) {
                char extra;
                auto n = reader->read(&extra, 1);
                if (!n.ok())
                    return n.takeError();
                if (*n != 0)
                    return Error(ErrorCode::IoError, QStringLiteral("%1 grew while it was being packed. Close programs that change it "
                                                                    "and try again.").arg(QString::fromUtf8(files[readFile].path)));
                reader.reset();
                ++readFile;
            }
        }
        return success();
    };

    auto drain = [&](std::vector<Slot>& b, int count) -> Status {
        for (int i = 0; i < count; ++i) {
            Slot& s = b[size_t(i)];
            if (!s.error.isEmpty())
                return Error(ErrorCode::VerificationFailed,
                             QStringLiteral("Packing %1 failed: %2.").arg(QString::fromUtf8(files[size_t(s.file)].path), s.error));
            ZstdChunk c;
            c.codec = s.codec;
            c.stored = quint32(s.outLen);
            FW_TRY(vw.put(s.codec ? s.out.data() : s.in.data(), s.outLen, c));
            ix.chunks[nextChunk] = c.encode();
            res.chunkCrcs[nextChunk] = s.crc;
            ++nextChunk;
            res.storedBytes += s.outLen;
            (s.codec ? res.zstdChunks : res.rawChunks) += 1;
            if (progress)
                progress->advance(s.inLen);
        }
        return success();
    };

    int counts[2] = {0, 0};
    int cur = 0;
    FW_TRY(fill(batches[cur], counts[cur]));
    while (counts[cur] > 0) {
        if (!token.checkpoint())
            return Error::cancelled();
        pool.start(batches[cur].data(), counts[cur]);
        const int nxt = 1 - cur;
        Status readNext = fill(batches[nxt], counts[nxt]);
        pool.wait();
        FW_TRY(readNext);
        FW_TRY(drain(batches[cur], counts[cur]));
        cur = nxt;
    }
    if (nextChunk != chunkTotal)
        return Error(ErrorCode::Internal, QStringLiteral("The pack is incomplete."));
    FW_TRY(vw.close());
    ix.volumeSizes = vw.sizes();
    if (ix.volumeSizes.empty() && !ix.files.empty())
        return Error(ErrorCode::Internal, QStringLiteral("No pack volume was written."));
    FW_TRY(vw.writeHeaders(ix.archiveId));
    const QByteArray encoded = encodeZstdIndex(ix);
    FW_TRY(writeFileAtomic(QDir(outDir).filePath(kZstdIndexName), encoded));
    // The index must read back as written.
    auto check = decodeZstdIndex(encoded);
    if (!check.ok())
        return Error(ErrorCode::Internal, QStringLiteral("The pack index did not validate."), check.error().message);
    res.volumes = vw.names();
    res.elapsedMs = timer.elapsed();
    committed = true;
    return res;
}

Status verifyWrittenZstdPack(const QString& outDir, const ZstdPackResult& r, const CancellationToken& token,
                             ProgressReporter* progress)
{
    const ZstdIndex& ix = r.index;
    QFile idx(QDir(outDir).filePath(kZstdIndexName));
    if (!idx.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1 back.").arg(kZstdIndexName), idx.errorString());
    const QByteArray encoded = idx.readAll();
    if (encoded != encodeZstdIndex(ix))
        return Error(ErrorCode::VerificationFailed, QStringLiteral("ampr_zstd.index did not read back as written."));
    if (progress)
        progress->setTotal(r.storedBytes);
    // Chunks are in index order inside each volume: walk them per volume.
    std::vector<std::vector<quint32>> byVolume(ix.volumeSizes.size());
    for (quint32 i = 0; i < ix.chunks.size(); ++i)
        byVolume[ZstdChunk::decode(ix.chunks[i]).volume].push_back(i);
    QByteArray buf;
    for (size_t v = 0; v < ix.volumeSizes.size(); ++v) {
        QFile f(QDir(outDir).filePath(zstdVolumeName(int(v))));
        if (!f.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1 back.").arg(f.fileName()), f.errorString());
        if (quint64(f.size()) != ix.volumeSizes[v])
            return Error(ErrorCode::VerificationFailed, QStringLiteral("%1 has the wrong size.").arg(zstdVolumeName(int(v))));
        FW_TRY(checkZstdVolumeHeader(f.read(kZstdVolumeHeaderBytes), quint32(v), ix));
        for (quint32 ci : byVolume[v]) {
            if (!token.checkpoint())
                return Error::cancelled();
            const ZstdChunk c = ZstdChunk::decode(ix.chunks[ci]);
            if (!f.seek(c.offset))
                return Error(ErrorCode::IoError, QStringLiteral("Could not read %1 back.").arg(f.fileName()));
            buf.resize(qsizetype(c.stored));
            if (f.read(buf.data(), c.stored) != qint64(c.stored) || crc32(buf.constData(), c.stored) != r.chunkCrcs[ci])
                return Error(ErrorCode::VerificationFailed,
                             QStringLiteral("%1 did not read back as written (chunk at offset %2).").arg(zstdVolumeName(int(v))).arg(c.offset));
            if (progress)
                progress->advance(c.stored);
        }
    }
    return success();
}

// ---- reading -------------------------------------------------------------------------

ZstdPackReader::~ZstdPackReader()
{
    if (m_dctx)
        ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(m_dctx));
}

Result<std::unique_ptr<ZstdPackReader>> ZstdPackReader::open(const QByteArray& indexBytes, VolumeOpener opener)
{
    std::unique_ptr<ZstdPackReader> r(new ZstdPackReader);
    FW_TRY_ASSIGN(r->m_index, decodeZstdIndex(indexBytes));
    for (quint32 v = 0; v < r->m_index.volumeSizes.size(); ++v) {
        auto vol = opener(v);
        if (!vol.ok())
            return vol.takeError();
        std::shared_ptr<RandomAccess> ra = std::move(vol).value();
        if (!ra || quint64(ra->size()) != r->m_index.volumeSizes[v])
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is missing or has the wrong size.").arg(zstdVolumeName(int(v))));
        QByteArray h(kZstdVolumeHeaderBytes, '\0');
        FW_TRY(ra->readAt(0, h.data(), h.size()));
        FW_TRY(checkZstdVolumeHeader(h, v, r->m_index));
        r->m_volumes.push_back(std::move(ra));
    }
    r->m_dctx = ZSTD_createDCtx();
    r->m_chunk.resize(qsizetype(kZstdChunkBytes));
    return r;
}

Status ZstdPackReader::loadChunk(quint32 chunkIndex, qint64 logicalBytes)
{
    if (m_chunkIndex == qint64(chunkIndex))
        return success();
    m_chunkIndex = -1;
    const ZstdChunk c = ZstdChunk::decode(m_index.chunks[chunkIndex]);
    if (c.codec == 0) {
        FW_TRY(m_volumes[c.volume]->readAt(c.offset, m_chunk.data(), c.stored));
    } else {
        m_stored.resize(qsizetype(c.stored));
        FW_TRY(m_volumes[c.volume]->readAt(c.offset, m_stored.data(), c.stored));
        const unsigned long long declared = ZSTD_getFrameContentSize(m_stored.constData(), size_t(c.stored));
        const size_t n = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(m_dctx), m_chunk.data(), size_t(kZstdChunkBytes),
                                             m_stored.constData(), size_t(c.stored));
        if (ZSTD_isError(n) || qint64(n) != logicalBytes || declared != (unsigned long long)logicalBytes)
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("A chunk of %1 is damaged (%2).")
                             .arg(zstdVolumeName(int(c.volume)), ZSTD_isError(n) ? QString::fromLatin1(ZSTD_getErrorName(n))
                                                                                  : QStringLiteral("wrong size")));
    }
    m_chunkIndex = chunkIndex;
    return success();
}

Result<qint64> ZstdPackReader::read(int file, qint64 offset, char* dst, qint64 len)
{
    if (file < 0 || size_t(file) >= m_index.files.size())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No such packed file."));
    const ZstdFileRecord& f = m_index.files[size_t(file)];
    if (offset < 0 || quint64(offset) >= f.size || len <= 0)
        return qint64(0);
    len = std::min<qint64>(len, qint64(f.size) - offset);
    qint64 done = 0;
    while (done < len) {
        const quint64 pos = quint64(offset + done);
        const quint32 k = quint32(pos >> kZstdChunkShift);
        const qint64 logical = chunkLogicalBytes(f.size, k);
        FW_TRY(loadChunk(f.firstChunk + k, logical));
        const qint64 within = qint64(pos & quint64(kZstdChunkBytes - 1));
        const qint64 n = std::min(len - done, logical - within);
        std::memcpy(dst + done, m_chunk.constData() + within, size_t(n));
        done += n;
    }
    return done;
}

} // namespace fw::ampr
