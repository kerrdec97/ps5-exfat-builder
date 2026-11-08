#include "engines/dlc/DlcPackage.h"

#include "core/FileUtil.h"
#include "engines/dlc/DlcIdentity.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>

namespace fw::dlc {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

int parseHex(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("0x([0-9A-Fa-f]+)"));
    const auto m = re.match(s);
    bool ok = false;
    const int v = m.hasMatch() ? m.captured(1).toInt(&ok, 16) : -1;
    return ok ? v : -1;
}

QString valueOf(const std::vector<std::pair<QString, QString>>& kv, const QString& key)
{
    for (const auto& [k, v] : kv)
        if (k.compare(key, Qt::CaseInsensitive) == 0)
            return v;
    return {};
}

bool isDash(const QString& v) { return v.isEmpty() || v == QStringLiteral("—") || v == u"-"; }

// The Companion's rebuild gate, in its order (first match wins).
void applyRebuildGate(DlcPackageInfo& i, bool drmConflict, bool idConflict)
{
    i.canRebuild = false;
    if (!i.isDlc)
        i.rebuildLimitation = QStringLiteral("This package is not an additional-content package. The DLC page only "
                                             "rebuilds DLC; use Build image › FPKG for games and updates.");
    else if (i.official)
        i.rebuildLimitation = QStringLiteral("Official/retail signed packages are inspection-only. This workflow rebuilds "
                                             "supported debug DLC and cannot preserve or recreate official signatures.");
    else if (!i.hasParam)
        i.rebuildLimitation = QStringLiteral("Readable param.json metadata is required. Missing or encrypted metadata makes "
                                             "rebuilding unsupported in this workflow.");
    else if (drmConflict)
        i.rebuildLimitation = QStringLiteral("Original CNT header and param.json DRM declarations disagree. Rebuilding is "
                                             "blocked until the extracted source metadata is reviewed.");
    else if (idConflict)
        i.rebuildLimitation = QStringLiteral("Original header and param.json identities disagree; rebuilding this input is blocked.");
    else if (i.entitlementOnly)
        i.rebuildLimitation = QStringLiteral("Entitlement-only DLC (no data) cannot be rebuilt by the DLC engine, which builds "
                                             "DLC with data. Save an updated INI for it instead.");
    else
        i.canRebuild = true;
    if (i.canRebuild && !validateIdentity(i.titleId, i.contentId).ok()) {
        i.warnings << validateIdentity(i.titleId, i.contentId).error().message;
        i.canRebuild = false;
        i.rebuildLimitation = QStringLiteral("Original package identifiers are malformed or inconsistent. Inspect/export the "
                                             "metadata and correct the extracted sources manually.");
    }
}

} // namespace

