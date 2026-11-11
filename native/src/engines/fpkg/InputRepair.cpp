#include "engines/fpkg/InputRepair.h"

#include "core/FileUtil.h"
#include "core/OrderedJson.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
namespace fw::fpkg {

// ---- PNG ---------------------------------------------------------------------------

PngInfo pngInfoFromBytes(const QByteArray& h)
{
    PngInfo info;
    static const QByteArray sig("\x89PNG\r\n\x1A\n", 8);
    if (h.size() < 33 || !h.startsWith(sig)) {
        info.problem = QStringLiteral("not a PNG file");
        return info;
    }
    if (qFromBigEndian<quint32>(h.constData() + 8) != 13 || h.mid(12, 4) != "IHDR") {
        info.problem = QStringLiteral("the PNG header (IHDR) is missing");
        return info;
    }
    info.bitDepth = quint8(h[24]);
    info.colorType = quint8(h[25]);
    info.interlace = quint8(h[28]);
    info.valid = true;
    return info;
}

PngInfo readPngInfo(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        PngInfo i;
        i.problem = QStringLiteral("could not be read");
        return i;
    }
    return pngInfoFromBytes(f.read(64));
}

PngRequirement presentationRequirement(const QString& fileName)
{
    static const QRegularExpression rgb(QStringLiteral("^(icon0|pic0|pic1)(_[0-9]{2})?\\.png$"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rgba(QStringLiteral("^pic2(_[0-9]{2})?\\.png$"), QRegularExpression::CaseInsensitiveOption);
    if (rgb.match(fileName).hasMatch())
        return PngRequirement::Rgb;
    if (rgba.match(fileName).hasMatch())
        return PngRequirement::Rgba;
    return PngRequirement::None;
}

bool pngMeets(const PngInfo& info, PngRequirement req)
{
    if (!info.valid)
        return false;
    switch (req) {
    case PngRequirement::None: return true;
    case PngRequirement::Rgb: return info.bitDepth == 8 && info.colorType == 2;
    case PngRequirement::Rgba: return info.bitDepth == 8 && info.colorType == 6;
    }
    return true;
}

QString requirementText(PngRequirement req)
{
    switch (req) {
    case PngRequirement::Rgb: return QStringLiteral("8-bit RGB");
    case PngRequirement::Rgba: return QStringLiteral("8-bit RGBA");
    case PngRequirement::None: break;
    }
    return QStringLiteral("a PNG");
}

namespace {
QString describePng(const PngInfo& i)
{
    if (!i.valid)
        return i.problem;
    const QString kind = i.colorType == 2 ? QStringLiteral("RGB")
                       : i.colorType == 6 ? QStringLiteral("RGBA")
                       : i.colorType == 3 ? QStringLiteral("palette")
                       : i.colorType == 0 ? QStringLiteral("grey")
                       : i.colorType == 4 ? QStringLiteral("grey + alpha")
                                          : QStringLiteral("colour type %1").arg(i.colorType);
    return QStringLiteral("%1-bit %2").arg(i.bitDepth).arg(kind);
}
} // namespace

Result<ImagePlan> planImageRecovery(const ScanResult& scan, bool recoverEnabled, bool haveConverter)
{
    ImagePlan plan;
    QHash<QString, QString> byPng; // lower dst -> dds rel
    QSet<QString> covered;
    for (const auto& [ddsRel, ddsAbs] : scan.sceSysDds) {
        const QString name = ddsRel.section(u'/', -1);
        const QString pngName = name.left(name.size() - 4) + QStringLiteral(".png");
        const QString dst = QStringLiteral("sce_sys/") + pngName;
        const QString key = dst.toLower();
        if (byPng.contains(key))
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 and %2 would both become %3. Remove one of them from the dump.")
                             .arg(byPng.value(key), ddsRel, dst));
        byPng.insert(key, ddsRel);
        covered.insert(key);

        const PngRequirement req = presentationRequirement(pngName);
        const ScannedFile* existing = scan.findFile(dst);
        QString reason;
        if (!existing) {
            reason = QStringLiteral("%1 is missing").arg(dst);
        } else {
            const PngInfo info = readPngInfo(existing->absPath);
            if (!pngMeets(info, req))
                reason = QStringLiteral("%1 is %2, expected %3").arg(existing->rel, describePng(info), requirementText(req));
        }
        if (reason.isEmpty())
            continue;
        if (!recoverEnabled) {
            if (existing && req != PngRequirement::None)
                return Error(ErrorCode::CorruptData,
                             QStringLiteral("%1. The Publishing Tools reject it. Turn on \"Rebuild sce_sys images from "
                                            ".dds\" or replace the image.").arg(reason));
            plan.notes << QStringLiteral("%1; not rebuilt (\"Rebuild sce_sys images from .dds\" is off)").arg(reason);
            continue;
        }
        if (!haveConverter)
            return Error(ErrorCode::DependencyMissing,
                         QStringLiteral("%1 and has to be rebuilt from %2, but prospero-dds2png.exe was not found. Put it "
                                        "in the toolkit folder or choose it in Settings > Tools.").arg(reason, ddsRel));
        ImageConversion c;
        c.dst = existing ? existing->rel : dst;
        c.ddsRel = ddsRel;
        c.ddsAbs = ddsAbs;
        c.replacedPngRel = existing ? existing->rel : QString();
        c.requirement = req;
        c.reason = reason;
        plan.conversions.push_back(c);
    }
    // Presentation images with the wrong format and no .dds to rebuild them from.
    for (const ScannedFile& f : scan.files) {
        const qsizetype slash = f.rel.indexOf(u'/');
        if (slash < 0 || f.rel.left(slash).compare(QStringLiteral("sce_sys"), Qt::CaseInsensitive) != 0
            || f.rel.indexOf(u'/', slash + 1) >= 0)
            continue;
        const PngRequirement req = presentationRequirement(f.rel.mid(slash + 1));
        if (req == PngRequirement::None || covered.contains(f.rel.toLower()))
            continue;
        const PngInfo info = readPngInfo(f.absPath);
        if (!pngMeets(info, req))
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 is %2, but the Publishing Tools need %3, and there is no .dds copy to "
                                        "rebuild it from. Replace the image.").arg(f.rel, describePng(info), requirementText(req)));
    }
    return plan;
}

