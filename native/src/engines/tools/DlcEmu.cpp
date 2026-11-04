#include "engines/tools/DlcEmu.h"

#include "core/FileUtil.h"
#include "core/OrderedJson.h"
#include "engines/gameinfo/GameInfo.h"
#include "engines/gameinfo/Sfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace fw::tools::dlcemu {

namespace {

const QRegularExpression& alnumRe()
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9]+$"));
    return re;
}
const QRegularExpression& hex32Re()
{
    static const QRegularExpression re(QStringLiteral("^[0-9A-Fa-f]{32}$"));
    return re;
}
const QRegularExpression& sectionRe()
{
    static const QRegularExpression re(QStringLiteral("^\\[([A-Za-z0-9_]+)\\]$"));
    return re;
}

int codePoints(const QString& s)
{
    return int(s.toUcs4().size());
}

QString unifiedLabel(const Entry& e)
{
    for (const QString& k : {QStringLiteral("label"), QStringLiteral("entitlement_label")}) {
        const QString v = e.get(k);
        if (!v.isEmpty())
            return v;
    }
    const QString cid = e.get(QStringLiteral("content_id"));
    return cid.size() >= 16 ? cid.right(16) : cid;
}

QStringList validateEntry(const Entry& e, const QString& app0, QStringList* warnings)
{
    QStringList errors;
    const QString sec = e.section.toUpper();
    if (!sectionTypes().contains(sec))
        errors << QStringLiteral("Unknown section type [%1].").arg(e.section);
    const QString cid = e.get(QStringLiteral("content_id"));
    if (cid.isEmpty())
        errors << QStringLiteral("content_id is required.");
    else if (codePoints(cid) != 36)
        errors << QStringLiteral("content_id must be exactly 36 characters (got %1).").arg(codePoints(cid));
    for (const QString& k : {QStringLiteral("label"), QStringLiteral("entitlement_label")}) {
        const QString v = e.get(k);
        if (!v.isEmpty() && !(codePoints(v) >= 1 && codePoints(v) <= 16 && alnumRe().match(v).hasMatch()))
            errors << QStringLiteral("%1 must be 1-16 alphanumeric characters.").arg(k);
    }
    const QString sl = e.get(QStringLiteral("service_label"));
    if (!sl.isEmpty() && !(codePoints(sl) <= 6 && alnumRe().match(sl).hasMatch()))
        errors << QStringLiteral("service_label must be 1-6 alphanumeric characters.");
    const QString mp = e.get(QStringLiteral("mount_point"));
    if (!mp.isEmpty()) {
        if (codePoints(mp) > 15)
            errors << QStringLiteral("mount_point must be at most 15 characters (got %1).").arg(codePoints(mp));
        if (!app0.isEmpty() && warnings) {
            QString rel = mp;
            if (rel.startsWith(QStringLiteral("/app0/")))
                rel = rel.mid(6);
            if (!QFileInfo(QDir(app0).filePath(rel)).isDir())
                *warnings << QStringLiteral("mount_point \"%1\" does not exist locally (it may exist on the console; dlc_emu will not create it).").arg(mp);
        }
    }
    const QString key = e.get(QStringLiteral("entitlement_key"));
    if (!key.isEmpty() && !hex32Re().match(key).hasMatch())
        errors << QStringLiteral("entitlement_key must be 32 hexadecimal characters.");
    const QString ds = e.get(QStringLiteral("download_status"));
    if (!ds.isEmpty() && !downloadStatuses().contains(ds))
        errors << QStringLiteral("download_status must be one of: %1.").arg(downloadStatuses().join(QStringLiteral(", ")));
    return errors;
}

QString findParamJson(const QString& folder)
{
    for (const QString& c : {QDir(folder).filePath(QStringLiteral("sce_sys/param.json")), QDir(folder).filePath(QStringLiteral("param.json"))})
        if (QFileInfo(c).isFile())
            return c;
    return {};
}