DlcPackageInfo parsePkgInfoOutput(const QString& output)
{
    DlcPackageInfo i;
    std::vector<std::pair<QString, QString>> general;
    QString section;
    for (QString line : output.split(u'\n')) {
        line = line.trimmed();
        if (line.startsWith(u'[') && line.endsWith(u']')) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const int colon = line.indexOf(QStringLiteral(": "));
        const int colonEnd = line.endsWith(u':') ? int(line.size()) - 1 : -1;
        const int at = colon > 0 ? colon : colonEnd;
        if (at <= 0)
            continue;
        const QString key = line.left(at).trimmed();
        const QString value = line.mid(at + 1).trimmed();
        if (section == u"General")
            general.emplace_back(key, value);
        else if (section == u"param.json")
            i.metadata.emplace_back(key, value);
    }
    const QString type = valueOf(general, QStringLiteral("Package type"));
    i.type = type.isEmpty() ? QStringLiteral("PS5 PKG") : QStringLiteral("PS5 PKG (%1)").arg(type.section(QStringLiteral(" — "), 0, 0));
    const QString mode = valueOf(general, QStringLiteral("Image mode"));
    i.imageMode = mode.startsWith(QStringLiteral("PLAINTEXT"), Qt::CaseInsensitive) ? QStringLiteral("plaintext")
                : mode.startsWith(QStringLiteral("Native"), Qt::CaseInsensitive)    ? QStringLiteral("native")
                                                                                     : QString();
    i.contentId = valueOf(general, QStringLiteral("Content ID")).section(u' ', 0, 0);
    i.contentType = parseHex(valueOf(general, QStringLiteral("Content type")));
    i.headerDrm = parseHex(valueOf(general, QStringLiteral("DRM")));
    const QString ctype = valueOf(general, QStringLiteral("Content type"));
    i.isDlc = ctype.contains(QStringLiteral("Additional content"), Qt::CaseInsensitive) || type.startsWith(QStringLiteral("DLC"));
    i.entitlementOnly = type.contains(QStringLiteral("entitlement only"), Qt::CaseInsensitive)
                     || ctype.contains(QStringLiteral("no data"), Qt::CaseInsensitive);
    i.official = type.contains(QStringLiteral("Retail"), Qt::CaseInsensitive) || type.contains(QStringLiteral("official"), Qt::CaseInsensitive);
    i.hasParam = !i.metadata.empty();
    const QString pcid = valueOf(i.metadata, QStringLiteral("contentId"));
    bool idConflict = false;
    if (i.contentId.isEmpty())
        i.contentId = pcid;
    else if (!pcid.isEmpty() && pcid != i.contentId) {
        i.warnings << QStringLiteral("Header and param.json Content IDs differ. Rebuilding requires consistent original "
                                     "identity metadata.");
        idConflict = true;
    }
    i.titleId = valueOf(i.metadata, QStringLiteral("titleId"));
    if (i.titleId.isEmpty())
        i.titleId = titleIdOf(i.contentId);
    i.version = valueOf(i.metadata, QStringLiteral("contentVersion"));
    i.masterVersion = valueOf(i.metadata, QStringLiteral("masterVersion"));
    i.firmware = valueOf(i.metadata, QStringLiteral("requiredSystemSoftwareVersion"));
    for (const auto& [k, v] : i.metadata)
        if (k.startsWith(QStringLiteral("titleName")) && !v.isEmpty() && i.title.isEmpty())
            i.title = v;
    if (i.title.isEmpty() && !isDash(valueOf(general, QStringLiteral("Title"))))
        i.title = valueOf(general, QStringLiteral("Title"));
    if (i.title.isEmpty())
        i.title = suffixOf(i.contentId);
    // DRM classification: header 0 = Free, 0x10 = Entitlement.
    const QString declared = valueOf(i.metadata, QStringLiteral("applicationDrmType")).remove(u'_').remove(u'-').toLower();
    QString headerClass;
    if (i.headerDrm == 0) {
        headerClass = QStringLiteral("free");
        i.drm = QStringLiteral("Free (CNT header 0x0)");
    } else if (i.headerDrm == 0x10) {
        headerClass = QStringLiteral("entitlement");
        i.drm = QStringLiteral("Entitlement / additional content (CNT header 0x10)");
    } else {
        i.drm = QStringLiteral("Unknown (CNT header %1)").arg(i.headerDrm < 0 ? QStringLiteral("missing")
                                                                               : QStringLiteral("0x%1").arg(i.headerDrm, 0, 16).toUpper());
        if (i.isDlc)
            i.warnings << QStringLiteral("Additional-content CNT DRM %1 is unsupported or unknown; choose DRM and entitlement "
                                         "key explicitly.").arg(i.headerDrm < 0 ? QStringLiteral("missing") : QString::number(i.headerDrm, 16));
    }
    bool drmConflict = false;
    if (i.isDlc && !declared.isEmpty() && !headerClass.isEmpty()) {
        const QString d = declared == u"free" ? QStringLiteral("free")
                        : (declared == u"additionalcontent" || declared == u"entitlement") ? QStringLiteral("entitlement")
                                                                                          : QString();
        if (d.isEmpty())
            i.warnings << QStringLiteral("Unrecognised param.json applicationDrmType='%1'; the supported CNT header "
                                         "classification is used.").arg(valueOf(i.metadata, QStringLiteral("applicationDrmType")));
        else if (d != headerClass) {
            i.warnings << QStringLiteral("CNT header and param.json DRM declarations conflict: %1; applicationDrmType=%2.")
                              .arg(i.drm, valueOf(i.metadata, QStringLiteral("applicationDrmType")));
            drmConflict = true;
        }
    }
    if (i.entitlementOnly)
        i.warnings << QStringLiteral("Entitlement-only DLC has no inner data image.");
    applyRebuildGate(i, drmConflict, idConflict);
    return i;
}

