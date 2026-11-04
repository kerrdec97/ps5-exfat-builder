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
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QtEndian>

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
    QStringList problems;
    if (o.contains(QStringLiteral("contentVersion")) && o.value(QStringLiteral("contentVersion")).toString() != QLatin1String(kBlockedContentVersion))
        problems << QStringLiteral("contentVersion is %1, expected %2").arg(o.value(QStringLiteral("contentVersion")).toVariant().toString(),
                                                                             QLatin1String(kBlockedContentVersion));
    if (o.contains(QStringLiteral("versionFileUri")) && o.value(QStringLiteral("versionFileUri")).toString() != QLatin1String(kBlockedVersionFileUri))
        problems << QStringLiteral("versionFileUri is %1, expected %2").arg(o.value(QStringLiteral("versionFileUri")).toVariant().toString(),
                                                                             QLatin1String(kBlockedVersionFileUri));
    if (!problems.isEmpty())
        return {false, problems.join(QStringLiteral("; "))};
    return {true, QStringLiteral("values confirmed")};
}

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
                back.open(QIODevice::ReadOnly);
                const PatchCheck v = verifyParamJsonText(back.readAll());
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
    if (!c.upload(backupPath, original).ok())
        log.warn(QStringLiteral("[Y2JB] Could not write the remote backup %1 (the local backup is still there).").arg(backupPath));
}

