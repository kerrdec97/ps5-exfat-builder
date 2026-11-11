#include "core/ArchiveReader.h"

#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtEndian>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>

namespace fw::archive {

namespace {

constexpr qint64 kReadBlock = 256 * 1024;
constexpr qsizetype kMaxEntries = 2'000'000;
// The list of files of a 7z archive, packed and unpacked. Sixty-four MiB
// hold several hundred thousand long names; libarchive needs about twenty
// times the header's size in memory while it reads it.
constexpr quint64 kMaxSevenZipHeader = 64ull * 1024 * 1024;
// libarchive's RAR 5 reader refuses larger dictionaries.
constexpr qint64 kMaxRarDictionary = 64ll * 1024 * 1024;
constexpr quint64 kMaxRar5Header = 2ull * 1024 * 1024; // the format's own limit
constexpr int kMaxVolumes = 10000;
// A 7z listing may have to decode data to read where a symbolic link points.
constexpr qint64 kListingDataBudget = 32ll * 1024 * 1024;
constexpr int kListingDataMs = 20000;

QString nameOf(const QString& path) { return QFileInfo(path).fileName(); }

const char kAdvice[] = "Unpack it yourself and choose the folder.";

Error passwordProtected(const QString& archive)
{
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("%1 is password-protected. %2").arg(nameOf(archive), QLatin1String(kAdvice)));
}

Error damagedArchive(const QString& archive, const QString& detail)
{
    return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged or incomplete.").arg(nameOf(archive)), detail);
}

Error incompleteArchive(const QString& archive, const QString& detail)
{
    return Error(ErrorCode::CorruptData,
                 QStringLiteral("%1 is incomplete: its end is missing. Download or copy it again.").arg(nameOf(archive)),
                 detail);
}

Error tooManyEntries(const QString& archive)
{
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("%1 holds more than 2,000,000 entries. %2").arg(nameOf(archive), QLatin1String(kAdvice)));
}

Error listTooLarge(const QString& archive, const QString& detail)
{
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("%1 has a list of files too large for this app to read. %2")
                     .arg(nameOf(archive), QLatin1String(kAdvice)),
                 detail);
}

Error unreadableName(const QString& archive, const QString& detail)
{
    return Error(ErrorCode::Unsupported,
                 QStringLiteral("%1 holds a file name this computer cannot read. %2")
                     .arg(nameOf(archive), QLatin1String(kAdvice)),
                 detail);
}

// "./a\b/" -> "a/b". Archives written on Windows may use '\'; the ZIP reader
// treats it as a separator on every host as well.
QString normalisedName(QString n)
{
    n.replace(u'\\', u'/');
    while (n.startsWith(QLatin1String("./")))
        n.remove(0, 2);
    while (n.endsWith(u'/'))
        n.chop(1);
    return n;
}

