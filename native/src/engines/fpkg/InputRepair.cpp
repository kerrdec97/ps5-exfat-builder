#include "engines/fpkg/InputRepair.h"

#include "core/FileUtil.h"

#include <QCryptographicHash>
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

Status writeSelfRepairCopy(const QString& src, const QString& dst, const CancellationToken& token)
{
    FW_TRY(copyFileStreaming(src, dst, token, nullptr, true));
    QFile f(toLongPath(dst));
    if (!f.open(QIODevice::ReadWrite))
        return Error(ErrorCode::IoError, QStringLiteral("Could not fix %1.").arg(QFileInfo(src).fileName()), f.errorString());
    const QByteArray head = f.read(4);
    if (head != kPs4SelfMagic)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 changed while it was being copied.").arg(QFileInfo(src).fileName()));
    f.seek(0);
    if (f.write(kPs5SelfMagic) != 4)
        return Error(ErrorCode::IoError, QStringLiteral("Could not fix %1.").arg(QFileInfo(src).fileName()), f.errorString());
    f.flush();
    f.setFileTime(QFileInfo(src).lastModified(), QFileDevice::FileModificationTime);
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
        if (off < quint64(tableEnd) || off + size > quint64(d.size())) {
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
    QByteArray out = data;
    const QByteArray digest = ucpDigest(data);
    std::copy(digest.begin(), digest.end(), out.begin() + kUcpDigestAt);
    return out;
}

} // namespace fw::fpkg
