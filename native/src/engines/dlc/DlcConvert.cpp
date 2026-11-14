#include "engines/dlc/DlcConvert.h"

#include "core/FileUtil.h"
#include "core/OrderedJson.h"
#include "engines/dlc/DlcIdentity.h"
#include "engines/dlc/DlcIni.h"
#include "engines/dlc/DlcPackage.h"
#include "engines/dlc/FpkgCli.h"
#include "engines/fpkg/ParamJson.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <map>

namespace fw::dlc {

namespace {

QString native(const QString& p) { return QDir::toNativeSeparators(p); }

// System.Text.Json's default string encoding: quotes, and \uXXXX for non-ASCII,
// control characters and the HTML-sensitive characters.
QString jsonString(const QString& s)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u == '\\')
            out += QStringLiteral("\\\\");
        else if (u == '\n')
            out += QStringLiteral("\\n");
        else if (u == '\r')
            out += QStringLiteral("\\r");
        else if (u == '\t')
            out += QStringLiteral("\\t");
        else if (u == '\b')
            out += QStringLiteral("\\b");
        else if (u == '\f')
            out += QStringLiteral("\\f");
        else if (u < 0x20 || u > 0x7E || u == '"' || u == '<' || u == '>' || u == '&' || u == '\'' || u == '+' || u == '`')
            out += QStringLiteral("\\u%1").arg(u, 4, 16, QLatin1Char('0')).toUpper().replace(QStringLiteral("\\U"), QStringLiteral("\\u"));
        else
            out += c;
    }
    out += u'"';
    return out;
}

QString titleOr(const DlcItem& i)
{
    if (!i.title.trimmed().isEmpty())
        return i.title.trimmed();
    const int dash = i.contentId.lastIndexOf(u'-');
    return dash >= 0 ? i.contentId.mid(dash + 1) : i.contentId;
}

// UpdateParam: target identity into an extracted param.json.
Status updateParam(const QString& sourceRoot, const DlcItem& item)
{
    const QString path = QDir(sourceRoot).filePath(QStringLiteral("sce_sys/param.json"));
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("The extracted param.json could not be read."));
    auto doc = fpkg::parseParamJson(f.readAll());
    f.close();
    if (!doc.ok() || !doc->isObject())
        return Error(ErrorCode::CorruptData, QStringLiteral("Extracted param.json is not a JSON object."));
    json::Value p = doc.value();
    const QString oldCid = p.value(QStringLiteral("contentId")).toString();
    p.insert(QStringLiteral("contentId"), json::Value(item.contentId));
    p.insert(QStringLiteral("titleId"), json::Value(item.titleId));
    const json::Value& primary = p.value(QStringLiteral("primaryId"));
    if (primary.isString() && primary.toString() == oldCid)
        p.insert(QStringLiteral("primaryId"), json::Value(item.contentId));
    if (p.contains(QStringLiteral("localizedParameters")) && p.value(QStringLiteral("localizedParameters")).isObject()) {
        json::Value& lp = p[QStringLiteral("localizedParameters")];
        const json::Value& dl = lp.value(QStringLiteral("defaultLanguage"));
        const QString lang = dl.isString() && !dl.toString().isEmpty() ? dl.toString() : QStringLiteral("en-US");
        if (!lp.value(lang).isObject())
            lp.insert(lang, json::Value::object());
        lp[lang].insert(QStringLiteral("titleName"), json::Value(titleOr(item)));
    }
    json::WriteOptions wo;
    wo.indent = 2;
    wo.ensureAscii = true;
    FW_TRY(writeFileAtomic(path, json::write(p, wo)));
    auto again = fpkg::readParamJson(path);
    if (!again.ok() || again->value(QStringLiteral("contentId")).toString() != item.contentId
        || again->value(QStringLiteral("titleId")).toString() != item.titleId)
        return Error(ErrorCode::VerificationFailed, QStringLiteral("Updated param.json failed identity validation."));
    return success();
}

QString findPackage(const QString& dir)
{
    QDirIterator it(dir, {QStringLiteral("*.pkg")}, QDir::Files);
    QString best;
    while (it.hasNext()) {
        const QString p = it.next();
        if (best.isEmpty() || QFileInfo(p).size() > QFileInfo(best).size())
            best = p;
    }
    return best;
}