bool decodeUtf8(const char* bytes, qsizetype len, QString& out)
{
    QStringDecoder dec(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    out = dec.decode(QByteArrayView(bytes, len));
    return !dec.hasError() && !out.contains(QChar(0));
}

// ---------------------------------------------------------------------------
// Multi-part RAR names

struct PartName {
    QString head;  // "game.part"
    QString digits; // "03"
    QString tail;  // ".rar"
    bool valid() const { return !digits.isEmpty(); }
    int number() const { return digits.toInt(); }
    QString with(int n) const
    {
        return head + QStringLiteral("%1").arg(n, int(digits.size()), 10, QLatin1Char('0')) + tail;
    }
};

PartName splitPartName(const QString& fileName)
{
    static const QRegularExpression re(QStringLiteral("^(.*\\.part)(\\d{1,9})(\\.rar)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    PartName p;
    if (const auto m = re.match(fileName); m.hasMatch()) {
        p.head = m.captured(1);
        p.digits = m.captured(2);
        p.tail = m.captured(3);
    }
    return p;
}

// The file a multi-part RAR continues in: "x.part02.rar" after
// "x.part01.rar"; with the old naming "x.r00" after "x.rar", then "x.r01"
// ... "x.r99", "x.s00". Empty when the name follows neither scheme.
QString nextVolume(const QString& path, bool oldNaming)
{
    const QFileInfo fi(path);
    const QString name = fi.fileName();
    if (!oldNaming) {
        const PartName p = splitPartName(name);
        return p.valid() ? fi.dir().filePath(p.with(p.number() + 1)) : QString();
    }
    const QString suffix = fi.suffix();
    const QString stem = name.left(name.size() - suffix.size());
    if (suffix.compare(QLatin1String("rar"), Qt::CaseInsensitive) == 0)
        return fi.dir().filePath(stem + QStringLiteral("r00"));
    if (suffix.size() != 3 || !suffix.at(1).isDigit() || !suffix.at(2).isDigit())
        return {};
    const int n = suffix.mid(1).toInt();
    const char16_t letter = suffix.at(0).unicode();
    if (n < 99)
        return fi.dir().filePath(stem + QChar(letter) + QStringLiteral("%1").arg(n + 1, 2, 10, QLatin1Char('0')));
    if (letter == u'z' || letter == u'Z')
        return {};
    return fi.dir().filePath(stem + QChar(char16_t(letter + 1)) + QStringLiteral("00"));
}

// Started from "x.part03.rar": the user is told which file to choose.
Status mustBeFirstPart(const QString& path)
{
    const QFileInfo fi(path);
    const PartName p = splitPartName(fi.fileName());
    if (!p.valid() || p.number() <= 1)
        return success();
    const QString first = p.with(1);
    if (QFileInfo::exists(fi.dir().filePath(first)))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("%1 is part %2 of a multi-part archive. Choose the first part, %3.")
                         .arg(fi.fileName())
                         .arg(p.number())
                         .arg(first));
    return Error(ErrorCode::InvalidArgument,
                 QStringLiteral("%1 is part %2 of a multi-part archive and the first part, %3, is not in its folder.")
                     .arg(fi.fileName())
                     .arg(p.number())
                     .arg(first));
}

Error missingPart(const QString& path)
{
    return Error(ErrorCode::NotFound,
                 QStringLiteral("The next part of this archive, %1, is missing. Put every part in one folder.")
                     .arg(nameOf(path)));
}

// ---------------------------------------------------------------------------
// A bounds-checked view of header bytes

struct Cursor {
    const uchar* p = nullptr;
    const uchar* end = nullptr;
    bool ok = true;

    Cursor(const QByteArray& b, qsizetype from = 0)
        : p(reinterpret_cast<const uchar*>(b.constData()) + std::min(from, b.size())),
          end(reinterpret_cast<const uchar*>(b.constData()) + b.size())
    {
    }
    qsizetype left() const { return end - p; }
    int byte()
    {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return *p++;
    }
    void skip(quint64 n)
    {
        if (n > quint64(left())) {
            ok = false;
            p = end;
            return;
        }
        p += n;
    }
    // 7z: the number of leading 1 bits of the first byte says how many bytes follow.
    quint64 sevenZipNumber()
    {
        const int first = byte();
        quint64 v = 0;
        int mask = 0x80;
        for (int i = 0; i < 8; ++i) {
            if ((first & mask) == 0)
                return v | (quint64(first & (mask - 1)) << (8 * i));
            v |= quint64(byte()) << (8 * i);
            mask >>= 1;
        }
        return v;
    }
    // RAR 5: seven bits a byte, lowest first, the top bit says "more".
    quint64 vint()
    {
        quint64 v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const int b = byte();
            if (!ok)
                return 0;
            if (shift < 64)
                v |= quint64(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
                return v;
        }
        ok = false; // more than ten bytes
        return 0;
    }
};

// ---------------------------------------------------------------------------
// 7z: the header is checked before libarchive decodes it

// libarchive reads the whole list of files into memory, and a packed header
// of a few kilobytes can announce any size. The 32-byte start header and,
// for a packed header, the description of its one stream say how large the
// list is; an archive over the limit, cut short or with an encrypted header
// is refused here.
Status checkSevenZipHeader(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(nameOf(path)), f.errorString());
    const qint64 size = f.size();
    const QByteArray start = f.read(32);
    if (start.size() < 32)
        return incompleteArchive(path, QStringLiteral("shorter than a 7z start header"));
    const char* s = start.constData();
    if (zip::crc32(start.mid(12, 20)) != qFromLittleEndian<quint32>(s + 8))
        return damagedArchive(path, QStringLiteral("7z start header checksum"));
    const quint64 offset = qFromLittleEndian<quint64>(s + 12);
    const quint64 length = qFromLittleEndian<quint64>(s + 20);
    if (length == 0)
        return success(); // an archive without entries
    const quint64 room = quint64(size - 32);
    if (offset > room || length > room - offset)
        return incompleteArchive(path, QStringLiteral("7z header at %1+%2, file size %3").arg(offset).arg(length).arg(size));
    if (length > kMaxSevenZipHeader)
        return listTooLarge(path, QStringLiteral("7z header of %1 bytes").arg(length));
    if (!f.seek(qint64(32 + offset)))
        return damagedArchive(path, QStringLiteral("7z header out of reach"));
    // The description of a packed header is a few dozen bytes.
    const QByteArray head = f.read(qint64(std::min<quint64>(length, 65536)));
    const auto bad = [&](const char* why) { return damagedArchive(path, QLatin1String("7z header: ") + QLatin1String(why)); };
    Cursor c(head);
    const int id = c.byte();
    if (id == 0x01)
        return success(); // a plain header: as large as it is stored
    if (id != 0x17)
        return bad("unexpected first record");
    if (c.byte() != 0x06) // pack info
        return bad("no pack info");
    c.sevenZipNumber(); // position
    const quint64 packStreams = c.sevenZipNumber();
    if (!c.ok || packStreams > 8)
        return bad("pack streams");
    for (;;) {
        const int rec = c.byte();
        if (!c.ok)
            return bad("pack info cut short");
        if (rec == 0x00)
            break;
        if (rec == 0x09) { // sizes
            for (quint64 i = 0; i < packStreams; ++i)
                c.sevenZipNumber();
        } else if (rec == 0x0A) { // checksums
            quint64 defined = packStreams;
            if (c.byte() == 0) {
                defined = 0;
                for (quint64 i = 0; i < (packStreams + 7) / 8; ++i)
                    defined += quint64(qPopulationCount(quint8(c.byte())));
            }
            c.skip(4 * defined);
        } else {
            return bad("unknown pack record");
        }
    }
    if (c.byte() != 0x07 || c.byte() != 0x0B) // unpack info, folders
        return bad("no folder info");
    const quint64 folders = c.sevenZipNumber();
    if (!c.ok || folders < 1 || folders > 8 || c.byte() != 0)
        return bad("folders");
    bool encrypted = false;
    quint64 outStreams = 0;
    for (quint64 fo = 0; fo < folders; ++fo) {
        const quint64 coders = c.sevenZipNumber();
        if (!c.ok || coders < 1 || coders > 8)
            return bad("coders");
        quint64 in = 0, out = 0;
        for (quint64 co = 0; co < coders; ++co) {
            const int flags = c.byte();
            if (!c.ok || (flags & 0xC0))
                return bad("coder flags");
            const int idSize = flags & 0x0F;
            QByteArray coderId;
            for (int i = 0; i < idSize; ++i)
                coderId.append(char(c.byte()));
            if (coderId == QByteArray::fromHex("06f10701")) // AES-256 + SHA-256
                encrypted = true;
            quint64 coderIn = 1, coderOut = 1;
            if (flags & 0x10) {
                coderIn = c.sevenZipNumber();
                coderOut = c.sevenZipNumber();
                if (!c.ok || coderIn > 8 || coderOut > 8)
                    return bad("coder streams");
            }
            if (flags & 0x20)
                c.skip(c.sevenZipNumber());
            in += coderIn;
            out += coderOut;
        }
        if (!c.ok || out < 1)
            return bad("coder list cut short");
        const quint64 binds = out - 1;
        for (quint64 i = 0; i < 2 * binds; ++i)
            c.sevenZipNumber();
        if (in < binds)
            return bad("bind pairs");
        if (in - binds > 1)
            for (quint64 i = 0; i < in - binds; ++i)
                c.sevenZipNumber();
        outStreams += out;
    }
    if (c.byte() != 0x0C) // unpack sizes
        return bad("no unpack sizes");
    quint64 largest = 0;
    for (quint64 i = 0; i < outStreams; ++i)
        largest = std::max(largest, c.sevenZipNumber());
    if (!c.ok)
        return bad("unpack sizes cut short");
    if (encrypted)
        return passwordProtected(path);
    if (largest > kMaxSevenZipHeader)
        return listTooLarge(path, QStringLiteral("7z header of %1 bytes when unpacked").arg(largest));
    return success();
}

// ---------------------------------------------------------------------------
// RAR 4: what the main header says

struct Rar4Info {
    bool volume = false;
    bool solid = false;
    bool newNaming = false;
    bool encryptedHeaders = false;
};

Result<Rar4Info> readRar4Info(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(nameOf(path)), f.errorString());
    const QByteArray h = f.read(7 + 7);
    if (h.size() < 14)
        return incompleteArchive(path, QStringLiteral("shorter than a RAR main header"));
    if (uchar(h.at(7 + 2)) != 0x73)
        return damagedArchive(path, QStringLiteral("no RAR main header"));
    const quint16 flags = qFromLittleEndian<quint16>(h.constData() + 7 + 3);
    Rar4Info info;
    info.volume = (flags & 0x0001) != 0;
    info.solid = (flags & 0x0008) != 0;
    info.newNaming = (flags & 0x0010) != 0;
    info.encryptedHeaders = (flags & 0x0080) != 0;
    return info;
}

