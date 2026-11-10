#include "engines/exfat/ExfatFormat.h"

#include "core/FileUtil.h"

#include <QTimeZone>

#include <algorithm>
#include <bit>
#include <limits>

namespace fw::exfat {

namespace detail {
extern const quint16 kRecommendedUpcase[];
extern const int kRecommendedUpcaseCount;
} // namespace detail

// ---- checksums -------------------------------------------------------------------

quint32 bootChecksum(const uchar* region, int bytesPerSector)
{
    const qint64 n = qint64(bytesPerSector) * 11;
    quint32 sum = 0;
    for (qint64 i = 0; i < n; ++i) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + quint32(region[i]);
    }
    return sum;
}

quint16 entrySetChecksum(const uchar* entries, int entryCount)
{
    const int n = entryCount * kEntrySize;
    quint16 sum = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 2 || i == 3)
            continue;
        sum = quint16(((sum & 1) ? 0x8000u : 0u) + (sum >> 1) + quint16(entries[i]));
    }
    return sum;
}

quint32 tableChecksum(const uchar* data, qint64 size)
{
    quint32 sum = 0;
    for (qint64 i = 0; i < size; ++i)
        sum = ((sum & 1) ? 0x80000000u : 0u) + (sum >> 1) + quint32(data[i]);
    return sum;
}

// ---- timestamps -------------------------------------------------------------------

Timestamp encodeTimestamp(const QDateTime& in)
{
    Timestamp ts;
    QDateTime t = in.isValid() ? in.toLocalTime() : QDateTime(QDate(1980, 1, 1), QTime(0, 0), QTimeZone::utc());
    int offsetSec = in.isValid() ? t.offsetFromUtc() : 0;
    // Offsets must be whole 15-minute steps in -16h..+15h45 (7-bit signed).
    if (offsetSec % 900 != 0 || offsetSec / 900 < -64 || offsetSec / 900 > 63) {
        t = t.toUTC();
        offsetSec = 0;
    }
    QDate d = t.date();
    QTime tm = t.time();
    if (d.year() < 1980) {
        d = QDate(1980, 1, 1);
        tm = QTime(0, 0);
    } else if (d.year() > 2107) {
        d = QDate(2107, 12, 31);
        tm = QTime(23, 59, 59, 990);
    }
    ts.value = (quint32(d.year() - 1980) << 25) | (quint32(d.month()) << 21) | (quint32(d.day()) << 16)
               | (quint32(tm.hour()) << 11) | (quint32(tm.minute()) << 5) | quint32(tm.second() / 2);
    ts.tenMs = quint8((tm.second() % 2) * 100 + tm.msec() / 10);
    ts.utcOffset = quint8(0x80 | (quint8(offsetSec / 900) & 0x7F));
    return ts;
}

QDateTime decodeTimestamp(quint32 v, quint8 tenMs, quint8 utcOffset)
{
    const int year = 1980 + int(v >> 25);
    const int month = int((v >> 21) & 0x0F);
    const int day = int((v >> 16) & 0x1F);
    const int hour = int((v >> 11) & 0x1F);
    const int minute = int((v >> 5) & 0x3F);
    const int sec = int(v & 0x1F) * 2;
    const QDate date(year, month, day);
    const QTime time(hour, minute, std::min(sec, 58));
    if (!date.isValid() || !time.isValid() || hour > 23 || minute > 59 || sec > 58 || tenMs > 199)
        return {};
    const int extraMs = int(tenMs) * 10;
    if (utcOffset & 0x80) {
        int steps = utcOffset & 0x7F;
        if (steps & 0x40)
            steps -= 0x80;
        QDateTime dt(date, time, QTimeZone(steps * 900));
        return dt.addMSecs(extraMs);
    }
    QDateTime dt(date, time); // local time, no offset recorded
    return dt.addMSecs(extraMs);
}

// ---- names ---------------------------------------------------------------------------

static bool isForbiddenNameChar(char16_t c)
{
    if (c < 0x20)
        return true;
    switch (c) {
    case u'"': case u'*': case u'/': case u':': case u'<': case u'>': case u'?': case u'\\': case u'|':
        return true;
    default:
        return false;
    }
}

QString checkFileName(const QString& name)
{
    if (name.isEmpty())
        return QStringLiteral("the name is empty");
    if (name.size() > kMaxNameLength)
        return QStringLiteral("the name is %1 characters long; exFAT allows %2").arg(name.size()).arg(kMaxNameLength);
    if (name == QStringLiteral(".") || name == QStringLiteral(".."))
        return QStringLiteral("\".\" and \"..\" are reserved names");
    for (const QChar ch : name) {
        if (isForbiddenNameChar(ch.unicode())) {
            if (ch.unicode() < 0x20)
                return QStringLiteral("the name contains a control character (U+%1)")
                    .arg(int(ch.unicode()), 4, 16, QLatin1Char('0'));
            return QStringLiteral("the name contains \"%1\", which exFAT does not allow").arg(ch);
        }
    }
    return {};
}

