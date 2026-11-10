#include "connect/ps5/Y2jb.h"

#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "engines/gameinfo/JsonText.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QtEndian>

#include <functional>

namespace fw::ps5 {

namespace json = fw::gameinfo::json;

QString y2jbTitleId(Y2jbRegion region)
{
    switch (region) {
    case Y2jbRegion::USA: return QStringLiteral("PPSA01650");
    case Y2jbRegion::EU: return QStringLiteral("PPSA01651");
    case Y2jbRegion::JP: return QStringLiteral("PPSA01652");
    }
    return QStringLiteral("PPSA01650");
}

QString y2jbRegionName(Y2jbRegion region)
{
    switch (region) {
    case Y2jbRegion::USA: return QStringLiteral("USA");
    case Y2jbRegion::EU: return QStringLiteral("EU");
    case Y2jbRegion::JP: return QStringLiteral("JP");
    }
    return QStringLiteral("USA");
}

std::optional<Y2jbRegion> y2jbRegionFromName(const QString& name)
{
    const QString n = name.trimmed().toUpper();
    if (n == u"USA" || n == u"US")
        return Y2jbRegion::USA;
    if (n == u"EU")
        return Y2jbRegion::EU;
    if (n == u"JP")
        return Y2jbRegion::JP;
    return std::nullopt;
}

QList<Y2jbRegion> allY2jbRegions()
{
    return {Y2jbRegion::USA, Y2jbRegion::EU, Y2jbRegion::JP};
}

QString Y2jbReport::summary() const
{
    QStringList lines;
    for (const auto& f : files)
        lines << QStringLiteral("%1 %2 %3").arg(f.status, f.name, f.detail);
    return lines.join(u'\n');
}

// ---- param.json -------------------------------------------------------------------------

Result<QByteArray> patchParamJsonText(const QByteArray& original)
{
    auto members = json::topLevelMembers(original);
    if (!members.ok())
        return Error(ErrorCode::CorruptData, QStringLiteral("param.json is not valid JSON, so it was not changed."),
                     members.error().detail);
    std::vector<std::pair<json::Member, QByteArray>> edits;
    const json::Member* master = nullptr;
    const json::Member* local = nullptr;
    for (const auto& m : members.value()) {
        if (m.key == u"contentVersion")
            edits.emplace_back(m, json::quote(QString::fromLatin1(kBlockedContentVersion)));
        else if (m.key == u"versionFileUri")
            edits.emplace_back(m, json::quote(QString::fromLatin1(kBlockedVersionFileUri)));
        else if (m.key == u"masterVersion")
            master = &m;
        else if (m.key == u"localVersion")
            local = &m;
    }
    if (master && local)
        edits.emplace_back(*local, original.mid(master->valueBegin, master->valueEnd - master->valueBegin));
    const QByteArray out = json::replaceValues(original, std::move(edits));
    QJsonParseError err{};
    QJsonDocument::fromJson(out.startsWith("\xEF\xBB\xBF") ? out.mid(3) : out, &err);
    if (err.error != QJsonParseError::NoError)
        return Error(ErrorCode::Internal, QStringLiteral("The patched param.json would not be valid; nothing was changed."),
                     err.errorString());
    return out;
}

PatchCheck verifyParamJsonText(const QByteArray& textIn)
{
    const QByteArray text = textIn.startsWith("\xEF\xBB\xBF") ? textIn.mid(3) : textIn;
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(text, &err);
    if (!d.isObject())
        return {false, QStringLiteral("read-back failed: ") + err.errorString()};
    const QJsonObject o = d.object();
    // Legacy: absent keys are not checked. Without both there is nothing to
    // confirm, and the reason says so (callers show such a file as "nothing
    // to block", not as patched).
    if (!o.contains(QStringLiteral("contentVersion")) && !o.contains(QStringLiteral("versionFileUri")))
        return {true, QStringLiteral("nothing to block: the file has neither contentVersion nor versionFileUri")};
    QStringList problems;
    if (o.contains(QStringLiteral("contentVersion")) &&o.value(QStringLiteral("contentVersion")).toString() != QLatin1String(kBlockedContentVersion))
        problems << QStringLiteral("contentVersion is %1, expected %2").arg(o.value(QStringLiteral("contentVersion")).toVariant().toString(),
                                                                             QLatin1String(kBlockedContentVersion));
    if (o.contains(QStringLiteral("versionFileUri")) && o.value(QStringLiteral("versionFileUri")).toString() != QLatin1String(kBlockedVersionFileUri))
        problems << QStringLiteral("versionFileUri is %1, expected %2").arg(o.value(QStringLiteral("versionFileUri")).toVariant().toString(),
                                                                             QLatin1String(kBlockedVersionFileUri));
    if (!problems.isEmpty())
        return {false, problems.join(QStringLiteral("; "))};
    return {true, QStringLiteral("values confirmed")};
}

namespace {

// False for a param.json that carries neither key the patch sets: nothing in
// it blocks updates and nothing can be changed, so it is not "patched".
bool paramJsonHasBlockKeys(const QByteArray& textIn)
{
    const QByteArray text = textIn.startsWith("\xEF\xBB\xBF") ? textIn.mid(3) : textIn;
    const QJsonObject o = QJsonDocument::fromJson(text).object();
    return o.contains(QStringLiteral("contentVersion")) || o.contains(QStringLiteral("versionFileUri"));
}

const QString kNothingToBlock = QStringLiteral("nothing to block (the file has neither contentVersion nor versionFileUri)");

} // namespace

// ---- SQLite ------------------------------------------------------------------------------

namespace {

class Sqlite {
public:
    Sqlite(const QString& path, bool readOnly)
        : m_name(QStringLiteral("fw-y2jb-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
            m_error = QStringLiteral("SQLite support (the Qt SQL plugin qsqlite) is missing from this installation.");
            return;
        }
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(path);
        if (readOnly)
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open())
            m_error = db.lastError().text();
        m_added = true;
    }
    ~Sqlite()
    {
        if (!m_added)
            return;
        {
            QSqlDatabase db = QSqlDatabase::database(m_name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(m_name);
    }
    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    QSqlDatabase db() const { return QSqlDatabase::database(m_name, false); }

private:
    QString m_name;
    QString m_error;
    bool m_added = false;
};

Result<int> runUpdate(QSqlDatabase db, const QString& sql, const QString& titleId)
{
    QSqlQuery q(db);
    if (!q.prepare(sql))
        return Error(ErrorCode::CorruptData, QStringLiteral("The database does not have the expected tables."),
                     q.lastError().text());
    q.addBindValue(titleId);
    if (!q.exec())
        return Error(ErrorCode::CorruptData, QStringLiteral("The database could not be updated."), q.lastError().text());
    return q.numRowsAffected();
}

} // namespace

Result<int> patchAppInfoDb(const QString& dbPath, const QString& titleId)
{
    if (!QFileInfo(dbPath).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("appinfo.db was not found."));
    Sqlite s(dbPath, false);
    if (!s.ok())
        return Error(ErrorCode::IoError, QStringLiteral("appinfo.db could not be opened."), s.error());
    QSqlDatabase db = s.db();
    {
        QSqlQuery q(db);
        (void)q.exec(QStringLiteral("PRAGMA journal_mode = DELETE"));
    }
    if (!db.transaction())
        return Error(ErrorCode::IoError, QStringLiteral("appinfo.db could not be updated."), db.lastError().text());
    auto r1 = runUpdate(db, QStringLiteral("UPDATE tbl_appinfo SET val = 'http://127.0.0.2' WHERE titleId = ? AND key = 'VERSION_FILE_URI'"), titleId);
    auto r2 = r1.ok() ? runUpdate(db, QStringLiteral("UPDATE tbl_appinfo SET val = '99.999.999' WHERE titleId = ? AND key = 'CONTENT_VERSION'"), titleId)
                      : Result<int>(r1.error());
    if (!r1.ok() || !r2.ok()) {
        db.rollback();
        return r1.ok() ? r2.takeError() : r1.takeError();
    }
    if (!db.commit())
        return Error(ErrorCode::IoError, QStringLiteral("appinfo.db could not be saved."), db.lastError().text());
    return r1.value() + r2.value();
}

PatchCheck verifyAppInfoDb(const QString& dbPath, const QString& titleId)
{
    if (!QFileInfo(dbPath).isFile())
        return {false, QStringLiteral("read-back failed: file missing")};
    Sqlite s(dbPath, true);
    if (!s.ok())
        return {false, QStringLiteral("read-back failed: ") + s.error()};
    QSqlQuery q(s.db());
    q.prepare(QStringLiteral("SELECT key, val FROM tbl_appinfo WHERE titleId = ? AND key IN ('VERSION_FILE_URI', 'CONTENT_VERSION')"));
    q.addBindValue(titleId);
    if (!q.exec())
        return {false, QStringLiteral("read-back failed: ") + q.lastError().text()};
    QMap<QString, QString> got;
    while (q.next())
        got.insert(q.value(0).toString(), q.value(1).toString());
    if (got.isEmpty())
        return {false, QStringLiteral("no rows for %1 in tbl_appinfo (wrong region or YouTube not installed)").arg(titleId)};
    QStringList problems;
    if (got.value(QStringLiteral("VERSION_FILE_URI")) != QLatin1String(kBlockedVersionFileUri))
        problems << QStringLiteral("VERSION_FILE_URI=%1").arg(got.value(QStringLiteral("VERSION_FILE_URI")));
    if (got.value(QStringLiteral("CONTENT_VERSION")) != QLatin1String(kBlockedContentVersion))
        problems << QStringLiteral("CONTENT_VERSION=%1").arg(got.value(QStringLiteral("CONTENT_VERSION")));
    if (!problems.isEmpty())
        return {false, QStringLiteral("unexpected values: ") + problems.join(QStringLiteral("; "))};
    return {true, QStringLiteral("both values confirmed for %1").arg(titleId)};
}

Result<int> patchAppDb(const QString& dbPath, const QString& titleId)
{
    if (!QFileInfo(dbPath).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("app.db was not found."));
    Sqlite s(dbPath, false);
    if (!s.ok())
        return Error(ErrorCode::IoError, QStringLiteral("app.db could not be opened."), s.error());
    QSqlDatabase db = s.db();
    {
        QSqlQuery probe(db);
        if (!probe.exec(QStringLiteral("SELECT json('{}')")))
            return Error(ErrorCode::DependencyMissing,
                         QStringLiteral("This installation's SQLite has no JSON functions, so app.db was not changed."),
                         probe.lastError().text());
        QSqlQuery q(db);
        (void)q.exec(QStringLiteral("PRAGMA journal_mode = DELETE"));
    }
    // Detect the schema: Sony's real one keeps the values in $.field_list[].
    bool hasFieldList = false;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT json_type(AppInfoJson, '$.field_list') FROM tbl_contentinfo WHERE titleId = ?"));
        q.addBindValue(titleId);
        if (q.exec() && q.next())
            hasFieldList = q.value(0).toString() == u"array";
    }
    // The ELSE json(e.value) keeps the other entries as JSON (without it they
    // would be stored as strings and the database corrupted) — legacy note.
    const QString sql = hasFieldList
        ? QStringLiteral(
              "UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.field_list', ("
              " SELECT json_group_array(CASE"
              "  WHEN json_extract(e.value,'$.key') = 'CONTENT_VERSION' THEN json_set(e.value,'$.data','99.999.999')"
              "  WHEN json_extract(e.value,'$.key') = 'VERSION_FILE_URI' THEN json_set(e.value,'$.data','http://127.0.0.2')"
              "  ELSE json(e.value) END)"
              " FROM json_each(AppInfoJson, '$.field_list') AS e)) WHERE titleId = ?")
        : QStringLiteral("UPDATE tbl_contentinfo SET AppInfoJson = json_set(AppInfoJson, '$.CONTENT_VERSION', '99.999.999', "
                         "'$.VERSION_FILE_URI', 'http://127.0.0.2') WHERE titleId = ?");
    if (!db.transaction())
        return Error(ErrorCode::IoError, QStringLiteral("app.db could not be updated."), db.lastError().text());
    auto rows = runUpdate(db, sql, titleId);
    if (!rows.ok()) {
        db.rollback();
        return rows.takeError();
    }
    if (!db.commit())
        return Error(ErrorCode::IoError, QStringLiteral("app.db could not be saved."), db.lastError().text());
    return rows.value();
}

PatchCheck verifyAppDb(const QString& dbPath, const QString& titleId)
{
    if (!QFileInfo(dbPath).isFile())
        return {false, QStringLiteral("read-back failed: file missing")};
    Sqlite s(dbPath, true);
    if (!s.ok())
        return {false, QStringLiteral("read-back failed: ") + s.error()};
    QSqlQuery q(s.db());
    q.prepare(QStringLiteral("SELECT AppInfoJson FROM tbl_contentinfo WHERE titleId = ?"));
    q.addBindValue(titleId);
    if (!q.exec())
        return {false, QStringLiteral("read-back failed: ") + q.lastError().text()};
    if (!q.next())
        return {false, QStringLiteral("no row for %1 in tbl_contentinfo (wrong region or YouTube not installed)").arg(titleId)};
    const QString blobText = q.value(0).toString();
    const QJsonDocument d = QJsonDocument::fromJson(blobText.toUtf8());
    if (!d.isObject()) {
        if (blobText.contains(QLatin1String(kBlockedContentVersion)) && blobText.contains(QLatin1String(kBlockedVersionFileUri)))
            return {true, QStringLiteral("values present in AppInfoJson blob")};
        return {false, QStringLiteral("AppInfoJson is not valid JSON and does not contain expected values")};
    }
    const QJsonObject blob = d.object();
    if (blob.value(QStringLiteral("field_list")).isArray()) {
        bool cv = false, vf = false;
        for (const auto& v : blob.value(QStringLiteral("field_list")).toArray()) {
            const QJsonObject e = v.toObject();
            const QString k = e.value(QStringLiteral("key")).toString();
            const QJsonValue data = e.value(QStringLiteral("data"));
            if (k == u"CONTENT_VERSION" && data.toString() == QLatin1String(kBlockedContentVersion))
                cv = true;
            if (k == u"VERSION_FILE_URI" && data.toString() == QLatin1String(kBlockedVersionFileUri))
                vf = true;
        }
        if (cv && vf)
            return {true, QStringLiteral("field_list[CONTENT_VERSION] and field_list[VERSION_FILE_URI] both confirmed for ") + titleId};
        QStringList missing;
        if (!cv)
            missing << QStringLiteral("CONTENT_VERSION");
        if (!vf)
            missing << QStringLiteral("VERSION_FILE_URI");
        return {false, QStringLiteral("field_list present but missing expected values: ") + missing.join(QStringLiteral(", "))};
    }
    QStringList problems;
    if (blob.value(QStringLiteral("CONTENT_VERSION")).toString() != QLatin1String(kBlockedContentVersion))
        problems << QStringLiteral("CONTENT_VERSION=%1").arg(blob.value(QStringLiteral("CONTENT_VERSION")).toVariant().toString());
    if (blob.value(QStringLiteral("VERSION_FILE_URI")).toString() != QLatin1String(kBlockedVersionFileUri))
        problems << QStringLiteral("VERSION_FILE_URI=%1").arg(blob.value(QStringLiteral("VERSION_FILE_URI")).toVariant().toString());
    if (!problems.isEmpty())
        return {false, QStringLiteral("top-level values not as expected: ") + problems.join(QStringLiteral("; "))};
    return {true, QStringLiteral("top-level CONTENT_VERSION and VERSION_FILE_URI confirmed for ") + titleId};
}

PatchCheck checkSqliteIntegrity(const QString& dbPath)
{
    if (!QFileInfo(dbPath).isFile())
        return {false, QStringLiteral("file missing")};
    Sqlite s(dbPath, true);
    if (!s.ok())
        return {false, s.error()};
    QSqlQuery q(s.db());
    if (!q.exec(QStringLiteral("PRAGMA integrity_check")))
        return {false, q.lastError().text()};
    QStringList rows;
    while (q.next())
        rows << q.value(0).toString();
    if (rows == QStringList{QStringLiteral("ok")})
        return {true, QStringLiteral("integrity_check ok")};
    return {false, QStringLiteral("integrity_check: ") + rows.mid(0, 5).join(QStringLiteral("; "))};
}

// ---- local folder ---------------------------------------------------------------------------

namespace {

// "<file>.bak", or ".bak.2", ".bak.3"... so an older backup is never overwritten.
QString freeBackupName(const QString& file)
{
    QString b = file + QStringLiteral(".bak");
    for (int n = 2; QFileInfo::exists(b); ++n)
        b = file + QStringLiteral(".bak.%1").arg(n);
    return b;
}

using DbPatch = Result<int> (*)(const QString&, const QString&);
using DbVerify = PatchCheck (*)(const QString&, const QString&);

} // namespace

Result<Y2jbReport> patchLocalFolder(const QString& folder, Y2jbRegion region)
{
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("Select a folder first."));
    const QString tid = y2jbTitleId(region);
    Y2jbReport rep;
    // param.json
    {
        const QString pj = QDir(folder).filePath(QStringLiteral("param.json"));
        Y2jbFileResult r{QStringLiteral("param.json"), pj, {}, {}, {}};
        if (!QFileInfo(pj).isFile()) {
            r.status = QStringLiteral("○");
            r.detail = QStringLiteral("not found");
        } else {
            QFile f(pj);
            QByteArray text;
            if (f.open(QIODevice::ReadOnly))
                text = f.readAll();
            f.close();
            auto patched = patchParamJsonText(text);
            const QString bak = freeBackupName(pj);
            if (!patched.ok()) {
                r.status = QStringLiteral("!");
                r.detail = patched.error().message;
            } else if (!paramJsonHasBlockKeys(text)) {
                r.status = QStringLiteral("○");
                r.detail = kNothingToBlock;
            } else if (!QFile::copy(pj, bak)) {
                r.status = QStringLiteral("!");
                r.detail = QStringLiteral("the backup could not be written; the file was not changed");
            } else if (auto st = writeFileAtomic(pj, patched.value()); !st.ok()) {
                r.status = QStringLiteral("!");
                r.detail = st.error().message;
                r.backup = bak;
            } else {
                r.backup = bak;
                QFile back(pj);
                const PatchCheck v = back.open(QIODevice::ReadOnly) ? verifyParamJsonText(back.readAll())
                                                                     : PatchCheck{false, QStringLiteral("could not read it back")};
                r.status = v.ok ? QStringLiteral("✓") : QStringLiteral("✗");
                r.detail = v.ok ? QStringLiteral("patched + verified (%1)").arg(v.reason)
                                : QStringLiteral("patched but VERIFICATION FAILED: ") + v.reason;
                if (!v.ok)
                    rep.verifyFailures << QStringLiteral("param.json: ") + v.reason;
            }
        }
        rep.files.push_back(r);
    }
    // the two databases
    const struct {
        const char* name;
        DbPatch patch;
        DbVerify verify;
    } dbs[] = {{"appinfo.db", &patchAppInfoDb, &verifyAppInfoDb}, {"app.db", &patchAppDb, &verifyAppDb}};
    for (const auto& db : dbs) {
        const QString path = QDir(folder).filePath(QLatin1String(db.name));
        Y2jbFileResult r{QLatin1String(db.name), path, {}, {}, {}};
        if (!QFileInfo(path).isFile()) {
            r.status = QStringLiteral("○");
            r.detail = QStringLiteral("not found — skipped");
            rep.files.push_back(r);
            continue;
        }
        const QString bak = freeBackupName(path);
        if (!QFile::copy(path, bak)) {
            r.status = QStringLiteral("!");
            r.detail = QStringLiteral("the backup could not be written; the file was not changed");
            rep.files.push_back(r);
            continue;
        }
        r.backup = bak;
        auto rows = db.patch(path, tid);
        if (!rows.ok()) {
            r.status = QStringLiteral("!");
            r.detail = rows.error().message;
        } else if (rows.value() == 0) {
            r.status = QStringLiteral("✗");
            r.detail = QStringLiteral("NO ROWS for %1 (wrong region selected, or YouTube not installed in this dump?)").arg(tid);
            rep.verifyFailures << QStringLiteral("%1: 0 rows matched %2").arg(QLatin1String(db.name), tid);
        } else {
            const PatchCheck v = db.verify(path, tid);
            r.status = v.ok ? QStringLiteral("✓") : QStringLiteral("✗");
            r.detail = v.ok ? QStringLiteral("patched + verified (%1)").arg(v.reason)
                            : QStringLiteral("patched but VERIFY FAILED: ") + v.reason;
            if (!v.ok)
                rep.verifyFailures << QStringLiteral("%1: %2").arg(QLatin1String(db.name), v.reason);
        }
        rep.files.push_back(r);
    }
    return rep;
}

