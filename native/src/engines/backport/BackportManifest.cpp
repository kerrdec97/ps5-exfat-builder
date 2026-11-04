#include "engines/backport/BackportManifest.h"

#include "core/BuildInfo.h"
#include "core/FileUtil.h"
#include "engines/backport/Common.h"
#include "engines/backport/SdkTable.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fw::backport {

namespace {

using fw::json::Value;

QString now()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

const QStringList& execExts()
{
    static const QStringList e = {QStringLiteral(".bin"), QStringLiteral(".elf"), QStringLiteral(".self"),
                                  QStringLiteral(".sprx"), QStringLiteral(".prx")};
    return e;
}

bool hasExecExt(const QString& lowRel)
{
    for (const QString& e : execExts())
        if (lowRel.endsWith(e))
            return true;
    return false;
}

QMap<QString, QString> stringMap(const Value& obj)
{
    QMap<QString, QString> out;
    if (!obj.isObject())
        return out;
    for (const QString& k : obj.keys())
        out.insert(k, obj.value(k).toString());
    return out;
}

Value objectOf(const QMap<QString, QString>& m)
{
    Value o = Value::object();
    for (auto it = m.cbegin(); it != m.cend(); ++it)
        o.insert(it.key(), Value(it.value()));
    return o;
}

Value arrayOf(const QStringList& l)
{
    Value a = Value::array();
    for (const QString& s : l)
        a.append(Value(s));
    return a;
}

} // namespace

int Manifest::schemaVersion() const
{
    return int(doc.value(QStringLiteral("schema_version")).toInt(1));
}

QString Manifest::titleId() const
{
    return doc.value(QStringLiteral("title_id")).toString();
}

std::optional<QString> Manifest::currentTarget() const
{
    const Value& v = doc.value(QStringLiteral("current_target"));
    if (!v.isString() || v.toString().isEmpty())
        return std::nullopt;
    return v.toString();
}

QString Manifest::originalsZipAbs() const
{
    return doc.path(QStringLiteral("originals.zip_abs")).toString();
}

QString Manifest::originalsZipSha256() const
{
    return doc.path(QStringLiteral("originals.zip_sha256")).toString();
}

QMap<QString, QString> Manifest::originalsFiles() const
{
    return stringMap(doc.path(QStringLiteral("originals.files")));
}

QMap<QString, QString> Manifest::postHashes() const
{
    QMap<QString, QString> out;
    const Value& targets = doc.value(QStringLiteral("targets"));
    if (!targets.isObject())
        return out;
    for (const QString& k : targets.keys()) {
        const auto m = stringMap(targets.value(k).value(QStringLiteral("post_hashes")));
        for (auto it = m.cbegin(); it != m.cend(); ++it)
            out.insert(it.key(), it.value());
    }
    return out;
}

QStringList Manifest::addedFiles() const
{
    QStringList out;
    const Value& a = doc.value(QStringLiteral("added_files"));
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).isString())
            out << a.at(i).toString();
    return out;
}

QMap<QString, QString> Manifest::addedFileHashes() const
{
    QMap<QString, QString> out;
    const Value& after = doc.value(QStringLiteral("files_after"));
    for (const QString& rel : addedFiles())
        out.insert(rel, after.value(rel).value(QStringLiteral("sha256")).toString());
    return out;
}

bool Manifest::applied() const
{
    return doc.value(QStringLiteral("applied")).toBool(false);
}

QString manifestPathFor(const QString& originalsDir, const QString& titleId)
{
    return QDir(QDir(originalsDir).filePath(QStringLiteral("BackportBackup"))).filePath(titleId + QStringLiteral("/manifest.json"));
}

Result<std::optional<Manifest>> readManifest(const QString& path)
{
    if (path.isEmpty() || !QFileInfo(path).isFile())
        return std::optional<Manifest>();
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return makeError(Code::Unreadable, QStringLiteral("The backport manifest could not be read."), f.errorString());
    auto v = fw::json::parse(f.readAll());
    if (!v.ok() || !v->isObject())
        return makeError(Code::Internal,
                         QStringLiteral("The backport manifest %1 is damaged.").arg(QDir::toNativeSeparators(path)),
                         v.ok() ? QStringLiteral("not an object") : v.error().toLogString());
    Manifest m;
    m.path = path;
    m.doc = v.value();
    return std::optional<Manifest>(m);
}

