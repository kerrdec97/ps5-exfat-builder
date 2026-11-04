#include "core/Zip.h"

#include <QDir>
#include <QFileInfo>
#include <QtEndian>

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace fw::zip {

namespace {

constexpr quint32 kLocalSig = 0x04034b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kEocdSig = 0x06054b50;
constexpr quint32 kZip64EocdSig = 0x06064b50;
constexpr quint32 kZip64LocatorSig = 0x07064b50;
constexpr quint16 kFlagUtf8 = 0x0800;
constexpr quint16 kZip64ExtraId = 0x0001;
constexpr int kChunk = 1 << 20;

void put16(QByteArray& b, quint16 v)
{
    char c[2];
    qToLittleEndian(v, c);
    b.append(c, 2);
}
void put32(QByteArray& b, quint32 v)
{
    char c[4];
    qToLittleEndian(v, c);
    b.append(c, 4);
}
void put64(QByteArray& b, quint64 v)
{
    char c[8];
    qToLittleEndian(v, c);
    b.append(c, 8);
}
quint16 get16(const char* p) { return qFromLittleEndian<quint16>(p); }
quint32 get32(const char* p) { return qFromLittleEndian<quint32>(p); }
quint64 get64(const char* p) { return qFromLittleEndian<quint64>(p); }

void toDos(const QDateTime& dt, quint16& time, quint16& date)
{
    QDateTime t = dt.isValid() ? dt.toLocalTime() : QDateTime::currentDateTime();
    if (t.date().year() < 1980)
        t = QDateTime(QDate(1980, 1, 1), QTime(0, 0));
    if (t.date().year() > 2107)
        t = QDateTime(QDate(2107, 12, 31), QTime(23, 59, 58));
    date = quint16(((t.date().year() - 1980) << 9) | (t.date().month() << 5) | t.date().day());
    time = quint16((t.time().hour() << 11) | (t.time().minute() << 5) | (t.time().second() / 2));
}

QDateTime fromDos(quint16 time, quint16 date)
{
    const QDate d(1980 + (date >> 9), (date >> 5) & 0x0F, date & 0x1F);
    const QTime t(time >> 11, (time >> 5) & 0x3F, (time & 0x1F) * 2);
    return QDateTime(d.isValid() ? d : QDate(1980, 1, 1), t.isValid() ? t : QTime(0, 0));
}

QString normaliseName(QString name)
{
    name.replace(u'\\', u'/');
    while (name.startsWith(u'/'))
        name.remove(0, 1);
    return name;
}

Error ioError(const QString& what, const QFile& f)
{
    return Error(ErrorCode::IoError, what, f.errorString());
}

} // namespace

quint32 crc32(const QByteArray& data, quint32 seed)
{
    return quint32(::crc32(seed, reinterpret_cast<const Bytef*>(data.constData()), uInt(data.size())));
}

// ---------------------------------------------------------------------------
// Writer

Writer::Writer() = default;

Writer::~Writer()
{
    if (m_file.isOpen())
        m_file.close();
    // PartFile's destructor removes the .part unless committed.
}

Status Writer::open(const QString& path, bool replaceExisting)
{
    m_path = path;
    m_replace = replaceExisting;
    if (QFileInfo::exists(path) && !replaceExisting)
        return Error(ErrorCode::AlreadyExists, QStringLiteral("%1 already exists. Choose another name or folder.")
                                                    .arg(QFileInfo(path).fileName()));
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_part = std::make_unique<PartFile>(path);
    m_file.setFileName(toLongPath(m_part->partPath()));
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate))
        return ioError(QStringLiteral("Could not create %1.").arg(QFileInfo(path).fileName()), m_file);
    return success();
}

Status Writer::writeRaw(const QByteArray& b)
{
    if (m_file.write(b) != b.size())
        return ioError(QStringLiteral("Could not write %1.").arg(QFileInfo(m_path).fileName()), m_file);
    return success();
}

