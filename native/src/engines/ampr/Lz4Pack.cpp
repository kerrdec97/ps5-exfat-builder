// AMPRPAK4 reader (unpack only). Layout as ps5-dump-forge v0.5.4 reads it
// (crates/ps5-dump-forge-lz4/src/format.rs, reader.rs; GPL-3.0-or-later):
//
//   manifest ampr_assets.index, header 128 bytes:
//     0 "AMPRPAK4"  8 u32 4  12 u32 128  16 u32 0  20 u32 0x01020304
//     24 [16] build_id  40 u64 files  48 u64 chunks  56 u32 packs
//     60 u32 48  64 u32 12  68 u32 32  72 u64 files_off (128)
//     80 u64 chunks_off  88 u64 packs_off  96 u64 strings_off  104 u64 strings_len
//     112 u32 CRC [128, end)  116 u32 CRC of the header with 116..120 zeroed
//   file record 48: u64 hash, u64 size, i64 mtime, u32 first_chunk,
//     u32 chunk_count, u32 path_off, u32 path_len, u32 flags (1 PACKED,
//     2 STORE_ONLY, 4 STREAMING, 8 HOT, 16 RANDOM), u8 block_shift, u8 class, u16 0
//     path "/app0/<rel>" NUL-terminated
//   chunk record 12: u64 location (offset low 48 bits | pack_id << 48),
//     u32 descriptor ((stored-1) bits 0..19 | codec << 20 (0 raw, 1 LZ4 block))
//   pack record 32: u64 payload_bytes, u64 file_size, u32 name_off,
//     u32 name_len, u32 flags, u32 io_page_size
//   volume header AMPRDAT3 64: u32 3 @8, u32 64 @12, u32 pack_id @16,
//     u32 flags @20, build_id @24, u64 payload_offset @40, u64 payload_bytes @48,
//     u32 CRC @56 (56..60 zeroed)
//   optional ampr_assets.index.crc (AMPRCRC1): one CRC-32 per decoded chunk.
//
// A chunk is stored raw or as one raw LZ4 block (no frame).

#include "engines/ampr/Lz4Pack.h"

#include "engines/ampr/GameFiles.h"
#include "engines/ampr/ZstdPack.h"

#include <lz4.h>

#include <QByteArray>
#include <QHash>
#include <QSet>

#include <cstring>
#include <optional>

namespace fw::ampr {

namespace {

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
    return Error(ErrorCode::CorruptData, QStringLiteral("The LZ4 pack manifest (ampr_assets.index) is damaged: %1.").arg(what));
}

constexpr quint32 kPacked = 1;
constexpr qint64 kMaxManifest = 512LL * 1024 * 1024;
constexpr qint64 kMaxStored = 1LL << 20;

} // namespace

struct Lz4PackReader::Impl {
    struct File {
        QString path;
        qint64 size = 0;
        quint32 firstChunk = 0;
        quint32 chunkCount = 0;
        int shift = 16;
    };
    struct Chunk {
        quint64 offset = 0;
        quint32 pack = 0;
        quint32 stored = 0;
        quint8 codec = 0;
    };
    std::vector<File> files; // packed records only
    std::vector<Chunk> chunks;
    std::vector<std::shared_ptr<RandomAccess>> volumes;
    QStringList packFiles;
    std::vector<quint32> crcs; // decoded-chunk CRCs, empty when there is no sidecar
    QByteArray stored, chunk;
    qint64 cached = -1;
};

Lz4PackReader::Lz4PackReader() : d(std::make_unique<Impl>()) {}
Lz4PackReader::~Lz4PackReader() = default;

int Lz4PackReader::fileCount() const { return int(d->files.size()); }
QString Lz4PackReader::path(int file) const { return d->files[size_t(file)].path; }
qint64 Lz4PackReader::size(int file) const { return d->files[size_t(file)].size; }
QStringList Lz4PackReader::packFiles() const { return d->packFiles; }