QString value(const std::vector<std::pair<QString, QString>>& kv, const QString& key)
{
    for (const auto& [k, v] : kv)
        if (k.compare(key, Qt::CaseInsensitive) == 0)
            return v;
    return {};
}

} // namespace

// ---- items ------------------------------------------------------------------------------------------

QJsonObject DlcItem::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("source"), source);
    o.insert(QStringLiteral("entry"), entry);
    o.insert(QStringLiteral("detectedTitleId"), detectedTitleId);
    o.insert(QStringLiteral("detectedContentId"), detectedContentId);
    o.insert(QStringLiteral("titleId"), titleId);
    o.insert(QStringLiteral("contentId"), contentId);
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("targetMarket"), targetMarket);
    o.insert(QStringLiteral("icon"), iconPath);
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("metadata"), metadataLine);
    o.insert(QStringLiteral("sourceSize"), double(sourceSize));
    o.insert(QStringLiteral("sourceModifiedMs"), double(sourceModifiedMs));
    return o;
}

DlcItem DlcItem::fromJson(const QJsonObject& o)
{
    DlcItem i;
    i.source = o.value(QStringLiteral("source")).toString();
    i.entry = o.value(QStringLiteral("entry")).toInt(-1);
    i.detectedTitleId = o.value(QStringLiteral("detectedTitleId")).toString();
    i.detectedContentId = o.value(QStringLiteral("detectedContentId")).toString();
    i.titleId = o.value(QStringLiteral("titleId")).toString().trimmed().toUpper();
    i.contentId = o.value(QStringLiteral("contentId")).toString().trimmed().toUpper();
    i.title = o.value(QStringLiteral("title")).toString().trimmed();
    i.targetMarket = o.value(QStringLiteral("targetMarket")).toString();
    i.iconPath = o.value(QStringLiteral("icon")).toString();
    i.version = o.value(QStringLiteral("version")).toString();
    i.metadataLine = o.value(QStringLiteral("metadata")).toString();
    i.sourceSize = qint64(o.value(QStringLiteral("sourceSize")).toDouble());
    i.sourceModifiedMs = qint64(o.value(QStringLiteral("sourceModifiedMs")).toDouble());
    return i;
}

QJsonObject DlcConvertRequest::toJson() const
{
    QJsonArray a;
    for (const DlcItem& i : items)
        a.append(i.toJson());
    QJsonObject o;
    o.insert(QStringLiteral("items"), a);
    o.insert(QStringLiteral("output"), output);
    return o;
}