QString checkVolumeLabel(const QString& label)
{
    if (label.size() > kMaxLabelLength)
        return QStringLiteral("a volume label can have at most %1 characters").arg(kMaxLabelLength);
    for (const QChar ch : label)
        if (isForbiddenNameChar(ch.unicode()))
            return QStringLiteral("a volume label cannot contain \"%1\"").arg(ch);
    return {};
}

// ---- up-case table ------------------------------------------------------------------------

static Result<std::vector<char16_t>> expandUpcase(const quint16* v, int count)
{
    std::vector<char16_t> map(65536);
    for (int i = 0; i < 65536; ++i)
        map[size_t(i)] = char16_t(i);
    qint64 idx = 0;
    for (int i = 0; i < count; ++i) {
        if (idx >= 65536)
            break; // trailing data beyond the BMP is ignored
        if (v[i] == 0xFFFF && i + 1 < count) {
            idx += v[i + 1]; // identity run
            ++i;
            continue;
        }
        map[size_t(idx)] = char16_t(v[i]);
        ++idx;
    }
    return map;
}

const UpcaseTable& UpcaseTable::recommended()
{
    static const UpcaseTable table = [] {
        QByteArray data(detail::kRecommendedUpcaseCount * 2, Qt::Uninitialized);
        for (int i = 0; i < detail::kRecommendedUpcaseCount; ++i)
            put16(reinterpret_cast<uchar*>(data.data()) + 2 * i, detail::kRecommendedUpcase[i]);
        auto t = UpcaseTable::fromData(data);
        Q_ASSERT(t.ok());
        return std::move(t).value();
    }();
    return table;
}

Result<UpcaseTable> UpcaseTable::fromData(const QByteArray& data)
{
    if (data.size() < 2 || data.size() % 2 != 0 || data.size() > 2 * 65536 * 2)
        return corruptImage(QStringLiteral("its up-case table has an impossible size"),
                            QStringLiteral("up-case table length %1").arg(data.size()));
    std::vector<quint16> values(size_t(data.size() / 2));
    const auto* p = reinterpret_cast<const uchar*>(data.constData());
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = get16(p + 2 * i);
    auto map = expandUpcase(values.data(), int(values.size()));
    if (!map.ok())
        return map.takeError();
    UpcaseTable t;
    t.m_map = std::move(map).value();
    t.m_data = data;
    t.m_checksum = tableChecksum(p, data.size());
    return t;
}

QString UpcaseTable::upcase(const QString& s) const
{
    QString out(s.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < s.size(); ++i)
        out[i] = QChar(m_map[s.at(i).unicode()]);
    return out;
}

quint16 UpcaseTable::nameHash(const QString& name) const
{
    quint16 hash = 0;
    for (const QChar ch : name) {
        const char16_t c = m_map[ch.unicode()];
        hash = quint16(((hash & 1) ? 0x8000u : 0u) + (hash >> 1) + quint16(c & 0xFF));
        hash = quint16(((hash & 1) ? 0x8000u : 0u) + (hash >> 1) + quint16(c >> 8));
    }
    return hash;
}

// ---- geometry ------------------------------------------------------------------------------

