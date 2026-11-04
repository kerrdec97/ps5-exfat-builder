#pragma once

// exFAT on-disk format: constants, checksums, timestamps, the up-case table,
// volume geometry and the boot region. Written from the public Microsoft
// "exFAT file system specification" (2019); section numbers below refer to it.
//
// Everything here is pure computation (no I/O) so the writer, the reader and
// the tests share one definition of the format.

#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <cstdint>
#include <cstring>
#include <vector>

namespace fw::exfat {

// ---- constants ----------------------------------------------------------------
inline constexpr quint32 kFirstDataCluster = 2;
inline constexpr quint32 kFatEndOfChain = 0xFFFFFFFFu;
inline constexpr quint32 kFatBadCluster = 0xFFFFFFF7u;
inline constexpr quint32 kFatMediaEntry = 0xFFFFFFF8u;  // FatEntry[0] (§4.1.1)
inline constexpr quint32 kMaxClusterCount = 0xFFFFFFF5u; // 2^32 - 11 (§3.1.9)
inline constexpr int kBootRegionSectors = 12;            // main; backup follows (§3.1)
inline constexpr int kEntrySize = 32;
inline constexpr int kNameCharsPerEntry = 15;
inline constexpr int kMaxNameLength = 255;               // UTF-16 code units (§7.6.3)
inline constexpr int kMaxLabelLength = 11;               // §7.3.2
inline constexpr qint64 kVolumeFlagsOffset = 106;        // boot sector, outside the checksum (§3.1.13)
inline constexpr quint16 kVolumeDirty = 0x0002;          // VolumeFlags bit 1 (§3.1.13.2)
inline constexpr qint64 kMaxDirectoryBytes = 256LL * 1024 * 1024; // §6.2
inline constexpr qint64 kMiB = 1024LL * 1024;
inline constexpr qint64 kGiB = 1024LL * kMiB;

enum : quint8 {
    kTypeEndOfDirectory = 0x00,
    kTypeAllocationBitmap = 0x81,
    kTypeUpcaseTable = 0x82,
    kTypeVolumeLabel = 0x83,
    kTypeFile = 0x85,
    kTypeVolumeGuid = 0xA0,
    kTypeStreamExtension = 0xC0,
    kTypeFileName = 0xC1,
};
inline constexpr quint8 kEntryInUse = 0x80;
inline constexpr quint8 kEntryBenign = 0x20;     // TypeImportance bit
inline constexpr quint8 kEntrySecondary = 0x40;  // TypeCategory bit

// File attributes (§7.4.4).
enum : quint16 {
    kAttrReadOnly = 0x0001,
    kAttrHidden = 0x0002,
    kAttrSystem = 0x0004,
    kAttrDirectory = 0x0010,
    kAttrArchive = 0x0020,
};
inline constexpr quint16 kAttrMask = kAttrReadOnly | kAttrHidden | kAttrSystem | kAttrDirectory | kAttrArchive;

// GeneralSecondaryFlags (§6.4.2).
inline constexpr quint8 kFlagAllocationPossible = 0x01;
inline constexpr quint8 kFlagNoFatChain = 0x02;

// ---- little-endian helpers -------------------------------------------------------
inline void put16(uchar* p, quint16 v) { p[0] = uchar(v); p[1] = uchar(v >> 8); }
inline void put32(uchar* p, quint32 v) { for (int i = 0; i < 4; ++i) p[i] = uchar(v >> (8 * i)); }
inline void put64(uchar* p, quint64 v) { for (int i = 0; i < 8; ++i) p[i] = uchar(v >> (8 * i)); }
inline quint16 get16(const uchar* p) { return quint16(p[0] | (p[1] << 8)); }
inline quint32 get32(const uchar* p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }
inline quint64 get64(const uchar* p) { return quint64(get32(p)) | (quint64(get32(p + 4)) << 32); }

inline qint64 alignUp(qint64 v, qint64 a) { return (v + a - 1) / a * a; }
inline qint64 ceilDiv(qint64 v, qint64 d) { return (v + d - 1) / d; }

// ---- checksums -------------------------------------------------------------------
// Boot checksum over sectors 0..10 of a boot region, skipping VolumeFlags
// (bytes 106-107) and PercentInUse (byte 112) of sector 0 (§3.4).
quint32 bootChecksum(const uchar* region, int bytesPerSector);
// Entry-set checksum over `entryCount` entries, skipping SetChecksum (§6.3.3).
quint16 entrySetChecksum(const uchar* entries, int entryCount);
// Up-case table checksum (§7.2.2).
quint32 tableChecksum(const uchar* data, qint64 size);

// ---- timestamps (§7.4.8 - §7.4.10) -------------------------------------------------
struct Timestamp {
    quint32 value = 0;   // DOS-style date/time, 2-second resolution
    quint8 tenMs = 0;    // 0..199 (only for create/modify)
    quint8 utcOffset = 0; // bit 7 = valid, bits 0-6 = signed 15-minute steps
};
// Stores local time with its UTC offset, as Windows does. Times outside
// 1980..2107 are clamped. Invalid times become 1980-01-01 00:00:00 UTC.
Timestamp encodeTimestamp(const QDateTime& t);
// Returns an invalid QDateTime for impossible field values.
QDateTime decodeTimestamp(quint32 value, quint8 tenMs, quint8 utcOffset);

// ---- names ---------------------------------------------------------------------------
// Empty string when `name` is a legal exFAT file name, else the reason (§7.7.3).
QString checkFileName(const QString& name);
// Empty string when `label` is a legal volume label, else the reason.
QString checkVolumeLabel(const QString& label);

// ---- up-case table (§7.2) ---------------------------------------------------------------
class UpcaseTable {
public:
    // The specification's recommended table (what Windows writes).
    static const UpcaseTable& recommended();
    // Parses a table read from an image (compressed or not). Fails on a
    // malformed table.
    static Result<UpcaseTable> fromData(const QByteArray& data);

