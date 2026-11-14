#include "engines/tools/LanguageStripper.h"

#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Zip.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>
#include <map>

namespace fw::tools {

namespace {

const QSet<QString>& validCodes()
{
    static const QSet<QString> s = {
        QStringLiteral("af"), QStringLiteral("ar"), QStringLiteral("bg"), QStringLiteral("ca"), QStringLiteral("cs"),
        QStringLiteral("cy"), QStringLiteral("da"), QStringLiteral("de"), QStringLiteral("el"), QStringLiteral("en"),
        QStringLiteral("es"), QStringLiteral("et"), QStringLiteral("eu"), QStringLiteral("fi"), QStringLiteral("fr"),
        QStringLiteral("gl"), QStringLiteral("he"), QStringLiteral("hr"), QStringLiteral("hu"), QStringLiteral("id"),
        QStringLiteral("it"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("lt"), QStringLiteral("lv"),
        QStringLiteral("ms"), QStringLiteral("nb"), QStringLiteral("nl"), QStringLiteral("no"), QStringLiteral("pl"),
        QStringLiteral("pt"), QStringLiteral("ro"), QStringLiteral("ru"), QStringLiteral("sk"), QStringLiteral("sl"),
        QStringLiteral("sq"), QStringLiteral("sr"), QStringLiteral("sv"), QStringLiteral("th"), QStringLiteral("tr"),
        QStringLiteral("uk"), QStringLiteral("vi"), QStringLiteral("zh"),
    };
    return s;
}

const QSet<QString>& containerDirs()
{
    static const QSet<QString> s = {
        QStringLiteral("vo"), QStringLiteral("fmv"), QStringLiteral("i18n"), QStringLiteral("l10n"), QStringLiteral("lang"),
        QStringLiteral("text"), QStringLiteral("audio"), QStringLiteral("movie"), QStringLiteral("video"),
        QStringLiteral("cinema"), QStringLiteral("locale"), QStringLiteral("movies"), QStringLiteral("speech"),
        QStringLiteral("videos"), QStringLiteral("voices"), QStringLiteral("locales"), QStringLiteral("strings"),
        QStringLiteral("language"), QStringLiteral("cutscenes"), QStringLiteral("languages"), QStringLiteral("subtitles"),
        QStringLiteral("translation"), QStringLiteral("localization"), QStringLiteral("translations"),
    };
    return s;
}

const QSet<QString>& skipDirs()
{
    static const QSet<QString> s = {QStringLiteral("sce_sys"), QStringLiteral("sce_module"), QStringLiteral("decrypted"),
                                    QStringLiteral("fakelib"), QStringLiteral("at9tool"), QStringLiteral("scelocale")};
    return s;
}

QString codeAlternation()
{
    QStringList codes = validCodes().values();
    std::sort(codes.begin(), codes.end());
    return codes.join(u'|');
}

struct Patterns {
    QRegularExpression locale{QStringLiteral("^(?:[a-z]{2}-[A-Z]{2}|[a-z]{2}-[A-Z][a-z]+-[A-Z]{2}|[a-z]{2}-[A-Z][a-z]+|[a-z]{2}-\\d{3}|[a-z]{2})$")};
    QRegularExpression region{QStringLiteral("^[a-z]{2}-[A-Z]{2}$")};
    QRegularExpression pak, psarc, video, audio;
    Patterns()
    {
        const QString c = codeAlternation();
        const auto ci = QRegularExpression::CaseInsensitiveOption;
        pak = QRegularExpression(QStringLiteral("(?:_lang_(%1)|_(%1)_[Pp]\\.pak|[_\\-](%1)(?=\\.pak)|pakchunk\\d+.*?[_\\-](%1)[_\\-\\.])").arg(c), ci);
        psarc = QRegularExpression(QStringLiteral("(?:audio|speech|voice|vo|sub|subtitle|text|string|loc|lang|movie|video)[_\\-](%1)(?:_\\w+)?\\.psarc$").arg(c), ci);
        // (Only for the extensions the scan passes on: see the lists in scanLanguages().)
        video = QRegularExpression(QStringLiteral("[_\\-](%1)[_\\-]?(?:sub|dub|audio|voice)?\\.(?:mp4|m4v|webm|mkv|avi|mov|m2ts|ts)$").arg(c), ci);
        audio = QRegularExpression(QStringLiteral("[_\\-](%1)[_\\-]?\\.(?:bnk|wem|pck|at9|fsb|bank)$").arg(c), ci);
    }
};

const Patterns& patterns()
{
    static const Patterns p;
    return p;
}

QString firstCapture(const QRegularExpressionMatch& m)
{
    for (int i = 1; i <= m.lastCapturedIndex(); ++i)
        if (!m.captured(i).isEmpty())
            return m.captured(i).toLower();
    return {};
}

void measure(const QString& abs, LanguageItem& item)
{
    const QFileInfo fi(abs);
    if (fi.isFile()) {
        item.bytes = fi.size();
        item.files = 1;
        return;
    }
    QDirIterator it(abs, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        item.bytes += it.fileInfo().size();
        ++item.files;
    }
}

constexpr const char* kManifestName = "farewell-language-strip.json";

} // namespace

QString languageName(const QString& codeOrLocale)
{
    static const std::map<QString, QString> names = {
        {QStringLiteral("af"), QStringLiteral("Afrikaans")}, {QStringLiteral("ar"), QStringLiteral("Arabic")},
        {QStringLiteral("bg"), QStringLiteral("Bulgarian")}, {QStringLiteral("ca"), QStringLiteral("Catalan")},
        {QStringLiteral("cs"), QStringLiteral("Czech")}, {QStringLiteral("cy"), QStringLiteral("Welsh")},
        {QStringLiteral("da"), QStringLiteral("Danish")}, {QStringLiteral("de"), QStringLiteral("German")},
        {QStringLiteral("el"), QStringLiteral("Greek")}, {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("en-us"), QStringLiteral("English (US)")}, {QStringLiteral("en-gb"), QStringLiteral("English (UK)")},
        {QStringLiteral("es"), QStringLiteral("Spanish")}, {QStringLiteral("es-419"), QStringLiteral("Spanish (Latin America)")},
        {QStringLiteral("es-mx"), QStringLiteral("Spanish (Mexico)")}, {QStringLiteral("et"), QStringLiteral("Estonian")},
        {QStringLiteral("eu"), QStringLiteral("Basque")}, {QStringLiteral("fi"), QStringLiteral("Finnish")},
        {QStringLiteral("fr"), QStringLiteral("French")}, {QStringLiteral("fr-ca"), QStringLiteral("French (Canada)")},
        {QStringLiteral("gl"), QStringLiteral("Galician")}, {QStringLiteral("he"), QStringLiteral("Hebrew")},
        {QStringLiteral("hr"), QStringLiteral("Croatian")}, {QStringLiteral("hu"), QStringLiteral("Hungarian")},
        {QStringLiteral("id"), QStringLiteral("Indonesian")}, {QStringLiteral("it"), QStringLiteral("Italian")},
        {QStringLiteral("ja"), QStringLiteral("Japanese")}, {QStringLiteral("ko"), QStringLiteral("Korean")},
        {QStringLiteral("lt"), QStringLiteral("Lithuanian")}, {QStringLiteral("lv"), QStringLiteral("Latvian")},
        {QStringLiteral("ms"), QStringLiteral("Malay")}, {QStringLiteral("nb"), QStringLiteral("Norwegian Bokmål")},
        {QStringLiteral("nl"), QStringLiteral("Dutch")}, {QStringLiteral("no"), QStringLiteral("Norwegian")},
        {QStringLiteral("pl"), QStringLiteral("Polish")}, {QStringLiteral("pt"), QStringLiteral("Portuguese")},
        {QStringLiteral("pt-br"), QStringLiteral("Portuguese (Brazil)")}, {QStringLiteral("pt-pt"), QStringLiteral("Portuguese (Portugal)")},
        {QStringLiteral("ro"), QStringLiteral("Romanian")}, {QStringLiteral("ru"), QStringLiteral("Russian")},
        {QStringLiteral("sk"), QStringLiteral("Slovak")}, {QStringLiteral("sl"), QStringLiteral("Slovenian")},
        {QStringLiteral("sq"), QStringLiteral("Albanian")}, {QStringLiteral("sr"), QStringLiteral("Serbian")},
        {QStringLiteral("sv"), QStringLiteral("Swedish")}, {QStringLiteral("th"), QStringLiteral("Thai")},
        {QStringLiteral("tr"), QStringLiteral("Turkish")}, {QStringLiteral("uk"), QStringLiteral("Ukrainian")},
        {QStringLiteral("vi"), QStringLiteral("Vietnamese")}, {QStringLiteral("zh"), QStringLiteral("Chinese")},
        {QStringLiteral("zh-hans"), QStringLiteral("Chinese (Simplified)")}, {QStringLiteral("zh-hant"), QStringLiteral("Chinese (Traditional)")},
        {QStringLiteral("zh-cn"), QStringLiteral("Chinese (China)")}, {QStringLiteral("zh-tw"), QStringLiteral("Chinese (Taiwan)")},
    };
    const QString k = codeOrLocale.toLower();
    if (auto it = names.find(k); it != names.end())
        return it->second;
    const QString base = k.section(u'-', 0, 0);
    if (auto it = names.find(base); it != names.end())
        return k.contains(u'-') ? QStringLiteral("%1 (%2)").arg(it->second, codeOrLocale.section(u'-', 1)) : it->second;
    return codeOrLocale.toUpper();
}

QStringList LanguageScan::codes() const
{
    QStringList out;
    for (const auto& i : items)
        if (!out.contains(i.code))
            out << i.code;
    out.sort();
    return out;
}

QJsonObject LanguageScan::toJson() const
{
    QJsonArray arr;
    for (const auto& i : items)
        arr.append(QJsonObject{{QStringLiteral("path"), i.relativePath},
                               {QStringLiteral("folder"), i.isFolder},
                               {QStringLiteral("code"), i.code},
                               {QStringLiteral("language"), i.language},
                               {QStringLiteral("bytes"), i.bytes},
                               {QStringLiteral("files"), i.files}});
    return {{QStringLiteral("gameRoot"), gameRoot}, {QStringLiteral("items"), arr}, {QStringLiteral("totalBytes"), totalBytes}};
}

LanguageScan LanguageScan::fromJson(const QJsonObject& o)
{
    LanguageScan s;
    s.gameRoot = o.value(QStringLiteral("gameRoot")).toString();
    s.totalBytes = qint64(o.value(QStringLiteral("totalBytes")).toDouble());
    for (const auto& v : o.value(QStringLiteral("items")).toArray()) {
        const QJsonObject x = v.toObject();
        LanguageItem i;
        i.relativePath = x.value(QStringLiteral("path")).toString();
        i.isFolder = x.value(QStringLiteral("folder")).toBool();
        i.code = x.value(QStringLiteral("code")).toString();
        i.language = x.value(QStringLiteral("language")).toString();
        i.bytes = qint64(x.value(QStringLiteral("bytes")).toDouble());
        i.files = qint64(x.value(QStringLiteral("files")).toDouble());
        s.items.push_back(i);
    }
    return s;
}

bool isGameRoot(const QString& folder)
{
    return QFileInfo(QDir(folder).filePath(QStringLiteral("eboot.bin"))).isFile();
}

QStringList findGameRoots(const QString& folder)
{
    QStringList out;
    if (isGameRoot(folder))
        return {QDir(folder).absolutePath()};
    const auto children = QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& c : children) {
        if (isGameRoot(c.absoluteFilePath())) {
            out << c.absoluteFilePath();
            continue;
        }
        for (const QFileInfo& g : QDir(c.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase))
            if (isGameRoot(g.absoluteFilePath())) {
                out << g.absoluteFilePath();
                break;
            }
    }
    return out;
}

Result<LanguageScan> scanLanguages(const QString& gameRoot, const CancellationToken& token)
{
    if (!QFileInfo(gameRoot).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(gameRoot)));
    LanguageScan scan;
    scan.gameRoot = QDir(gameRoot).absolutePath();
    const QDir base(scan.gameRoot);
    const Patterns& pt = patterns();
    std::map<QString, size_t> byPath; // absolute → index (for .sig pairing)
    bool cancelled = false;