bool looksLikePs4Package(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray h = f.read(0x80);
    if (h.size() < 0x80 || !h.startsWith(QByteArray("\x7F" "CNT", 4)))
        return false;
    const QString cid = QString::fromLatin1(h.mid(0x40, 36));
    return cid.mid(7, 4) == u"CUSA";
}

Result<DlcPackageInfo> inspectPs4Package(const QString& path, QByteArray* icon)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(native(path)), f.errorString());
    const qint64 len = f.size();
    const QByteArray h = f.read(128);
    if (h.size() < 128)
        return Error(ErrorCode::CorruptData, QStringLiteral("Truncated PS4 header."));
    if (!h.startsWith(QByteArray("\x7F" "CNT", 4)))
        return Error(ErrorCode::CorruptData, QStringLiteral("Not a PS4 package."));
    const auto* p = reinterpret_cast<const uchar*>(h.constData());
    const quint32 count = qFromBigEndian<quint32>(p + 0x10);
    const quint32 table = qFromBigEndian<quint32>(p + 0x18);
    DlcPackageInfo i;
    i.path = QFileInfo(path).absoluteFilePath();
    i.size = len;
    i.modifiedUtc = QFileInfo(path).lastModified().toUTC();
    i.ps4 = true;
    i.type = QStringLiteral("PS4 PKG");
    QByteArray cidBytes = h.mid(0x40, 48);
    if (const int z = cidBytes.indexOf('\0'); z >= 0)
        cidBytes.truncate(z);
    i.contentId = QString::fromLatin1(cidBytes);
    i.headerDrm = int(qFromBigEndian<quint32>(p + 0x70));
    i.contentType = int(qFromBigEndian<quint32>(p + 0x74));
    i.isDlc = i.contentType == 0x1B || i.contentType == 0x1C;
    i.metadata.emplace_back(QStringLiteral("headerContentType"), QStringLiteral("0x%1").arg(i.contentType, 0, 16).toUpper());
    i.metadata.emplace_back(QStringLiteral("headerDrmType"), QStringLiteral("0x%1").arg(i.headerDrm, 0, 16).toUpper());
    i.drm = QStringLiteral("PS4 header DRM 0x%1").arg(i.headerDrm, 0, 16).toUpper();
    if (count > 100000)
        return Error(ErrorCode::CorruptData, QStringLiteral("Implausible PS4 entry count."));
    if (qint64(table) + qint64(count) * 32 > len)
        return Error(ErrorCode::CorruptData, QStringLiteral("PS4 entry table is outside the package bounds."));
    f.seek(table);
    const QByteArray entries = f.read(qint64(count) * 32);
    QByteArray sfo;
    for (quint32 n = 0; n < count; ++n) {
        const auto* e = reinterpret_cast<const uchar*>(entries.constData() + n * 32);
        const quint32 id = qFromBigEndian<quint32>(e);
        const quint32 flags = qFromBigEndian<quint32>(e + 8);
        const quint32 off = qFromBigEndian<quint32>(e + 16);
        const quint32 size = qFromBigEndian<quint32>(e + 20);
        if (id != 0x1000 && id != 0x1200)
            continue;
        if (qint64(off) + size > len)
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("PS4 entry 0x%1 is outside the package bounds.").arg(id, 0, 16).toUpper());
        if ((flags & 0x80000000u) || size == 0 || size > 64u * 1024 * 1024)
            continue;
        f.seek(off);
        const QByteArray data = f.read(size);
        if (data.size() != qint64(size))
            return Error(ErrorCode::CorruptData, QStringLiteral("Truncated PS4 entry 0x%1.").arg(id, 0, 16).toUpper());
        if (id == 0x1000)
            sfo = data;
        else if (icon)
            *icon = data;
    }
    if (sfo.size() >= 20 && sfo.startsWith(QByteArray("\0PSF", 4))) {
        const auto* s = reinterpret_cast<const uchar*>(sfo.constData());
        const quint32 keyTable = qFromLittleEndian<quint32>(s + 8);
        const quint32 dataTable = qFromLittleEndian<quint32>(s + 12);
        const quint32 n = qFromLittleEndian<quint32>(s + 16);
        if (keyTable > quint32(sfo.size()) || dataTable > quint32(sfo.size()) || 20 + qint64(n) * 16 > sfo.size())
            return Error(ErrorCode::CorruptData, QStringLiteral("Invalid param.sfo table bounds."));
        for (quint32 k = 0; k < n; ++k) {
            const auto* e = s + 20 + k * 16;
            const quint16 keyOff = qFromLittleEndian<quint16>(e);
            const quint16 fmt = qFromLittleEndian<quint16>(e + 2);
            const quint32 dlen = qFromLittleEndian<quint32>(e + 4);
            const quint32 doff = qFromLittleEndian<quint32>(e + 12);
            const qint64 ks = qint64(keyTable) + keyOff;
            if (ks >= sfo.size() || qint64(dataTable) + doff + dlen > sfo.size())
                return Error(ErrorCode::CorruptData, QStringLiteral("Invalid param.sfo entry bounds."));
            const int ke = sfo.indexOf('\0', ks);
            if (ke < 0)
                return Error(ErrorCode::CorruptData, QStringLiteral("Invalid or unterminated param.sfo key."));
            const QString key = QString::fromUtf8(sfo.mid(ks, ke - ks));
            const QByteArray val = sfo.mid(dataTable + doff, dlen);
            QString text;
            if (fmt == 0x0404) {
                if (val.size() < 4)
                    return Error(ErrorCode::CorruptData, QStringLiteral("Truncated param.sfo integer value."));
                text = QStringLiteral("%1").arg(qFromLittleEndian<quint32>(val.constData()), 8, 16, QLatin1Char('0')).toUpper();
            } else {
                QByteArray t = val;
                while (t.endsWith('\0'))
                    t.chop(1);
                text = QString::fromUtf8(t);
            }
            if (!valueOf(i.metadata, key).isEmpty())
                return Error(ErrorCode::CorruptData, QStringLiteral("Duplicate param.sfo key: %1").arg(key));
            i.metadata.emplace_back(key, text);
        }
        i.hasParam = true;
        i.title = valueOf(i.metadata, QStringLiteral("TITLE"));
        i.titleId = valueOf(i.metadata, QStringLiteral("TITLE_ID"));
        i.version = valueOf(i.metadata, QStringLiteral("VERSION"));
        i.masterVersion = valueOf(i.metadata, QStringLiteral("APP_VER"));
        i.firmware = valueOf(i.metadata, QStringLiteral("SYSTEM_VER"));
    } else {
        i.warnings << QStringLiteral("param.sfo is missing, encrypted or too large; only header metadata is available.");
    }
    if (i.titleId.isEmpty())
        i.titleId = titleIdOf(i.contentId);
    if (i.title.isEmpty())
        i.title = suffixOf(i.contentId);
    i.canRebuild = false;
    i.rebuildLimitation = QStringLiteral("PS4 PKG inspection is supported. This PS5 builder cannot rebuild PS4 packages; export "
                                         "a corrected INI and use a separate PS4 builder with extracted source files.");
    return i;
}