Result<Geometry> computeGeometry(qint64 volumeBytes, int sectorBytes, int clusterBytes)
{
    if (sectorBytes < 512 || sectorBytes > 4096 || !std::has_single_bit(unsigned(sectorBytes)))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The sector size must be 512, 1024, 2048 or 4096 bytes."),
                     QStringLiteral("sector size %1").arg(sectorBytes));
    if (clusterBytes < sectorBytes || clusterBytes > 32 * kMiB || !std::has_single_bit(unsigned(clusterBytes)))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The cluster size must be a power of two between the sector size and 32 MB."),
                     QStringLiteral("cluster size %1").arg(clusterBytes));
    Geometry g;
    g.bytesPerSectorShift = std::countr_zero(unsigned(sectorBytes));
    g.sectorsPerClusterShift = std::countr_zero(unsigned(clusterBytes / sectorBytes));

    volumeBytes = volumeBytes / sectorBytes * sectorBytes;
    const qint64 align = std::max<qint64>(kMiB, clusterBytes);
    const qint64 fatOff = alignUp(qint64(24) * sectorBytes, align);
    const auto tooSmall = [&] {
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The image size is too small for an exFAT volume."),
                     QStringLiteral("volume %1 bytes, cluster %2").arg(volumeBytes).arg(clusterBytes));
    };
    if (volumeBytes <= fatOff + clusterBytes)
        return tooSmall();
    const qint64 maxClusters = (volumeBytes - fatOff) / clusterBytes;
    if (maxClusters > qint64(kMaxClusterCount) + 4096)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The cluster size is too small for an image this large. Choose a larger cluster size."),
                     QStringLiteral("%1 clusters").arg(maxClusters));
    const qint64 fatBytes = alignUp((maxClusters + 2) * 4, sectorBytes);
    const qint64 heapOff = alignUp(fatOff + fatBytes, align);
    if (heapOff + clusterBytes > volumeBytes)
        return tooSmall();
    const qint64 clusters = (volumeBytes - heapOff) / clusterBytes;
    if (clusters > qint64(kMaxClusterCount))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The cluster size is too small for an image this large. Choose a larger cluster size."),
                     QStringLiteral("%1 clusters").arg(clusters));
    g.volumeLength = quint64(volumeBytes / sectorBytes);
    g.fatOffset = quint32(fatOff / sectorBytes);
    g.fatLength = quint32(fatBytes / sectorBytes);
    g.clusterHeapOffset = quint32(heapOff / sectorBytes);
    g.clusterCount = quint32(clusters);
    return g;
}

QString counted(qint64 n, const char* one, const char* many)
{
    return n == 1 ? QStringLiteral("1 ") + QLatin1String(one) : formatCount(n) + u' ' + QLatin1String(many);
}

// ---- boot region ----------------------------------------------------------------------------

Error corruptImage(const QString& what, const QString& detail)
{
    return Error(ErrorCode::CorruptData,
                 QStringLiteral("The image is damaged: %1. It cannot be used safely; rebuild it from the game folder.")
                     .arg(what),
                 detail);
}

QByteArray buildBootRegion(const BootInfo& info)
{
    const Geometry& g = info.geo;
    const int bps = int(g.sectorBytes());
    QByteArray region(bps * kBootRegionSectors, '\0');
    auto* s = reinterpret_cast<uchar*>(region.data());

    // Main boot sector (§3.1).
    s[0] = 0xEB;
    s[1] = 0x76;
    s[2] = 0x90;
    std::memcpy(s + 3, "EXFAT   ", 8);
    put64(s + 64, quint64(info.partitionOffset));
    put64(s + 72, g.volumeLength);
    put32(s + 80, g.fatOffset);
    put32(s + 84, g.fatLength);
    put32(s + 88, g.clusterHeapOffset);
    put32(s + 92, g.clusterCount);
    put32(s + 96, info.rootCluster);
    put32(s + 100, info.serial);
    put16(s + 104, info.revision);
    put16(s + 106, info.volumeFlags);
    s[108] = uchar(g.bytesPerSectorShift);
    s[109] = uchar(g.sectorsPerClusterShift);
    s[110] = info.numberOfFats;
    s[111] = 0x80; // DriveSelect
    s[112] = info.percentInUse;
    std::memset(s + 120, 0xF4, 390); // BootCode: HLT
    s[510] = 0x55;
    s[511] = 0xAA;

    // Main extended boot sectors 1..8 (§3.2): signature 0xAA550000 at the end.
    for (int i = 1; i <= 8; ++i)
        put32(s + qint64(i) * bps + bps - 4, 0xAA550000u);
    // Sector 9: OEM parameters (all zero = unused), sector 10: reserved.

    // Sector 11: boot checksum repeated (§3.4).
    const quint32 sum = bootChecksum(s, bps);
    for (int off = 0; off < bps; off += 4)
        put32(s + qint64(11) * bps + off, sum);
    return region;
}

int bootSectorBytesPerSector(const QByteArray& first)
{
    if (first.size() < 512)
        return 0;
    const int shift = uchar(first.at(108));
    if (shift < 9 || shift > 12)
        return 0;
    return 1 << shift;
}