// ---- over FTP ------------------------------------------------------------------------------

namespace {

// Writes the remote backup next to the file (legacy), but never over an older one.
void remoteBestEffortBackup(FtpClient& c, const QString& backupPath, const QByteArray& original, const Log& log)
{
    if (c.fileExists(backupPath)) {
        log.info(QStringLiteral("[Y2JB] Remote backup %1 already exists; kept as it is.").arg(backupPath));
        return;
    }
    Status st = c.upload(backupPath, original);
    if (st.ok()) {
        if (auto stored = c.size(backupPath); stored.ok() && stored.value() != original.size())
            st = Error(ErrorCode::IoError, QStringLiteral("stored %1 of %2 bytes").arg(stored.value()).arg(original.size()));
    }
    if (st.ok())
        return;
    // A cut-off backup must not stay: the next run would keep it "as it is".
    if (!c.isConnected())
        (void)c.open(c.host(), c.port());
    const bool removed = c.isConnected() && (c.dele(backupPath).ok() || !c.fileExists(backupPath));
    log.warn(QStringLiteral("[Y2JB] Could not write the remote backup %1 (the local backup is still there).%2")
                 .arg(backupPath, removed ? QString() : QStringLiteral(" An incomplete copy may remain on the PS5.")));
}

// RETR, then the size the server reports (when it reports one) must match:
// a short read must never become the "backup" or the base of a patch.
Result<QByteArray> downloadChecked(FtpClient& c, const QString& remote, const CancellationToken& token)
{
    FW_TRY_ASSIGN(QByteArray bytes, c.download(remote, token));
    auto size = c.size(remote);
    if (size.ok() && size.value() != bytes.size())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("%1 arrived incomplete (%2 of %3 bytes), so it was not changed.")
                         .arg(remote)
                         .arg(bytes.size())
                         .arg(size.value()));
    return bytes;
}