Status writeManifest(const Manifest& manifest)
{
    fw::json::WriteOptions o;
    o.indent = 2;
    o.ensureAscii = true; // like Python's json.dump default
    return writeFileAtomic(manifest.path, fw::json::write(manifest.doc, o));
}

Manifest newManifest(const QString& path, const QString& game, const QString& titleId, const QString& title)
{
    Manifest m;
    m.path = path;
    const QString t = now();
    m.doc.insert(QStringLiteral("schema_version"), Value(kManifestSchemaWritten));
    m.doc.insert(QStringLiteral("title_id"), Value(titleId));
    m.doc.insert(QStringLiteral("title"), Value(title));
    m.doc.insert(QStringLiteral("game_folder"), Value(QDir::toNativeSeparators(QDir::cleanPath(game))));
    m.doc.insert(QStringLiteral("tool_version"), Value(QString::fromLatin1(fw::build::kVersion)));
    m.doc.insert(QStringLiteral("created_at"), Value(t));
    m.doc.insert(QStringLiteral("updated_at"), Value(t));
    m.doc.insert(QStringLiteral("current_target"), Value());
    m.doc.insert(QStringLiteral("originals"), Value());
    m.doc.insert(QStringLiteral("targets"), Value::object());
    m.doc.insert(QStringLiteral("history"), Value::array());
    return m;
}

void addHistory(Manifest& m, const QString& action, const std::optional<QString>& target)
{
    Value h = Value::object();
    h.insert(QStringLiteral("action"), Value(action));
    h.insert(QStringLiteral("target"), target ? Value(*target) : Value());
    h.insert(QStringLiteral("timestamp"), Value(now()));
    h.insert(QStringLiteral("tool_version"), Value(QString::fromLatin1(fw::build::kVersion)));
    if (!m.doc.value(QStringLiteral("history")).isArray())
        m.doc.insert(QStringLiteral("history"), Value::array());
    m.doc[QStringLiteral("history")].append(h);
    m.doc.insert(QStringLiteral("updated_at"), Value(now()));
}

void recordOriginals(Manifest& m, const OriginalsRecord& o)
{
    Value v = Value::object();
    v.insert(QStringLiteral("zip"), Value(QDir::toNativeSeparators(o.zipAbs)));
    v.insert(QStringLiteral("zip_abs"), Value(QDir::toNativeSeparators(o.zipAbs)));
    v.insert(QStringLiteral("zip_sha256"), Value(o.zipSha256));
    v.insert(QStringLiteral("files"), objectOf(o.files));
    v.insert(QStringLiteral("snapshot_at"), Value(now()));
    m.doc.insert(QStringLiteral("originals"), v);
}

