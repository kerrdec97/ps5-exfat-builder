#include "core/ArchiveStream.h"

#include "core/ArchiveReader.h"
#include "core/FileUtil.h"
#include "core/Zip.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace fw::archive {

void ContentDigest::add(const char* data, qint64 size)
{
    // zlib takes 32-bit lengths.
    while (size > 0) {
        const uInt n = uInt(std::min<qint64>(size, 1 << 30));
        m_crc = quint32(::crc32(m_crc, reinterpret_cast<const Bytef*>(data), n));
        m_adler = quint32(::adler32(m_adler, reinterpret_cast<const Bytef*>(data), n));
        data += n;
        size -= n;
    }
}

QString streamingLogLine(const QString& archive)
{
    return QStringLiteral("Building straight from %1 (one pass through the archive; nothing is unpacked to disk)")
        .arg(QDir::toNativeSeparators(archive));
}

QString unpackFirstLogLine(const QString& archive, const QString& why)
{
    return QStringLiteral("Unpacking %1 into the job's temporary folder first: %2")
        .arg(QDir::toNativeSeparators(archive), why);
}

namespace {

constexpr qint64 kHoleMin = 1024 * 1024; // as the image writers: shorter zero runs are written

bool allZero(const char* p, qint64 n)
{
    qint64 i = 0;
    for (; i + 8 <= n; i += 8) {
        quint64 v;
        std::memcpy(&v, p + i, 8);
        if (v)
            return false;
    }
    for (; i < n; ++i)
        if (p[i])
            return false;
    return true;
}

// Gathers consecutive bytes and writes them at their offset in one call.
class ImageWriter {
public:
    ImageWriter(QFile& f, qint64 bufferBytes) : m_f(f) { m_buf.resize(size_t(std::max<qint64>(64 * 1024, bufferBytes))); }

    Status put(qint64 at, const char* p, qint64 n)
    {
        const qint64 cap = qint64(m_buf.size());
        while (n > 0) {
            if (m_len > 0 && at != m_start + m_len)
                FW_TRY(flush());
            if (m_len == 0)
                m_start = at;
            const qint64 take = std::min(n, cap - m_len);
            std::memcpy(m_buf.data() + m_len, p, size_t(take));
            m_len += take;
            at += take;
            p += take;
            n -= take;
            if (m_len == cap)
                FW_TRY(flush());
        }
        return success();
    }

    Status flush()
    {
        if (m_len == 0)
            return success();
        const qint64 n = m_len;
        m_len = 0;
        if (n >= kHoleMin && allZero(m_buf.data(), n))
            return success(); // a new image reads zeros there already
        if (!m_f.seek(m_start))
            return failed();
        for (qint64 done = 0; done < n;) {
            const qint64 w = m_f.write(m_buf.data() + done, n - done);
            if (w <= 0)
                return failed();
            done += w;
        }
        return success();
    }

private:
    Error failed() const
    {
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), m_f.errorString());
    }

    QFile& m_f;
    std::vector<char> m_buf;
    qint64 m_start = 0;
    qint64 m_len = 0;
};

// One file on its way into the image.
struct Current {
    int item = -1;
    const GameItem* listed = nullptr;
    const Placement* place = nullptr;
    qint64 produced = 0;
    size_t extent = 0;
    ContentDigest digest;
};

Status checkPlacements(const GameListing& game, const std::vector<Placement>& placements)
{
    const auto bad = [](const QString& why) {
        return Error(ErrorCode::Internal, QStringLiteral("The image layout is inconsistent."), why);
    };
    if (placements.size() != game.items.size())
        return bad(QStringLiteral("%1 placements for %2 items").arg(placements.size()).arg(game.items.size()));
    for (size_t i = 0; i < placements.size(); ++i) {
        const Placement& p = placements[i];
        if (!p.write)
            continue;
        const GameItem& it = game.items[i];
        if (it.isDir)
            return bad(QStringLiteral("folder %1 placed as a file").arg(it.path));
        qint64 at = 0;
        for (const Extent& e : p.extents) {
            if (e.fileOffset != at || e.length <= 0 || e.imageOffset < 0)
                return bad(QStringLiteral("%1: extent at %2").arg(it.path).arg(e.fileOffset));
            at += e.length;
        }
        if (at != it.size)
            return bad(QStringLiteral("%1: %2 of %3 bytes placed").arg(it.path).arg(at).arg(it.size));
    }
    return success();
}

} // namespace