// True when the last record of a RAR 4 part says that another part follows.
// The record is optional and of variable size, so it is looked for at the
// few places it can end the file at.
bool rar4ContinuesAfter(const QString& volume)
{
    QFile f(toLongPath(volume));
    if (!f.open(QIODevice::ReadOnly) || f.size() < 64)
        return false;
    f.seek(f.size() - 32);
    const QByteArray tail = f.read(32);
    for (int size = 7; size <= 20 && size <= tail.size(); ++size) {
        const char* block = tail.constData() + tail.size() - size;
        if (uchar(block[2]) == 0x7B && qFromLittleEndian<quint16>(block + 5) == size)
            return (qFromLittleEndian<quint16>(block + 3) & 0x0001) != 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// RAR 5: the listing, read from the block headers

Result<ArchiveListing> listRar5(const QString& path, const CancellationToken& token)
{
    ArchiveListing out;
    out.path = path;
    out.kind = ArchiveKind::Rar5;
    QString volume = path;
    for (int index = 0;; ++index) {
        QFile f(toLongPath(volume));
        if (!f.open(QIODevice::ReadOnly)) {
            if (index > 0 && !QFileInfo::exists(volume))
                return missingPart(volume);
            return Error(ErrorCode::IoError, QStringLiteral("Could not open %1.").arg(nameOf(volume)), f.errorString());
        }
        const qint64 size = f.size();
        if (f.read(8) != QByteArray("Rar!\x1A\x07\x01\x00", 8))
            return damagedArchive(volume, QStringLiteral("no RAR 5 signature"));
        out.volumes << volume;
        qint64 pos = 8;
        bool mainSeen = false, endSeen = false, more = false;
        for (quint64 blocks = 0; !endSeen; ++blocks) {
            if ((blocks & 0xFF) == 0 && !token.checkpoint())
                return Error::cancelled();
            if (size - pos < 7)
                return incompleteArchive(volume, QStringLiteral("no end-of-archive record"));
            if (!f.seek(pos))
                return damagedArchive(volume, f.errorString());
            const QByteArray lead = f.read(7);
            if (lead.size() < 7)
                return incompleteArchive(volume, QStringLiteral("block header cut short at %1").arg(pos));
            Cursor sizeField(lead, 4);
            const quint64 headerSize = sizeField.vint();
            const qint64 sizeBytes = 3 - sizeField.left();
            // Three bytes at most: a header is never larger than 2 MiB.
            if (!sizeField.ok || headerSize == 0 || headerSize > kMaxRar5Header
                || (sizeBytes == 3 && (uchar(lead.at(6)) & 0x80)))
                return damagedArchive(volume, QStringLiteral("block header size at %1").arg(pos));
            const qint64 headerEnd = pos + 4 + sizeBytes + qint64(headerSize);
            if (headerEnd > size)
                return incompleteArchive(volume, QStringLiteral("block header at %1 passes the end").arg(pos));
            if (!f.seek(pos + 4))
                return damagedArchive(volume, f.errorString());
            const QByteArray block = f.read(sizeBytes + qint64(headerSize));
            if (block.size() != sizeBytes + qint64(headerSize))
                return damagedArchive(volume, QStringLiteral("short read at %1").arg(pos));
            if (zip::crc32(block) != qFromLittleEndian<quint32>(lead.constData()))
                return damagedArchive(volume, QStringLiteral("block header checksum at %1").arg(pos));
            Cursor c(block, qsizetype(sizeBytes));
            const quint64 type = c.vint();
            const quint64 flags = c.vint();
            const quint64 extraSize = (flags & 0x0001) ? c.vint() : 0;
            const quint64 dataSize = (flags & 0x0002) ? c.vint() : 0;
            if (!c.ok || extraSize > quint64(c.left()))
                return damagedArchive(volume, QStringLiteral("block fields at %1").arg(pos));
            if (dataSize > quint64(size - headerEnd))
                return incompleteArchive(volume, QStringLiteral("data of the block at %1 passes the end").arg(pos));
            pos = headerEnd + qint64(dataSize);

            if (type == 4) // archive encryption header: everything after it is encrypted
                return passwordProtected(path);
            if (type == 1) {
                const quint64 archiveFlags = c.vint();
                const quint64 number = (archiveFlags & 0x0002) ? c.vint() : 0;
                if (!c.ok)
                    return damagedArchive(volume, QStringLiteral("main header"));
                out.solid = out.solid || (archiveFlags & 0x0004) != 0;
                mainSeen = true;
                if (index == 0 && number > 0) {
                    FW_TRY(mustBeFirstPart(path));
                    return Error(ErrorCode::InvalidArgument,
                                 QStringLiteral("%1 is not the first part of its multi-part archive. Choose the first part.")
                                     .arg(nameOf(path)));
                }
            } else if (type == 5) {
                more = (c.vint() & 0x0001) != 0;
                endSeen = true;
            } else if (type == 2) {
                if (!mainSeen)
                    return damagedArchive(volume, QStringLiteral("file before the main header"));
                const quint64 fileFlags = c.vint();
                const quint64 unpacked = c.vint();
                c.vint(); // attributes
                if (fileFlags & 0x0002)
                    c.skip(4); // time
                if (fileFlags & 0x0004)
                    c.skip(4); // checksum
                const quint64 compression = c.vint();
                c.vint(); // host system
                const quint64 nameLen = c.vint();
                if (!c.ok || nameLen == 0 || quint64(c.left()) < extraSize || nameLen > quint64(c.left()) - extraSize)
                    return damagedArchive(volume, QStringLiteral("file header at %1").arg(headerEnd - qint64(headerSize)));
                ArchiveEntry e;
                QString raw;
                if (!decodeUtf8(reinterpret_cast<const char*>(c.p), qsizetype(nameLen), raw))
                    return unreadableName(path, QStringLiteral("a RAR 5 name that is not UTF-8"));
                e.name = normalisedName(raw);
                const bool isDir = (fileFlags & 0x0001) != 0;
                e.type = isDir ? ArchiveEntry::Type::Directory : ArchiveEntry::Type::File;
                // The extra area is the tail of the header: records of [size][type][data].
                Cursor x(block, block.size() - qsizetype(extraSize));
                while (x.left() > 0) {
                    const quint64 recordSize = x.vint();
                    const uchar* recordStart = x.p;
                    if (!x.ok || recordSize == 0 || recordSize > quint64(x.left()))
                        return damagedArchive(volume, QStringLiteral("extra record of %1").arg(e.name));
                    const quint64 recordType = x.vint();
                    if (recordType == 0x01) {
                        e.encrypted = true;
                    } else if (recordType == 0x05) {
                        const quint64 redirection = x.vint();
                        if (redirection == 5)
                            e.type = ArchiveEntry::Type::FileCopy;
                        else if (redirection != 0)
                            e.type = ArchiveEntry::Type::Link;
                    }
                    x.p = recordStart;
                    x.ok = true;
                    x.skip(recordSize);
                }
                if (e.type == ArchiveEntry::Type::File) {
                    if (fileFlags & 0x0008)
                        return Error(ErrorCode::Unsupported,
                                     QStringLiteral("%1 does not say how large %2 is. %3")
                                         .arg(nameOf(path), e.name, QLatin1String(kAdvice)));
                    e.size = qint64(std::min<quint64>(unpacked, quint64(std::numeric_limits<qint64>::max())));
                    out.largestDictionary = std::max(out.largestDictionary, (128ll * 1024) << ((compression >> 10) & 15));
                }
                // A file that continues from the previous part was listed there.
                if (!(flags & 0x0008)) {
                    if (qsizetype(out.entries.size()) >= kMaxEntries)
                        return tooManyEntries(path);
                    out.entries.push_back(std::move(e));
                }
            }
        }
        if (!mainSeen)
            return damagedArchive(volume, QStringLiteral("no main header"));
        if (!more)
            break;
        if (index + 1 >= kMaxVolumes)
            return damagedArchive(path, QStringLiteral("more than %1 parts").arg(kMaxVolumes));
        const QString next = nextVolume(volume, false);
        if (next.isEmpty())
            return Error(ErrorCode::NotFound,
                         QStringLiteral("%1 continues in another part, which was not found next to it.").arg(nameOf(volume)));
        if (!QFileInfo::exists(next))
            return missingPart(next);
        volume = next;
    }
    return out;
}

// ---------------------------------------------------------------------------
// libarchive

struct Client;
struct Node {
    Client* client = nullptr;
    int index = 0;
};

// What libarchive reads through: the archive's files, one open at a time.
struct Client {
    QStringList paths;
    std::vector<Node> nodes;
    QFile file;
    int open = -1;
    QByteArray buffer;
    const CancellationToken* token = nullptr;
    qint64 budget = -1; // bytes that may still be read; -1: no limit
    QDeadlineTimer deadline{QDeadlineTimer::Forever};
    bool cancelled = false;
    bool budgetSpent = false;
    bool ioFailed = false;
    QString ioDetail;
};

int openCallback(struct archive* a, void* data)
{
    Node* node = static_cast<Node*>(data);
    Client& c = *node->client;
    c.file.close();
    c.file.setFileName(toLongPath(c.paths.at(node->index)));
    if (!c.file.open(QIODevice::ReadOnly)) {
        c.ioFailed = true;
        c.ioDetail = QStringLiteral("%1: %2").arg(c.paths.at(node->index), c.file.errorString());
        archive_set_error(a, EIO, "could not open the file");
        return ARCHIVE_FATAL;
    }
    c.open = node->index;
    return ARCHIVE_OK;
}

int closeCallback(struct archive*, void* data)
{
    Node* node = static_cast<Node*>(data);
    Client& c = *node->client;
    if (c.open == node->index) {
        c.file.close();
        c.open = -1;
    }
    return ARCHIVE_OK;
}

la_ssize_t readCallback(struct archive* a, void* data, const void** buffer)
{
    Client& c = *static_cast<Node*>(data)->client;
    if (c.token && !c.token->checkpoint()) {
        c.cancelled = true;
        archive_set_error(a, EINTR, "cancelled");
        return -1;
    }
    qint64 want = c.buffer.size();
    if (c.budget >= 0)
        want = std::min(want, c.budget);
    if (want == 0 || c.deadline.hasExpired()) {
        c.budgetSpent = true;
        archive_set_error(a, EINTR, "read budget spent");
        return -1;
    }
    const qint64 n = c.file.read(c.buffer.data(), want);
    if (n < 0) {
        c.ioFailed = true;
        c.ioDetail = c.file.errorString();
        archive_set_error(a, EIO, "read failed");
        return -1;
    }
    if (c.budget >= 0)
        c.budget -= n;
    *buffer = c.buffer.constData();
    return la_ssize_t(n);
}

la_int64_t skipCallback(struct archive*, void* data, la_int64_t request)
{
    Client& c = *static_cast<Node*>(data)->client;
    const qint64 pos = c.file.pos();
    const qint64 n = std::min<qint64>(request, c.file.size() - pos);
    if (n <= 0 || !c.file.seek(pos + n))
        return 0;
    return n;
}

la_int64_t seekCallback(struct archive* a, void* data, la_int64_t offset, int whence)
{
    Client& c = *static_cast<Node*>(data)->client;
    const qint64 size = c.file.size();
    const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? c.file.pos() : size;
    if (offset > std::numeric_limits<qint64>::max() - base || base + offset < 0 || !c.file.seek(base + offset)) {
        archive_set_error(a, EINVAL, "seek outside the file");
        return ARCHIVE_FATAL;
    }
    return base + offset;
}

class LaReader {
public:
    LaReader() = default;
    LaReader(const LaReader&) = delete;
    LaReader& operator=(const LaReader&) = delete;
    ~LaReader()
    {
        if (m_a)
            archive_read_free(m_a);
    }

    Status open(const ArchiveListing& listing, const CancellationToken& token)
    {
        m_archive = listing.path;
        m_kind = listing.kind;
        m_client.paths = listing.volumes;
        m_client.token = &token;
        m_client.buffer.resize(kReadBlock);
        m_client.nodes.resize(size_t(listing.volumes.size()));
        m_a = archive_read_new();
        if (!m_a)
            return Error(ErrorCode::Internal, QStringLiteral("The archive reader could not start."));
        switch (listing.kind) {
        case ArchiveKind::SevenZip:
            archive_read_support_format_7zip(m_a);
            break;
        case ArchiveKind::Rar4:
            archive_read_support_format_rar(m_a);
            // Names RAR 4 stores without its Unicode form are read as UTF-8;
            // one that is not is refused instead of guessed.
            archive_read_set_options(m_a, "rar:hdrcharset=UTF-8");
            break;
        case ArchiveKind::Rar5:
            archive_read_support_format_rar5(m_a);
            break;
        default:
            return Error(ErrorCode::Internal, QStringLiteral("Not an archive this reader opens."));
        }
        archive_read_set_open_callback(m_a, openCallback);
        archive_read_set_read_callback(m_a, readCallback);
        archive_read_set_skip_callback(m_a, skipCallback);
        archive_read_set_seek_callback(m_a, seekCallback);
        archive_read_set_close_callback(m_a, closeCallback);
        for (size_t i = 0; i < m_client.nodes.size(); ++i) {
            m_client.nodes[i] = Node{&m_client, int(i)};
            archive_read_append_callback_data(m_a, &m_client.nodes[i]);
        }
        if (archive_read_open1(m_a) != ARCHIVE_OK)
            return failure({});
        return success();
    }

    struct archive* handle() const { return m_a; }
    Client& client() { return m_client; }

    void setBudget(const ReadBudget& budget)
    {
        m_client.budget = budget.archiveBytes;
        m_client.deadline = budget.milliseconds < 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                                                    : QDeadlineTimer(budget.milliseconds);
    }

    // Why the last call failed, in the user's words. `entry` is the file that
    // was being read, when there is one.
    Error failure(const QString& entry) const
    {
        if (m_client.cancelled)
            return Error::cancelled();
        const QString archive = nameOf(m_archive);
        if (m_client.ioFailed)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(archive), m_client.ioDetail);
        const int code = m_a ? archive_errno(m_a) : 0;
        const char* text = m_a ? archive_error_string(m_a) : nullptr;
        const QString detail = text ? QString::fromUtf8(text) : QStringLiteral("no detail");
        const auto says = [&](const char* word) { return detail.contains(QLatin1String(word), Qt::CaseInsensitive); };
        if (says("ncrypt") || says("crypto") || says("passphrase"))
            return passwordProtected(m_archive);
        if (code == ENOMEM || says("allocate") || says("memory"))
            return listTooLarge(m_archive, detail);
        if (says("unsupported") || says("not supported") || says("no support") || says("support unavailable")
            || says("codec") || says("unknown unpacked size"))
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 uses a kind of compression this app cannot read (%2). %3")
                             .arg(archive, detail, QLatin1String(kAdvice)),
                         entry);
        if (entry.isEmpty())
            return damagedArchive(m_archive, detail);
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in %2.").arg(entry, archive), detail);
    }

    // The entry's name as the archive stores it. libarchive keeps a 7z or
    // RAR 4 name as wide characters on Windows and converts it to the locale
    // elsewhere; a RAR 5 name is kept as its UTF-8 bytes.
    bool entryName(struct archive_entry* e, QString& name) const
    {
#ifdef Q_OS_WIN
        if (m_kind != ArchiveKind::Rar5) {
            if (const wchar_t* w = archive_entry_pathname_w(e)) {
                name = normalisedName(QString::fromWCharArray(w));
                return true;
            }
        }
#endif
        const char* utf8 = archive_entry_pathname_utf8(e);
        QString raw;
        if (!utf8 || !decodeUtf8(utf8, qsizetype(qstrlen(utf8)), raw))
            return false;
        name = normalisedName(raw);
        return true;
    }

    static ArchiveEntry::Type entryType(struct archive_entry* e)
    {
        // Asked without the target's text: that may not exist in the PC's code page.
        if (archive_entry_hardlink_is_set(e) || archive_entry_filetype(e) == AE_IFLNK)
            return ArchiveEntry::Type::Link;
        if (archive_entry_filetype(e) == AE_IFDIR)
            return ArchiveEntry::Type::Directory;
        return ArchiveEntry::Type::File;
    }

private:
    struct archive* m_a = nullptr;
    Client m_client;
    QString m_archive;
    ArchiveKind m_kind = ArchiveKind::Unknown;
};