// ---- SELF -----------------------------------------------------------------------------

namespace {

struct SelfTail {
    bool isSelf = false;
    bool ps4 = false;
    qint64 size = 0;
    qint64 boundary = 0; // the file size the SELF header names
    qint64 start = 0;    // file offset of tail[0]
    QByteArray tail;     // from 15 bytes before the boundary to the end
    bool haveTail = false;
};

SelfTail readSelfTail(const QString& path)
{
    SelfTail t;
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return t;
    t.size = f.size();
    if (t.size < 0x20)
        return t;
    const QByteArray head = f.read(0x20);
    if (head.size() != 0x20)
        return t;
    t.ps4 = head.startsWith(kPs4SelfMagic);
    t.isSelf = t.ps4 || head.startsWith(kPs5SelfMagic);
    if (!t.isSelf)
        return t;
    const quint64 boundary = qFromLittleEndian<quint64>(head.constData() + 0x10);
    if (boundary < 0x20 || boundary > quint64(t.size))
        return t;
    t.boundary = qint64(boundary);
    t.start = std::max<qint64>(0, t.boundary - 0x0F);
    const qint64 length = t.size - t.start;
    if (length <= 0 || length > kSelfTrailerMaxBytes)
        return t;
    if (!f.seek(t.start))
        return t;
    t.tail = f.read(length);
    t.haveTail = t.tail.size() == length;
    return t;
}

// How many bytes before the boundary the complete trailer starts (0-15), or -1.
int trailerBacktrack(const SelfTail& t)
{
    if (!t.haveTail)
        return -1;
    const qint64 local = t.boundary - t.start;
    for (int back = 0; back <= std::min<qint64>(0x0F, local); ++back)
        if (isCompleteSceVersion(t.tail.mid(local - back)))
            return back;
    return -1;
}

} // namespace