Result<std::unique_ptr<Lz4PackReader>> Lz4PackReader::open(std::shared_ptr<GameFiles> files, const CancellationToken& token)
{
    std::unique_ptr<Lz4PackReader> r(new Lz4PackReader);
    Impl& d = *r->d;
    const int mi = files->find(kLz4IndexName);
    if (mi < 0)
        return Error(ErrorCode::NotFound, QStringLiteral("ampr_assets.index is missing."));
    const qint64 msize = files->items()[size_t(mi)].size;
    if (msize < 128 || msize > kMaxManifest)
        return corrupt(QStringLiteral("its size"));
    QByteArray m(qsizetype(msize), '\0');
    FW_TRY(itemAccess(files, mi)->readAt(0, m.data(), msize));
    const char* h = m.constData();
    if (std::memcmp(h, "AMPRPAK4", 8) != 0)
        return Error(ErrorCode::CorruptData, QStringLiteral("ampr_assets.index is not an LZ4 pack manifest."));
    if (get32(h + 8) != 4 || get32(h + 12) != 128 || get32(h + 16) != 0 || get32(h + 20) != 0x01020304U)
        return Error(ErrorCode::Unsupported, QStringLiteral("ampr_assets.index has a version this app does not read."));
    const quint64 fc = get64(h + 40), cc = get64(h + 48), pc = get32(h + 56);
    if (get32(h + 60) != 48 || get32(h + 64) != 12 || get32(h + 68) != 32 || fc > 2000000 || cc > 16000000 || pc > 1024)
        return corrupt(QStringLiteral("record sizes or counts"));
    const quint64 filesOff = get64(h + 72), chunksOff = get64(h + 80), packsOff = get64(h + 88);
    const quint64 stringsOff = get64(h + 96), stringsLen = get64(h + 104);
    if (filesOff != 128 || chunksOff != filesOff + 48 * fc || packsOff != chunksOff + 12 * cc || stringsOff != packsOff + 32 * pc
        || stringsOff > quint64(msize) || stringsLen != quint64(msize) - stringsOff)
        return corrupt(QStringLiteral("table offsets"));
    if (crc32(h + 128, msize - 128) != get32(h + 112))
        return corrupt(QStringLiteral("table checksum"));
    {
        char head[128];
        std::memcpy(head, h, 128);
        std::memset(head + 116, 0, 4);
        if (crc32(head, 128) != get32(h + 116))
            return corrupt(QStringLiteral("header checksum"));
    }
    const char* buildId = h + 24;
    auto stringAt = [&](quint32 off, quint32 len) -> std::optional<QByteArray> {
        if (quint64(off) + len + 1 > stringsLen || h[stringsOff + off + len] != '\0')
            return std::nullopt;
        QByteArray s(h + stringsOff + off, qsizetype(len));
        if (s.contains('\0') || s.contains('\\'))
            return std::nullopt;
        return s;
    };

    // Packs (volumes).
    std::vector<quint64> payloadOffset(static_cast<size_t>(pc), 0);
    for (quint64 i = 0; i < pc; ++i) {
        const char* p = h + packsOff + i * 32;
        const quint64 payload = get64(p), fileSize = get64(p + 8);
        const quint32 flags = get32(p + 24), page = get32(p + 28);
        auto name = stringAt(get32(p + 16), get32(p + 20));
        if (!name || name->isEmpty() || name->contains('/'))
            return corrupt(QStringLiteral("name of pack %1").arg(i));
        if (fileSize < 64 || payload > fileSize || !(flags & 2) || (flags & ~3U) || page < 4096 || page > (1U << 20)
            || (page & (page - 1)))
            return corrupt(QStringLiteral("pack %1").arg(i));
        payloadOffset[size_t(i)] = fileSize - payload;
        const QString volName = QString::fromUtf8(*name);
        const int vi = files->find(volName);
        if (vi < 0 || files->items()[size_t(vi)].isDir)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is missing from the LZ4-packed game.").arg(volName));
        if (quint64(files->items()[size_t(vi)].size) != fileSize)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 has the wrong size.").arg(volName));
        auto access = itemAccess(files, vi);
        QByteArray vh(64, '\0');
        FW_TRY(access->readAt(0, vh.data(), 64));
        QByteArray zeroed = vh;
        std::memset(zeroed.data() + 56, 0, 4);
        if (std::memcmp(vh.constData(), "AMPRDAT3", 8) != 0 || get32(vh.constData() + 8) != 3 || get32(vh.constData() + 12) != 64
            || get32(vh.constData() + 16) != quint32(i) || std::memcmp(vh.constData() + 24, buildId, 16) != 0
            || get64(vh.constData() + 40) != payloadOffset[size_t(i)] || get64(vh.constData() + 48) != payload
            || crc32(zeroed.constData(), 64) != get32(vh.constData() + 56))
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 does not belong to this LZ4 pack (or is damaged).").arg(volName));
        d.volumes.push_back(std::move(access));
        d.packFiles << volName;
    }
    if (!token.checkpoint())
        return Error::cancelled();

    // Chunks.
    d.chunks.resize(size_t(cc));
    for (quint64 i = 0; i < cc; ++i) {
        const char* c = h + chunksOff + i * 12;
        const quint64 loc = get64(c);
        const quint32 desc = get32(c + 8);
        Impl::Chunk& k = d.chunks[size_t(i)];
        k.offset = loc & 0xFFFFFFFFFFFFULL;
        k.pack = quint32(loc >> 48);
        k.stored = (desc & 0xFFFFF) + 1;
        k.codec = quint8((desc >> 20) & 3);
        if (k.pack >= pc || k.codec > 1 || (desc >> 30) != 0 || qint64(k.stored) > kMaxStored
            || k.offset < payloadOffset[k.pack] || k.offset + k.stored > quint64(d.volumes[k.pack]->size()))
            return corrupt(QStringLiteral("chunk %1").arg(i));
    }

    // Packed file records (loose records are listed by the game itself).
    QSet<QString> seen;
    for (quint64 i = 0; i < fc; ++i) {
        const char* f = h + filesOff + i * 48;
        const quint32 flags = get32(f + 40);
        auto raw = stringAt(get32(f + 32), get32(f + 36));
        if (!raw)
            return corrupt(QStringLiteral("path of record %1").arg(i));
        QString path = QString::fromUtf8(*raw);
        if (!path.startsWith(QLatin1String("/app0/"), Qt::CaseInsensitive))
            return corrupt(QStringLiteral("path \"%1\"").arg(path));
        path = path.mid(6);
        if (!zstdPathProblem(path.toUtf8()).isEmpty())
            return corrupt(QStringLiteral("path \"%1\"").arg(path));
        if (seen.contains(path.toLower()))
            return corrupt(QStringLiteral("\"%1\" is listed twice").arg(path));
        seen.insert(path.toLower());
        if (!(flags & kPacked))
            continue;
        Impl::File file;
        file.path = path;
        file.size = qint64(get64(f + 8));
        file.firstChunk = get32(f + 24);
        file.chunkCount = get32(f + 28);
        file.shift = quint8(f[44]);
        if (file.size < 0 || file.shift < 14 || file.shift > 20 || (flags & 4 && flags & 16)
            || quint64(file.chunkCount) != (quint64(file.size) + (1ULL << file.shift) - 1) >> file.shift
            || quint64(file.firstChunk) + file.chunkCount > cc)
            return corrupt(QStringLiteral("record \"%1\"").arg(path));
        const qint64 block = qint64(1) << file.shift;
        for (quint32 k = 0; k < file.chunkCount; ++k) {
            const Impl::Chunk& c = d.chunks[file.firstChunk + k];
            const qint64 derived = std::min(block, file.size - qint64(k) * block);
            if (qint64(c.stored) > block || (c.codec == 0 && qint64(c.stored) != derived))
                return corrupt(QStringLiteral("chunk %1 of \"%2\"").arg(k).arg(path));
        }
        d.files.push_back(file);
    }

    // The optional CRC sidecar (CRC of each decoded chunk).
    const QString crcName = kLz4IndexName + QStringLiteral(".crc");
    if (const int ci = files->find(crcName); ci >= 0) {
        const qint64 sz = files->items()[size_t(ci)].size;
        if (sz != qint64(48 + 4 * cc))
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 has the wrong size.").arg(crcName));
        QByteArray c(qsizetype(sz), '\0');
        FW_TRY(itemAccess(files, ci)->readAt(0, c.data(), sz));
        if (std::memcmp(c.constData(), "AMPRCRC1", 8) != 0 || std::memcmp(c.constData() + 16, buildId, 16) != 0
            || get64(c.constData() + 32) != cc || crc32(c.constData() + 48, sz - 48) != get32(c.constData() + 40))
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 does not match the manifest.").arg(crcName));
        d.crcs.resize(size_t(cc));
        for (quint64 i = 0; i < cc; ++i)
            d.crcs[size_t(i)] = get32(c.constData() + 48 + 4 * i);
        d.packFiles << crcName;
    }
    d.packFiles << kLz4IndexName;
    const QString runtimeProfile = kLz4IndexName + QStringLiteral(".runtime");
    if (files->find(runtimeProfile) >= 0)
        d.packFiles << runtimeProfile;
    d.chunk.resize(qsizetype(kMaxStored));
    return r;
}