// 7z and RAR 4: libarchive walks the headers without decoding file data.
Status listWithLibarchive(ArchiveListing& out, const CancellationToken& token)
{
    LaReader r;
    FW_TRY(r.open(out, token));
    bool first = true;
    for (;;) {
        struct archive_entry* e = nullptr;
        const int rc = archive_read_next_header(r.handle(), &e);
        if (first && out.kind == ArchiveKind::SevenZip) {
            // The list itself has been read; from here on only the target of
            // a symbolic link is ever decoded.
            r.setBudget(ReadBudget{kListingDataBudget, kListingDataMs});
        }
        first = false;
        if (rc == ARCHIVE_EOF)
            break;
        if (rc == ARCHIVE_RETRY)
            continue;
        if (rc < ARCHIVE_WARN) {
            if (r.client().budgetSpent)
                return Error(ErrorCode::Unsupported,
                             QStringLiteral("%1 holds symbolic links that cannot be listed without unpacking it. %2")
                                 .arg(nameOf(out.path), QLatin1String(kAdvice)));
            return r.failure({});
        }
        const char* warning = rc == ARCHIVE_WARN ? archive_error_string(r.handle()) : nullptr;
        ArchiveEntry entry;
        if (!r.entryName(e, entry.name) || (warning && QByteArray(warning).contains("cannot be converted")))
            return unreadableName(out.path, warning ? QString::fromUtf8(warning) : QString());
        entry.type = LaReader::entryType(e);
        entry.encrypted = archive_entry_is_data_encrypted(e) > 0 || archive_entry_is_metadata_encrypted(e) > 0;
        if (entry.type == ArchiveEntry::Type::File)
            entry.size = std::max<qint64>(0, archive_entry_size(e));
        if (qsizetype(out.entries.size()) >= kMaxEntries)
            return tooManyEntries(out.path);
        out.entries.push_back(std::move(entry));
    }
    return success();
}