bool isCompleteSceVersion(const QByteArray& r)
{
    // Records: 00 00, u16 payload size, 08, "<name>:", the 8-byte version twice.
    if (r.isEmpty())
        return false;
    qsizetype off = 0;
    int count = 0;
    while (off < r.size()) {
        if (r.size() - off < 5 || r[off] != '\0' || r[off + 1] != '\0')
            return false;
        const qsizetype payload = qFromLittleEndian<quint16>(r.constData() + off + 2);
        const qsizetype record = payload + 4;
        if (payload < 18 || record > r.size() - off || quint8(r[off + 4]) != 8)
            return false;
        const qsizetype nameSize = payload - 17;
        const qsizetype version = off + 5 + nameSize;
        if (r[version - 1] != ':')
            return false;
        for (qsizetype i = off + 5; i < version; ++i)
            if (quint8(r[i]) < 0x20 || quint8(r[i]) > 0x7E)
                return false;
        if (r.mid(version, 8) != r.mid(version + 8, 8))
            return false;
        off += record;
        ++count;
    }
    return count != 0;
}

QString SelfRepairPlan::describe() const
{
    QStringList parts;
    if (rewriteMagic)
        parts << QStringLiteral("PS4 signature replaced by the PS5 signature");
    if (padding > 0)
        parts << QStringLiteral(".sceversion trailer moved by %1 byte(s) to where the header expects it").arg(padding);
    return parts.join(QStringLiteral(", "));
}

std::optional<SelfRepairPlan> selfRepairPlan(const QString& path)
{
    const SelfTail t = readSelfTail(path);
    if (!t.isSelf)
        return std::nullopt;
    const int back = trailerBacktrack(t);
    if (back >= 0) {
        if (t.ps4 || back > 0)
            return SelfRepairPlan{t.ps4, t.boundary - back, back};
        return std::nullopt;
    }
    if (t.ps4)
        return SelfRepairPlan{true, t.size, 0};
    return std::nullopt;
}

std::optional<quint64> executableSdkVersion(const QString& path)
{
    const SelfTail t = readSelfTail(path);
    if (!t.isSelf)
        return std::nullopt;
    const int back = trailerBacktrack(t);
    if (back < 0)
        return std::nullopt;
    const QByteArray r = t.tail.mid(t.boundary - t.start - back);
    quint64 highest = 0;
    for (qsizetype off = 0; off < r.size();) {
        const qsizetype payload = qFromLittleEndian<quint16>(r.constData() + off + 2);
        const qsizetype version = off + 5 + (payload - 17);
        highest = std::max(highest, qFromBigEndian<quint64>(r.constData() + version));
        off += payload + 4;
    }
    return highest & 0xFFFF000000000000ULL;
}

Status writeSelfRepairCopy(const QString& src, const QString& dst, const SelfRepairPlan& plan, const CancellationToken& token)
{
    const QString name = QFileInfo(src).fileName();
    // Read again since planning: only a file that still needs the same correction is copied.
    if (selfRepairPlan(src) != std::optional<SelfRepairPlan>(plan))
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 changed while it was being packed.").arg(name));
    QFile in(toLongPath(src));
    if (!in.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(name), in.errorString());
    if (!QDir().mkpath(QFileInfo(dst).absolutePath()))
        return Error(ErrorCode::IoError, QStringLiteral("Could not fix %1.").arg(name),
                     QStringLiteral("cannot create %1").arg(QFileInfo(dst).absolutePath()));
    QFile out(toLongPath(dst));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("Could not fix %1.").arg(name), out.errorString());
    const auto failed = [&] {
        const QString why = out.error() != QFileDevice::NoError ? out.errorString() : in.errorString();
        out.close();
        QFile::remove(toLongPath(dst));
        return Error(ErrorCode::IoError, QStringLiteral("Could not fix %1.").arg(name), why);
    };
    qint64 position = 0;
    bool padded = plan.padding == 0;
    for (;;) {
        if (!token.checkpoint()) {
            out.close();
            QFile::remove(toLongPath(dst));
            return Error::cancelled();
        }
        if (!padded && position == plan.trailerOffset) {
            if (out.write(QByteArray(plan.padding, '\0')) != plan.padding)
                return failed();
            padded = true;
        }
        qint64 want = 1 << 20;
        if (!padded)
            want = std::min<qint64>(want, plan.trailerOffset - position);
        QByteArray block = in.read(want);
        if (block.isEmpty()) {
            if (in.error() != QFileDevice::NoError || !padded)
                return failed();
            break;
        }
        if (plan.rewriteMagic && position == 0 && block.size() >= 4)
            block.replace(0, 4, kPs5SelfMagic);
        if (out.write(block) != block.size())
            return failed();
        position += block.size();
    }
    // QFile buffers: a failed write (full disk) may only show at the flush.
    if (!out.flush() || out.error() != QFileDevice::NoError)
        return failed();
    out.setFileTime(QFileInfo(src).lastModified(), QFileDevice::FileModificationTime);
    out.close();
    return success();
}