Result<StreamStats> streamGameInto(const GameListing& game, const std::vector<Placement>& placements, const QString& image,
                                   qint64 bufferBytes, const CancellationToken& token, ProgressReporter* progress)
{
    FW_TRY(checkPlacements(game, placements));
    QElapsedTimer timer;
    timer.start();
    QFile out(toLongPath(image));
    if (!out.exists() || !out.open(QIODevice::ReadWrite))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(image)),
                     out.errorString());
    ImageWriter writer(out, bufferBytes);
    StreamStats stats;
    stats.digests.assign(game.items.size(), 0);
    std::vector<bool> done(game.items.size(), false);
    const QString archiveName = QFileInfo(game.archive).fileName();
    Current cur;

    const auto begin = [&](int item) {
        cur = Current();
        cur.item = item;
        cur.listed = &game.items[size_t(item)];
        cur.place = &placements[size_t(item)];
        if (progress)
            progress->setCurrentItem(cur.listed->path);
    };
    const auto feed = [&](const char* data, qint64 n) -> Status {
        if (n > cur.listed->size - cur.produced)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is larger than the archive says.").arg(cur.listed->entry));
        cur.digest.add(data, n);
        const qint64 total = n;
        while (n > 0) {
            const Extent& e = cur.place->extents[cur.extent];
            const qint64 inside = cur.produced - e.fileOffset;
            const qint64 take = std::min(n, e.length - inside);
            FW_TRY(writer.put(e.imageOffset + inside, data, take));
            cur.produced += take;
            data += take;
            n -= take;
            if (inside + take == e.length)
                ++cur.extent;
        }
        stats.bytes += total;
        if (progress)
            progress->advance(total);
        return success();
    };
    const auto end = [&]() -> Status {
        if (cur.produced != cur.listed->size)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in %2.").arg(cur.listed->entry, archiveName),
                         QStringLiteral("%1 of %2 bytes").arg(cur.produced).arg(cur.listed->size));
        stats.digests[size_t(cur.item)] = cur.digest.value();
        done[size_t(cur.item)] = true;
        ++stats.files;
        if (progress)
            progress->advanceFiles(1);
        cur = Current();
        return success();
    };

    if (game.zip) {
        // Any order: the image's, so the image is written front to back.
        zip::Reader r;
        FW_TRY(r.open(game.archive));
        std::vector<int> order;
        for (size_t i = 0; i < placements.size(); ++i)
            if (placements[i].write)
                order.push_back(int(i));
        const auto firstByte = [&](int i) {
            const auto& x = placements[size_t(i)].extents;
            return x.empty() ? qint64(-1) : x.front().imageOffset;
        };
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return firstByte(a) < firstByte(b); });
        for (const int i : order) {
            const GameItem& it = game.items[size_t(i)];
            if (it.source < 0 || size_t(it.source) >= r.entries().size() || r.entries()[size_t(it.source)].name != it.entry)
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 changed while the image was being built.").arg(archiveName));
            const zip::Entry& e = r.entries()[size_t(it.source)];
            if (qint64(e.uncompressedSize) != it.size)
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 changed while the image was being built.").arg(archiveName));
            begin(i);
            // The reader checks the CRC-32 and the size at the end of the entry.
            FW_TRY(r.read(e, [&](const QByteArray& chunk) { return feed(chunk.constData(), chunk.size()); }, token));
            FW_TRY(end());
            if (!token.checkpoint())
                return Error::cancelled();
        }
    } else {
        if (!game.listing)
            return Error(ErrorCode::Internal, QStringLiteral("The archive's list of files is missing."));
        QHash<QString, int> byEntry;
        for (size_t i = 0; i < game.items.size(); ++i)
            if (!game.items[i].isDir)
                byEntry.insert(game.items[i].entry, int(i));
        ArchiveVisitor v;
        v.begin = [&](const ArchiveEntry& listed, const QDateTime&) -> Result<bool> {
            const auto it = byEntry.constFind(listed.name);
            if (it == byEntry.cend() || !placements[size_t(*it)].write)
                return false;
            begin(*it);
            return true;
        };
        v.data = feed;
        v.end = end;
        FW_TRY(visitArchive(*game.listing, game.rootPrefix, v, token));
    }
    for (size_t i = 0; i < placements.size(); ++i)
        if (placements[i].write && !done[i])
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 does not match its own list of files (%2 is missing).")
                                                     .arg(archiveName, game.items[i].path));
    FW_TRY(writer.flush());
    if (!out.flush())
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), out.errorString());
    out.close();
    if (out.error() != QFileDevice::NoError)
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), out.errorString());
    stats.elapsedMs = timer.elapsed();
    return stats;
}

} // namespace fw::archive