// After a failed write or read-back: make sure the console holds the
// original again (never cancellable). Returns a sentence for the report.
QString restoreOriginal(FtpClient& c, const QString& remote, const QByteArray& original, const QString& localBackup,
                        const Log& log)
{
    const QString saved = QDir::toNativeSeparators(localBackup);
    if (!c.isConnected() && !c.open(c.host(), c.port()).ok())
        return QStringLiteral("could not reconnect to restore it; the original is saved at %1").arg(saved);
    auto now = c.download(remote);
    if (now.ok() && now.value() == original)
        return QStringLiteral("the original is still in place");
    if (c.uploadReplace(remote, original).ok()) {
        auto again = c.download(remote);
        if (again.ok() && again.value() == original) {
            log.warn(QStringLiteral("[Y2JB] Restored the original %1 from the local backup.").arg(remote));
            return QStringLiteral("the original was restored from the local backup");
        }
    }
    log.error(QStringLiteral("[Y2JB] Could NOT restore %1; the original is saved at %2").arg(remote, saved));
    return QStringLiteral("the original could NOT be restored automatically; it is saved at %1").arg(saved);
}

// Writes `patched` without ever truncating the live file (temp + rename),
// reads it back and checks it; restores the original when either step fails.
void writeVerified(FtpClient& c, Y2jbFileResult& r, const QByteArray& original, const QByteArray& patched,
                   const std::function<PatchCheck(const QByteArray&)>& verify, const CancellationToken& token,
                   const Log& log, Y2jbReport& rep)
{
    if (auto st = c.uploadReplace(r.path, patched, QStringLiteral(".uploading"), token); !st.ok()) {
        r.status = QStringLiteral("!");
        r.detail = QStringLiteral("%1 — %2").arg(st.error().message, restoreOriginal(c, r.path, original, r.backup, log));
        rep.errors << QStringLiteral("%1 @ %2: %3").arg(r.name, r.path, r.detail);
        return;
    }
    if (!c.isConnected())
        (void)c.open(c.host(), c.port());
    auto back = downloadChecked(c, r.path, token);
    const PatchCheck v = back.ok() ? verify(back.value())
                                   : PatchCheck{false, QStringLiteral("verify pass errored: ") + back.error().message};
    if (v.ok) {
        r.status = QStringLiteral("✓");
        r.detail = QStringLiteral("patched + verified (%1)").arg(v.reason);
    } else {
        r.status = QStringLiteral("✗");
        r.detail = QStringLiteral("VERIFY FAILED: %1 — %2").arg(v.reason, restoreOriginal(c, r.path, original, r.backup, log));
        rep.verifyFailures << QStringLiteral("%1 @ %2: %3").arg(r.name, r.path, v.reason);
    }
    log.info(QStringLiteral("[Y2JB] %1 %2 %3").arg(r.status, r.name, r.detail));
}

