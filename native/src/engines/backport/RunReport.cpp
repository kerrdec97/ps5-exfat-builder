#include "engines/backport/RunReport.h"

#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/Common.h"
#include "engines/backport/GameMetadata.h"
#include "engines/backport/SdkTable.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace fw::backport {

namespace {

const QString kDetailMarker = QStringLiteral("Run report: ");

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

QString scalarText(const QJsonValue& v)
{
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined: return QStringLiteral("—");
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == double(qint64(d)))
            return QString::number(qint64(d));
        return QString::number(d);
    }
    case QJsonValue::String: return v.toString().isEmpty() ? QStringLiteral("—") : v.toString();
    case QJsonValue::Array: {
        const QJsonArray a = v.toArray();
        bool scalars = true;
        for (const QJsonValue& x : a)
            if (x.isArray() || x.isObject())
                scalars = false;
        if (scalars) {
            QStringList parts;
            for (const QJsonValue& x : a)
                parts << scalarText(x);
            return parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(", "));
        }
        return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    }
    case QJsonValue::Object: return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    return {};
}

// Values that read better in a monospaced font (paths, hashes, offsets).
bool monoKey(const QString& key)
{
    static const QStringList hints = {QStringLiteral("sha"), QStringLiteral("path"), QStringLiteral("file"),
                                      QStringLiteral("zip"), QStringLiteral("folder"), QStringLiteral("journal"),
                                      QStringLiteral("manifest"), QStringLiteral("offset"), QStringLiteral("hex"),
                                      QStringLiteral("game"), QStringLiteral("output"), QStringLiteral("record"),
                                      QStringLiteral("ps4"), QStringLiteral("ps5"), QStringLiteral("anchor"),
                                      QStringLiteral("dir")};
    const QString low = key.toLower();
    for (const QString& h : hints)
        if (low.contains(h))
            return true;
    return false;
}

// "sha256Before" → "SHA-256 before", "sdkAfter" → "SDK after".
QString label(const QString& key)
{
    QString out;
    for (qsizetype i = 0; i < key.size(); ++i) {
        const QChar c = key.at(i);
        if (c.isUpper() && i > 0 && !key.at(i - 1).isUpper())
            out += u' ';
        out += c;
    }
    out = out.toLower();
    out.replace(QStringLiteral("sha256"), QStringLiteral("SHA-256"));
    out.replace(QStringLiteral("sdk"), QStringLiteral("SDK"));
    out.replace(QStringLiteral("zip"), QStringLiteral("ZIP"));
    out.replace(QStringLiteral("nid"), QStringLiteral("NID"));
    out.replace(QStringLiteral("fw"), QStringLiteral("FW"));
    out.replace(QStringLiteral("ps4"), QStringLiteral("PS4"));
    out.replace(QStringLiteral("ps5"), QStringLiteral("PS5"));
    out.replace(QStringLiteral("api"), QStringLiteral("API"));
    out.replace(QStringLiteral("ampr playgo"), QStringLiteral("AMPR/PlayGo"));
    out.replace(QStringLiteral("param json"), QStringLiteral("param.json"));
    out.replace(QStringLiteral("title id"), QStringLiteral("title ID"));
    out.replace(QStringLiteral(" self "), QStringLiteral(" SELF "));
    if (!out.isEmpty())
        out[0] = out[0].toUpper();
    return out;
}

bool looksLikePath(const QString& text)
{
    static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    return text.startsWith(u'/') || text.startsWith(QStringLiteral("\\\\")) || drive.match(text).hasMatch();
}

QString cell(const QString& key, const QJsonValue& v)
{
    const QString text = scalarText(v);
    if (v.isObject() || (v.isArray() && text.startsWith(u'[')))
        return QStringLiteral("<td><pre>%1</pre></td>").arg(esc(text));
    // Paths, hashes and offsets in a monospaced font; sentences never.
    const bool mono = looksLikePath(text) || (monoKey(key) && !text.contains(u' '));
    return mono ? QStringLiteral("<td class=\"mono\">%1</td>").arg(esc(text)) : QStringLiteral("<td>%1</td>").arg(esc(text));
}

QString kvTable(const QJsonObject& o)
{
    QString h = QStringLiteral("<table class=\"kv\">");
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        h += QStringLiteral("<tr><th>%1</th>%2</tr>").arg(esc(label(it.key())), cell(it.key(), it.value()));
    return h + QStringLiteral("</table>");
}