bool isUnder(const QString& name, const QString& prefix)
{
    return prefix.isEmpty() || name.startsWith(prefix, Qt::CaseInsensitive);
}

// "", "." and "./.": the destination folder itself, which some tools list.
bool isDestinationItself(const QString& name)
{
    for (const QString& part : name.split(u'/', Qt::SkipEmptyParts))
        if (part != u".")
            return false;
    return true;
}

} // namespace

ArchiveKind sniffArchive(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return ArchiveKind::Unknown;
    const QByteArray h = f.read(8);
    if (h.startsWith(QByteArray("7z\xBC\xAF\x27\x1C", 6)))
        return ArchiveKind::SevenZip;
    if (h.startsWith(QByteArray("Rar!\x1A\x07\x01\x00", 8)))
        return ArchiveKind::Rar5;
    if (h.startsWith(QByteArray("Rar!\x1A\x07\x00", 7)))
        return ArchiveKind::Rar4;
    if (h.startsWith("PK"))
        return ArchiveKind::Zip;
    return ArchiveKind::Unknown;
}

Result<ArchiveListing> listArchive(const QString& path, const CancellationToken& token)
{
    const ArchiveKind kind = sniffArchive(path);
    if (kind == ArchiveKind::SevenZip) {
        FW_TRY(checkSevenZipHeader(path));
        ArchiveListing out;
        out.path = path;
        out.kind = kind;
        out.volumes << path;
        FW_TRY(listWithLibarchive(out, token));
        return out;
    }
    if (kind == ArchiveKind::Rar5) {
        FW_TRY(mustBeFirstPart(path));
        return listRar5(path, token);
    }
    if (kind == ArchiveKind::Rar4) {
        FW_TRY_ASSIGN(const Rar4Info info, readRar4Info(path));
        if (info.encryptedHeaders)
            return passwordProtected(path);
        if (info.solid)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 is a solid archive in the old RAR 4 format, which this app cannot read. %2")
                             .arg(nameOf(path), QLatin1String(kAdvice)));
        ArchiveListing out;
        out.path = path;
        out.kind = kind;
        out.volumes << path;
        // "x.rar, x.r00, x.r01" unless the archive or its name says "x.part1.rar".
        const bool oldNaming = !info.newNaming && !splitPartName(nameOf(path)).valid();
        if (info.volume) {
            if (!oldNaming)
                FW_TRY(mustBeFirstPart(path));
            for (QString next = nextVolume(path, oldNaming); !next.isEmpty() && QFileInfo::exists(next);
                 next = nextVolume(next, oldNaming)) {
                if (out.volumes.size() >= kMaxVolumes)
                    return damagedArchive(path, QStringLiteral("more than %1 parts").arg(kMaxVolumes));
                out.volumes << next;
            }
            if (rar4ContinuesAfter(out.volumes.last())) {
                const QString next = nextVolume(out.volumes.last(), oldNaming);
                if (!next.isEmpty())
                    return missingPart(next);
            }
        }
        if (Status st = listWithLibarchive(out, token); !st.ok()) {
            // The parts are taken from the folder; libarchive only sees the
            // set end early when one is not there.
            const QString next = info.volume ? nextVolume(out.volumes.last(), oldNaming) : QString();
            if (!next.isEmpty() && st.error().code == ErrorCode::CorruptData)
                return Error(ErrorCode::CorruptData,
                             QStringLiteral("%1 is damaged or incomplete (if the archive has more parts, %2 is missing).")
                                 .arg(nameOf(path), nameOf(next)),
                             st.error().detail);
            return st.takeError();
        }
        return out;
    }
    return Error(ErrorCode::CorruptData, QStringLiteral("%1 is not a 7z or RAR archive.").arg(nameOf(path)));
}