Status Writer::writeEntry(const QString& rawName, Method method, const QDateTime& modified, quint64 sizeHint,
                          const std::function<Result<qint64>(QByteArray&)>& readChunk, const CancellationToken& token,
                          ProgressReporter* progress)
{
    if (!m_file.isOpen() || m_finished)
        return Error(ErrorCode::Internal, QStringLiteral("The archive is not open."));
    const QString name = normaliseName(rawName);
    if (name.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("An archive entry needs a name."));
    const QByteArray nameUtf8 = name.toUtf8();
    if (nameUtf8.size() > 0xFFFF)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("An archive entry name is too long."));

    Entry e;
    e.name = name;
    e.method = method;
    e.modified = modified;
    e.localHeaderOffset = quint64(m_file.pos());
    // Reserve ZIP64 fields when the entry might reach the 4 GiB limit (deflate
    // can grow incompressible data slightly).
    const quint64 margin = (m_zip64Threshold / 1000) + 65536;
    const bool zip64Local = sizeHint + margin >= m_zip64Threshold;

    quint16 dosTime = 0, dosDate = 0;
    toDos(modified, dosTime, dosDate);
    QByteArray h;
    put32(h, kLocalSig);
    put16(h, zip64Local ? 45 : 20);
    put16(h, kFlagUtf8);
    put16(h, quint16(method));
    put16(h, dosTime);
    put16(h, dosDate);
    put32(h, 0); // crc, patched
    put32(h, zip64Local ? 0xFFFFFFFFu : 0); // compressed size, patched
    put32(h, zip64Local ? 0xFFFFFFFFu : 0); // uncompressed size, patched
    put16(h, quint16(nameUtf8.size()));
    put16(h, zip64Local ? 20 : 0);
    h += nameUtf8;
    if (zip64Local) {
        put16(h, kZip64ExtraId);
        put16(h, 16);
        put64(h, 0);
        put64(h, 0);
    }
    FW_TRY(writeRaw(h));

    // Stream data.
    quint32 crc = 0;
    quint64 inBytes = 0, outBytes = 0;
    z_stream zs{};
    bool deflating = method == Method::Deflate;
    if (deflating && deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return Error(ErrorCode::Internal, QStringLiteral("Compression could not start."));
    struct ZGuard {
        z_stream* z;
        bool active;
        ~ZGuard() { if (active) deflateEnd(z); }
    } guard{&zs, deflating};

    QByteArray in, out;
    out.resize(kChunk + 1024);
    bool eof = false;
    while (!eof) {
        if (!token.checkpoint())
            return Error::cancelled();
        auto n = readChunk(in);
        if (!n.ok())
            return n.takeError();
        eof = n.value() == 0;
        if (!eof) {
            crc = quint32(::crc32(crc, reinterpret_cast<const Bytef*>(in.constData()), uInt(n.value())));
            inBytes += quint64(n.value());
            if (progress)
                progress->advance(n.value());
        }
        if (!deflating) {
            if (!eof) {
                FW_TRY(writeRaw(QByteArray::fromRawData(in.constData(), int(n.value()))));
                outBytes += quint64(n.value());
            }
            continue;
        }
        zs.next_in = reinterpret_cast<Bytef*>(in.data());
        zs.avail_in = eof ? 0 : uInt(n.value());
        const int flush = eof ? Z_FINISH : Z_NO_FLUSH;
        int rc = Z_OK;
        do {
            zs.next_out = reinterpret_cast<Bytef*>(out.data());
            zs.avail_out = uInt(out.size());
            rc = deflate(&zs, flush);
            if (rc == Z_STREAM_ERROR)
                return Error(ErrorCode::Internal, QStringLiteral("Compression failed."));
            const qint64 produced = qint64(out.size()) - qint64(zs.avail_out);
            if (produced > 0) {
                FW_TRY(writeRaw(QByteArray::fromRawData(out.constData(), int(produced))));
                outBytes += quint64(produced);
            }
        } while (zs.avail_out == 0 || (eof && rc != Z_STREAM_END));
    }

    const bool needZip64 = inBytes >= m_zip64Threshold || outBytes >= m_zip64Threshold;
    if (needZip64 && !zip64Local)
        return Error(ErrorCode::IoError, QStringLiteral("%1 grew while it was being archived.").arg(name));

    // Patch the local header.
    const qint64 end = m_file.pos();
    QByteArray patch;
    put32(patch, crc);
    put32(patch, zip64Local ? 0xFFFFFFFFu : quint32(outBytes));
    put32(patch, zip64Local ? 0xFFFFFFFFu : quint32(inBytes));
    if (!m_file.seek(qint64(e.localHeaderOffset) + 14))
        return ioError(QStringLiteral("Could not finish %1.").arg(name), m_file);
    FW_TRY(writeRaw(patch));
    if (zip64Local) {
        QByteArray x;
        put64(x, inBytes);
        put64(x, outBytes);
        if (!m_file.seek(qint64(e.localHeaderOffset) + 30 + nameUtf8.size() + 4))
            return ioError(QStringLiteral("Could not finish %1.").arg(name), m_file);
        FW_TRY(writeRaw(x));
    }
    if (!m_file.seek(end))
        return ioError(QStringLiteral("Could not finish %1.").arg(name), m_file);

    e.crc32 = crc;
    e.compressedSize = outBytes;
    e.uncompressedSize = inBytes;
    m_entries.push_back(e);
    return success();
}