QString rowsTable(const QJsonArray& rows)
{
    // Columns: the preferred ones first, then every other key in first-seen order.
    static const QStringList preferred = {
        QStringLiteral("file"),        QStringLiteral("role"),       QStringLiteral("result"),
        QStringLiteral("severity"),    QStringLiteral("why"),        QStringLiteral("message"),
        QStringLiteral("sdkBefore"),   QStringLiteral("sdkAfter"),   QStringLiteral("path"),
        QStringLiteral("sha256Before"), QStringLiteral("sha256After"), QStringLiteral("sha256")};
    QStringList cols;
    for (const QString& p : preferred)
        for (const QJsonValue& r : rows)
            if (r.toObject().contains(p)) {
                cols << p;
                break;
            }
    for (const QJsonValue& r : rows)
        for (const QString& k : r.toObject().keys())
            if (!cols.contains(k))
                cols << k;
    QString h = QStringLiteral("<div class=\"scroll\"><table class=\"rows\"><thead><tr>");
    for (const QString& c : std::as_const(cols))
        h += QStringLiteral("<th>%1</th>").arg(esc(label(c)));
    h += QStringLiteral("</tr></thead><tbody>");
    for (const QJsonValue& r : rows) {
        const QJsonObject o = r.toObject();
        QString cls;
        const QString result = o.value(QStringLiteral("result")).toString() + o.value(QStringLiteral("severity")).toString();
        if (result.contains(QStringLiteral("fail")) || result.contains(QStringLiteral("refuse")))
            cls = QStringLiteral(" class=\"bad\"");
        else if (result.contains(QStringLiteral("warn")) || result.contains(QStringLiteral("left")))
            cls = QStringLiteral(" class=\"warn\"");
        h += QStringLiteral("<tr%1>").arg(cls);
        for (const QString& c : std::as_const(cols))
            h += o.contains(c) ? cell(c, o.value(c)) : QStringLiteral("<td></td>");
        h += QStringLiteral("</tr>");
    }
    return h + QStringLiteral("</tbody></table></div>");
}

QString sectionBody(const QJsonValue& v)
{
    if (v.isObject())
        return kvTable(v.toObject());
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.isEmpty())
            return QStringLiteral("<p class=\"muted\">None.</p>");
        bool objects = true;
        for (const QJsonValue& x : a)
            if (!x.isObject())
                objects = false;
        if (objects)
            return rowsTable(a);
        QString h = QStringLiteral("<ul>");
        for (const QJsonValue& x : a)
            h += QStringLiteral("<li>%1</li>").arg(esc(scalarText(x)));
        return h + QStringLiteral("</ul>");
    }
    return QStringLiteral("<p>%1</p>").arg(esc(scalarText(v)));
}

QString toneOf(const QString& status)
{
    const QString s = status.toLower();
    if (s == u"succeeded" || s == u"nothingtodo" || s == u"passed")
        return QStringLiteral("ok");
    if (s == u"partial")
        return QStringLiteral("warn");
    if (s == u"failed" || s == u"cancelled")
        return QStringLiteral("err");
    return QStringLiteral("info");
}