    auto add = [&](const QString& abs, bool folder, const QString& code, const QString& suffix) {
        LanguageItem it;
        it.relativePath = base.relativeFilePath(abs);
        it.isFolder = folder;
        it.code = code;
        it.language = languageName(code) + suffix;
        measure(abs, it);
        byPath[abs] = scan.items.size();
        scan.items.push_back(it);
    };

    std::function<void(const QString&, int, bool)> walk = [&](const QString& dir, int depth, bool inContainer) {
        if (depth > 7 || cancelled)
            return;
        if (!token.checkpoint()) {
            cancelled = true;
            return;
        }
        const auto entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                                                     QDir::Name | QDir::IgnoreCase);
        // How many folders here are named like a known language: a lone
        // two-letter folder ("bg", "id", "it") is more likely game data.
        int languageFolders = 0;
        for (const QFileInfo& e : entries)
            if (e.isDir() && !e.isSymLink() && pt.locale.match(e.fileName()).hasMatch()
                && validCodes().contains(e.fileName().toLower().section(u'-', 0, 0)))
                ++languageFolders;
        for (const QFileInfo& e : entries) {
            const QString name = e.fileName();
            const QString low = name.toLower();
            const QString abs = e.absoluteFilePath();
            if (skipDirs().contains(low) || e.isSymLink())
                continue;
            if (e.isDir()) {
                if (pt.locale.match(name).hasMatch()) {
                    const QString code = low.section(u'-', 0, 0);
                    const bool known = validCodes().contains(code);
                    // A bare code counts in a language container or next to another
                    // language folder. "xx-YY" may be a language the list does not
                    // know; the other spellings ("xx-Word", "xx-123") must be known.
                    const bool accepted = !name.contains(u'-') ? known && (inContainer || languageFolders >= 2)
                                                                : known || pt.region.match(name).hasMatch();
                    if (accepted) {
                        // Grouped by base language like the legacy app (keeping
                        // English keeps en-US and en-GB); the label names the region.
                        add(abs, true, code, {});
                        scan.items.back().language = languageName(name);
                        continue;
                    }
                }
                if (containerDirs().contains(low)) {
                    walk(abs, depth + 1, true);
                    continue;
                }
                if (inContainer && validCodes().contains(low)) {
                    add(abs, true, low, {});
                    continue;
                }
                if (depth < 6)
                    walk(abs, depth + 1, inContainer);
                continue;
            }
            const QString ext = QStringLiteral(".") + e.suffix().toLower();
            QString code, suffix;
            if (ext == u".pak") {
                code = firstCapture(pt.pak.match(name));
            } else if (ext == u".psarc") {
                code = firstCapture(pt.psarc.match(name));
            } else if (QStringList{QStringLiteral(".mp4"), QStringLiteral(".m4v"), QStringLiteral(".webm"), QStringLiteral(".mkv"),
                                   QStringLiteral(".avi"), QStringLiteral(".mov"), QStringLiteral(".m2ts"), QStringLiteral(".ts")}
                           .contains(ext)) {
                code = firstCapture(pt.video.match(name));
                suffix = QStringLiteral(" (video)");
            } else if (QStringList{QStringLiteral(".bnk"), QStringLiteral(".wem"), QStringLiteral(".pck"), QStringLiteral(".at9"),
                                   QStringLiteral(".fsb"), QStringLiteral(".bank")}
                           .contains(ext)) {
                code = firstCapture(pt.audio.match(name));
                suffix = QStringLiteral(" (audio)");
            } else if (ext == u".sig") {
                const QString pak = abs.left(abs.size() - 4) + QStringLiteral(".pak");
                if (auto it = byPath.find(pak); it != byPath.end()) {
                    const LanguageItem& p = scan.items[it->second];
                    add(abs, false, p.code, {});
                    scan.items.back().language = p.language;
                }
                continue;
            }
            if (!code.isEmpty())
                add(abs, false, code, suffix);
        }
    };
    walk(scan.gameRoot, 0, false);
    if (cancelled)
        return Error::cancelled();
    std::sort(scan.items.begin(), scan.items.end(), [](const LanguageItem& a, const LanguageItem& b) {
        return a.relativePath.compare(b.relativePath, Qt::CaseInsensitive) < 0;
    });
    for (const auto& i : scan.items)
        scan.totalBytes += i.bytes;
    return scan;
}