Status Writer::addFile(const QString& archiveName, const QString& sourcePath, Method method,
                       const CancellationToken& token, ProgressReporter* progress)
{
    QFile src(toLongPath(sourcePath));
    if (!src.open(QIODevice::ReadOnly))
        return ioError(QStringLiteral("Could not open %1.").arg(QFileInfo(sourcePath).fileName()), src);
    const QDateTime modified = QFileInfo(sourcePath).lastModified();
    return writeEntry(archiveName, method, modified, quint64(src.size()),
                      [&](QByteArray& chunk) -> Result<qint64> {
                          chunk.resize(kChunk);
                          const qint64 n = src.read(chunk.data(), kChunk);
                          if (n < 0)
                              return ioError(QStringLiteral("Could not read %1.").arg(QFileInfo(sourcePath).fileName()), src);
                          return n;
                      },
                      token, progress);
}

Status Writer::addData(const QString& archiveName, const QByteArray& data, Method method, const QDateTime& modified)
{
    bool done = false;
    return writeEntry(archiveName, method, modified, quint64(data.size()),
                      [&](QByteArray& chunk) -> Result<qint64> {
                          if (done)
                              return qint64(0);
                          done = true;
                          chunk = data;
                          return qint64(data.size());
                      },
                      CancellationToken(), nullptr);
}

Status Writer::addDirectory(const QString& archiveName, const QDateTime& modified)
{
    QString name = normaliseName(archiveName);
    if (!name.endsWith(u'/'))
        name += u'/';
    return writeEntry(name, Method::Store, modified, 0, [](QByteArray&) -> Result<qint64> { return qint64(0); },
                      CancellationToken(), nullptr);
}