Result<DlcPackageInfo> inspectDlcPackage(const FpkgCli& cli, const QString& path, const QString& passcode,
                                         const CancellationToken& token)
{
    const QFileInfo fi(path);
    if (!fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("DLC package does not exist."));
    if (looksLikePs4Package(path))
        return inspectPs4Package(path);
    QStringList args{QStringLiteral("pkg-info"), native(fi.absoluteFilePath())};
    if (!passcode.isEmpty())
        args << QStringLiteral("--passcode") << passcode;
    auto run = runFpkgCli(cli, args, QDir::tempPath(), token, {}, 120000);
    if (!run.ok())
        return run.takeError();
    if (run->cancelled)
        return Error::cancelled();
    if (!run->ok())
        return Error(ErrorCode::ExternalToolFailed, run->failureText());
    DlcPackageInfo i = parsePkgInfoOutput(run->output);
    i.path = fi.absoluteFilePath();
    i.size = fi.size();
    i.modifiedUtc = fi.lastModified().toUTC();
    if (i.type.contains(QStringLiteral("CNT")) && !i.entitlementOnly && !i.isDlc)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("The selected file is a metadata container, not a complete PS5 package."));
    return i;
}

Result<QString> extractDlcPackage(const FpkgCli& cli, const DlcPackageInfo& info, const QString& destination,
                                  const QString& passcode, const QString& tempDir, int priorityMode,
                                  const CancellationToken& token, const std::function<void(const QString&)>& onLine)
{
    if (!info.canRebuild)
        return Error(ErrorCode::Unsupported, info.rebuildLimitation);
    const QFileInfo now(info.path);
    if (!now.isFile() || now.size() != info.size || now.lastModified().toUTC() != info.modifiedUtc)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The input package changed after import. Reload it before rebuilding."));
    if (QFileInfo(destination).isFile())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The extraction destination is a file."));
    if (QFileInfo::exists(destination)
        && !QDir(destination).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The extraction folder must be empty and cannot be a link or junction."));
    QDir().mkpath(destination);
    QDir().mkpath(tempDir);
    const QStringList args{QStringLiteral("pkg-extract"), native(info.path), QStringLiteral("--output"), native(destination),
                           QStringLiteral("--passcode"), passcode, QStringLiteral("--temp"), native(tempDir)};
    auto run = runFpkgCli(cli, args, tempDir, token, onLine, 0, priorityMode);
    if (!run.ok())
        return run.takeError();
    if (run->cancelled)
        return Error::cancelled();
    if (!run->ok())
        return Error(ErrorCode::ExternalToolFailed,
                     QStringLiteral("The source package failed structural/authentication checks; check the source passcode "
                                    "and input integrity: %1").arg(run->failureText()));
    QString root = destination;
    if (!QFileInfo(QDir(root).filePath(QStringLiteral("sce_sys/param.json"))).isFile()) {
        QDirIterator it(destination, {QStringLiteral("param.json")}, QDir::Files, QDirIterator::Subdirectories);
        root.clear();
        while (it.hasNext()) {
            const QFileInfo pj(it.next());
            if (pj.absoluteDir().dirName().compare(QStringLiteral("sce_sys"), Qt::CaseInsensitive) == 0) {
                root = QFileInfo(pj.absolutePath()).absolutePath();
                break;
            }
        }
    }
    if (root.isEmpty())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("Extraction did not produce sce_sys/param.json. No rebuilt output was created."));
    return root;
}