Result<DlcConvertRequest> DlcConvertRequest::fromJson(const QJsonObject& o)
{
    DlcConvertRequest r;
    for (const QJsonValue& v : o.value(QStringLiteral("items")).toArray())
        r.items.push_back(DlcItem::fromJson(v.toObject()));
    r.output = o.value(QStringLiteral("output")).toString().trimmed();
    if (r.items.empty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Add DLC INIs or PKGs first."));
    if (r.output.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Select the output folder."));
    return r;
}

bool sourceUnchanged(const DlcItem& i)
{
    const QFileInfo fi(i.source);
    return fi.isFile() && fi.size() == i.sourceSize && fi.lastModified().toMSecsSinceEpoch() == i.sourceModifiedMs;
}

QString itemProblem(const DlcItem& i, bool iniMode)
{
    if (Status st = validateIdentity(i.titleId, i.contentId); !st.ok())
        return st.error().message;
    if (!sourceUnchanged(i))
        return QStringLiteral("Input changed or moved. Clear the list and import it again.");
    if (!iniMode && !isPs5TitleId(i.titleId))
        return QStringLiteral("PS4 INI can be exported; this builder supports PS5 PKG generation only.");
    if (!i.detectedContentId.isEmpty() && isContentId(i.detectedContentId)
        && i.detectedContentId.mid(7, 4) != i.contentId.mid(7, 4))
        return QStringLiteral("A PS4 DLC cannot be converted into PS5 DLC, or vice versa.");
    return {};
}

QString uniqueOutput(const QString& dir, const QString& name, std::set<QString>& used)
{
    const QFileInfo fi(QDir(dir).filePath(name));
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    QString cand = fi.absoluteFilePath();
    for (int n = 2; QFileInfo::exists(cand) || used.count(cand.toLower()); ++n)
        cand = QDir(dir).filePath(QStringLiteral("%1-%2%3").arg(base).arg(n).arg(ext));
    used.insert(cand.toLower());
    return cand;
}

QByteArray templateParamJson(const QString& cid, const QString& tid, const QString& title)
{
    const QString s = QStringLiteral("{\r\n    \"contentVersion\": \"01.000.000\",\r\n    \"contentId\": %1,\r\n    \"titleId\": "
                                     "%2,\r\n    \"localizedParameters\": {\r\n        \"defaultLanguage\": \"en-US\",\r\n        "
                                     "\"en-US\": {\r\n            \"titleName\": %3\r\n        }\r\n    },\r\n    "
                                     "\"masterVersion\": \"01.00\"\r\n}")
                          .arg(jsonString(cid), jsonString(tid), jsonString(title));
    return s.toUtf8();
}

namespace {

// Which exported INI each item goes into (-1: blocked, none). Items of one
// input, title ID and publisher prefix share a file; the same INI entry with a
// second target gets a file of its own. The review text and the export both
// use this, so they name the same files.
std::vector<int> iniGroups(const std::vector<DlcItem>& items)
{
    std::vector<int> group(items.size(), -1);
    std::map<QString, int> byKey;
    int next = 0;
    for (size_t n = 0; n < items.size(); ++n) {
        const DlcItem& i = items[n];
        if (!itemProblem(i, true).isEmpty())
            continue;
        QString key = QStringLiteral("%1|%2|%3").arg(i.source.toLower(), i.titleId, i.contentId.left(6));
        if (auto it = byKey.find(key); it != byKey.end())
            for (size_t other = 0; other < n; ++other)
                if (group[other] == it->second && items[other].entry == i.entry
                    && (items[other].contentId != i.contentId || items[other].title != i.title)) {
                    key += QStringLiteral("|%1").arg(n);
                    break;
                }
        auto it = byKey.find(key);
        if (it == byKey.end())
            it = byKey.emplace(key, next++).first;
        group[n] = it->second;
    }
    return group;
}

} // namespace

QString buildPlanText(const std::vector<DlcItem>& items, bool iniMode, const QString& output)
{
    QStringList t;
    t << QStringLiteral("Original inputs are preserved. Existing outputs are preserved; a numeric suffix is used for collisions.")
      << QStringLiteral("Each PKG has one content identity. Multiple targets create separate outputs.")
      << QStringLiteral("Store market and package publisher prefix are separate. Regional releases may share one package "
                        "identity; a title ID alone does not identify a region.")
      << QStringLiteral("Identifier mappings and successful structural builds do not confirm in-game DLC compatibility.");
    if (iniMode)
        t << QStringLiteral("INI export groups entries from the same input and target into one file, preserving other entries, "
                            "unknown fields, encoding and comments. Untouched malformed entries are retained; correct them "
                            "separately. PKG-only entries export a new INI.");
    else
        t << QStringLiteral("INI entries generate PS5 DLC template packages. Imported PS5 DLC keeps data and version; PS4 builds "
                            "are unsupported.")
          << QStringLiteral("Development entitlement records are regenerated for the target Content ID and the Entitlement key "
                            "field. The PKG source passcode is used for extraction and rebuilt outputs. Default key and "
                            "passcode: 32 zeroes. Official entitlement/signatures are not preserved.");
    std::set<QString> used;
    const std::vector<int> groups = iniMode ? iniGroups(items) : std::vector<int>();
    std::map<int, QString> groupPath;
    for (size_t n = 0; n < items.size(); ++n) {
        const DlcItem& i = items[n];
        t << QString();
        t << QStringLiteral("INPUT: %1%2").arg(native(i.source),
                                               i.entry >= 0 ? QStringLiteral(" [entry %1]").arg(i.entry + 1) : QString());
        t << QStringLiteral("Detected: %1 / %2").arg(i.detectedTitleId, i.detectedContentId);
        t << QStringLiteral("Target:   %1 / %2 — %3").arg(i.titleId, i.contentId, titleOr(i));
        if (!i.targetMarket.isEmpty())
            t << QStringLiteral("Target Store market: %1; package prefix %2").arg(i.targetMarket, i.contentId.left(6));
        if (!i.metadataLine.isEmpty())
            t << QStringLiteral("Metadata: %1").arg(i.metadataLine);
        const QString problem = itemProblem(i, iniMode);
        if (!problem.isEmpty()) {
            t << QStringLiteral("BLOCKED: %1").arg(problem);
            continue;
        }
        QString out;
        if (iniMode) {
            auto it = groupPath.find(groups[n]);
            if (it == groupPath.end())
                it = groupPath.emplace(groups[n], uniqueOutput(output, QStringLiteral("%1-%2-updated.ini")
                                                                           .arg(QFileInfo(i.source).completeBaseName(), i.titleId),
                                                               used)).first;
            out = it->second;
        } else {
            out = uniqueOutput(output, i.contentId + QStringLiteral(".pkg"), used);
        }
        t << QStringLiteral("OUTPUT: %1").arg(native(out));
    }
    t << QString()
      << QStringLiteral("Affected fields: content_id / title_id / title in INI; contentId / titleId / selected localised title "
                        "and builder identity in supported PS5 PKG.");
    return t.join(u'\n');
}

std::vector<IniExportResult> exportUpdatedInis(const std::vector<DlcItem>& items, const QString& output, QStringList& log)
{
    std::vector<IniExportResult> results;
    QDir().mkpath(output);
    std::set<QString> used;
    const std::vector<int> groupOf = iniGroups(items);
    std::map<int, size_t> groups; // group -> results index
    for (size_t n = 0; n < items.size(); ++n) {
        const DlcItem& i = items[n];
        if (groupOf[n] < 0) {
            const QString problem = itemProblem(i, true);
            results.push_back({{}, {int(n)}, problem});
            log << QStringLiteral("INI FAILED: %1 — %2").arg(native(i.source), problem);
            continue;
        }
        auto it = groups.find(groupOf[n]);
        if (it == groups.end()) {
            IniExportResult r;
            r.path = uniqueOutput(output, QStringLiteral("%1-%2-updated.ini").arg(QFileInfo(i.source).completeBaseName(), i.titleId),
                                  used);
            results.push_back(r);
            it = groups.emplace(groupOf[n], results.size() - 1).first;
        }
        results[it->second].items.push_back(int(n));
    }
    for (IniExportResult& r : results) {
        if (!r.error.isEmpty() || r.path.isEmpty())
            continue;
        const DlcItem& first = items[size_t(r.items.front())];
        Status st = success();
        bool changed = false;
        for (int idx : r.items)
            if (!sourceUnchanged(items[size_t(idx)]))
                changed = true;
        if (changed) {
            st = Error(ErrorCode::InvalidArgument, QStringLiteral("Input changed since preview. Import it again."));
        } else if (first.isPackage()) {
            auto bytes = DlcIniDocument::newIniForPackage(titleOr(first), first.contentId);
            st = bytes.ok() ? writeNewFile(r.path, bytes.value()) : Status(bytes.takeError());
        } else {
            auto doc = DlcIniDocument::load(first.source);
            if (!doc.ok()) {
                st = doc.takeError();
            } else {
                std::map<int, DlcTarget> changes;
                for (int idx : r.items) {
                    const DlcItem& i = items[size_t(idx)];
                    changes[i.entry] = DlcTarget{i.titleId, i.contentId, i.title.trimmed().isEmpty() ? std::optional<QString>()
                                                                                                    : std::optional<QString>(i.title.trimmed())};
                }
                st = doc->saveUpdated(r.path, changes);
            }
        }
        if (st.ok()) {
            // Read-back: every target must be in the written file.
            auto back = DlcIniDocument::load(r.path);
            bool okBack = back.ok();
            if (okBack)
                for (int idx : r.items) {
                    bool found = false;
                    for (const DlcIniEntry& e : back->entries())
                        if (e.contentId == items[size_t(idx)].contentId && e.titleId == items[size_t(idx)].titleId)
                            found = true;
                    okBack = okBack && found;
                }
            if (!okBack) {
                st = Error(ErrorCode::VerificationFailed, QStringLiteral("INI read-back did not match the target."));
                QFile::remove(r.path); // written just now by this export; do not leave a file reported as failed
            }
        }
        if (st.ok()) {
            log << QStringLiteral("INI SAVED: %1 (read-back validated; compatibility unverified)").arg(native(r.path));
        } else {
            r.error = st.error().message;
            log << QStringLiteral("INI FAILED: %1 — %2").arg(native(first.source), r.error);
            r.path.clear();
        }
    }
    return results;
}

QString saveBatchReport(const QString& output, const std::vector<DlcItem>& items, const QStringList& outputs,
                        const QStringList& errors, bool cancelled, const QStringList& logLines)
{
    std::set<QString> used;
    const QString path = uniqueOutput(output, QStringLiteral("DLC-results-%1.json")
                                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))),
                                      used);
    QJsonArray files;
    for (size_t n = 0; n < items.size(); ++n) {
        const DlcItem& i = items[n];
        const QString out = n < size_t(outputs.size()) ? outputs[int(n)] : QString();
        const QString err = n < size_t(errors.size()) ? errors[int(n)] : QString();
        QJsonObject f;
        f.insert(QStringLiteral("Input"), i.source);
        f.insert(QStringLiteral("SourceEntry"), i.entry >= 0 ? QJsonValue(i.entry) : QJsonValue());
        f.insert(QStringLiteral("DetectedTitleId"), i.detectedTitleId);
        f.insert(QStringLiteral("DetectedContentId"), i.detectedContentId);
        f.insert(QStringLiteral("TargetTitleId"), i.titleId);
        f.insert(QStringLiteral("TargetContentId"), i.contentId);
        f.insert(QStringLiteral("TargetMarket"), i.targetMarket);
        f.insert(QStringLiteral("Output"), out);
        f.insert(QStringLiteral("Error"), err.isEmpty() ? QJsonValue() : QJsonValue(err));
        f.insert(QStringLiteral("Result"), !err.isEmpty()                      ? QStringLiteral("Failed")
                                           : out.isEmpty()                     ? QStringLiteral("Not processed")
                                           : out.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)
                                               ? QStringLiteral("Written; edited identifiers validated")
                                               : QStringLiteral("Written and structurally validated"));
        files.append(f);
    }
    QJsonObject root;
    root.insert(QStringLiteral("CreatedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("Cancelled"), cancelled);
    root.insert(QStringLiteral("Compatibility"), QStringLiteral("Unverified. Structural validation is not in-game DLC compatibility."));
    root.insert(QStringLiteral("Files"), files);
    if (!writeNewFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented)).ok())
        return {};
    const QString logPath = QFileInfo(path).absoluteDir().filePath(QFileInfo(path).completeBaseName() + QStringLiteral(".log"));
    (void)writeNewFile(logPath, (logLines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8());
    return path;
}