QString statusLabel(const QString& status)
{
    if (status == u"nothingToDo")
        return QStringLiteral("Nothing to do");
    if (status == u"reportOnly")
        return QStringLiteral("Report only");
    QString s = status;
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

QString kindTitle(const QString& kind)
{
    if (kind == u"sdkFix")
        return QStringLiteral("SDK fix report");
    if (kind == u"audioScan")
        return QStringLiteral("Audio report (read-only)");
    return QStringLiteral("Backport report");
}

const char* kCss = R"CSS(
:root{--bg:#f6f7f9;--card:#fff;--fg:#1d2330;--muted:#5b6475;--line:#dde1e8;--ok:#1d7a46;--warn:#9a6200;--err:#b42318;--info:#2557a7;--okbg:#e6f4ec;--warnbg:#fdf3e1;--errbg:#fdecea;--infobg:#e8effb}
@media (prefers-color-scheme:dark){:root{--bg:#14171c;--card:#1c2027;--fg:#e6e9ef;--muted:#9aa3b2;--line:#2c323c;--ok:#5fd394;--warn:#f0b54d;--err:#ff7b6e;--info:#7fa7ff;--okbg:#14301f;--warnbg:#33270f;--errbg:#3a1714;--infobg:#172440}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 "Segoe UI",system-ui,-apple-system,sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 16px 48px}
header h1{margin:2px 0 4px;font-size:22px}.eyebrow{margin:0;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:12px}
.meta,.muted{color:var(--muted)}section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px 16px;margin:14px 0}
h2{font-size:15px;margin:0 0 10px}table{border-collapse:collapse;width:100%}th,td{text-align:left;vertical-align:top;padding:5px 8px;border-bottom:1px solid var(--line)}
table.kv th{width:28%;color:var(--muted);font-weight:500}table.rows th{color:var(--muted);font-weight:500;white-space:nowrap}
.mono,pre{font-family:ui-monospace,Consolas,"Cascadia Mono",monospace;font-size:12.5px;word-break:break-all}pre{margin:0;white-space:pre-wrap}
.scroll{overflow-x:auto}tr.bad td{background:var(--errbg)}tr.warn td{background:var(--warnbg)}
.outcome{display:flex;gap:12px;align-items:flex-start}.pill{flex:none;border-radius:999px;padding:2px 10px;font-weight:600;font-size:12.5px}
.ok .pill{background:var(--okbg);color:var(--ok)}.warn .pill{background:var(--warnbg);color:var(--warn)}.err .pill{background:var(--errbg);color:var(--err)}.info .pill{background:var(--infobg);color:var(--info)}
.outcome .body{min-width:0;flex:1}.outcome pre{margin-top:6px}footer{color:var(--muted);font-size:12px;margin-top:20px}
)CSS";

} // namespace

QString defaultReportDir()
{
    return QDir(Paths::dataDir()).filePath(QStringLiteral("backport/reports"));
}

QString reportDirForManifest(const QString& manifestPath)
{
    if (manifestPath.isEmpty())
        return defaultReportDir();
    return QDir(QFileInfo(manifestPath).absolutePath()).filePath(QStringLiteral("reports"));
}

QString reportDirForGame(const QString& originalsDir, const QString& game)
{
    if (originalsDir.trimmed().isEmpty() || !QFileInfo(originalsDir).isDir() || game.isEmpty())
        return defaultReportDir();
    const QString abs = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    const ManifestSet set = findManifests(originalsDir, manifestTitleId(readGameMetadata(abs), abs), abs);
    return reportDirForManifest(set.ownPath);
}

QJsonObject newReport(const QString& kind)
{
    QJsonObject r;
    r.insert(QStringLiteral("format"), QString::fromLatin1(kReportFormat));
    r.insert(QStringLiteral("schema"), kReportSchema);
    r.insert(QStringLiteral("kind"), kind);
    QJsonObject engine;
    engine.insert(QStringLiteral("name"), QString::fromLatin1(kEngineName));
    engine.insert(QStringLiteral("version"), QString::fromLatin1(kEngineVersion));
    engine.insert(QStringLiteral("tableHash"), QString::fromLatin1(tableHash()));
    r.insert(QStringLiteral("engine"), engine);
    r.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    return r;
}

QString reportBaseName(const QString& what)
{
    return sanitizeFileName(QStringLiteral("%1 %2")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")), what))
        .trimmed();
}

Result<ReportFiles> writeReport(const QString& dir, const QString& baseName, const QJsonObject& report)
{
    if (!QDir().mkpath(dir))
        return makeError(Code::Unreadable,
                         QStringLiteral("Could not create the report folder %1.").arg(QDir::toNativeSeparators(dir)));
    ReportFiles files;
    for (int n = 1; n < 1000; ++n) {
        const QString stem = n == 1 ? baseName : QStringLiteral("%1 (%2)").arg(baseName).arg(n);
        const QString json = QDir(dir).filePath(stem + QStringLiteral(".json"));
        const QString html = QDir(dir).filePath(stem + QStringLiteral(".html"));
        if (QFileInfo::exists(json) || QFileInfo::exists(html))
            continue;
        files = {json, html};
        break;
    }
    if (files.json.isEmpty())
        return makeError(Code::Internal, QStringLiteral("No free report name in %1.").arg(QDir::toNativeSeparators(dir)));
    if (Status st = writeFileAtomic(files.json, QJsonDocument(report).toJson(QJsonDocument::Indented)); !st.ok())
        return st.takeError();
    if (Status st = writeFileAtomic(files.html, renderReportHtml(report).toUtf8()); !st.ok())
        return st.takeError();
    return files;
}

QString renderReportHtml(const QJsonObject& r)
{
    const QString kind = r.value(QStringLiteral("kind")).toString();
    const QString title = r.value(QStringLiteral("title")).toString();
    const QString titleId = r.value(QStringLiteral("titleId")).toString();
    QString heading = title.isEmpty() ? titleId : title;
    if (heading.isEmpty())
        heading = QFileInfo(r.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("game")).toString()).fileName();
    if (heading.isEmpty())
        heading = kindTitle(kind);

    QString h;
    h += QStringLiteral("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'\">"
                        "<meta name=\"generator\" content=\"%1\">")
             .arg(esc(QString::fromLatin1(kReportFormat)));
    h += QStringLiteral("<title>%1 · %2</title><style>%3</style></head><body><main>")
             .arg(esc(kindTitle(kind)), esc(heading), QString::fromLatin1(kCss));

    const QJsonObject engine = r.value(QStringLiteral("engine")).toObject();
    QStringList meta;
    if (!titleId.isEmpty() && titleId != heading)
        meta << titleId;
    meta << QStringLiteral("Created %1").arg(r.value(QStringLiteral("created")).toString());
    if (!r.value(QStringLiteral("jobId")).toString().isEmpty())
        meta << QStringLiteral("Job %1").arg(r.value(QStringLiteral("jobId")).toString());
    meta << QStringLiteral("%1 %2").arg(engine.value(QStringLiteral("name")).toString(),
                                        engine.value(QStringLiteral("version")).toString());
    h += QStringLiteral("<header><p class=\"eyebrow\">%1</p><h1>%2</h1><p class=\"meta\">%3</p></header>")
             .arg(esc(kindTitle(kind)), esc(heading), esc(meta.join(QStringLiteral(" · "))));

    const QJsonObject outcome = r.value(QStringLiteral("outcome")).toObject();
    if (!outcome.isEmpty()) {
        const QString status = outcome.value(QStringLiteral("status")).toString();
        QString body = esc(outcome.value(QStringLiteral("summary")).toString());
        const QJsonObject err = outcome.value(QStringLiteral("error")).toObject();
        if (!err.isEmpty()) {
            body += QStringLiteral("<br><span class=\"muted\">%1</span>").arg(esc(err.value(QStringLiteral("code")).toString()));
            if (!err.value(QStringLiteral("detail")).toString().isEmpty())
                body += QStringLiteral("<pre>%1</pre>").arg(esc(err.value(QStringLiteral("detail")).toString()));
        }
        h += QStringLiteral("<section class=\"outcome %1\"><span class=\"pill\">%2</span><div class=\"body\">%3</div></section>")
                 .arg(toneOf(status), esc(statusLabel(status)), body);
    }

    static const QStringList header = {QStringLiteral("format"), QStringLiteral("schema"), QStringLiteral("kind"),
                                       QStringLiteral("engine"), QStringLiteral("created"), QStringLiteral("title"),
                                       QStringLiteral("titleId"), QStringLiteral("jobId"), QStringLiteral("outcome")};
    static const QStringList order = {QStringLiteral("inputs"),   QStringLiteral("target"),  QStringLiteral("verification"),
                                      QStringLiteral("zips"),     QStringLiteral("files"),   QStringLiteral("skipped"),
                                      QStringLiteral("summary"),  QStringLiteral("issues"),  QStringLiteral("warnings")};
    QStringList keys = order;
    for (const QString& k : r.keys())
        if (!keys.contains(k) && !header.contains(k))
            keys << k;
    for (const QString& k : std::as_const(keys)) {
        if (!r.contains(k))
            continue;
        h += QStringLiteral("<section><h2>%1</h2>%2</section>").arg(esc(label(k)), sectionBody(r.value(k)));
    }
    h += QStringLiteral("<footer>Written by %1 %2 (SDK table %3). Nothing on this page was loaded from the "
                        "network. Console results are not verified by this report.</footer>")
             .arg(esc(engine.value(QStringLiteral("name")).toString()), esc(engine.value(QStringLiteral("version")).toString()),
                  esc(engine.value(QStringLiteral("tableHash")).toString().left(16)));
    h += QStringLiteral("</main></body></html>\n");
    return h;
}

QString appendReportToDetail(const QString& detail, const QString& htmlPath)
{
    if (htmlPath.isEmpty())
        return detail;
    return detail + u'\n' + kDetailMarker + QDir::toNativeSeparators(htmlPath);
}

QString reportFromDetail(const QString& detail)
{
    const qsizetype at = detail.lastIndexOf(kDetailMarker);
    if (at < 0)
        return {};
    QString rest = detail.mid(at + kDetailMarker.size());
    const qsizetype nl = rest.indexOf(u'\n');
    if (nl >= 0)
        rest.truncate(nl);
    return rest.trimmed();
}

QJsonObject reportFilesJson(const ReportFiles& files)
{
    QJsonObject o;
    o.insert(QStringLiteral("json"), QDir::toNativeSeparators(files.json));
    o.insert(QStringLiteral("html"), QDir::toNativeSeparators(files.html));
    return o;
}

} // namespace fw::backport
