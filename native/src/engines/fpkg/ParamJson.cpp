#include "engines/fpkg/ParamJson.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace fw::fpkg {

Result<json::Value> parseParamJson(const QByteArray& bytes)
{
    json::ParseOptions opt;
    opt.allowBom = true;
    opt.allowTrailingCommas = true;
    opt.allowComments = true;
    auto doc = json::parse(bytes, opt);
    if (!doc.ok()) {
        Error e = doc.takeError();
        return Error(ErrorCode::CorruptData, QStringLiteral("sce_sys/param.json is not valid JSON. %1").arg(e.message),
                     e.detail);
    }
    if (!doc->isObject())
        return Error(ErrorCode::CorruptData, QStringLiteral("sce_sys/param.json does not contain a JSON object."));
    return std::move(doc).value();
}

Result<json::Value> readParamJson(const QString& path)
{
    QFile f(path);
    if (!f.exists())
        return Error(ErrorCode::NotFound, QStringLiteral("sce_sys/param.json was not found in the game folder."), path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("sce_sys/param.json could not be read."), f.errorString());
    if (f.size() > 16 * 1024 * 1024)
        return Error(ErrorCode::CorruptData, QStringLiteral("sce_sys/param.json is too large to be a param.json."));
    return parseParamJson(f.readAll());
}

QString paramDefaultLanguage(const json::Value& param)
{
    const QString lang = param.path(QStringLiteral("localizedParameters.defaultLanguage")).toString();
    return lang.isEmpty() ? QStringLiteral("en-US") : lang;
}

QString paramTitle(const json::Value& param)
{
    const json::Value& loc = param.value(QStringLiteral("localizedParameters"));
    if (!loc.isObject())
        return {};
    return loc.value(paramDefaultLanguage(param)).value(QStringLiteral("titleName")).toString();
}

std::optional<quint64> parseHexOrNumber(const json::Value& v)
{
    if (v.isString()) {
        QString s = v.toString().trimmed();
        bool ok = false;
        quint64 n = 0;
        if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            n = s.mid(2).toULongLong(&ok, 16);
        else
            n = s.toULongLong(&ok, 10);
        if (ok)
            return n;
        return std::nullopt;
    }
    if (v.isNumber()) {
        bool ok = false;
        const quint64 n = v.numberText().toULongLong(&ok);
        if (ok)
            return n;
    }
    return std::nullopt;
}

namespace {
QString describe(const json::Value& v)
{
    if (!v.isString() && v.isNull())
        return QStringLiteral("missing");
    return QString::fromUtf8(json::write(v, {-1, false, false}));
}
} // namespace

QStringList applyParamPatch(json::Value& p, const ParamPatchOptions& o)
{
    QStringList notes;
    const QString drmKey = QStringLiteral("applicationDrmType");
    const json::Value& drm = p.value(drmKey);
    if (!(drm.isString() && drm.toString() == u"standard")) {
        notes << QStringLiteral("applicationDrmType %1 -> \"standard\"").arg(p.contains(drmKey) ? describe(drm) : QStringLiteral("missing"));
        p.insert(drmKey, json::Value(QStringLiteral("standard")));
    }

    if (o.clearVersionFileUri) {
        const QString key = QStringLiteral("versionFileUri");
        const json::Value& uri = p.value(key);
        if (uri.isString() && !uri.toString().isEmpty()) {
            notes << QStringLiteral("versionFileUri cleared (was %1)").arg(uri.toString());
            p.insert(key, json::Value(QString()));
        }
    }

    if (o.clearAttribute3) {
        const QString key = QStringLiteral("attribute3");
        const json::Value& a3 = p.value(key);
        if (a3.isNumber() && a3.toDouble() != 0.0) {
            notes << QStringLiteral("attribute3 %1 -> 0").arg(a3.numberText());
            p.insert(key, json::Value(0));
        }
    }

    if (o.lowerRequiredFirmware) {
        const QString reqKey = QStringLiteral("requiredSystemSoftwareVersion");
        const auto req = parseHexOrNumber(p.value(reqKey));
        const auto sdk = parseHexOrNumber(p.value(QStringLiteral("sdkVersion")));
        if (req && sdk) {
            const quint64 sdkFw = *sdk & 0xFFFF000000000000ULL;
            if (sdkFw != 0 && *req > sdkFw) {
                const QString text = QStringLiteral("0x%1").arg(sdkFw, 16, 16, QLatin1Char('0'));
                notes << QStringLiteral("requiredSystemSoftwareVersion %1 -> %2 (the firmware of sdkVersion)")
                             .arg(describe(p.value(reqKey)), text);
                p.insert(reqKey, json::Value(text));
            }
        }
    }

    if (!o.contentVersion.isEmpty()) {
        const QString key = QStringLiteral("contentVersion");
        if (p.value(key).toString() != o.contentVersion) {
            notes << QStringLiteral("contentVersion %1 -> %2").arg(describe(p.value(key)), o.contentVersion);
            p.insert(key, json::Value(o.contentVersion));
        }
    }

    if (!o.title.isEmpty()) {
        const QString lang = paramDefaultLanguage(p);
        json::Value& loc = p[QStringLiteral("localizedParameters")];
        if (!loc.isObject())
            loc = json::Value::object();
        if (!loc.contains(QStringLiteral("defaultLanguage")))
            loc.insert(QStringLiteral("defaultLanguage"), json::Value(lang));
        json::Value& entry = loc[lang];
        if (!entry.isObject())
            entry = json::Value::object();
        if (entry.value(QStringLiteral("titleName")).toString() != o.title) {
            notes << QStringLiteral("localizedParameters.%1.titleName -> \"%2\"").arg(lang, o.title);
            entry.insert(QStringLiteral("titleName"), json::Value(o.title));
        }
    }
    return notes;
}