Result<BootInfo> parseBootRegion(const QByteArray& region)
{
    const auto bad = [](const QString& why) {
        return corruptImage(QStringLiteral("its boot sector is not valid"), why);
    };
    if (region.size() < 512)
        return bad(QStringLiteral("boot region shorter than one sector"));
    const auto* s = reinterpret_cast<const uchar*>(region.constData());
    if (std::memcmp(s + 3, "EXFAT   ", 8) != 0)
        return corruptImage(QStringLiteral("it is not an exFAT volume"),
                            QStringLiteral("file system name is not \"EXFAT   \""));
    if (s[0] != 0xEB || s[1] != 0x76 || s[2] != 0x90)
        return bad(QStringLiteral("JumpBoot is not EB 76 90"));
    for (int i = 11; i < 64; ++i)
        if (s[i] != 0)
            return bad(QStringLiteral("MustBeZero region is not zero"));
    if (s[510] != 0x55 || s[511] != 0xAA)
        return bad(QStringLiteral("boot signature missing"));

    BootInfo info;
    Geometry& g = info.geo;
    g.bytesPerSectorShift = s[108];
    g.sectorsPerClusterShift = s[109];
    if (g.bytesPerSectorShift < 9 || g.bytesPerSectorShift > 12)
        return bad(QStringLiteral("BytesPerSectorShift %1 out of range").arg(g.bytesPerSectorShift));
    if (g.sectorsPerClusterShift > 25 - g.bytesPerSectorShift)
        return bad(QStringLiteral("SectorsPerClusterShift %1 out of range").arg(g.sectorsPerClusterShift));
    const int bps = int(g.sectorBytes());
    if (region.size() < bps * kBootRegionSectors)
        return bad(QStringLiteral("boot region truncated"));

    info.partitionOffset = qint64(get64(s + 64));
    g.volumeLength = get64(s + 72);
    g.fatOffset = get32(s + 80);
    g.fatLength = get32(s + 84);
    g.clusterHeapOffset = get32(s + 88);
    g.clusterCount = get32(s + 92);
    info.rootCluster = get32(s + 96);
    info.serial = get32(s + 100);
    info.revision = get16(s + 104);
    info.volumeFlags = get16(s + 106);
    info.numberOfFats = s[110];
    info.percentInUse = s[112];

    if ((info.revision >> 8) != 1)
        return bad(QStringLiteral("file system revision %1.%2 is not supported")
                       .arg(info.revision >> 8).arg(info.revision & 0xFF));
    if (info.numberOfFats != 1 && info.numberOfFats != 2)
        return bad(QStringLiteral("NumberOfFats %1").arg(info.numberOfFats));
    if (g.volumeLength < quint64((1u << 20) >> g.bytesPerSectorShift))
        return bad(QStringLiteral("VolumeLength %1 too small").arg(g.volumeLength));
    // Larger than any file can be: the byte size would not fit 63 bits.
    if (g.volumeLength > (quint64(std::numeric_limits<qint64>::max()) >> g.bytesPerSectorShift))
        return bad(QStringLiteral("VolumeLength %1 too large").arg(g.volumeLength));
    if (g.fatOffset < 24)
        return bad(QStringLiteral("FatOffset %1 < 24").arg(g.fatOffset));
    const quint64 fatEnd = quint64(g.fatOffset) + quint64(g.fatLength) * info.numberOfFats;
    if (g.clusterHeapOffset < fatEnd)
        return bad(QStringLiteral("ClusterHeapOffset %1 overlaps the FAT").arg(g.clusterHeapOffset));
    if (quint64(g.clusterHeapOffset) >= g.volumeLength)
        return bad(QStringLiteral("ClusterHeapOffset %1 beyond VolumeLength").arg(g.clusterHeapOffset));
    const quint64 maxClusters = (g.volumeLength - g.clusterHeapOffset) >> g.sectorsPerClusterShift;
    if (g.clusterCount == 0 || g.clusterCount > kMaxClusterCount || g.clusterCount > maxClusters)
        return bad(QStringLiteral("ClusterCount %1 out of range (max %2)").arg(g.clusterCount).arg(maxClusters));
    if (quint64(g.fatLength) * quint64(bps) < (quint64(g.clusterCount) + 2) * 4)
        return bad(QStringLiteral("FatLength %1 too small for %2 clusters").arg(g.fatLength).arg(g.clusterCount));
    if (!g.isValidCluster(info.rootCluster))
        return bad(QStringLiteral("FirstClusterOfRootDirectory %1 out of range").arg(info.rootCluster));

    for (int i = 1; i <= 8; ++i)
        if (get32(s + qint64(i) * bps + bps - 4) != 0xAA550000u)
            return bad(QStringLiteral("extended boot sector %1 has no signature").arg(i));

    const quint32 sum = bootChecksum(s, bps);
    for (int off = 0; off < bps; off += 4)
        if (get32(s + qint64(11) * bps + off) != sum)
            return corruptImage(QStringLiteral("its boot checksum does not match"),
                                QStringLiteral("computed %1, stored %2")
                                    .arg(sum, 8, 16, QLatin1Char('0'))
                                    .arg(get32(s + qint64(11) * bps + off), 8, 16, QLatin1Char('0')));
    return info;
}

} // namespace fw::exfat