    char16_t map(char16_t c) const { return m_map[c]; }
    QString upcase(const QString& s) const;
    // NameHash of a file name (§7.6.4): computed over the up-cased name.
    quint16 nameHash(const QString& name) const;

    const QByteArray& data() const { return m_data; }
    quint32 checksum() const { return m_checksum; }

private:
    std::vector<char16_t> m_map; // 65536 entries
    QByteArray m_data;
    quint32 m_checksum = 0;
};

// ---- geometry (§3.1) -------------------------------------------------------------------
struct Geometry {
    int bytesPerSectorShift = 9;
    int sectorsPerClusterShift = 0;
    quint64 volumeLength = 0;      // sectors
    quint32 fatOffset = 0;         // sectors
    quint32 fatLength = 0;         // sectors
    quint32 clusterHeapOffset = 0; // sectors
    quint32 clusterCount = 0;

    qint64 sectorBytes() const { return qint64(1) << bytesPerSectorShift; }
    qint64 clusterBytes() const { return qint64(1) << (bytesPerSectorShift + sectorsPerClusterShift); }
    qint64 volumeBytes() const { return qint64(volumeLength) << bytesPerSectorShift; }
    qint64 fatOffsetBytes() const { return qint64(fatOffset) << bytesPerSectorShift; }
    qint64 fatBytes() const { return qint64(fatLength) << bytesPerSectorShift; }
    qint64 heapOffsetBytes() const { return qint64(clusterHeapOffset) << bytesPerSectorShift; }
    qint64 clusterOffset(quint32 cluster) const
    {
        return heapOffsetBytes() + qint64(cluster - kFirstDataCluster) * clusterBytes();
    }
    bool isValidCluster(quint32 c) const { return c >= kFirstDataCluster && c < kFirstDataCluster + clusterCount; }
    qint64 bitmapBytes() const { return ceilDiv(clusterCount, 8); }
};

// Lays out a volume of `volumeBytes` (rounded down to whole sectors): FAT and
// cluster heap aligned to max(1 MiB, cluster size), like exfatprogs' mkfs.
Result<Geometry> computeGeometry(qint64 volumeBytes, int sectorBytes, int clusterBytes);

// ---- boot region (§3) ------------------------------------------------------------------
struct BootInfo {
    Geometry geo;
    quint32 rootCluster = 0;
    quint32 serial = 0;
    quint16 revision = 0x0100;
    quint16 volumeFlags = 0;
    quint8 percentInUse = 0xFF;
    quint8 numberOfFats = 1;
    qint64 partitionOffset = 0;
};

// The 12 sectors of a boot region (main or backup are identical).
QByteArray buildBootRegion(const BootInfo& info);

// Parses and validates a 12-sector boot region. On failure the error is
// CorruptData with a user-facing message and the technical reason in detail.
Result<BootInfo> parseBootRegion(const QByteArray& region);

// Bytes per sector announced by a boot sector (reads byte 108), or 0.
int bootSectorBytesPerSector(const QByteArray& firstSector);

// User-facing CorruptData error ("The image is damaged...").
Error corruptImage(const QString& what, const QString& detail = {});

} // namespace fw::exfat