qint64 estimatePackageBytes(qint64 u, qint64 f)
{
    const qint64 onePercent = (u + 99) / 100;
    return u + std::max<qint64>(512LL * 1024 * 1024, onePercent) + 4096 * f;
}

SizeClassChoice chooseSizeClass(qint64 estimate, int level)
{
    SizeClassChoice c;
    if (level == 2 && estimate > kMountLevel2Max)
        level = 1;
    if (level == 1 && estimate > kSizeClass1Max)
        level = 0;
    c.mountLevel = level;
    if (level == 2 || estimate <= kSizeClass0Max)
        c.attributePub = 0;
    else if (estimate <= kSizeClass1Max)
        c.attributePub = 1;
    else
        c.attributePub = 2;
    return c;
}

namespace {
QString gb(qint64 b) { return QString::number(double(b) / 1e9, 'f', 1) + QStringLiteral(" GB"); }

// Integer literal 0, 1 or 2 (not 1.0, true or "1").
std::optional<int> mountLevelOf(const json::Value& v)
{
    if (!v.isNumber())
        return std::nullopt;
    const QString t = v.numberText();
    if (t == u"0") return 0;
    if (t == u"1") return 1;
    if (t == u"2") return 2;
    return std::nullopt;
}
} // namespace

Result<SizeClassDecision> applyPackageSizeClass(json::Value& p, qint64 u, qint64 f)
{
    SizeClassDecision d;
    d.estimate = estimatePackageBytes(u, f);

    const QString catKey = QStringLiteral("applicationCategoryType");
    if (!p.contains(catKey))
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("param.json has no applicationCategoryType, so the package size class cannot be "
                                    "chosen. Fix param.json or turn off \"Choose the package size class\"."));
    const json::Value& cat = p.value(catKey);
    if (!cat.isNumber() || cat.numberText().contains(u'.') || cat.numberText().contains(u'e', Qt::CaseInsensitive))
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("applicationCategoryType in param.json is not a whole number (%1).")
                         .arg(QString::fromUtf8(json::write(cat, {-1}))));
    if (cat.toInt() != 0) {
        d.warnings << QStringLiteral("applicationCategoryType is %1, not a game application: the package size class "
                                     "was left unchanged.").arg(cat.numberText());
        d.notes << d.warnings.last();
        return d;
    }

    const QString kernelKey = QStringLiteral("kernel");
    int level = 0;
    if (p.contains(kernelKey)) {
        const json::Value& kernel = p.value(kernelKey);
        if (!kernel.isObject())
            return Error(ErrorCode::CorruptData, QStringLiteral("\"kernel\" in param.json is not an object."));
        if (kernel.contains(QStringLiteral("addcontMountLevel"))) {
            const auto lv = mountLevelOf(kernel.value(QStringLiteral("addcontMountLevel")));
            if (!lv)
                return Error(ErrorCode::CorruptData,
                             QStringLiteral("kernel.addcontMountLevel in param.json must be 0, 1 or 2 (found %1).")
                                 .arg(QString::fromUtf8(json::write(kernel.value(QStringLiteral("addcontMountLevel")), {-1}))));
            level = *lv;
        }
    }
    d.mountLevelBefore = level;
    const SizeClassChoice choice = chooseSizeClass(d.estimate, level);
    d.mountLevelAfter = choice.mountLevel;
    const int cls = choice.attributePub;
    if (d.estimate > kSizeClass2Max)
        d.warnings << QStringLiteral("The estimated package size (%1) is above the largest size class the Publishing "
                                     "Tools 2.79 know (%2). img_create decides whether it can be built.")
                          .arg(gb(d.estimate), gb(kSizeClass2Max));
    if (cls == 2 && f > kLv2MaxFiles)
        return Error(ErrorCode::Unsupported,
                     QStringLiteral("The game has %1 files; packages of this size allow at most %2.")
                         .arg(f).arg(kLv2MaxFiles));
    d.attributePub = cls;
    d.applied = true;

    p.insert(QStringLiteral("attributePub"), json::Value(cls));
    d.notes << QStringLiteral("size: estimated %1 bytes (%2) for %3 files -> attributePub %4").arg(d.estimate).arg(gb(d.estimate)).arg(f).arg(cls);
    if (d.mountLevelAfter != d.mountLevelBefore) {
        p[kernelKey].insert(QStringLiteral("addcontMountLevel"), json::Value(d.mountLevelAfter));
        d.warnings << QStringLiteral("kernel.addcontMountLevel %1 -> %2 for this package size. DLC installed as separate "
                                     "packages may not mount.").arg(d.mountLevelBefore).arg(d.mountLevelAfter);
        d.notes << d.warnings.last();
    }
    if (p.value(kernelKey).isObject() && p.value(kernelKey).contains(QStringLiteral("appSizeInGib"))) {
        p[kernelKey].remove(QStringLiteral("appSizeInGib"));
        d.notes << QStringLiteral("kernel.appSizeInGib removed (only used by a larger size class of newer SDKs)");
    }
    return d;
}

QByteArray toCrlf(const QByteArray& lf)
{
    QByteArray out;
    out.reserve(lf.size() + lf.count('\n'));
    for (const char c : lf) {
        if (c == '\n')
            out += '\r';
        out += c;
    }
    return out;
}

QByteArray serializeParamJson(const json::Value& param)
{
    return toCrlf(json::write(param, {2, false, true, /*pythonFloats=*/true}));
}

} // namespace fw::fpkg