void recordTarget(Manifest& m, const TargetRecord& t)
{
    const QString key = targetKey(t.sdk);
    m.doc.insert(QStringLiteral("schema_version"), Value(kManifestSchemaWritten));
    m.doc.insert(QStringLiteral("tool_version"), Value(QString::fromLatin1(fw::build::kVersion)));

    Value after = Value::object();
    for (auto it = t.filesAfter.cbegin(); it != t.filesAfter.cend(); ++it) {
        Value f = Value::object();
        f.insert(QStringLiteral("sha256"), Value(it.value().first));
        f.insert(QStringLiteral("size"), Value(it.value().second));
        after.insert(it.key(), f);
    }

    Value target = Value::object();
    target.insert(QStringLiteral("built_at"), Value(now()));
    target.insert(QStringLiteral("tool_version"), Value(QString::fromLatin1(fw::build::kVersion)));
    target.insert(QStringLiteral("post_hashes"), objectOf(t.postHashes));
    target.insert(QStringLiteral("applied"), Value(t.applied));
    target.insert(QStringLiteral("files_after"), after);
    if (!m.doc.value(QStringLiteral("targets")).isObject())
        m.doc.insert(QStringLiteral("targets"), Value::object());
    m.doc[QStringLiteral("targets")].insert(key, target);

    Value engine = Value::object();
    engine.insert(QStringLiteral("name"), Value(kEngineName));
    engine.insert(QStringLiteral("version"), Value(kEngineVersion));
    engine.insert(QStringLiteral("sdk_table_hash"), Value(QString::fromLatin1(tableHash())));
    m.doc.insert(QStringLiteral("engine"), engine);
    if (!t.notes.isEmpty())
        m.doc.insert(QStringLiteral("notes"), Value(t.notes));
    if (t.applied) {
        m.doc.insert(QStringLiteral("current_target"), Value(key));
        m.doc.insert(QStringLiteral("applied"), Value(true));
        m.doc.insert(QStringLiteral("added_files"), arrayOf(t.addedFiles));
        m.doc.insert(QStringLiteral("files_after"), after);
        m.doc.insert(QStringLiteral("journal"), Value(t.journal));
        addHistory(m, QStringLiteral("apply_target"), key);
    } else {
        if (!m.doc.contains(QStringLiteral("applied")))
            m.doc.insert(QStringLiteral("applied"), Value(false));
        addHistory(m, QStringLiteral("build_target"), key);
    }
}

void recordRestore(Manifest& m, const QString& zip)
{
    m.doc.insert(QStringLiteral("current_target"), Value());
    m.doc.insert(QStringLiteral("applied"), Value(false));
    m.doc.insert(QStringLiteral("added_files"), Value::array());
    addHistory(m, QStringLiteral("restore"), std::nullopt);
    m.doc.insert(QStringLiteral("last_restore_zip"), Value(QDir::toNativeSeparators(zip)));
}

QString verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Unknown: return QStringLiteral("unknown");
    case Verdict::Patched: return QStringLiteral("patched");
    case Verdict::Differs: return QStringLiteral("differs");
    case Verdict::Original: return QStringLiteral("original");
    }
    return QStringLiteral("unknown");
}

std::pair<Verdict, QStringList> manifestVerdict(const ScanResult& scan, const Manifest* manifest,
                                                const CancellationToken& token)
{
    if (!manifest)
        return {Verdict::Unknown, {}};
    const auto current = [&](const QString& key) -> QString {
        const QString rel = scan.resolve(key);
        if (rel.isEmpty())
            return {};
        if (const ExecFile* e = scan.find(rel); e && !e->sha256.isEmpty())
            return e->sha256;
        auto h = sha256File(joinPath(scan.root, rel), token);
        return h.ok() ? h.value() : QString();
    };

    QMap<QString, QString> patched;
    const auto post = manifest->postHashes();
    for (auto it = post.cbegin(); it != post.cend(); ++it)
        patched.insert(relKey(it.key()), it.value().toLower());
    QStringList hits;
    for (auto it = patched.cbegin(); it != patched.cend(); ++it) {
        if (it.key().startsWith(QStringLiteral("fakelib/")))
            continue;
        const QString cur = current(it.key());
        if (!cur.isEmpty() && cur == it.value())
            hits << it.key();
    }
    if (!hits.isEmpty())
        return {Verdict::Patched, hits};

    QMap<QString, QString> originals;
    const auto files = manifest->originalsFiles();
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QString k = relKey(it.key());
        if (!hasExecExt(k) || k.startsWith(QStringLiteral("decrypted/")) || k.startsWith(QStringLiteral("fakelib/")))
            continue;
        originals.insert(k, it.value().toLower());
    }
    QStringList differs;
    int same = 0;
    for (auto it = originals.cbegin(); it != originals.cend(); ++it) {
        const QString cur = current(it.key());
        if (cur.isEmpty())
            continue;
        if (cur == it.value())
            ++same;
        else
            differs << it.key();
    }
    if (!differs.isEmpty())
        return {Verdict::Differs, differs};
    return {same > 0 ? Verdict::Original : Verdict::Unknown, {}};
}

} // namespace fw::backport