void fail(Y2jbFileResult& r, Y2jbReport& rep, const QString& why)
{
    r.status = QStringLiteral("!");
    r.detail = why;
    rep.errors << QStringLiteral("%1 @ %2: %3").arg(r.name, r.path, why);
}

// False: cancelled before anything of this file was changed (the run ends as
// cancelled, not as a failed file).
bool patchDbOverFtp(FtpClient& c, const QString& remote, const QString& label, const QString& tid, DbPatch patch,
                    DbVerify verify, const QString& backupDir, const CancellationToken& token, const Log& log,
                    Y2jbReport& rep)
{
    Y2jbFileResult r{label, remote, {}, {}, {}};
    auto bytes = downloadChecked(c, remote, token);
    if (!bytes.ok()) {
        if (bytes.error().isCancelled())
            return false;
        if (bytes.error().code == ErrorCode::NotFound && c.isConnected()) {
            log.info(QStringLiteral("[Y2JB] Not found on PS5: %1").arg(label));
            r.status = QStringLiteral("○");
            r.detail = QStringLiteral("not found on PS5");
        } else {
            fail(r, rep, bytes.error().message);
        }
        rep.files.push_back(r);
        return true;
    }
    log.info(QStringLiteral("[Y2JB] Downloaded %1 (%2 bytes)").arg(label).arg(bytes.value().size()));
    auto local = saveRemoteCopy(bytes.value(), remote, backupDir);
    if (!local.ok()) {
        fail(r, rep, local.error().message);
        rep.files.push_back(r);
        return true;
    }
    r.backup = local.value();
    TempDir tmp(QStringLiteral("y2jb"));
    const QString work = tmp.filePath(QStringLiteral("work.db"));
    if (auto st = writeFileAtomic(work, bytes.value()); !st.ok()) {
        fail(r, rep, st.error().message);
        rep.files.push_back(r);
        return true;
    }
    auto rows = patch(work, tid);
    if (!rows.ok()) {
        fail(r, rep, rows.error().message + QStringLiteral(" Nothing was uploaded."));
        rep.files.push_back(r);
        return true;
    }
    if (rows.value() == 0) {
        log.warn(QStringLiteral("[Y2JB] ✗ %1: 0 rows matched %2 — NOT uploading (would be a no-op).").arg(label, tid));
        r.status = QStringLiteral("✗");
        r.detail = QStringLiteral("0 rows matched %1 (wrong region or YouTube not installed)").arg(tid);
        rep.verifyFailures << QStringLiteral("%1: 0 rows matched %2").arg(label, tid);
        rep.files.push_back(r);
        return true;
    }
    if (const PatchCheck ic = checkSqliteIntegrity(work); !ic.ok) {
        fail(r, rep, QStringLiteral("the patched copy failed SQLite's integrity check (%1); nothing was uploaded.").arg(ic.reason));
        rep.files.push_back(r);
        return true;
    }
    QFile pf(work);
    if (!pf.open(QIODevice::ReadOnly)) {
        fail(r, rep, pf.errorString());
        rep.files.push_back(r);
        return true;
    }
    const QByteArray patched = pf.readAll();
    pf.close();
    remoteBestEffortBackup(c, remote + QStringLiteral(".bak"), bytes.value(), log);
    log.info(QStringLiteral("[Y2JB] Uploading patched %1 (%2 rows changed)").arg(label).arg(rows.value()));
    const QString vpath = tmp.filePath(QStringLiteral("verify.db"));
    writeVerified(
        c, r, bytes.value(), patched,
        [&](const QByteArray& back) {
            if (auto st = writeFileAtomic(vpath, back); !st.ok())
                return PatchCheck{false, st.error().message};
            if (const PatchCheck ic = checkSqliteIntegrity(vpath); !ic.ok)
                return ic;
            return verify(vpath, tid);
        },
        token, log, rep);
    rep.files.push_back(r);
    return true;
}

} // namespace