Y2jbFileResult patchDbOverFtp(FtpClient& c, const QString& remote, const QString& label, const QString& tid,
                              DbPatch patch, DbVerify verify, const QString& backupDir, const CancellationToken& token,
                              const Log& log, QStringList& verifyFailures)
{
    Y2jbFileResult r{label, remote, {}, {}, {}};
    auto bytes = c.download(remote, token);
    if (!bytes.ok()) {
        if (bytes.error().code == ErrorCode::NotFound && c.isConnected()) {
            log.info(QStringLiteral("[Y2JB] Not found on PS5: %1").arg(label));
            r.status = QStringLiteral("○");
            r.detail = QStringLiteral("not found on PS5");
        } else {
            r.status = QStringLiteral("!");
            r.detail = bytes.error().message;
        }
        return r;
    }
    log.info(QStringLiteral("[Y2JB] Downloaded %1 (%2 bytes)").arg(label).arg(bytes.value().size()));
    auto local = saveRemoteCopy(bytes.value(), remote, backupDir);
    if (!local.ok()) {
        r.status = QStringLiteral("!");
        r.detail = local.error().message;
        return r;
    }
    r.backup = local.value();
    TempDir tmp(QStringLiteral("y2jb"));
    const QString work = tmp.filePath(QStringLiteral("work.db"));
    if (auto st = writeFileAtomic(work, bytes.value()); !st.ok()) {
        r.status = QStringLiteral("!");
        r.detail = st.error().message;
        return r;
    }
    remoteBestEffortBackup(c, remote + QStringLiteral(".bak"), bytes.value(), log);
    auto rows = patch(work, tid);
    if (!rows.ok()) {
        r.status = QStringLiteral("!");
        r.detail = rows.error().message;
        return r;
    }
    if (rows.value() == 0) {
        log.warn(QStringLiteral("[Y2JB] ✗ %1: 0 rows matched %2 — NOT uploading (would be a no-op).").arg(label, tid));
        r.status = QStringLiteral("✗");
        r.detail = QStringLiteral("0 rows matched %1 (wrong region or YouTube not installed)").arg(tid);
        verifyFailures << QStringLiteral("%1: 0 rows matched %2").arg(label, tid);
        return r;
    }
    QFile pf(work);
    if (!pf.open(QIODevice::ReadOnly)) {
        r.status = QStringLiteral("!");
        r.detail = pf.errorString();
        return r;
    }
    const QByteArray patched = pf.readAll();
    pf.close();
    if (auto st = c.upload(remote, patched, token); !st.ok()) {
        r.status = QStringLiteral("!");
        r.detail = st.error().message;
        return r;
    }
    log.info(QStringLiteral("[Y2JB] Uploaded patched %1 (%2 rows changed)").arg(label).arg(rows.value()));
    auto back = c.download(remote, token);
    if (!back.ok()) {
        r.status = QStringLiteral("✗");
        r.detail = QStringLiteral("verify pass errored: ") + back.error().message;
        verifyFailures << label + QStringLiteral(": ") + r.detail;
        return r;
    }
    const QString vpath = tmp.filePath(QStringLiteral("verify.db"));
    (void)writeFileAtomic(vpath, back.value());
    const PatchCheck v = verify(vpath, tid);
    r.status = v.ok ? QStringLiteral("✓") : QStringLiteral("✗");
    r.detail = v.ok ? QStringLiteral("patched + verified (%1)").arg(v.reason) : QStringLiteral("VERIFY FAILED: ") + v.reason;
    if (!v.ok)
        verifyFailures << label + QStringLiteral(": ") + v.reason;
    log.info(QStringLiteral("[Y2JB] %1 %2 %3").arg(r.status, label, r.detail));
    return r;
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
        auto bytes = c.download(remote, token);
        if (!bytes.ok()) {
            if (bytes.error().isCancelled())
                return bytes.takeError();
            r.status = bytes.error().code == ErrorCode::NotFound ? QStringLiteral("○") : QStringLiteral("!");
            r.detail = bytes.error().code == ErrorCode::NotFound ? QStringLiteral("not found") : bytes.error().message;
            log.info(QStringLiteral("[Y2JB] Not found: %1").arg(remote));
            rep.files.push_back(r);
            continue;
        }
        auto local = saveRemoteCopy(bytes.value(), remote, backupDir);
        if (!local.ok()) {
            r.status = QStringLiteral("!");
            r.detail = local.error().message;
            rep.files.push_back(r);
            continue;
        }
        r.backup = local.value();
        auto patched = patchParamJsonText(bytes.value());
        if (!patched.ok()) {
            r.status = QStringLiteral("!");
            r.detail = patched.error().message;
            rep.files.push_back(r);
            continue;
        }
        QString backupName = remote;
        backupName.replace(QLatin1String("param.json"), QLatin1String("param_backup.json"));
        remoteBestEffortBackup(c, backupName, bytes.value(), log);
        if (patched.value() != bytes.value()) {
            if (auto st = c.upload(remote, patched.value(), token); !st.ok()) {
                r.status = QStringLiteral("!");
                r.detail = st.error().message;
                rep.files.push_back(r);
                continue;
            }
        }
        auto back = c.download(remote, token);
        const PatchCheck v = back.ok() ? verifyParamJsonText(back.value())
                                       : PatchCheck{false, QStringLiteral("verify pass errored: ") + back.error().message};
        r.status = v.ok ? QStringLiteral("✓") : QStringLiteral("✗");
        r.detail = v.ok ? QStringLiteral("patched + verified at %1").arg(remote) : QStringLiteral("VERIFY FAILED: ") + v.reason;
        if (!v.ok)
            rep.verifyFailures << QStringLiteral("param.json @ %1: %2").arg(remote, v.reason);
        log.info(QStringLiteral("[Y2JB] %1 param.json %2").arg(r.status, r.detail));
        rep.files.push_back(r);
    }
    rep.files.push_back(patchDbOverFtp(c, QStringLiteral("/system_data/priv/mms/appinfo.db"), QStringLiteral("appinfo.db"),
                                       tid, &patchAppInfoDb, &verifyAppInfoDb, backupDir, token, log, rep.verifyFailures));
    if (!token.checkpoint())
        return Error::cancelled();
    rep.files.push_back(patchDbOverFtp(c, QStringLiteral("/system_data/priv/mms/app.db"), QStringLiteral("app.db"), tid,
                                       &patchAppDb, &verifyAppDb, backupDir, token, log, rep.verifyFailures));
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

} // namespace fw::ps5