Result<QByteArray> readArchiveFile(const ArchiveListing& listing, const QString& name, qint64 maxSize,
                                   const ReadBudget& budget, bool* withinBudget, const CancellationToken& token)
{
    if (withinBudget)
        *withinBudget = true;
    LaReader r;
    FW_TRY(r.open(listing, token));
    const auto failed = [&](const QString& entry) -> Result<QByteArray> {
        if (r.client().budgetSpent && !r.client().cancelled) {
            if (withinBudget)
                *withinBudget = false;
            return QByteArray();
        }
        return r.failure(entry);
    };
    bool first = true;
    for (;;) {
        struct archive_entry* e = nullptr;
        const int rc = archive_read_next_header(r.handle(), &e);
        if (first) // a 7z has read its list of files by now, which every use of the archive costs
            r.setBudget(budget);
        first = false;
        if (rc == ARCHIVE_EOF)
            return Error(ErrorCode::NotFound, QStringLiteral("%1 is not in %2.").arg(name, nameOf(listing.path)));
        if (rc == ARCHIVE_RETRY)
            continue;
        if (rc < ARCHIVE_WARN)
            return failed({});
        QString entryName;
        if (!r.entryName(e, entryName) || entryName.compare(name, Qt::CaseInsensitive) != 0
            || LaReader::entryType(e) != ArchiveEntry::Type::File)
            continue;
        if (archive_entry_size(e) > maxSize)
            return Error(ErrorCode::Unsupported, QStringLiteral("%1 is too large to read into memory.").arg(name));
        QByteArray all;
        for (;;) {
            const void* block = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            const int rd = archive_read_data_block(r.handle(), &block, &size, &offset);
            if (rd == ARCHIVE_EOF)
                return all;
            if (rd == ARCHIVE_RETRY)
                continue;
            if (rd != ARCHIVE_OK)
                return failed(name);
            if (offset != all.size() || qint64(size) > maxSize - all.size())
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 is larger than the archive says.").arg(name));
            all.append(static_cast<const char*>(block), qsizetype(size));
        }
    }
}