// ---- UCP --------------------------------------------------------------------------------

bool isUcpPath(const QString& rel)
{
    static const QRegularExpression re(QStringLiteral("^sce_sys/(trophy2|uds)/[^/]+\\.ucp$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re.match(rel).hasMatch();
}

namespace {
constexpr quint32 kUcpMagic = 0xB228C60A;
constexpr int kUcpHeader = 0x60;
constexpr int kUcpRecord = 0x40;
constexpr int kUcpDigestAt = 0x1C;

QByteArray ucpDigest(QByteArray copy)
{
    std::fill(copy.begin() + kUcpDigestAt, copy.begin() + kUcpDigestAt + 20, '\0');
    return QCryptographicHash::hash(copy, QCryptographicHash::Sha1);
}
} // namespace

UcpCheck checkUcp(const QByteArray& d)
{
    UcpCheck c;
    if (d.size() < kUcpHeader) {
        c.problem = QStringLiteral("too small to be a UCP archive");
        return c;
    }
    if (qFromBigEndian<quint32>(d.constData()) != kUcpMagic) {
        c.problem = QStringLiteral("not a UCP archive (unknown signature)");
        return c;
    }
    if (qFromBigEndian<quint32>(d.constData() + 4) != 1) {
        c.problem = QStringLiteral("unknown UCP version");
        return c;
    }
    if (qFromBigEndian<quint64>(d.constData() + 8) != quint64(d.size())) {
        c.problem = QStringLiteral("the stored size does not match the file size");
        return c;
    }
    if (qFromBigEndian<quint32>(d.constData() + 0x14) != kUcpRecord) {
        c.problem = QStringLiteral("unknown entry record size");
        return c;
    }
    const qint64 count = qFromBigEndian<quint32>(d.constData() + 0x10);
    const qint64 tableEnd = kUcpHeader + count * kUcpRecord;
    if (tableEnd > d.size()) {
        c.problem = QStringLiteral("the entry table runs past the end of the file");
        return c;
    }
    for (qint64 i = 0; i < count; ++i) {
        const qint64 rec = kUcpHeader + i * kUcpRecord;
        const quint64 off = qFromBigEndian<quint64>(d.constData() + rec + 32);
        const quint64 size = qFromBigEndian<quint64>(d.constData() + rec + 40);
        // Written so that off + size cannot wrap.
        if (off < quint64(tableEnd) || size > quint64(d.size()) || off > quint64(d.size()) - size) {
            c.problem = QStringLiteral("entry %1 lies outside the file").arg(i);
            return c;
        }
    }
    c.structureOk = true;
    c.digestOk = ucpDigest(d) == d.mid(kUcpDigestAt, 20);
    return c;
}

QByteArray withRepairedUcpDigest(const QByteArray& data)
{
    if (data.size() < kUcpHeader)
        return data; // not an archive: nothing to correct
    QByteArray out = data;
    const QByteArray digest = ucpDigest(data);
    std::copy(digest.begin(), digest.end(), out.begin() + kUcpDigestAt);
    return out;
}

// ---- UDS archives named by npbind.dat ------------------------------------------------------

Result<std::vector<UcpMember>> unpackUcp(const QByteArray& d)
{
    const auto bad = [](const QString& why) { return Error(ErrorCode::CorruptData, why); };
    if (d.size() < kUcpHeader)
        return bad(QStringLiteral("%1 bytes, shorter than the header").arg(d.size()));
    if (qFromBigEndian<quint32>(d.constData()) != kUcpMagic)
        return bad(QStringLiteral("unknown signature"));
    if (qFromBigEndian<quint32>(d.constData() + 4) != 1)
        return bad(QStringLiteral("unknown version"));
    if (qFromBigEndian<quint64>(d.constData() + 8) != quint64(d.size()))
        return bad(QStringLiteral("the stored size does not match the file size"));
    if (qFromBigEndian<quint32>(d.constData() + 0x14) != kUcpRecord)
        return bad(QStringLiteral("unknown entry record size"));
    const qint64 count = qFromBigEndian<quint32>(d.constData() + 0x10);
    const qint64 tableEnd = kUcpHeader + count * kUcpRecord;
    if (tableEnd > d.size())
        return bad(QStringLiteral("%1 entries do not fit in the file").arg(count));
    std::vector<UcpMember> members;
    QSet<QString> seen;
    for (qint64 i = 0; i < count; ++i) {
        const qint64 rec = kUcpHeader + i * kUcpRecord;
        QByteArray raw = d.mid(rec, 0x20);
        const qsizetype nul = raw.indexOf('\0');
        if (nul >= 0)
            raw.truncate(nul);
        const QString name = QString::fromLatin1(raw);
        if (name.isEmpty() || seen.contains(name) || name.contains(u'/') || name.contains(u'\\') || name == u"." || name == u"..")
            return bad(QStringLiteral("member %1 has an empty, repeated or unsafe name").arg(i));
        seen.insert(name);
        const quint64 off = qFromBigEndian<quint64>(d.constData() + rec + 0x20);
        const quint64 size = qFromBigEndian<quint64>(d.constData() + rec + 0x28);
        if (off < quint64(tableEnd) || off > quint64(d.size()) || size > quint64(d.size()) - off)
            return bad(QStringLiteral("member %1 runs past the end of the file").arg(name));
        members.push_back({name, d.mid(qsizetype(off), qsizetype(size))});
    }
    return members;
}

QByteArray packUcp(const std::vector<UcpMember>& members)
{
    const qsizetype tableEnd = kUcpHeader + kUcpRecord * qsizetype(members.size());
    std::vector<qsizetype> offsets;
    qsizetype at = tableEnd;
    for (const UcpMember& m : members) {
        offsets.push_back(at);
        // Real archives add a full 16 bytes after a member that is already aligned.
        at += m.data.size() + (16 - m.data.size() % 16);
    }
    QByteArray out(at, '\0');
    qToBigEndian<quint32>(kUcpMagic, out.data());
    qToBigEndian<quint32>(1, out.data() + 4);
    qToBigEndian<quint64>(quint64(at), out.data() + 8);
    qToBigEndian<quint32>(quint32(members.size()), out.data() + 0x10);
    qToBigEndian<quint32>(kUcpRecord, out.data() + 0x14);
    for (size_t i = 0; i < members.size(); ++i) {
        const qsizetype rec = kUcpHeader + qsizetype(i) * kUcpRecord;
        const QByteArray name = members[i].name.toLatin1().left(0x20);
        std::copy(name.begin(), name.end(), out.begin() + rec);
        qToBigEndian<quint64>(quint64(offsets[i]), out.data() + rec + 0x20);
        qToBigEndian<quint64>(quint64(members[i].data.size()), out.data() + rec + 0x28);
        std::copy(members[i].data.begin(), members[i].data.end(), out.begin() + offsets[i]);
    }
    return withRepairedUcpDigest(out);
}

Result<QStringList> readNpbindIds(const QByteArray& d)
{
    const auto bad = [](const QString& why) { return Error(ErrorCode::CorruptData, why); };
    if (d.size() != 0x214 || qFromBigEndian<quint32>(d.constData()) != 0xD294A018u)
        return bad(QStringLiteral("it is not an npbind.dat"));
    if (qFromBigEndian<quint32>(d.constData() + 4) != 1 || qFromBigEndian<quint32>(d.constData() + 0x0C) != quint32(d.size())
        || QCryptographicHash::hash(d.left(0x200), QCryptographicHash::Sha1) != d.mid(0x200, 20))
        return bad(QStringLiteral("its version, size or SHA-1 is not valid"));
    const qint64 entrySize = qFromBigEndian<quint32>(d.constData() + 0x14);
    const qint64 count = qFromBigEndian<quint32>(d.constData() + 0x1C);
    if (entrySize < 4 || 0x80 + count * entrySize > 0x200)
        return bad(QStringLiteral("%1 entries of %2 bytes do not fit").arg(count).arg(entrySize));
    static const QRegularExpression npwr(QStringLiteral("^NPWR[0-9]{5}_[0-9]{2}$"));
    QStringList ids;
    for (qint64 i = 0; i < count; ++i) {
        qint64 at = 0x80 + i * entrySize;
        const qint64 end = at + entrySize;
        bool found = false;
        while (at + 4 <= end) {
            const int tag = qFromBigEndian<quint16>(d.constData() + at);
            const int length = qFromBigEndian<quint16>(d.constData() + at + 2);
            if (tag == 0)
                break;
            if (at + 4 + length > end)
                return bad(QStringLiteral("entry %1 is cut short").arg(i));
            if (tag == 0x10) {
                QByteArray raw = d.mid(at + 4, length);
                const qsizetype nul = raw.indexOf('\0');
                if (nul >= 0)
                    raw.truncate(nul);
                const QString id = QString::fromLatin1(raw);
                if (!npwr.match(id).hasMatch())
                    return bad(QStringLiteral("entry %1 has no valid NPWR id").arg(i));
                ids << id;
                found = true;
                break;
            }
            at += 4 + length;
        }
        if (!found)
            return bad(QStringLiteral("entry %1 has no NPWR id").arg(i));
    }
    return ids;
}

namespace {
const char* const kUdsMembers[] = {"enum_definition.json", "events_definition.json", "stats_definition.json",
                                   "stats_extraction.json"};

// "npCommunicationId" of a definition file: nullopt when the file is not a
// JSON object; an empty string when the key is missing or not a string.
std::optional<QString> communicationIdOf(const QByteArray& body)
{
    auto doc = json::parse(body, {true, false, false, 64});
    if (!doc.ok() || !doc->isObject())
        return std::nullopt;
    const json::Value& id = doc->value(QStringLiteral("npCommunicationId"));
    return id.isString() ? id.toString() : QString();
}
} // namespace

UdsUcpVerdict checkUdsUcp(const QByteArray& data, const QString& npwr)
{
    UdsUcpVerdict v;
    auto members = unpackUcp(data);
    if (!members.ok()) {
        v.problems << members.error().message;
        return v;
    }
    const bool staleDigest = ucpDigest(data) != data.mid(kUcpDigestAt, 20);
    if (staleDigest)
        v.problems << QStringLiteral("the digest is stale");
    QStringList missing;
    for (const char* n : kUdsMembers) {
        const QString name = QLatin1String(n);
        if (std::none_of(members->begin(), members->end(), [&](const UcpMember& m) { return m.name == name; }))
            missing << name;
    }
    if (!missing.isEmpty())
        v.problems << QStringLiteral("it lacks %1").arg(missing.join(QStringLiteral(", ")));
    for (const UcpMember& m : *members) {
        if (m.name != u"enum_definition.json" && m.name != u"stats_definition.json" && m.name != u"stats_extraction.json")
            continue;
        const auto id = communicationIdOf(m.data);
        if (!id)
            v.problems << QStringLiteral("%1 is not valid JSON").arg(m.name);
        else if (*id != npwr)
            v.problems << QStringLiteral("%1 names %2, npbind.dat says %3").arg(m.name, id->isEmpty() ? QStringLiteral("no id") : *id, npwr);
    }
    v.digestOnly = staleDigest && v.problems.size() == 1;
    return v;
}

QByteArray emptyUdsUcp(const QString& npwr)
{
    // The four definition files without entries, written like Python's json.dumps.
    const QByteArray id = npwr.toLatin1();
    const QByteArray base = "{\"schemaVersion\": \"1.1\", \"npCommunicationId\": \"" + id + "\", ";
    return packUcp({
        {QStringLiteral("enum_definition.json"), base + "\"enumDefinitionRevision\": 1, \"enumArray\": []}"},
        {QStringLiteral("events_definition.json"),
         QByteArray("{\"configurationId\": 54474, \"events\": [], \"nextOffset\": 0, \"previousOffset\": 0, \"totalItemCount\": 0}")},
        {QStringLiteral("stats_definition.json"), base + "\"statsDefinitionRevision\": 1, \"statDefinitionArray\": []}"},
        {QStringLiteral("stats_extraction.json"), base + "\"statsExtractionRevision\": 1, \"statsExtractionRuleArray\": []}"},
    });
}

} // namespace fw::fpkg
