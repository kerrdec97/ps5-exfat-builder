#include "engines/gameinfo/ParamJson.h"

#include "engines/gameinfo/JsonText.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>

#include <cmath>

namespace fw::gameinfo {

namespace {

constexpr qint64 kMaxParamJsonBytes = 8 * 1024 * 1024;

// Python's str(value).strip() for the JSON scalars the legacy code stringified.
QString scalarText(const QJsonValue& v)
{
    if (v.isString())
        return v.toString().trimmed();
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (std::floor(d) == d && std::fabs(d) < 9.0e15)
            return QString::number(qint64(d));
        return QString::number(d);
    }
    if (v.isBool())
        return v.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    return {};
}

std::optional<qint64> integerField(const QJsonObject& o, const char* key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        return qint64(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const QString s = v.toString().trimmed();
        const qint64 n = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? s.mid(2).toLongLong(&ok, 16)
                                                                                 : s.toLongLong(&ok, 10);
        if (ok)
            return n;
    }
    return std::nullopt;
}

// Python truthiness of `data.get(a) or data.get(b) or ''` for strings.
QString firstNonEmpty(const QJsonObject& o, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        const QString s = scalarText(o.value(QLatin1String(k)));
        if (!s.isEmpty())
            return s;
    }
    return {};
}

} // namespace

QString ParamJson::titleFor(const QString& language) const
{
    for (const auto& [lang, t] : localizedTitles)
        if (lang == language)
            return t;
    return {};
}

Result<ParamJson> parseParamJson(const QByteArray& data)
{
    bool changed = false;
    const QByteArray text = json::repair(data, &changed);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return Error(ErrorCode::CorruptData, QStringLiteral("param.json is not valid JSON."),
                     err.error != QJsonParseError::NoError ? err.errorString() + QStringLiteral(" at offset %1").arg(err.offset)
                                                           : QStringLiteral("not an object"));
    ParamJson p;
    p.raw = doc.object();
    p.repaired = changed;
    const QJsonObject& o = p.raw;

    p.contentId = scalarText(o.value(QLatin1String("contentId")));
    p.titleId = scalarText(o.value(QLatin1String("titleId")));
    p.contentVersion = scalarText(o.value(QLatin1String("contentVersion")));
    p.masterVersion = scalarText(o.value(QLatin1String("masterVersion")));
    p.version = firstNonEmpty(o, {"version", "masterVersion"});
    p.applicationDrmType = scalarText(o.value(QLatin1String("applicationDrmType")));
    p.versionFileUri = scalarText(o.value(QLatin1String("versionFileUri")));
    p.applicationCategoryType = integerField(o, "applicationCategoryType");
    p.attribute = integerField(o, "attribute");
    p.attribute2 = integerField(o, "attribute2");
    p.attribute3 = integerField(o, "attribute3");

    // Localized titles, in file order (QJsonObject would sort them).
    const QJsonObject lp = o.value(QLatin1String("localizedParameters")).toObject();
    if (!lp.isEmpty()) {
        p.defaultLanguage = lp.value(QLatin1String("defaultLanguage")).toString();
        auto members = json::membersAt(text, {QStringLiteral("localizedParameters")});
        if (members.ok()) {
            for (const auto& m : members.value()) {
                const QJsonValue v = lp.value(m.key);
                if (v.isObject())
                    p.localizedTitles.append({m.key, v.toObject().value(QLatin1String("titleName")).toString()});
            }
        } else {
            for (auto it = lp.begin(); it != lp.end(); ++it)
                if (it.value().isObject())
                    p.localizedTitles.append({it.key(), it.value().toObject().value(QLatin1String("titleName")).toString()});
        }
        const QString def = p.defaultLanguage.isEmpty() ? QStringLiteral("en-US") : p.defaultLanguage;
        for (const QString& lang : {def, QStringLiteral("en-US"), QStringLiteral("en-GB")}) {
            const QString t = p.titleFor(lang);
            if (!t.isEmpty()) {
                p.title = t;
                break;
            }
        }
        if (p.title.isEmpty())
            for (const auto& [lang, t] : p.localizedTitles)
                if (!t.isEmpty()) {
                    p.title = t;
                    break;
                }
    }
    if (p.title.isEmpty())
        p.title = firstNonEmpty(o, {"titleName", "name"});
    p.title = p.title.trimmed();

    p.sdkVersionRaw = scalarText(o.value(QLatin1String("sdkVersion")));
    p.requiredSystemSoftwareVersionRaw = scalarText(o.value(QLatin1String("requiredSystemSoftwareVersion")));
    for (const char* k : {"sdkVersion", "sdk_version", "SDK_VER", "requiredSystemSoftwareVersion", "systemVersion",
                          "SYSTEM_VER"}) {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isUndefined() || v.isNull())
            continue;
        const SystemVersion sv = parseVersionField(v);
        if (sv.isValid() && sv.majorVersion <= 30) {
            p.sdkVersion = sv;
            break;
        }
    }
    p.requiredSystemVersion = parseVersionField(o.value(QLatin1String("requiredSystemSoftwareVersion")));
    return p;
}

Result<ParamJson> readParamJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("param.json could not be opened."), f.errorString());
    if (f.size() > kMaxParamJsonBytes)
        return Error(ErrorCode::CorruptData, QStringLiteral("param.json is too large to be valid."));
    return parseParamJson(f.readAll());
}

} // namespace fw::gameinfo