Status checkExtractable(const ArchiveListing& listing, const QString& onlyUnder)
{
    const QString archive = nameOf(listing.path);
    if (listing.kind == ArchiveKind::Rar5 && listing.largestDictionary > kMaxRarDictionary)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("%1 was made with a %2 dictionary; this app reads RAR archives with dictionaries of up "
                                    "to 64 MiB. %3")
                         .arg(archive, formatBytes(listing.largestDictionary), QLatin1String(kAdvice)));
    struct Seen {
        QString spelling;
        bool directory = false;
    };
    QHash<QString, Seen> seen;
    // Two names that the destination's file system takes for one: reported,
    // never merged or overwritten.
    const auto claim = [&](const QString& path, bool directory) -> Status {
        const QString key = hostNameKey(path).toLower();
        const auto it = seen.constFind(key);
        if (it == seen.constEnd()) {
            seen.insert(key, Seen{path, directory});
            return success();
        }
        if (it->spelling != path)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 holds two names that differ only in upper and lower case (%2 and %3). "
                                        "Windows cannot store both.")
                             .arg(archive, it->spelling, path));
        if (it->directory != directory || !directory)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 holds %2 twice.").arg(archive, path));
        return success();
    };
    QString lastParent;
    for (const ArchiveEntry& e : listing.entries) {
        if (!isUnder(e.name, onlyUnder))
            continue;
        if (e.type == ArchiveEntry::Type::FileCopy)
            return Error(ErrorCode::Unsupported,
                         QStringLiteral("%1 stores identical files only once (%2), which this app cannot read. %3")
                             .arg(archive, e.name, QLatin1String(kAdvice)));
        if (e.encrypted && e.type == ArchiveEntry::Type::File)
            return passwordProtected(listing.path);
        const QString rel = zip::Reader::extractedPath(e.name);
        if (rel.isEmpty()) {
            if (isDestinationItself(e.name))
                continue;
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 contains an unsafe path (%2); nothing was unpacked.").arg(archive, e.name));
        }
        if (e.type == ArchiveEntry::Type::Link)
            continue; // never created, so it cannot collide
        const qsizetype slash = rel.lastIndexOf(u'/');
        const QString parent = slash < 0 ? QString() : rel.left(slash);
        if (parent != lastParent) {
            // "a/b/c": a, a/b, a/b/c
            for (qsizetype i = parent.indexOf(u'/'); !parent.isEmpty(); i = parent.indexOf(u'/', i + 1)) {
                FW_TRY(claim(i < 0 ? parent : parent.left(i), true));
                if (i < 0)
                    break;
            }
            lastParent = parent;
        }
        FW_TRY(claim(rel, e.type == ArchiveEntry::Type::Directory));
    }
    return success();
}