Status Writer::finish()
{
    if (m_finished)
        return success();
    if (!m_file.isOpen())
        return Error(ErrorCode::Internal, QStringLiteral("The archive is not open."));
    const quint64 cdOffset = quint64(m_file.pos());
    QByteArray cd;
    for (const Entry& e : m_entries) {
        const QByteArray nameUtf8 = e.name.toUtf8();
        const bool zUnc = e.uncompressedSize >= m_zip64Threshold;
        const bool zComp = e.compressedSize >= m_zip64Threshold;
        const bool zOff = e.localHeaderOffset >= m_zip64Threshold;
        QByteArray extra;
        if (zUnc || zComp || zOff) {
            QByteArray body;
            if (zUnc) put64(body, e.uncompressedSize);
            if (zComp) put64(body, e.compressedSize);
            if (zOff) put64(body, e.localHeaderOffset);
            put16(extra, kZip64ExtraId);
            put16(extra, quint16(body.size()));
            extra += body;
        }
        quint16 t = 0, d = 0;
        toDos(e.modified, t, d);
        const bool z = !extra.isEmpty();
        put32(cd, kCentralSig);
        put16(cd, z ? 45 : 20); // made by (MS-DOS host)
        put16(cd, z ? 45 : 20); // needed
        put16(cd, kFlagUtf8);
        put16(cd, quint16(e.method));
        put16(cd, t);
        put16(cd, d);
        put32(cd, e.crc32);
        put32(cd, zComp ? 0xFFFFFFFFu : quint32(e.compressedSize));
        put32(cd, zUnc ? 0xFFFFFFFFu : quint32(e.uncompressedSize));
        put16(cd, quint16(nameUtf8.size()));
        put16(cd, quint16(extra.size()));
        put16(cd, 0); // comment
        put16(cd, 0); // disk
        put16(cd, 0); // internal attrs
        put32(cd, e.isDirectory() ? 0x10 : 0x20);
        put32(cd, zOff ? 0xFFFFFFFFu : quint32(e.localHeaderOffset));
        cd += nameUtf8;
        cd += extra;
    }
    FW_TRY(writeRaw(cd));
    const quint64 cdSize = quint64(cd.size());
    const quint64 count = m_entries.size();
    const bool zip64 = count >= 0xFFFF || cdOffset >= m_zip64Threshold || cdSize >= m_zip64Threshold
        || cdOffset + cdSize >= m_zip64Threshold;
    QByteArray tail;
    if (zip64) {
        const quint64 eocd64Offset = quint64(m_file.pos());
        put32(tail, kZip64EocdSig);
        put64(tail, 44);
        put16(tail, 45);
        put16(tail, 45);
        put32(tail, 0);
        put32(tail, 0);
        put64(tail, count);
        put64(tail, count);
        put64(tail, cdSize);
        put64(tail, cdOffset);
        put32(tail, kZip64LocatorSig);
        put32(tail, 0);
        put64(tail, eocd64Offset);
        put32(tail, 1);
    }
    put32(tail, kEocdSig);
    put16(tail, 0);
    put16(tail, 0);
    put16(tail, zip64 ? 0xFFFF : quint16(count));
    put16(tail, zip64 ? 0xFFFF : quint16(count));
    put32(tail, zip64 ? 0xFFFFFFFFu : quint32(cdSize));
    put32(tail, zip64 ? 0xFFFFFFFFu : quint32(cdOffset));
    put16(tail, 0);
    FW_TRY(writeRaw(tail));
    if (!m_file.flush())
        return ioError(QStringLiteral("Could not write %1.").arg(QFileInfo(m_path).fileName()), m_file);
    m_file.close();
    FW_TRY(m_part->commit(m_replace));
    m_finished = true;
    return success();
}

// ---------------------------------------------------------------------------
// Reader

QString Reader::safeRelativePath(const QString& entryName)
{
    QString n = entryName;
    n.replace(u'\\', u'/');
    if (n.startsWith(u'/') || n.contains(u':') || n.contains(QChar(0)))
        return {};
    QStringList out;
    for (const QString& seg : n.split(u'/', Qt::SkipEmptyParts)) {
        if (seg == u".")
            continue;
        if (seg == u"..")
            return {};
        out << seg;
    }
    return out.join(u'/');
}

