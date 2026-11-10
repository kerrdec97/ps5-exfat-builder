#pragma once

// zstd asset packs: ampr_zstd.index (AMPRZPK1) + ampr_zstd-NNN.pak volumes
// (AMPRZDT1), the format ampr-emu-ng (quer3q, GPL-3.0-or-later) serves on the
// console and ps5-dump-forge (quer3q, GPL-3.0-or-later) writes. Layout from
// ampr-emu-ng docs/zstd-format.md and ps5-dump-forge docs/zstd.md (v0.5.4):
//
//   all integers little-endian; CRC = CRC-32 (IEEE); path hash = FNV-1a 64
//   over the UTF-8 path relative to the game root ('/' separators, no leading
//   slash, case kept).
//
//   index header (128 bytes)
//     0 "AMPRZPK1"  8 u32 version 1  12 u32 header size 128
//     16 u8 chunk_shift 18 (256 KiB)  17 u8 zstd level 1..22  18 u16 0
//     20 u32 volume_count  24 [16] archive_id  40 u64 file_count
//     48 u64 chunk_count  56 u64 files_off (128)  64 u64 chunks_off
//     72 u64 volumes_off  80 u64 strings_off  88 u64 strings_len
//     96 u64 total logical bytes  104 u64 0
//     112 u32 CRC of [128, end)  116 u32 CRC of the header with 112..120 zeroed
//     120 u64 0
//   file records (32 bytes, sorted by (path_hash, path bytes)):
//     u64 path_hash, u64 size, u32 first_chunk, u32 path_off, u16 path_len,
//     u16 flags (bit 0 = store-only), u32 0
//   chunk records (u64 per 256 KiB of each file):
//     bits 0..31 offset in the volume (from the file start, >= 64),
//     32..43 volume id, 44..61 stored_size - 1, 62..63 codec (0 raw, 1 zstd)
//   volume records (u64 volume file size, header included)
//   strings: the paths back to back, in file-record order
//
//   volume header (64 bytes)
//     0 "AMPRZDT1"  8 u32 1  12 u32 64  16 u32 volume id  20 u32 volume_count
//     24 [16] archive_id  40 u64 payload bytes (size - 64)  48 [12] 0
//     60 u32 CRC of the header with 60..64 zeroed
//   then the chunks back to back in index order. A volume is at most
//   4 GiB - 64 KiB (FAT32 limit, u32 offsets).
//
// Every chunk is one zstd frame (content size and checksum in the frame, no
// dictionary, window <= 256 KiB) or stored raw when zstd does not make it
// strictly smaller. The output does not depend on the thread count.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace fw::ampr {

inline constexpr char kZstdIndexMagic[] = "AMPRZPK1";
inline constexpr char kZstdVolumeMagic[] = "AMPRZDT1";
inline constexpr int kZstdChunkShift = 18;
inline constexpr qint64 kZstdChunkBytes = qint64(1) << kZstdChunkShift; // 256 KiB
inline constexpr qint64 kZstdVolumeLimit = 4294901760LL;               // 4 GiB - 64 KiB, header included
inline constexpr int kZstdIndexHeaderBytes = 128;
inline constexpr int kZstdVolumeHeaderBytes = 64;
inline constexpr int kZstdFileRecordBytes = 32;
inline constexpr int kZstdMaxVolumes = 4096;
inline constexpr int kZstdDefaultLevel = 5;
inline constexpr int kZstdMinLevel = 1;
inline constexpr int kZstdMaxLevel = 22;

inline const QString kZstdIndexName = QStringLiteral("ampr_zstd.index");
// "ampr_zstd-000.pak" (three digits at least).
QString zstdVolumeName(int volumeId);
// True for ampr_zstd.index and ampr_zstd-NNN.pak (exact case, as the runtime opens them).
bool isZstdPackFileName(const QString& name);

quint64 fnv1a64(const QByteArray& bytes);
quint32 crc32(const char* data, qint64 length, quint32 seed = 0);

struct ZstdChunk {
    quint32 offset = 0;  // in the volume, from the file start
    quint32 volume = 0;
    quint32 stored = 0;  // 1 .. 262144
    quint8 codec = 0;    // 0 raw, 1 zstd

    quint64 encode() const;
    static ZstdChunk decode(quint64 v);
};

struct ZstdFileRecord {
    QByteArray path;     // UTF-8, '/'-separated, case kept
    quint64 hash = 0;
    quint64 size = 0;
    quint32 firstChunk = 0;
    quint16 flags = 0;   // bit 0: store-only

    quint32 chunkCount() const { return quint32((size + quint64(kZstdChunkBytes) - 1) >> kZstdChunkShift); }
    // Larger files cannot be described (u32 chunk numbers).
    static constexpr quint64 kMaxSize = quint64(0xFFFFFFFFULL) << kZstdChunkShift;
};