Result<ExtractResult> extractArchive(const ArchiveListing& listing, const QString& destination,
                                     const QString& onlyUnder, const CancellationToken& token,
                                     ProgressReporter* progress)
{
    // Every name is checked before anything is written.
    FW_TRY(checkExtractable(listing, onlyUnder));
    const QString archive = nameOf(listing.path);
    struct Expected {
        qint64 size = 0;
        bool seen = false;
    };
    // What the pass must deliver: each listed file once, no larger than
    // listed. The free-space check was made from this list.
    QHash<QString, Expected> expected;
    qsizetype remaining = 0;
    for (const ArchiveEntry& e : listing.entries) {
        if (!isUnder(e.name, onlyUnder))
            continue;
        ++remaining;
        if (e.type == ArchiveEntry::Type::File)
            expected.insert(e.name, Expected{e.size, false});
    }
    const auto mismatch = [&](const QString& what) {
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("%1 does not match its own list of files (%2).").arg(archive, what));
    };

    QDir().mkpath(destination);
    const QDir dest(destination);
    ExtractResult result;
    LaReader r;
    FW_TRY(r.open(listing, token));
    while (remaining > 0) {
        struct archive_entry* e = nullptr;
        const int rc = archive_read_next_header(r.handle(), &e);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc == ARCHIVE_RETRY)
            continue;
        if (rc < ARCHIVE_WARN)
            return r.failure({});
        QString name;
        if (!r.entryName(e, name))
            return unreadableName(listing.path, {});
        if (!isUnder(name, onlyUnder))
            continue; // libarchive passes over its data
        --remaining;
        const QString rel = zip::Reader::extractedPath(name);
        if (rel.isEmpty())
            continue; // the destination itself
        const QString target = dest.filePath(rel);
        if (!isSameOrInside(target, destination))
            return mismatch(name);
        const ArchiveEntry::Type type = LaReader::entryType(e);
        if (type == ArchiveEntry::Type::Link) {
            ++result.linksSkipped;
            continue;
        }
        if (type == ArchiveEntry::Type::Directory) {
            QDir().mkpath(target);
            continue;
        }
        const auto listed = expected.find(name);
        if (listed == expected.end() || listed->seen)
            return mismatch(name);
        listed->seen = true;
        QDir().mkpath(QFileInfo(target).absolutePath());
        PartFile part(target);
        QFile out(toLongPath(part.partPath()));
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(rel), out.errorString());
        qint64 produced = 0;
        for (;;) {
            const void* block = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            const int rd = archive_read_data_block(r.handle(), &block, &size, &offset);
            if (rd == ARCHIVE_EOF)
                break;
            if (rd == ARCHIVE_RETRY)
                continue;
            // A warning here is a checksum that did not match or data that
            // could not be decoded: never a file to build from.
            if (rd != ARCHIVE_OK)
                return r.failure(name);
            if (size == 0)
                continue;
            if (offset != produced)
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in %2.").arg(name, archive),
                             QStringLiteral("data at %1, expected at %2").arg(offset).arg(produced));
            if (qint64(size) > listed->size - produced)
                return Error(ErrorCode::CorruptData, QStringLiteral("%1 is larger than the archive says.").arg(name));
            if (out.write(static_cast<const char*>(block), qint64(size)) != qint64(size))
                return Error(ErrorCode::IoError, QStringLiteral("Could not write %1.").arg(rel), out.errorString());
            produced += qint64(size);
            if (progress)
                progress->advance(qint64(size));
            if (!token.checkpoint())
                return Error::cancelled();
        }
        if (produced != listed->size)
            return Error(ErrorCode::CorruptData, QStringLiteral("%1 is damaged in %2.").arg(name, archive),
                         QStringLiteral("%1 of %2 bytes").arg(produced).arg(listed->size));
        QDateTime modified;
        if (archive_entry_mtime_is_set(e))
            modified = QDateTime::fromSecsSinceEpoch(qint64(archive_entry_mtime(e)));
        FW_TRY(finishWrittenFile(out, rel, modified));
        FW_TRY(part.commit(true));
        ++result.files;
        result.bytes += produced;
        if (progress)
            progress->advanceFiles(1);
    }
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        if (!it->seen)
            return mismatch(QStringLiteral("%1 is missing").arg(it.key()));
    return result;
}

} // namespace fw::archive