Result<Y2jbReport> autoBlockOverFtp(FtpClient& c, Y2jbRegion region, const QString& backupDir,
                                    const CancellationToken& token, const Log& log)
{
    if (!c.isConnected())
        return Error(ErrorCode::IoError, QStringLiteral("Not connected to the PS5 FTP server."));
    const QString tid = y2jbTitleId(region);
    log.info(QStringLiteral("[Y2JB] Connected — patching %1 (%2)").arg(y2jbRegionName(region), tid));
    Y2jbReport rep;
    const QStringList paramPaths = {QStringLiteral("/user/appmeta/%1/param.json").arg(tid),
                                    QStringLiteral("/system_data/priv/appmeta/%1/param.json").arg(tid)};
    for (const QString& remote : paramPaths) {
        if (!token.checkpoint())
            return Error::cancelled();
        Y2jbFileResult r{QStringLiteral("param.json"), remote, {}, {}, {}};
        auto bytes = downloadChecked(c, remote, token);
        if (!bytes.ok()) {
            if (bytes.error().isCancelled())
                return bytes.takeError();
            if (bytes.error().code == ErrorCode::NotFound) {
                r.status = QStringLiteral("○");
                r.detail = QStringLiteral("not found");
                log.info(QStringLiteral("[Y2JB] Not found: %1").arg(remote));
            } else {
                fail(r, rep, bytes.error().message);
            }
            rep.files.push_back(r);
            continue;
        }
        auto local = saveRemoteCopy(bytes.value(), remote, backupDir);
        if (!local.ok()) {
            fail(r, rep, local.error().message);
            rep.files.push_back(r);
            continue;
        }
        r.backup = local.value();
        auto patched = patchParamJsonText(bytes.value());
        if (!patched.ok()) {
            fail(r, rep, patched.error().message);
            rep.files.push_back(r);
            continue;
        }
        if (patched.value() != bytes.value()) {
            // The console is written to only when something changes.
            QString backupName = remote;
            backupName.replace(QLatin1String("param.json"), QLatin1String("param_backup.json"));
            remoteBestEffortBackup(c, backupName, bytes.value(), log);
            writeVerified(c, r, bytes.value(), patched.value(), verifyParamJsonText, token, log, rep);
        } else if (!paramJsonHasBlockKeys(bytes.value())) {
            r.status = QStringLiteral("○");
            r.detail = kNothingToBlock;
            log.info(QStringLiteral("[Y2JB] %1: %2").arg(remote, kNothingToBlock));
        } else {
            // Already blocked: only check it.
            const PatchCheck v = verifyParamJsonText(bytes.value());
            r.status = v.ok ? QStringLiteral("✓") : QStringLiteral("✗");
            r.detail = v.ok ? QStringLiteral("already patched (%1)").arg(v.reason) : QStringLiteral("VERIFY FAILED: ") + v.reason;
            if (!v.ok)
                rep.verifyFailures << QStringLiteral("param.json @ %1: %2").arg(remote, v.reason);
        }
        rep.files.push_back(r);
    }
    if (!patchDbOverFtp(c, QStringLiteral("/system_data/priv/mms/appinfo.db"), QStringLiteral("appinfo.db"), tid,
                        &patchAppInfoDb, &verifyAppInfoDb, backupDir, token, log, rep))
        return Error::cancelled();
    if (!token.checkpoint())
        return Error::cancelled();
    if (!patchDbOverFtp(c, QStringLiteral("/system_data/priv/mms/app.db"), QStringLiteral("app.db"), tid, &patchAppDb,
                        &verifyAppDb, backupDir, token, log, rep))
        return Error::cancelled();
    return rep;
}