Result<qint64> Lz4PackReader::read(int file, qint64 offset, char* dst, qint64 len)
{
    if (file < 0 || size_t(file) >= d->files.size())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("No such packed file."));
    const Impl::File& f = d->files[size_t(file)];
    if (offset < 0 || offset >= f.size || len <= 0)
        return qint64(0);
    len = std::min(len, f.size - offset);
    const qint64 block = qint64(1) << f.shift;
    qint64 done = 0;
    while (done < len) {
        const qint64 pos = offset + done;
        const quint32 k = quint32(pos >> f.shift);
        const qint64 derived = std::min(block, f.size - qint64(k) * block);
        const quint32 ci = f.firstChunk + k;
        if (d->cached != qint64(ci)) {
            d->cached = -1;
            const Impl::Chunk& c = d->chunks[ci];
            if (c.codec == 0) {
                FW_TRY(d->volumes[c.pack]->readAt(qint64(c.offset), d->chunk.data(), c.stored));
            } else {
                d->stored.resize(qsizetype(c.stored));
                FW_TRY(d->volumes[c.pack]->readAt(qint64(c.offset), d->stored.data(), c.stored));
                const int n = LZ4_decompress_safe(d->stored.constData(), d->chunk.data(), int(c.stored), int(derived));
                if (n != derived)
                    return Error(ErrorCode::CorruptData, QStringLiteral("A chunk of %1 is damaged (LZ4).").arg(f.path));
            }
            if (!d->crcs.empty() && crc32(d->chunk.constData(), derived) != d->crcs[ci])
                return Error(ErrorCode::CorruptData, QStringLiteral("A chunk of %1 fails its checksum.").arg(f.path));
            d->cached = qint64(ci);
        }
        const qint64 within = pos - qint64(k) * block;
        const qint64 n = std::min(len - done, derived - within);
        std::memcpy(dst + done, d->chunk.constData() + within, size_t(n));
        done += n;
    }
    return done;
}

} // namespace fw::ampr