Status Reader::open(const QString& path)
{
    m_path = path;
    m_entries.clear();
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return ioError(QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName()), f);
    const qint64 size = f.size();
    const auto corrupt = [&](const QString& why) {
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is not a valid ZIP archive.").arg(QFileInfo(path).fileName()), why);
    };
    if (size < 22)
        return corrupt(QStringLiteral("too small"));
    const qint64 tailLen = std::min<qint64>(size, 22 + 65535 + 20);
    f.seek(size - tailLen);
    const QByteArray tail = f.read(tailLen);
    qsizetype eocd = -1;
    for (qsizetype i = tail.size() - 22; i >= 0; --i)
        if (get32(tail.constData() + i) == kEocdSig) {
            eocd = i;
            break;
        }
    if (eocd < 0)
        return corrupt(QStringLiteral("no end-of-central-directory record"));
    const char* p = tail.constData() + eocd;
    quint64 count = get16(p + 10);
    quint64 cdSize = get32(p + 12);
    quint64 cdOffset = get32(p + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) {
        if (eocd < 20 || get32(tail.constData() + eocd - 20) != kZip64LocatorSig)
            return corrupt(QStringLiteral("missing ZIP64 locator"));
        const quint64 z64 = get64(tail.constData() + eocd - 20 + 8);
        if (!f.seek(qint64(z64)))
            return corrupt(QStringLiteral("bad ZIP64 offset"));
        const QByteArray r = f.read(56);
        if (r.size() < 56 || get32(r.constData()) != kZip64EocdSig)
            return corrupt(QStringLiteral("bad ZIP64 record"));
        count = get64(r.constData() + 32);
        cdSize = get64(r.constData() + 40);
        cdOffset = get64(r.constData() + 48);
    }
    if (cdOffset + cdSize > quint64(size) || cdSize > 512ull * 1024 * 1024)
        return corrupt(QStringLiteral("central directory out of range"));
    f.seek(qint64(cdOffset));
    const QByteArray cd = f.read(qint64(cdSize));
    if (quint64(cd.size()) != cdSize)
        return corrupt(QStringLiteral("short central directory"));
    qsizetype pos = 0;
    for (quint64 i = 0; i < count; ++i) {
        if (pos + 46 > cd.size() || get32(cd.constData() + pos) != kCentralSig)
            return corrupt(QStringLiteral("bad central directory entry"));
        const char* c = cd.constData() + pos;
        Entry e;
        const quint16 flags = get16(c + 8);
        e.method = Method(get16(c + 10));
        e.modified = fromDos(get16(c + 12), get16(c + 14));
        e.crc32 = get32(c + 16);
        e.compressedSize = get32(c + 20);
        e.uncompressedSize = get32(c + 24);
        const quint16 nameLen = get16(c + 28), extraLen = get16(c + 30), commentLen = get16(c + 32);
        e.localHeaderOffset = get32(c + 42);
        if (pos + 46 + nameLen + extraLen + commentLen > cd.size())
            return corrupt(QStringLiteral("truncated central directory entry"));
        const QByteArray rawName = cd.mid(pos + 46, nameLen);
        e.name = (flags & kFlagUtf8) ? QString::fromUtf8(rawName) : QString::fromLatin1(rawName);
        // ZIP64 extra
        qsizetype x = pos + 46 + nameLen;
        const qsizetype xEnd = x + extraLen;
        while (x + 4 <= xEnd) {
            const quint16 id = get16(cd.constData() + x), len = get16(cd.constData() + x + 2);
            if (id == kZip64ExtraId) {
                qsizetype q = x + 4;
                if (e.uncompressedSize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.uncompressedSize = get64(cd.constData() + q); q += 8; }
                if (e.compressedSize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.compressedSize = get64(cd.constData() + q); q += 8; }
                if (e.localHeaderOffset == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.localHeaderOffset = get64(cd.constData() + q); }
            }
            x += 4 + len;
        }
        if (e.localHeaderOffset + 30 > quint64(size))
            return corrupt(QStringLiteral("entry outside the archive"));
        m_entries.push_back(e);
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return success();
}