// ---- file placement ----------------------------------------------------------------------

QString y2jbDownload0Path(Y2jbRegion region)
{
    return QStringLiteral("/user/download/%1/download0.dat").arg(y2jbTitleId(region));
}

namespace {

Status storeFile(FtpClient& c, const QString& remote, const QString& local, const CancellationToken& token)
{
    QFile f(local);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be read.").arg(QDir::toNativeSeparators(local)),
                     f.errorString());
    // Icons, art and download0.dat are small: upload beside the live file and
    // swap, so a dropped connection never leaves the console's copy truncated.
    if (f.size() <= 32LL * 1024 * 1024)
        return c.uploadReplace(remote, f.readAll(), QStringLiteral(".uploading"), token);
    return c.store(remote, f, 0, {}, token);
}

} // namespace

Status placeDownload0Dat(FtpClient& c, Y2jbRegion region, const QString& localFile, const QString& backupDir,
                         const CancellationToken& token)
{
    if (!QFileInfo(localFile).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("Select a download0.dat file first."));
    const QString remote = y2jbDownload0Path(region);
    (void)c.mkdirs(remoteParent(remote)); // already exists is fine
    FW_TRY(backupRemoteFile(c, remote, backupDir, token));
    return storeFile(c, remote, localFile, token);
}