void harvest(const json::Value& v, QStringList& out)
{
    if (v.isObject()) {
        for (const QString& k : {QStringLiteral("contentId"), QStringLiteral("content_id")}) {
            const json::Value& c = v.value(k);
            if (c.isString() && !c.toString().isEmpty()) {
                out << c.toString();
                break;
            }
        }
        for (const QString& k : v.keys())
            harvest(v.value(k), out);
    } else if (v.isArray()) {
        for (int i = 0; i < v.size(); ++i)
            harvest(v.at(i), out);
    }
}

QString sfoContentId(const QString& folder)
{
    for (const QString& c : {QDir(folder).filePath(QStringLiteral("sce_sys/param.sfo")), QDir(folder).filePath(QStringLiteral("param.sfo"))}) {
        if (!QFileInfo(c).isFile())
            continue;
        auto sfo = gameinfo::readSfo(c);
        if (!sfo.ok())
            return {};
        for (const QString& k : {QStringLiteral("CONTENT_ID"), QStringLiteral("CONTENT_ID_00")}) {
            const QString v = sfo->string(k).trimmed();
            if (v.size() == 36)
                return v;
        }
        return {};
    }
    return {};
}

} // namespace

const QStringList& sectionTypes()
{
    static const QStringList s = {QStringLiteral("NONE"), QStringLiteral("PSGD"), QStringLiteral("PSAC"), QStringLiteral("PSAL"),
                                  QStringLiteral("PSCONS"), QStringLiteral("PSVC"), QStringLiteral("PSSUBS")};
    return s;
}

const QStringList& downloadStatuses()
{
    static const QStringList s = {QStringLiteral("NO_EXTRA_DATA"), QStringLiteral("NO_IN_QUEUE"), QStringLiteral("DOWNLOADING"),
                                  QStringLiteral("DOWNLOAD_SUSPENDED"), QStringLiteral("INSTALLED")};
    return s;
}

const QStringList& paramKeys()
{
    static const QStringList s = {QStringLiteral("content_id"), QStringLiteral("label"), QStringLiteral("entitlement_label"),
                                  QStringLiteral("download_status"), QStringLiteral("mount_point"), QStringLiteral("entitlement_key"),
                                  QStringLiteral("service_label"), QStringLiteral("active_flag"), QStringLiteral("active_date"),
                                  QStringLiteral("inactive_date"), QStringLiteral("use_count"), QStringLiteral("use_limit"),
                                  QStringLiteral("consumable"), QStringLiteral("is_consumable")};
    return s;
}

QString Entry::get(const QString& key) const
{
    for (const auto& [k, v] : params)
        if (k == key)
            return v;
    return {};
}

void Entry::set(const QString& key, const QString& value)
{
    for (auto& [k, v] : params)
        if (k == key) {
            v = value;
            return;
        }
    params.emplace_back(key, value);
}

std::vector<Entry> parse(const QString& text)
{
    std::vector<Entry> entries;
    Entry* cur = nullptr;
    static const QRegularExpression lineBreak(QStringLiteral("\\r\\n|[\\n\\r\\x0b\\x0c\\x1c\\x1d\\x1e\\x85\\x{2028}\\x{2029}]"));
    for (const QString& raw : text.split(lineBreak)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';'))
            continue;
        const auto m = sectionRe().match(line);
        if (m.hasMatch()) {
            Entry e;
            e.section = m.captured(1).toUpper();
            entries.push_back(e);
            cur = &entries.back();
            continue;
        }
        if (!cur)
            continue;
        const int eq = line.indexOf(u'=');
        if (eq >= 0)
            cur->set(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return entries;
}