const Entry* Reader::find(const QString& name) const
{
    for (const auto& e : m_entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

Status Reader::read(const Entry& entry, const std::function<Status(const QByteArray&)>& sink,
                    const CancellationToken& token, ProgressReporter* progress)
{
    if (entry.method != Method::Store && entry.method != Method::Deflate)
        return Error(ErrorCode::Unsupported, QStringLiteral("%1 uses an unsupported compression method.").arg(entry.name));
    QFile f(toLongPath(m_path));
    if (!f.open(QIODevice::ReadOnly) || !f.seek(qint64(entry.localHeaderOffset)))
        return ioError(QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()), f);
    const QByteArray lh = f.read(30);
    if (lh.size() < 30 || get32(lh.constData()) != kLocalSig)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in the archive.").arg(entry.name));
    const qint64 dataStart = qint64(entry.localHeaderOffset) + 30 + get16(lh.constData() + 26) + get16(lh.constData() + 28);
    if (!f.seek(dataStart))
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in the archive.").arg(entry.name));

    quint32 crc = 0;
    quint64 produced = 0;
    quint64 remaining = entry.compressedSize;
    z_stream zs{};
    const bool inflating = entry.method == Method::Deflate;
    if (inflating && inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return Error(ErrorCode::Internal, QStringLiteral("Decompression could not start."));
    struct ZGuard {
        z_stream* z;
        bool active;
        ~ZGuard() { if (active) inflateEnd(z); }
    } guard{&zs, inflating};

    QByteArray out;
    out.resize(kChunk);
    bool streamEnd = !inflating;
    while (remaining > 0 || (inflating && !streamEnd)) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 want = qint64(std::min<quint64>(remaining, quint64(kChunk)));
        QByteArray in = want > 0 ? f.read(want) : QByteArray();
        if (want > 0 && in.size() != want)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is truncated in the archive.").arg(entry.name));
        remaining -= quint64(in.size());
        if (progress)
            progress->advance(in.size());
        if (!inflating) {
            crc = quint32(::crc32(crc, reinterpret_cast<const Bytef*>(in.constData()), uInt(in.size())));
            produced += quint64(in.size());
            FW_TRY(sink(in));
            continue;
        }
        zs.next_in = reinterpret_cast<Bytef*>(in.data());
        zs.avail_in = uInt(in.size());
        do {
            zs.next_out = reinterpret_cast<Bytef*>(out.data());
            zs.avail_out = uInt(out.size());
            const int rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && !(rc == Z_BUF_ERROR && zs.avail_in == 0))
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in the archive.").arg(entry.name),
                             QStringLiteral("inflate %1").arg(rc));
            const qint64 n = qint64(out.size()) - qint64(zs.avail_out);
            if (n > 0) {
                const QByteArray chunk = QByteArray::fromRawData(out.constData(), int(n));
                crc = quint32(::crc32(crc, reinterpret_cast<const Bytef*>(chunk.constData()), uInt(n)));
                produced += quint64(n);
                FW_TRY(sink(QByteArray(chunk.constData(), int(n))));
            }
            if (rc == Z_STREAM_END) {
                streamEnd = true;
                break;
            }
        } while (zs.avail_out == 0 || zs.avail_in > 0);
        if (remaining == 0 && !streamEnd && zs.avail_in == 0) {
            // Input exhausted without the end of the stream.
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is truncated in the archive.").arg(entry.name));
        }
    }
    if (produced != entry.uncompressedSize || crc != entry.crc32)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 failed its checksum in the archive.").arg(entry.name),
                     QStringLiteral("crc %1/%2 size %3/%4").arg(crc, 8, 16).arg(entry.crc32, 8, 16).arg(produced).arg(entry.uncompressedSize));
    return success();
}

Result<QByteArray> Reader::readAll(const Entry& entry, qint64 maxBytes)
{
    if (qint64(entry.uncompressedSize) > maxBytes)
        return Error(ErrorCode::Unsupported, QStringLiteral("%1 is too large to read into memory.").arg(entry.name));
    QByteArray all;
    FW_TRY(read(entry, [&](const QByteArray& c) { all += c; return success(); }, CancellationToken()));
    return all;
}

Status Reader::verifyAll(const CancellationToken& token, ProgressReporter* progress)
{
    for (const Entry& e : m_entries) {
        if (e.isDirectory())
            continue;
        FW_TRY(read(e, [](const QByteArray&) { return success(); }, token, progress));
    }
    return success();
}

Status Reader::extractAll(const QString& destination, const CancellationToken& token, ProgressReporter* progress,
                          bool overwrite)
{
    // Validate every name before writing anything.
    for (const Entry& e : m_entries)
        if (safeRelativePath(e.name).isEmpty())
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("The archive contains an unsafe path (%1); nothing was extracted.").arg(e.name));
    QDir().mkpath(destination);
    const QDir dest(destination);
    for (const Entry& e : m_entries) {
        const QString rel = safeRelativePath(e.name);
        const QString target = dest.filePath(rel);
        if (!isSameOrInside(target, destination))
            return Error(ErrorCode::CorruptData, QStringLiteral("The archive contains an unsafe path (%1).").arg(e.name));
        if (e.isDirectory()) {
            QDir().mkpath(target);
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        PartFile part(target);
        QFile out(toLongPath(part.partPath()));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return ioError(QStringLiteral("Could not create %1.").arg(rel), out);
        FW_TRY(read(e, [&](const QByteArray& c) -> Status {
            if (out.write(c) != c.size())
                return ioError(QStringLiteral("Could not write %1.").arg(rel), out);
            return success();
        }, token, progress));
        out.setFileTime(e.modified, QFileDevice::FileModificationTime);
        out.close();
        FW_TRY(part.commit(overwrite));
    }
    return success();
}

} // namespace fw::zip