// ---- the job --------------------------------------------------------------------------------------------

QStringList convertPhases() { return {QStringLiteral("Converting DLC")}; }

Result<JobOutcome> runDlcConvert(const DlcConvertRequest& req, const DlcSettings& s, JobContext& ctx)
{
    ProgressReporter* pr = ctx.progress;
    pr->setPhases(convertPhases());
    pr->beginPhase(0, qint64(req.items.size()) * 100, qint64(req.items.size()), false);
    const FpkgCli cli = locateFpkgCli(s.fpkgCliPath);
    if (!cli.ok())
        return Error(ErrorCode::DependencyMissing, cli.problems.join(u' '));
    if (const QString kp = hex32Problem(s.entitlementKey); !kp.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The entitlement key %1").arg(kp));
    if (const QString pp = hex32Problem(s.sourcePasscode); !pp.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The PKG source passcode %1").arg(pp));
    if (!QDir().mkpath(req.output))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the output folder %1.").arg(native(req.output)));

    QStringList logLines;
    const auto say = [&](const QString& line) {
        logLines << QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line);
        ctx.log.info(line);
    };
    QStringList outputs, errors;
    for (size_t n = 0; n < req.items.size(); ++n) {
        outputs << QString();
        errors << QString();
    }
    std::set<QString> used;
    QStringList unread; // built and kept, but the job was cancelled while they were read back
    int ok = 0, failed = 0;
    bool cancelled = false;
    const int total = int(req.items.size());
    for (int n = 0; n < total; ++n) {
        if (ctx.token.isCancelled()) {
            cancelled = true;
            break;
        }
        const DlcItem& item = req.items[size_t(n)];
        say(QStringLiteral("— %1/%2: %3 (%4)").arg(n + 1).arg(total).arg(item.contentId, titleOr(item)));
        pr->setDone(qint64(n) * 100);
        pr->setCurrentItem(QStringLiteral("%1/%2: %3").arg(n + 1).arg(total).arg(item.contentId));

        const auto fail = [&](const QString& why) {
            errors[n] = why;
            ++failed;
            say(QStringLiteral("  FAILED: %1").arg(why));
        };
        // (Also refuses anything but a PS5 title ID: this engine builds PS5 DLC only.)
        if (const QString problem = itemProblem(item, false); !problem.isEmpty()) {
            fail(problem);
            continue;
        }
        const QString work = QDir(ctx.tempDir).filePath(QStringLiteral("dlc-%1").arg(n));
        removeTreeNoFollow(work);
        const QString source = QDir(work).filePath(QStringLiteral("source"));
        const QString build = QDir(work).filePath(QStringLiteral("build"));
        const QString tmp = QDir(work).filePath(QStringLiteral("tmp"));
        QDir().mkpath(build);
        QDir().mkpath(tmp);
        struct Drop {
            QString dir;
            ~Drop() { removeTreeNoFollow(dir); }
        } drop{work};

        QString root = source;
        QString passcode = kZeroKey;
        if (item.isPackage()) {
            auto info = inspectDlcPackage(cli, item.source, s.sourcePasscode, ctx.token);
            if (!info.ok()) {
                if (info.error().isCancelled()) {
                    cancelled = true;
                    break;
                }
                fail(info.error().message);
                continue;
            }
            // The extracted files and the rebuilt package both pass through the temp folder.
            if (Status sp = ensureFreeSpace(work, info->size * 2, s.freeSpaceMarginBytes); !sp.ok()) {
                fail(sp.error().message);
                continue;
            }
            auto ex = extractDlcPackage(cli, info.value(), source, s.sourcePasscode, tmp, s.priorityMode, ctx.token,
                                        [&](const QString& l) { ctx.log.debug(l); });
            if (!ex.ok()) {
                if (ex.error().isCancelled()) {
                    cancelled = true;
                    break;
                }
                fail(ex.error().message);
                continue;
            }
            root = ex.value();
            if (Status st = updateParam(root, item); !st.ok()) {
                fail(st.error().message);
                continue;
            }
            for (const QString& lic : {QStringLiteral("sce_sys/license.dat"), QStringLiteral("sce_sys/license.info")})
                QFile::remove(QDir(root).filePath(lic));
            say(QStringLiteral("  Development entitlement records will be regenerated for the target Content ID and "
                               "selected key."));
            passcode = s.sourcePasscode;
        } else {
            QDir().mkpath(QDir(root).filePath(QStringLiteral("sce_sys")));
            if (Status st = writeFileAtomic(QDir(root).filePath(QStringLiteral("sce_sys/param.json")),
                                            templateParamJson(item.contentId, item.titleId, titleOr(item)));
                !st.ok()) {
                fail(st.error().message);
                continue;
            }
        }
        if (!item.iconPath.isEmpty()) {
            const QString icon = QDir(root).filePath(QStringLiteral("sce_sys/icon0.png"));
            QFile::remove(icon);
            QFile::remove(QDir(root).filePath(QStringLiteral("sce_sys/icon0.dds"))); // the engine would keep a stale one
            if (!QFile::copy(item.iconPath, icon)) {
                fail(QStringLiteral("The chosen icon0.png could not be copied."));
                continue;
            }
        }
        // The engine has no PS5 entitlement-key flag: a GP5 next to the source carries it.
        if (s.entitlementKey.compare(kZeroKey, Qt::CaseInsensitive) != 0) {
            const QString gp5 = QStringLiteral(
                                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<psproject fmt=\"gp5\">\r\n  <volume>\r\n"
                                    "    <volume_type>prospero_ac</volume_type>\r\n    <package content_id=\"%1\" passcode=\"%2\" "
                                    "entitlement_key=\"%3\" />\r\n  </volume>\r\n  <global_exclude>*.gp5</global_exclude>\r\n"
                                    "  <rootdir src_path=\".\" />\r\n</psproject>\r\n")
                                    .arg(item.contentId, passcode, s.entitlementKey.toLower());
            if (Status st = writeFileAtomic(QDir(root).filePath(item.contentId + QStringLiteral(".gp5")), gp5.toUtf8()); !st.ok()) {
                fail(st.error().message);
                continue;
            }
        }
        QStringList args{QStringLiteral("build"), QStringLiteral("--source"), native(root), QStringLiteral("--output"),
                         native(build), QStringLiteral("--temp"), native(tmp), QStringLiteral("--kind"), QStringLiteral("dlc"),
                         QStringLiteral("--image"), s.imageMode, QStringLiteral("--no-sony-sdk"), QStringLiteral("--passcode"),
                         passcode, QStringLiteral("--level"), QString::number(s.compressionLevel), QStringLiteral("--threads"),
                         QString::number(s.threads), QStringLiteral("--backend"), s.backend, QStringLiteral("--pfs"),
                         s.pfsFormat, QStringLiteral("--block-size"), QStringLiteral("256"), QStringLiteral("--no-sleep-guard")};
        if (s.sdkOverride > 0)
            args << QStringLiteral("--sdk") << QString::number(s.sdkOverride);
        if (!s.deterministic)
            args << QStringLiteral("--no-deterministic");
        if (s.fullVerify)
            args << QStringLiteral("--full-verify");
        if (item.isPackage())
            args << QStringLiteral("--keep-required-fw") << QStringLiteral("--keep-version-uri");
        auto run = runFpkgCli(cli, args, tmp, ctx.token, [&](const QString& l) {
            const QString t = l.trimmed();
            if (t.contains(QStringLiteral("WARNING"), Qt::CaseInsensitive) || t.contains(QStringLiteral("ERROR")))
                say(QStringLiteral("  %1").arg(t));
            else
                ctx.log.debug(t);
        }, 0, s.priorityMode);
        if (!run.ok()) {
            fail(run.error().message);
            continue;
        }
        if (run->cancelled || ctx.token.isCancelled()) {
            cancelled = true;
            break;
        }
        if (!run->ok()) {
            fail(run->failureText());
            continue;
        }
        const QString built = findPackage(build);
        if (built.isEmpty()) {
            fail(QStringLiteral("The DLC engine reported success but wrote no package."));
            continue;
        }
        // Read the identity back. The package is finished from here on: a
        // cancel during the read-back keeps it, not read back (GitHub #97),
        // and the job stops after it.
        auto back = inspectDlcPackage(cli, built, passcode, ctx.token);
        const bool readBackCancelled = !back.ok() && (back.error().isCancelled() || ctx.token.isCancelled());
        if (!back.ok() && !readBackCancelled) {
            fail(QStringLiteral("The built package could not be read back: %1").arg(back.error().message));
            continue;
        }
        if (back.ok()
            && (back->contentId != item.contentId || value(back->metadata, QStringLiteral("titleId")) != item.titleId || !back->isDlc)) {
            fail(QStringLiteral("Built metadata does not match the reviewed DLC target."));
            continue;
        }
        if (Status sp = ensureFreeSpace(req.output, QFileInfo(built).size(), s.freeSpaceMarginBytes); !sp.ok()) {
            fail(sp.error().message);
            continue;
        }
        const QString finalPath = uniqueOutput(req.output, item.contentId + QStringLiteral(".pkg"), used);
        // Never QFile::rename(): to another drive it copies the package straight
        // under its final name, and no cancel can stop it.
        if (QString why; !renameWithoutCopying(built, finalPath, &why)) {
            ctx.log.debug(why);
            // Another drive: copied through <name>.part and flushed, so a
            // pulled drive never leaves a short file under the package's name.
            // A cancel does not cut it short: the work folder holds the only
            // copy of the finished package and goes with this item. The
            // cancel is seen once the package has its name.
            if (Status mv = copyFileStreaming(built, finalPath, CancellationToken(), nullptr, false, true); !mv.ok()) {
                fail(QStringLiteral("Could not move the package to %1.").arg(native(finalPath)));
                ctx.log.warn(mv.error().toLogString());
                continue;
            }
            QFile::remove(built);
        }
        outputs[n] = finalPath;
        ++ok;
        if (readBackCancelled) {
            unread << item.contentId;
            say(QStringLiteral("  → %1 (kept, but not read back: the job was cancelled)").arg(native(finalPath)));
            cancelled = true;
            break;
        }
        say(QStringLiteral("  → %1 (%2 structural verification passed; DLC compatibility unverified)")
                .arg(native(finalPath), s.fullVerify ? QStringLiteral("full") : QStringLiteral("quick")));
    }
    pr->setDone(qint64(total) * 100);
    const QString status = cancelled ? QStringLiteral("Cancelled — %1 built").arg(ok)
                                     : QStringLiteral("Done — %1 built, %2 failed").arg(ok).arg(failed);
    say(status);
    QString report;
    if (s.saveReport)
        report = saveBatchReport(req.output, req.items, outputs, errors, cancelled, logLines);
    if (!report.isEmpty())
        ctx.log.info(QStringLiteral("Results saved: %1").arg(native(report)));
    if (cancelled && ok == 0)
        return Error::cancelled();
    JobOutcome o;
    for (const QString& p : outputs)
        if (!p.isEmpty())
            o.outputs << p;
    for (int n = 0; n < total; ++n)
        if (!errors[n].isEmpty())
            o.warnings << QStringLiteral("%1: %2").arg(req.items[size_t(n)].contentId, errors[n]);
    if (ok == 0)
        return Error(ErrorCode::ExternalToolFailed,
                     failed == 1 ? errors.join(QString()).trimmed() : QStringLiteral("No DLC package was built (%1 failed).").arg(failed));
    if (cancelled) {
        // Some packages exist, so the job is kept; the ones never reached are named.
        QStringList skipped;
        for (int n = 0; n < total; ++n)
            if (outputs[n].isEmpty() && errors[n].isEmpty())
                skipped << req.items[size_t(n)].contentId;
        if (!skipped.isEmpty())
            o.warnings.prepend(QStringLiteral("Cancelled before %1 DLC package%2 could be built: %3")
                                   .arg(skipped.size()).arg(skipped.size() == 1 ? QString() : QStringLiteral("s"), skipped.join(QStringLiteral(", "))));
        if (!unread.isEmpty())
            o.warnings.prepend(QStringLiteral("Kept, but not read back because the job was cancelled: %1").arg(unread.join(QStringLiteral(", "))));
    }
    o.verification = cancelled ? Verification::Inconclusive : Verification::Passed;
    o.verificationDetail = cancelled ? QStringLiteral("Cancelled: the %1 package(s) that were built passed the %2 structural verification; the rest were not built.")
                                           .arg(ok).arg(s.fullVerify ? QStringLiteral("full") : QStringLiteral("quick"))
                                     : QStringLiteral("%1 structural verification passed; DLC compatibility on the console is unverified.")
                                           .arg(s.fullVerify ? QStringLiteral("Full") : QStringLiteral("Quick"));
    o.summary = status;
    o.warnings << QStringLiteral("Identifier mappings and structural checks do not confirm in-game DLC compatibility.");
    if (!report.isEmpty())
        o.details.insert(QStringLiteral("report"), report);
    o.details.insert(QStringLiteral("built"), ok);
    o.details.insert(QStringLiteral("failed"), failed);
    pr->flush();
    return o;
}

QString DlcConvertJob::title() const
{
    return m_request.items.size() == 1 ? m_request.items.front().contentId
                                       : QStringLiteral("%1 DLC packages").arg(m_request.items.size());
}

QString DlcConvertJob::subtitle() const { return QStringLiteral("DLC → %1").arg(native(m_request.output)); }

QStringList DlcConvertJob::busyPaths() const { return {QFileInfo(m_request.output).absoluteFilePath()}; }

Status DlcConvertJob::validate(const SettingsValues& settings) const
{
    const DlcSettings s = DlcSettings::from(settings);
    const FpkgCli cli = locateFpkgCli(s.fpkgCliPath);
    if (!cli.ok())
        return Error(ErrorCode::DependencyMissing, cli.problems.join(u' '));
    if (const QString kp = hex32Problem(s.entitlementKey); !kp.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The entitlement key %1").arg(kp));
    if (const QString pp = hex32Problem(s.sourcePasscode); !pp.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The PKG source passcode %1").arg(pp));
    if (QFileInfo(m_request.output).isFile())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The output folder is a file: %1").arg(native(m_request.output)));
    return success();
}

Result<JobOutcome> DlcConvertJob::run(JobContext& ctx) { return runDlcConvert(m_request, DlcSettings::from(ctx.settings), ctx); }

} // namespace fw::dlc