QString render(const std::vector<Entry>& entries, const QString& newline)
{
    QStringList blocks;
    for (const Entry& e : entries) {
        QStringList lines{QStringLiteral("[%1]").arg(e.section)};
        QSet<QString> seen;
        for (const QString& k : paramKeys()) {
            const QString v = e.get(k);
            bool present = false;
            for (const auto& p : e.params)
                if (p.first == k)
                    present = true;
            if (present && !v.isEmpty()) {
                lines << k + u'=' + v;
                seen.insert(k);
            }
        }
        for (const auto& [k, v] : e.params)
            if (!seen.contains(k) && !v.isEmpty())
                lines << k + u'=' + v;
        blocks << lines.join(newline);
    }
    QString text = blocks.join(newline + newline);
    if (!blocks.isEmpty())
        text += newline;
    return text;
}

int byteSize(const std::vector<Entry>& entries)
{
    return int(render(entries).toUtf8().size());
}

Validation validate(const std::vector<Entry>& entries, const QString& app0)
{
    Validation v;
    if (int(entries.size()) > kMaxEntries)
        v.errors << QStringLiteral("Too many entries: %1 (max %2).").arg(entries.size()).arg(kMaxEntries);
    const int size = byteSize(entries);
    if (size > kMaxIniBytes)
        v.errors << QStringLiteral("INI is %1 bytes, over the 32 KiB limit (%2).").arg(size).arg(kMaxIniBytes);
    QHash<QString, int> seenUnified, seenService;
    for (int i = 0; i < int(entries.size()); ++i) {
        const Entry& e = entries[size_t(i)];
        const QString tag = QStringLiteral("Entry %1 (%2)").arg(i + 1).arg(e.section);
        QStringList warns;
        for (const QString& m : validateEntry(e, app0, &warns))
            v.errors << tag + QStringLiteral(": ") + m;
        for (const QString& m : warns)
            v.warnings << tag + QStringLiteral(": ") + m;
        const QString ul = unifiedLabel(e);
        if (!ul.isEmpty()) {
            if (seenUnified.contains(ul))
                v.warnings << QStringLiteral("%1: duplicate unified label \"%2\" (also Entry %3) — the module skips duplicates.")
                                  .arg(tag, ul).arg(seenUnified.value(ul) + 1);
            else
                seenUnified.insert(ul, i);
        }
        const QString sl = e.get(QStringLiteral("service_label"));
        if (!sl.isEmpty()) {
            if (seenService.contains(sl))
                v.warnings << QStringLiteral("%1: duplicate service_label \"%2\" (also Entry %3) — the module skips duplicates.")
                                  .arg(tag, sl).arg(seenService.value(sl) + 1);
            else
                seenService.insert(sl, i);
        }
    }
    return v;
}

Result<std::vector<Entry>> load(const QString& app0)
{
    const QString path = QDir(app0).filePath(QStringLiteral("dlc_emu.ini"));
    if (!QFileInfo::exists(path))
        return std::vector<Entry>{};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read dlc_emu.ini."), f.errorString());
    if (f.size() > 1024 * 1024)
        return Error(ErrorCode::CorruptData, QStringLiteral("dlc_emu.ini is far too large to be a DLC list."));
    return parse(QString::fromUtf8(f.readAll()));
}

Status save(const QString& app0, const std::vector<Entry>& entries)
{
    if (!QFileInfo(app0).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The game folder %1 does not exist.").arg(QDir::toNativeSeparators(app0)));
    const Validation v = validate(entries);
    if (!v.ok())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("dlc_emu.ini was not saved: %1").arg(v.errors.first()),
                     v.errors.join(QStringLiteral("; ")));
    const QString path = QDir(app0).filePath(QStringLiteral("dlc_emu.ini"));
    if (QFileInfo::exists(path)) {
        const QString bak = uniquePath(path + QStringLiteral(".bak"));
        if (!QFile::copy(path, bak))
            return Error(ErrorCode::IoError, QStringLiteral("Could not back up the existing dlc_emu.ini, so it was not changed."));
    }
    return writeFileAtomic(path, render(entries).toUtf8());
}