QByteArray extractDlcIcon(const FpkgCli& cli, const QString& path, const QString& passcode, const QString& tempDir,
                          const CancellationToken& token)
{
    if (looksLikePs4Package(path)) {
        QByteArray icon;
        (void)inspectPs4Package(path, &icon);
        return icon;
    }
    const QString dest = QDir(tempDir).filePath(QStringLiteral("icon-%1").arg(qHash(path), 0, 16));
    removeTreeNoFollow(dest);
    QDir().mkpath(dest);
    const QStringList args{QStringLiteral("pkg-extract"), native(path), QStringLiteral("--output"), native(dest),
                           QStringLiteral("--cnt"), QStringLiteral("--include"), QStringLiteral("sce_sys/icon0.png"),
                           QStringLiteral("--passcode"), passcode, QStringLiteral("--temp"), native(tempDir)};
    auto run = runFpkgCli(cli, args, tempDir, token, {}, 120000);
    QByteArray icon;
    if (run.ok()) {
        QDirIterator it(dest, {QStringLiteral("icon0.png")}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            QFile f(it.next());
            if (f.open(QIODevice::ReadOnly) && f.size() < 8 * 1024 * 1024)
                icon = f.readAll();
        }
    }
    removeTreeNoFollow(dest);
    return icon;
}

} // namespace fw::dlc
