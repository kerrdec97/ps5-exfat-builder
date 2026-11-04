#pragma once

// sce_sys/param.json (PS5) reader.
//
// Tolerant: a BOM, comments, trailing commas, NUL padding and invalid UTF-8
// are repaired before parsing (`repaired` says so). The whole object stays
// available as `raw`; the fields the app uses are lifted out.

#include "core/Result.h"
#include "engines/gameinfo/Version.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

#include <optional>

namespace fw::gameinfo {

struct ParamJson {
    QJsonObject raw;
    bool repaired = false;

    QString contentId;
    QString titleId;
    QString contentVersion;      // "01.000.000"
    QString masterVersion;       // "01.00"
    QString version;             // legacy: "version" or else "masterVersion"
    QString defaultLanguage;     // localizedParameters.defaultLanguage
    // (language, titleName) in file order.
    QList<QPair<QString, QString>> localizedTitles;
    QString title;               // legacy resolution, see resolvedTitle()

    QString applicationDrmType;  // "standard", "free", ...
    QString versionFileUri;
    std::optional<qint64> applicationCategoryType;
    std::optional<qint64> attribute;
    std::optional<qint64> attribute2;
    std::optional<qint64> attribute3;

    QString sdkVersionRaw;                    // as written, e.g. "0x0450000000000000"
    QString requiredSystemSoftwareVersionRaw;
    SystemVersion sdkVersion;                 // legacy key order: sdkVersion, sdk_version, SDK_VER,
                                              // requiredSystemSoftwareVersion, systemVersion, SYSTEM_VER
    SystemVersion requiredSystemVersion;      // requiredSystemSoftwareVersion

    // titleName for a language (exact key), empty when absent.
    QString titleFor(const QString& language) const;
};

// Legacy title order: localizedParameters[defaultLanguage] -> en-US -> en-GB ->
// first language (in file order) with a titleName -> top-level titleName ->
// top-level name.
Result<ParamJson> parseParamJson(const QByteArray& data);
Result<ParamJson> readParamJson(const QString& path);

} // namespace fw::gameinfo