bool looksLikeBaseGameId(const QString& contentId)
{
    if (contentId.isEmpty() || !contentId.contains(u'-'))
        return false;
    const QString suffix = contentId.section(u'-', -1);
    if (suffix.size() != 16)
        return false;
    int zeros = 0;
    for (int i = suffix.size() - 1; i >= 0 && suffix[i] == u'0'; --i)
        ++zeros;
    return zeros >= 8;
}

QStringList detectAddcontFolders(const QString& app0)
{
    static const QRegularExpression re(QStringLiteral("^addcont\\d*$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression digits(QStringLiteral("(\\d+)$"));
    QStringList names;
    for (const QFileInfo& fi : QDir(app0).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (re.match(fi.fileName()).hasMatch())
            names << fi.fileName();
    std::stable_sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        const auto ma = digits.match(a), mb = digits.match(b);
        const std::pair<int, qint64> ka = ma.hasMatch() ? std::pair<int, qint64>{0, ma.captured(1).toLongLong()} : std::pair<int, qint64>{1, 0};
        const std::pair<int, qint64> kb = mb.hasMatch() ? std::pair<int, qint64>{0, mb.captured(1).toLongLong()} : std::pair<int, qint64>{1, 0};
        return ka < kb;
    });
    QStringList out;
    for (const QString& n : names)
        out << QStringLiteral("/app0/") + n;
    return out;
}

QStringList paramJsonContentIds(const QString& app0)
{
    QStringList raw;
    const QString path = findParamJson(app0);
    if (path.isEmpty())
        return raw;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return raw;
    json::ParseOptions opt;
    opt.allowComments = true;
    opt.allowTrailingCommas = true;
    auto doc = json::parse(f.readAll(), opt);
    if (!doc.ok())
        return raw;
    if (doc->isObject() && doc->value(QStringLiteral("disc")).isArray())
        harvest(doc->value(QStringLiteral("disc")), raw);
    harvest(doc.value(), raw);
    QStringList out;
    for (QString c : raw) {
        c = c.trimmed();
        if (!c.isEmpty() && !out.contains(c))
            out << c;
    }
    return out;
}

std::pair<QString, bool> detectContentPrefix(const QString& app0)
{
    const QString cid = sfoContentId(app0);
    if (cid.size() >= 20 && cid.contains(u'-'))
        return {cid.left(cid.lastIndexOf(u'-') + 1), false};
    for (const QString& c : paramJsonContentIds(app0))
        if (c.size() >= 20 && c.contains(u'-'))
            return {c.left(c.lastIndexOf(u'-') + 1), false};
    QString ppsa = QStringLiteral("PPSA00000");
    if (auto meta = gameinfo::readGameMetadata(app0); meta.ok() && !meta->titleId.isEmpty())
        ppsa = meta->titleId;
    return {QStringLiteral("UP9000-%1_00-").arg(ppsa), true};
}

std::vector<Entry> scanBinaryForCodes(const QString& path, const QString& contentPrefix)
{
    std::vector<Entry> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray data = f.readAll();
    QSet<QString> seen;
    const auto isCodeChar = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    const qsizetype n = data.size();
    for (qsizetype i = 0; i + 16 <= n; ++i) {
        if (i > 0 && data[i - 1] != '\0')
            continue;
        bool ok = true;
        for (int k = 0; k < 16 && ok; ++k)
            ok = isCodeChar(data[i + k]);
        if (!ok || (i + 16 < n && data[i + 16] != '\0'))
            continue;
        const QString code = QString::fromLatin1(data.constData() + i, 16);
        if (code == u"0123456789ABCDEF" || seen.contains(code))
            continue;
        seen.insert(code);
        Entry e;
        e.set(QStringLiteral("content_id"), contentPrefix + code);
        out.push_back(e);
        i += 16;
    }
    return out;
}

Suggestions autoFind(const QString& app0, bool scanEboot, const std::vector<Entry>& existing)
{
    Suggestions s;
    QSet<QString> seen;
    for (const Entry& e : existing)
        if (!e.get(QStringLiteral("content_id")).isEmpty())
            seen.insert(e.get(QStringLiteral("content_id")));
    const auto [prefix, placeholder] = detectContentPrefix(app0);
    s.prefix = prefix;
    s.prefixIsPlaceholder = placeholder;
    const QString baseCid = sfoContentId(app0);
    QSet<QString> baseIds;
    if (!baseCid.isEmpty()) {
        seen.insert(baseCid);
        baseIds.insert(baseCid);
    }
    s.addcont = detectAddcontFolders(app0);

    if (auto current = load(app0); current.ok())
        for (Entry e : current.value()) {
            const QString c = e.get(QStringLiteral("content_id"));
            if (!c.isEmpty() && !seen.contains(c)) {
                seen.insert(c);
                e.confidence = QStringLiteral("high");
                e.source = QStringLiteral("existing dlc_emu.ini");
                s.entries.push_back(e);
            }
        }

    int ai = 0;
    for (const QString& c : paramJsonContentIds(app0)) {
        if (seen.contains(c))
            continue;
        if (looksLikeBaseGameId(c) || (!baseCid.isEmpty() && c == baseCid)) {
            seen.insert(c);
            baseIds.insert(c);
            continue;
        }
        seen.insert(c);
        Entry e;
        e.set(QStringLiteral("content_id"), c);
        if (ai < s.addcont.size()) {
            e.set(QStringLiteral("mount_point"), s.addcont[ai]);
            e.set(QStringLiteral("download_status"), QStringLiteral("INSTALLED"));
            ++ai;
        }
        e.confidence = QStringLiteral("high");
        e.source = QStringLiteral("param.json contentId");
        s.entries.push_back(e);
    }
    for (int idx = ai; idx < s.addcont.size(); ++idx) {
        const QString synth = prefix + QStringLiteral("ADDCONT%1").arg(idx, 9, 10, QLatin1Char('0')).left(16);
        if (seen.contains(synth))
            continue;
        seen.insert(synth);
        Entry e;
        e.set(QStringLiteral("content_id"), synth);
        e.set(QStringLiteral("mount_point"), s.addcont[idx]);
        e.set(QStringLiteral("download_status"), QStringLiteral("INSTALLED"));
        e.placeholder = true;
        e.confidence = QStringLiteral("medium");
        e.source = QStringLiteral("matched to %1").arg(s.addcont[idx]);
        s.entries.push_back(e);
    }
    if (scanEboot) {
        QString root = app0;
        const auto roots = gameinfo::findGameRoots(app0);
        if (!roots.root.isEmpty())
            root = roots.root;
        const QString eboot = QDir(root).filePath(QStringLiteral("eboot.bin"));
        if (QFileInfo(eboot).isFile())
            for (Entry e : scanBinaryForCodes(eboot, prefix)) {
                const QString c = e.get(QStringLiteral("content_id"));
                if (c.isEmpty() || seen.contains(c))
                    continue;
                seen.insert(c);
                if (looksLikeBaseGameId(c)) {
                    baseIds.insert(c);
                    continue;
                }
                e.placeholder = placeholder;
                e.confidence = QStringLiteral("low");
                e.source = QStringLiteral("eboot.bin scan");
                s.entries.push_back(e);
            }
    }
    s.baseIds = baseIds.values();
    s.baseIds.sort();
    if (s.entries.empty()) {
        if (!s.baseIds.isEmpty())
            s.notes << QStringLiteral("No DLC entries found locally. This dump only contains the base game content ID (%1), not add-on content. Import a DLC-emu pack if you have one, or look up the title's DLC content IDs and add them manually.")
                           .arg(s.baseIds.first());
        else
            s.notes << QStringLiteral("No local DLC metadata found. You can add entries manually.");
    }
    return s;
}

} // namespace fw::tools::dlcemu