struct ZstdIndex {
    quint8 level = kZstdDefaultLevel;
    std::array<char, 16> archiveId{};
    std::vector<ZstdFileRecord> files;  // in record order
    std::vector<quint64> chunks;        // encoded chunk records
    std::vector<quint64> volumeSizes;   // header included
    quint64 totalLogical = 0;

    // The record for a path (exact bytes), or -1.
    int find(const QByteArray& path) const;
};

// Serialises the index (header CRCs included). The files must already be in
// record order with consistent first_chunk values.
QByteArray encodeZstdIndex(const ZstdIndex& index);
// Parses and validates an index file: magic, version, sizes, both CRCs, the
// table bounds, record order, chunk numbering, chunk fields and paths.
Result<ZstdIndex> decodeZstdIndex(const QByteArray& bytes);

QByteArray encodeZstdVolumeHeader(quint32 volumeId, quint32 volumeCount, const std::array<char, 16>& archiveId,
                                  quint64 payloadBytes);
// Checks a volume header against the index (id, count, archive id, size, CRC).
Status checkZstdVolumeHeader(const QByteArray& header, quint32 volumeId, const ZstdIndex& index);

// Paths a pack may hold: non-empty, no '\\' or NUL, no empty, "." or ".."
// component, at most 65,535 bytes. A drive letter passes ("C:/x" is a folder
// named "C:" to the console): whoever writes such a path to this computer
// goes through HostPaths.
QString zstdPathProblem(const QByteArray& path);

// ---- writing -------------------------------------------------------------------------

class SequentialReader {
public:
    virtual ~SequentialReader() = default;
    // Reads up to `max` bytes; 0 at the end, an error on failure.
    virtual Result<qint64> read(char* dst, qint64 max) = 0;
};

struct PackInput {
    QByteArray path; // relative path stored in the index
    qint64 size = 0;
    std::function<Result<std::unique_ptr<SequentialReader>>()> open;
};

struct ZstdPackOptions {
    int level = kZstdDefaultLevel; // 1..22
    int threads = 0;               // 0 = every core (at most 64)
    // Compress-time check: every zstd frame is decoded again and compared with
    // its input before it is written.
    bool roundTripCheck = true;
    // Upper volume size (tests use small volumes); header included.
    qint64 volumeLimit = kZstdVolumeLimit;
    std::array<char, 16> archiveId{}; // all zero = random
    bool setProgressTotal = true;     // false: the caller set the phase total
};

struct ZstdPackResult {
    ZstdIndex index;
    QStringList volumes;          // file names written, in id order
    qint64 storedBytes = 0;       // chunk bytes in the volumes
    qint64 rawChunks = 0;
    qint64 zstdChunks = 0;
    std::vector<quint32> chunkCrcs; // CRC of each chunk's stored bytes (read-back check)
    qint64 elapsedMs = 0;
    int threads = 1;
};

// Packs `files` into `outDir` (which must exist): the volumes and
// ampr_zstd.index. Files are packed in record order. A file that reads short
// or long fails the pack. Progress advances by input bytes.
Result<ZstdPackResult> writeZstdPack(std::vector<PackInput> files, const ZstdPackOptions& options, const QString& outDir,
                                     const CancellationToken& token, ProgressReporter* progress = nullptr);

// Reads every volume back and checks the headers and each chunk's CRC
// against what was written (nothing is decompressed here: every frame was
// decoded and compared when it was packed).
// Progress advances by stored bytes.
Status verifyWrittenZstdPack(const QString& outDir, const ZstdPackResult& result, const CancellationToken& token,
                             ProgressReporter* progress = nullptr);

// ---- reading -------------------------------------------------------------------------

class RandomAccess {
public:
    virtual ~RandomAccess() = default;
    virtual qint64 size() const = 0;
    // Reads exactly `len` bytes at `offset`.
    virtual Status readAt(qint64 offset, char* dst, qint64 len) = 0;
};
using VolumeOpener = std::function<Result<std::shared_ptr<RandomAccess>>(quint32 volumeId)>;

// Serves the packed files of one archive. Not thread-safe: one reader per thread.
class ZstdPackReader {
public:
    // Parses the index and checks every volume's header and size.
    static Result<std::unique_ptr<ZstdPackReader>> open(const QByteArray& indexBytes, VolumeOpener opener);

    const ZstdIndex& index() const { return m_index; }
    // Reads up to `len` bytes of file `file` (record index) from `offset`;
    // returns the bytes read (0 at or past the end).
    Result<qint64> read(int file, qint64 offset, char* dst, qint64 len);

    ~ZstdPackReader();

private:
    ZstdPackReader() = default;
    Status loadChunk(quint32 chunkIndex, qint64 logicalBytes);

    ZstdIndex m_index;
    std::vector<std::shared_ptr<RandomAccess>> m_volumes;
    void* m_dctx = nullptr;
    QByteArray m_stored;
    QByteArray m_chunk;
    qint64 m_chunkIndex = -1;
};

} // namespace fw::ampr