Status validateY2jbIcon(const QString& pngPath)
{
    QFile f(pngPath);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("Select an icon0.png file first."));
    const QByteArray head = f.read(24);
    if (!head.startsWith("\x89PNG\r\n\x1a\n"))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Selected file is not a valid PNG."));
    if (head.size() < 24 || head.mid(12, 4) != "IHDR")
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Could not read PNG info: missing IHDR."));
    const quint32 w = qFromBigEndian<quint32>(head.constData() + 16);
    const quint32 h = qFromBigEndian<quint32>(head.constData() + 20);
    if (w != 512 || h != 512)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Icon must be exactly 512x512 pixels. Your image is %1x%2.").arg(w).arg(h));
    return success();
}

Result<QStringList> uploadY2jbIcon(FtpClient& c, const QList<Y2jbRegion>& regions, const QString& pngPath,
                                   const QString& backupDir, const CancellationToken& token)
{
    FW_TRY(validateY2jbIcon(pngPath));
    if (regions.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Select at least one region."));
    QStringList done;
    for (Y2jbRegion r : regions) {
        const QString remote = QStringLiteral("/user/appmeta/%1/icon0.png").arg(y2jbTitleId(r));
        (void)c.mkdirs(remoteParent(remote));
        FW_TRY(backupRemoteFile(c, remote, backupDir, token));
        FW_TRY(storeFile(c, remote, pngPath, token));
        done << remote;
    }
    return done;
}

Result<ArtSetReport> uploadY2jbArtSet(FtpClient& c, const QList<Y2jbRegion>& regions, const QString& folder,
                                      const QString& backupDir, const CancellationToken& token)
{
    if (!QFileInfo(folder).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("Select an art folder first."));
    if (regions.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Select at least one region."));
    const QList<QPair<QString, QStringList>> plan = {
        {QStringLiteral("/user/appmeta/%1"),
         {QStringLiteral("pic0.png"), QStringLiteral("pic0.dds"), QStringLiteral("pic1.dds"), QStringLiteral("pic2.dds"),
          QStringLiteral("icon0.png")}},
        {QStringLiteral("/user/catalog_downloader/appmeta/%1"), {QStringLiteral("icon0.png"), QStringLiteral("pic0.png")}},
    };
    ArtSetReport rep;
    QStringList all;
    for (const auto& [dir, names] : plan)
        all << names;
    all.removeDuplicates();
    all.sort();
    for (const QString& n : all)
        if (!QFileInfo(QDir(folder).filePath(n)).isFile())
            rep.missing << n;
    for (Y2jbRegion r : regions) {
        for (const auto& [dirTemplate, names] : plan) {
            const QString dir = dirTemplate.arg(y2jbTitleId(r));
            (void)c.mkdirs(dir);
            for (const QString& name : names) {
                const QString local = QDir(folder).filePath(name);
                if (!QFileInfo(local).isFile())
                    continue;
                if (!token.checkpoint())
                    return Error::cancelled();
                const QString remote = dir + u'/' + name;
                auto bak = backupRemoteFile(c, remote, backupDir, token);
                if (!bak.ok()) {
                    ++rep.failed;
                    rep.lines << QStringLiteral("✗ %1 → %2 FAILED: %3").arg(name, remote, bak.error().message);
                    continue;
                }
                if (!bak.value().isEmpty())
                    rep.backups << bak.value();
                if (auto st = storeFile(c, remote, local, token); !st.ok()) {
                    ++rep.failed;
                    rep.lines << QStringLiteral("✗ %1 → %2 FAILED: %3").arg(name, remote, st.error().message);
                    continue;
                }
                ++rep.uploaded;
                rep.lines << QStringLiteral("✓ %1 → %2 (%3 bytes)").arg(name, remote).arg(QFileInfo(local).size());
            }
        }
    }
    return rep;
}

const QList<Y2jbYoutubeVersion>& y2jbYoutubeVersions()
{
    static const QList<Y2jbYoutubeVersion> v = {
        {QStringLiteral("4.03"), QStringLiteral("12.40"), QStringLiteral("v01.000.003"),
         QStringLiteral("Y2JB 1.6 (Lapse up to 10.01, Y2JB's kernel exploit above)"), QStringLiteral("Gezine/Y2JB 1.6")},
        {QStringLiteral("12.60"), QStringLiteral("13.20"), QStringLiteral("v01.000.030"), QStringLiteral("Y2JB 1.6"),
         QStringLiteral("Gezine/Y2JB 1.6")},
        {QStringLiteral("13.40"), QStringLiteral("13.60"), QStringLiteral("v01.009.253"), QStringLiteral("Relapse (notmaj0r)"),
         QStringLiteral("itsPLK/ps5-y2jb-autoloader 1.1.0")},
    };
    return v;
}

QString normalizeYoutubeVersion(const QString& version)
{
    static const QRegularExpression re(QStringLiteral("^v?([0-9]{2})\\.([0-9]{3})\\.([0-9]{3})$"));
    const auto m = re.match(version.trimmed());
    return m.hasMatch() ? QStringLiteral("v%1.%2.%3").arg(m.captured(1), m.captured(2), m.captured(3)) : QString();
}

QString y2jbFirmwareRangeFor(const QString& youtubeVersion)
{
    const QString v = normalizeYoutubeVersion(youtubeVersion);
    for (const Y2jbYoutubeVersion& row : y2jbYoutubeVersions())
        if (row.youtube == v)
            return QStringLiteral("%1 \u2013 %2").arg(row.firmwareFrom, row.firmwareTo);
    return {};
}

std::optional<Y2jbRegion> y2jbRegionFromTitleId(const QString& titleId)
{
    for (const auto r : allY2jbRegions())
        if (titleId.compare(y2jbTitleId(r), Qt::CaseInsensitive) == 0)
            return r;
    return std::nullopt;
}

QString y2jbYoutubeVersionFor(const QString& firmware)
{
    // "12.6" and "12.60" are the same firmware: compare as hundredths.
    auto hundredths = [](const QString& fw) {
        const QStringList p = fw.trimmed().split(u'.');
        bool ok1 = false, ok2 = true;
        const int major = p.value(0).toInt(&ok1);
        QString minor = p.value(1, QStringLiteral("0")).left(2);
        if (minor.size() == 1)
            minor += u'0';
        const int m = minor.toInt(&ok2);
        return ok1 && ok2 ? major * 100 + m : -1;
    };
    const int f = hundredths(firmware);
    if (f < 0)
        return {};
    for (const Y2jbYoutubeVersion& v : y2jbYoutubeVersions())
        if (f >= hundredths(v.firmwareFrom) && f <= hundredths(v.firmwareTo))
            return v.youtube;
    return {};
}

} // namespace fw::ps5