Result<StripResult> stripLanguages(const StripPlan& plan, const CancellationToken& token, ProgressReporter* progress)
{
    Log log(QStringLiteral("tools"));
    const QString root = QDir(plan.gameRoot).absolutePath();
    if (!isGameRoot(root))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not a game folder (no eboot.bin).").arg(QDir::toNativeSeparators(root)));
    if (plan.remove.isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Nothing was selected to remove."));
    if (plan.backupFolder.trimmed().isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose a backup folder first. Removed languages are always backed up."));
    if (isSameOrInside(plan.backupFolder, root))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The backup folder cannot be inside the game folder."));

    // Validate every path before touching anything.
    qint64 total = 0;
    QStringList absPaths;
    for (const QString& rel : plan.remove) {
        const QString safe = zip::Reader::safeRelativePath(rel);
        const QString abs = QDir(root).filePath(safe);
        if (safe.isEmpty() || !isSameOrInside(abs, root) || QDir::cleanPath(abs) == QDir::cleanPath(root))
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not inside the game folder.").arg(rel));
        const QString top = safe.section(u'/', 0, 0).toLower();
        if (top == u"sce_sys" || top == u"sce_module" || safe.compare(QStringLiteral("eboot.bin"), Qt::CaseInsensitive) == 0)
            return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is a system file and is never removed.").arg(rel));
        if (!QFileInfo::exists(abs))
            return Error(ErrorCode::NotFound, QStringLiteral("%1 no longer exists. Scan the game again.").arg(rel));
        LanguageItem m;
        measure(abs, m);
        total += m.bytes;
        absPaths << abs;
    }
    FW_TRY(ensureFreeSpace(plan.backupFolder, total, plan.freeSpaceMarginBytes));

    StripResult result;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH.mm.ss"));
    result.backupZip = uniquePath(QDir(plan.backupFolder).filePath(
        QStringLiteral("%1_languages_%2.zip").arg(sanitizeFileName(QFileInfo(root).fileName()), stamp)));

    if (progress) {
        progress->setPhases({QStringLiteral("Backing up"), QStringLiteral("Checking backup"), QStringLiteral("Removing")}, {10, 4, 1});
        progress->beginPhase(0, total);
    }
    // What goes into the backup, as it was just before it was read. Removal
    // works from this list (checked against the verified ZIP), never from a
    // second look at the folder.
    struct BackedFile {
        QString rel, abs;
        qint64 size;
        QDateTime modified;
    };
    struct BackedItem {
        QString rel, abs;
        bool isFile = false;
        std::vector<BackedFile> files;
        QStringList dirs; // below `abs`
    };
    std::vector<BackedItem> backed;
    {
        zip::Writer w;
        FW_TRY(w.open(result.backupZip));
        const QDir base(root);
        for (const QString& abs : absPaths) {
            BackedItem item;
            item.abs = abs;
            item.rel = base.relativeFilePath(abs);
            const QFileInfo fi(abs);
            if (fi.isFile()) {
                item.isFile = true;
                item.files.push_back({item.rel, abs, fi.size(), fi.lastModified()});
                FW_TRY(w.addFile(item.rel, abs, zip::Method::Store, token, progress));
                backed.push_back(std::move(item));
                continue;
            }
            FW_TRY(w.addDirectory(item.rel));
            QDirIterator it(abs, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QString rel = base.relativeFilePath(it.filePath());
                if (it.fileInfo().isDir()) {
                    FW_TRY(w.addDirectory(rel));
                    item.dirs << it.filePath();
                } else {
                    const QFileInfo now(it.filePath()); // a fresh look, like the one before removal
                    item.files.push_back({rel, it.filePath(), now.size(), now.lastModified()});
                    FW_TRY(w.addFile(rel, it.filePath(), zip::Method::Store, token, progress));
                }
            }
            backed.push_back(std::move(item));
        }
        QJsonArray items;
        for (const QString& abs : absPaths)
            items.append(base.relativeFilePath(abs));
        const QJsonObject manifest{{QStringLiteral("game"), QFileInfo(root).fileName()},
                                   {QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate)},
                                   {QStringLiteral("removed"), items}};
        FW_TRY(w.addData(QString::fromLatin1(kManifestName), QJsonDocument(manifest).toJson()));
        FW_TRY(w.finish());
    }
    if (progress)
        progress->beginPhase(1, total);
    QHash<QString, qint64> verified; // file entry name -> size, from the ZIP that was read back
    {
        zip::Reader r;
        FW_TRY(r.open(result.backupZip));
        auto st = r.verifyAll(token, progress);
        if (!st.ok()) {
            QFile::remove(result.backupZip);
            if (st.error().isCancelled())
                return Error::cancelled();
            return Error(ErrorCode::VerificationFailed, QStringLiteral("The backup could not be verified, so nothing was removed."),
                         st.error().toLogString());
        }
        for (const auto& e : r.entries())
            if (!e.isDirectory())
                verified.insert(e.name, qint64(e.uncompressedSize));
    }
    log.info(QStringLiteral("Language backup verified: %1").arg(QDir::toNativeSeparators(result.backupZip)));

    if (progress)
        progress->beginPhase(2, qint64(backed.size()), 0, false);
    const auto removeLink = [](const QString& p) { return QFile::remove(p) || QDir().rmdir(p); };
    const auto removeFile = [](const QString& p) {
        if (QFile::remove(p))
            return true;
        QFile::setPermissions(p, QFileInfo(p).permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
        return QFile::remove(p);
    };
    const auto removeEmptyDir = [](const QString& p) {
        if (QDir().rmdir(p))
            return true;
        // A read-only folder: Windows refuses to remove it until the attribute is cleared.
        QFile::setPermissions(p, QFileInfo(p).permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
        return QDir().rmdir(p);
    };
    constexpr qsizetype kMaxListed = 200;
    for (const BackedItem& item : backed) {
        // Links are removed as links; nothing is deleted through one.
        QStringList links;
        if (!item.isFile && isJunctionOrSymlink(item.abs))
            links << item.abs;
        for (const QString& d : item.dirs)
            if (isJunctionOrSymlink(d))
                links << d;
        const auto behindLink = [&links](const QString& p) {
            for (const QString& l : links)
                if (p.size() > l.size() && p.startsWith(l) && p.at(l.size()) == u'/')
                    return true;
            return false;
        };
        for (const BackedFile& f : item.files) {
            if (behindLink(f.abs))
                continue;
            if (isJunctionOrSymlink(f.abs)) {
                removeLink(f.abs);
                continue;
            }
            const QFileInfo now(f.abs);
            if (!now.exists())
                continue;
            const auto inZip = verified.constFind(f.rel);
            if (!now.isFile() || now.size() != f.size || now.lastModified() != f.modified || inZip == verified.cend()
                || inZip.value() != f.size) {
                // Not what was backed up: it stays.
                if (result.leftInPlace.size() < kMaxListed)
                    result.leftInPlace << f.rel;
                log.warn(QStringLiteral("Left in place, changed after it was backed up: %1").arg(f.rel));
                continue;
            }
            if (removeFile(f.abs))
                result.bytesFreed += f.size;
            else
                log.warn(QStringLiteral("Could not remove %1").arg(f.rel));
        }
        if (!item.isFile) {
            for (const QString& l : std::as_const(links))
                if (!behindLink(l))
                    removeLink(l);
            // Deepest first. Only an empty folder goes, so a file that appeared
            // after the backup keeps its folder.
            QStringList dirs = item.dirs;
            dirs << item.abs;
            std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
            for (const QString& d : std::as_const(dirs))
                if (!links.contains(d) && !behindLink(d) && QFileInfo(d).isDir())
                    removeEmptyDir(d);
        }
        if (!QFileInfo::exists(item.abs) && !QFileInfo(item.abs).isSymLink()) {
            ++result.removed;
            log.info(QStringLiteral("Removed %1").arg(item.rel));
        } else {
            result.failed << item.rel;
            log.warn(QStringLiteral("Could not remove %1 completely").arg(item.rel));
            // Say what is still there (files that were not in the backup included).
            if (!item.isFile && !isJunctionOrSymlink(item.abs)) {
                QDirIterator it(item.abs, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (it.hasNext() && result.leftInPlace.size() < kMaxListed) {
                    const QString rel = QDir(root).relativeFilePath(it.next());
                    if (!result.leftInPlace.contains(rel))
                        result.leftInPlace << rel;
                }
            }
        }
        if (progress)
            progress->advance(1);
    }
    return result;
}

Result<RestoreResult> restoreLanguageBackup(const QString& backupZip, const QString& gameRoot,
                                            const CancellationToken& token, ProgressReporter* progress,
                                            qint64 freeSpaceMarginBytes)
{
    if (!isGameRoot(gameRoot))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not a game folder (no eboot.bin).").arg(QDir::toNativeSeparators(gameRoot)));
    const QString zipName = QFileInfo(backupZip).fileName();
    zip::Reader r;
    FW_TRY(r.open(backupZip));
    const zip::Entry* manifestEntry = r.find(QString::fromLatin1(kManifestName));
    if (!manifestEntry)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is not a Language Stripper backup.").arg(zipName));

    // The manifest says what the backup may put back; the ZIP must agree with it.
    auto manifestBytes = r.readAll(*manifestEntry, 4 * 1024 * 1024);
    if (!manifestBytes.ok())
        return Error(ErrorCode::CorruptData, QStringLiteral("The list of removed items in %1 could not be read; nothing was restored.").arg(zipName),
                     manifestBytes.error().toLogString());
    const QJsonObject manifest = QJsonDocument::fromJson(manifestBytes.value()).object();
    const QJsonValue removedValue = manifest.value(QStringLiteral("removed"));
    if (!removedValue.isArray() || removedValue.toArray().isEmpty())
        return Error(ErrorCode::CorruptData, QStringLiteral("The list of removed items in %1 is damaged; nothing was restored.").arg(zipName));
    QStringList listed;
    for (const auto& v : removedValue.toArray()) {
        // The same rules as for removing: inside the game, never a system file.
        const QString safe = v.isString() ? zip::Reader::safeRelativePath(v.toString()) : QString();
        const QString top = safe.section(u'/', 0, 0).toLower();
        if (safe.isEmpty() || top == u"sce_sys" || top == u"sce_module"
            || safe.compare(QStringLiteral("eboot.bin"), Qt::CaseInsensitive) == 0)
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 lists an item that a language backup never holds (%2); nothing was restored.")
                             .arg(zipName, v.toString().left(200)));
        listed << safe;
    }
    const auto isListed = [&listed](const QString& rel) {
        for (const QString& l : listed)
            if (rel == l || (rel.size() > l.size() && rel.startsWith(l) && rel.at(l.size()) == u'/'))
                return true;
        return false;
    };
    const QDir root(QDir(gameRoot).absolutePath());
    qint64 total = 0, needed = 0; // all of it; what is not in the game yet
    // The sizes come from the ZIP's own headers: added without overflow, so a
    // size no file can have asks for more space than any drive has instead
    // of turning the check off.
    const auto add = [](qint64 sum, quint64 size) {
        constexpr qint64 kMax = std::numeric_limits<qint64>::max();
        return size > quint64(kMax - sum) ? kMax : sum + qint64(size);
    };
    for (const auto& e : r.entries()) {
        if (e.name == QLatin1String(kManifestName))
            continue;
        QString safe = zip::Reader::safeRelativePath(e.name);
        if (safe.isEmpty())
            return Error(ErrorCode::CorruptData, QStringLiteral("The backup contains an unsafe path (%1); nothing was restored.").arg(e.name));
        while (safe.endsWith(u'/'))
            safe.chop(1);
        if (!isListed(safe))
            return Error(ErrorCode::CorruptData,
                         QStringLiteral("%1 holds a file that its list of removed items does not cover (%2); nothing was restored.")
                             .arg(zipName, e.name.left(200)));
        total = add(total, e.uncompressedSize);
        if (!e.isDirectory() && !QFileInfo::exists(root.filePath(safe)))
            needed = add(needed, e.uncompressedSize);
    }

    RestoreResult result;
    result.backupGame = manifest.value(QStringLiteral("game")).toString();
    result.otherGame = !result.backupGame.isEmpty()
                       && result.backupGame.compare(root.dirName(), Qt::CaseInsensitive) != 0;
    // Before the first file, not half-way through the game.
    FW_TRY(ensureFreeSpace(root.absolutePath(), needed, freeSpaceMarginBytes));
    if (progress)
        progress->begin(QStringLiteral("Restoring"), total);
    for (const auto& e : r.entries()) {
        if (e.name == QLatin1String(kManifestName))
            continue;
        const QString rel = zip::Reader::safeRelativePath(e.name);
        const QString target = root.filePath(rel);
        if (e.isDirectory()) {
            QDir().mkpath(target);
            continue;
        }
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) {
            result.skippedExisting << rel; // never overwrite what is there now
            continue;
        }
        QDir().mkpath(QFileInfo(target).absolutePath());
        // A game file called "<target>.part" is never taken for the temporary
        // file (PartFile picks another name then).
        PartFile part(target);
        QFile out(toLongPath(part.partPath()));
        const auto failed = [&] { return Error(ErrorCode::IoError, QStringLiteral("Could not restore %1.").arg(e.name), out.errorString()); };
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return failed();
        FW_TRY(r.read(e, [&](const QByteArray& c) -> Status {
            if (out.write(c) != c.size())
                return failed();
            return success();
        }, token, progress));
        // A full or vanished drive often shows only here, not in write().
        FW_TRY(finishWrittenFile(out, e.name, e.modified));
        FW_TRY(part.commit(false));
        ++result.restored;
    }
    return result;
}

Result<int> restoreLanguages(const QString& backupZip, const QString& gameRoot, const CancellationToken& token,
                             ProgressReporter* progress)
{
    auto r = restoreLanguageBackup(backupZip, gameRoot, token, progress);
    if (!r.ok())
        return r.takeError();
    return r->restored;
}

} // namespace fw::tools
