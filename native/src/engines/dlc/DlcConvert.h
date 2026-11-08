#pragma once

// Converting DLC (the ppr-pkg Companion's DLC Converter build and INI export).
//
//  * An INI entry becomes a PS5 DLC "template" package: a param.json made
//    from the target identity (version 01.000.000, masterVersion 01.00) and
//    an optional icon0.png, built as additional content with data.
//  * An imported PS5 DLC package is extracted, its param.json gets the
//    target contentId / titleId / title (primaryId too when it was the old
//    Content ID), license.dat/license.info are dropped so development
//    entitlement records are generated for the target, and it is rebuilt with
//    its own version.
//  * Packages are built by fpkg-cli into a private folder, verified (full
//    structural check, then the identity is read back) and moved to
//    "<ContentId>.pkg" in the output folder; an existing file is never
//    replaced ("-2", "-3" ...).
//  * Updated INIs keep everything but the edited fields and are written as
//    "<input>-<TitleId>-updated.ini" (never over the original).

#include "engines/dlc/DlcSettings.h"
#include "jobs/Job.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <set>
#include <vector>

namespace fw::dlc {

inline const QString kJobConvert = QStringLiteral("dlc.convert");

struct DlcItem {
    QString source;            // .ini or .pkg
    int entry = -1;            // INI entry index; -1 for a package
    QString detectedTitleId;
    QString detectedContentId;
    QString titleId;           // target
    QString contentId;
    QString title;
    QString targetMarket;      // "Americas (US)" when chosen from the Store, else ""
    QString iconPath;          // a user-chosen icon0.png (512x512), "" = keep
    QString version;           // package contentVersion ("" for INI entries)
    QString metadataLine;      // "PS5 PKG (FIH Debug); version 01.000.000; firmware -; DRM ..." for the plan
    qint64 sourceSize = 0;
    qint64 sourceModifiedMs = 0;

    bool isPackage() const { return entry < 0; }
    QJsonObject toJson() const;
    static DlcItem fromJson(const QJsonObject& o);
};

struct DlcConvertRequest {
    std::vector<DlcItem> items;
    QString output;
    QJsonObject toJson() const;
    static Result<DlcConvertRequest> fromJson(const QJsonObject& o);
};

// The file still has the size and modification time recorded in `item`.
bool sourceUnchanged(const DlcItem& item);
// Why `item` cannot be converted to a package ("" = it can).
QString itemProblem(const DlcItem& item, bool iniMode);

// "<dir>/<name>", else "<base>-2<ext>", "-3" ... (a name is taken when the
// file or a folder exists, or `used` already has it). Adds the result to `used`.
QString uniqueOutput(const QString& dir, const QString& name, std::set<QString>& used);

// The text of the review dialog (Build PKG outputs / Save updated INIs).
QString buildPlanText(const std::vector<DlcItem>& items, bool iniMode, const QString& output);

// The Companion's template param.json for an INI entry.
QByteArray templateParamJson(const QString& contentId, const QString& titleId, const QString& title);

// Save updated INIs: one file per (input, target title, publisher prefix).
struct IniExportResult {
    QString path;              // written file ("" when it failed)
    std::vector<int> items;    // indexes into the items vector
    QString error;
};
std::vector<IniExportResult> exportUpdatedInis(const std::vector<DlcItem>& items, const QString& output, QStringList& log);

// Writes DLC-results-<yyyyMMdd-HHmmss>.json (+ .log with `logLines`) into
// `output`. `results[i]` = the final path of items[i] or "" ; `errors[i]`.
QString saveBatchReport(const QString& output, const std::vector<DlcItem>& items, const QStringList& outputs,
                        const QStringList& errors, bool cancelled, const QStringList& logLines);

QStringList convertPhases();
Result<JobOutcome> runDlcConvert(const DlcConvertRequest& request, const DlcSettings& settings, JobContext& ctx);

class DlcConvertJob final : public Job {
public:
    explicit DlcConvertJob(DlcConvertRequest request) : m_request(std::move(request)) {}
    QString kind() const override { return kJobConvert; }
    QString title() const override;
    QString subtitle() const override;
    QStringList phases() const override { return convertPhases(); }
    QJsonObject parameters() const override { return m_request.toJson(); }
    QStringList busyPaths() const override;
    Status validate(const SettingsValues& settings) const override;
    Result<JobOutcome> run(JobContext& ctx) override;

private:
    DlcConvertRequest m_request;
};

} // namespace fw::dlc
