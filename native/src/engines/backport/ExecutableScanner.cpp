#include "engines/backport/ExecutableScanner.h"

#include "core/FileUtil.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfReader.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <functional>

namespace fw::backport {

QString fileClassName(FileClass c)
{
    switch (c) {
    case FileClass::Elf: return QStringLiteral("ELF");
    case FileClass::Self: return QStringLiteral("SELF");
    case FileClass::SelfEncrypted: return QStringLiteral("encrypted SELF");
    case FileClass::SelfCompressed: return QStringLiteral("compressed SELF");
    case FileClass::SelfMalformed: return QStringLiteral("malformed SELF");
    case FileClass::ElfUnsupported: return QStringLiteral("unsupported ELF");
    case FileClass::Unreadable: return QStringLiteral("unreadable");
    }
    return QStringLiteral("unknown");
}

bool hasExecutableMagic(const QByteArray& head)
{
    return elf::hasElfMagic(head) || selfio::hasSelfMagic(head);
}

const ExecFile* ScanResult::find(const QString& rel) const
{
    const QString key = relKey(rel);
    for (const ExecFile& e : executables)
        if (relKey(e.rel) == key)
            return &e;
    return nullptr;
}

const ExecFile* ScanResult::eboot() const
{
    return hasEboot ? find(ebootRel) : nullptr;
}

std::vector<const ExecFile*> ScanResult::processable() const
{
    std::vector<const ExecFile*> out;
    for (const ExecFile& e : executables)
        if (e.isProcessable())
            out.push_back(&e);
    return out;
}

qint64 ScanResult::processableBytes() const
{
    qint64 n = 0;
    for (const ExecFile& e : executables)
        if (e.isProcessable())
            n += e.size;
    return n;
}

std::vector<const ExecFile*> ScanResult::encryptedOrCompressed() const
{
    std::vector<const ExecFile*> out;
    for (const ExecFile& e : executables)
        if (e.area == Area::Game && e.isEncryptedOrCompressed())
            out.push_back(&e);
    return out;
}

std::optional<int> ScanResult::gameSdk() const
{
    const ExecFile* e = eboot();
    return e ? e->sdk : std::nullopt;
}

namespace {

bool nameLooksExecutable(const QString& lowName)
{
    return lowName == QStringLiteral("eboot.bin") || lowName.endsWith(QStringLiteral(".prx"))
        || lowName.endsWith(QStringLiteral(".sprx")) || lowName.endsWith(QStringLiteral(".self"))
        || lowName.endsWith(QStringLiteral(".elf"));
}

void readParams(ExecFile& f, QByteArrayView elfData)
{
    auto scan = findParams(elfData);
    if (!scan.ok()) {
        f.problemCode = codeOf(scan.error());
        f.problem = scan.error().message;
        if (f.problemCode == Code::ParamMagicInvalid)
            f.problem += QStringLiteral(" (the file may still be encrypted)");
        return;
    }
    f.moduleWithoutParam = scan->moduleWithoutParam;
    const ParamInfo* chosen = nullptr;
    for (const ParamInfo& p : scan->params)
        if (p.kind == ParamKind::Process) {
            chosen = &p;
            break;
        }
    if (!chosen && !scan->params.empty())
        chosen = &scan->params.front();
    if (chosen) {
        f.ps4 = chosen->ps4;
        f.ps5 = chosen->ps5;
        f.sdk = bcdMajor(chosen->ps5);
    }
}

void classifyFull(ExecFile& f, const QString& path)
{
    auto data = readFileBytes(path);
    if (!data.ok()) {
        f.cls = FileClass::Unreadable;
        f.problemCode = codeOf(data.error());
        f.problem = data.error().message;
        return;
    }
    f.sha256 = sha256Hex(data.value());
    const QByteArray& d = data.value();
    if (elf::hasElfMagic(d)) {
        f.cls = FileClass::Elf;
        auto img = ElfImage::parse(d, Strictness::Signable);
        if (!img.ok()) {
            f.cls = FileClass::ElfUnsupported;
            f.problemCode = codeOf(img.error());
            f.problem = img.error().message;
            return;
        }
        readParams(f, d);
        return;
    }
    auto info = selfio::inspect(d);
    if (!info.ok()) {
        f.cls = FileClass::SelfMalformed;
        f.problemCode = Code::SelfMalformed;
        f.problem = info.error().message;
        return;
    }
    switch (info->kind) {
    case SelfKind::Encrypted:
        f.cls = FileClass::SelfEncrypted;
        f.problemCode = Code::SelfEncrypted;
        f.problem = info->problem;
        return;
    case SelfKind::Compressed:
        f.cls = FileClass::SelfCompressed;
        f.problemCode = Code::SelfCompressed;
        f.problem = info->problem;
        return;
    case SelfKind::Malformed:
        f.cls = FileClass::SelfMalformed;
        f.problemCode = Code::SelfMalformed;
        f.problem = info->problem;
        return;
    case SelfKind::FakeSigned: break;
    }
    f.cls = FileClass::Self;
    auto elfData = selfio::unwrap(d);
    if (!elfData.ok()) {
        f.cls = FileClass::SelfMalformed;
        f.problemCode = codeOf(elfData.error());
        f.problem = elfData.error().message;
        return;
    }
    readParams(f, elfData.value());
}

void classifyQuick(ExecFile& f, const QString& path, const QByteArray& head4)
{
    if (elf::hasElfMagic(head4)) {
        f.cls = FileClass::Elf;
        return;
    }
    auto head = readHead(path, 64 * 1024);
    if (!head.ok() || head->size() < self::kCommonHeaderSize) {
        f.cls = FileClass::SelfMalformed;
        f.problemCode = Code::SelfMalformed;
        f.problem = QStringLiteral("the SELF header is truncated");
        return;
    }
    QByteArray h = head.value();
    const int n = qFromLittleEndian<quint16>(h.constData() + 0x18);
    const qsizetype need = self::kCommonHeaderSize + qsizetype(n) * self::kEntrySize;
    if (h.size() < need) {
        auto more = readHead(path, need);
        if (!more.ok() || more->size() < need) {
            f.cls = FileClass::SelfMalformed;
            f.problemCode = Code::SelfMalformed;
            f.problem = QStringLiteral("the SELF entry table is cut short");
            return;
        }
        h = more.value();
    }
    f.cls = FileClass::Self;
    for (int i = 0; i < n; ++i) {
        const quint64 p = qFromLittleEndian<quint64>(h.constData() + self::kCommonHeaderSize + i * self::kEntrySize);
        if (!(p & self::props::kHasBlocks))
            continue;
        if (p & self::props::kEncrypted) {
            f.cls = FileClass::SelfEncrypted;
            f.problemCode = Code::SelfEncrypted;
            f.problem = QStringLiteral("its segments are encrypted (the dump may still be encrypted)");
            return;
        }
        if (p & self::props::kCompressed) {
            f.cls = FileClass::SelfCompressed;
            f.problemCode = Code::SelfCompressed;
            f.problem = QStringLiteral("its segments are compressed, which this tool does not unpack");
        }
    }
}

struct Walker {
    ScanResult& r;
    const CancellationToken& token;
    ScanMode mode;
    QStringList excludes;
    QStringList paramJsonCandidates;
    qint64 visited = 0;
    bool cancelled = false;

    bool excluded(const QString& abs) const
    {
        for (const QString& ex : excludes)
            if (isSameOrInside(abs, ex))
                return true;
        return false;
    }

    void walk(const QString& absDir, const QString& relDir, Area area, bool decryptedListed)
    {
        const QFileInfoList list = QDir(absDir).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& fi : list) {
            if (cancelled)
                return;
            if ((++visited & 0x3F) == 0 && !token.checkpoint()) {
                cancelled = true;
                return;
            }
            const QString name = fi.fileName();
            const QString rel = relDir.isEmpty() ? name : relDir + u'/' + name;
            if (fi.isSymLink() || fi.isJunction()) {
                r.skippedSymlinks << rel;
                continue;
            }
            if (fi.isDir()) {
                if (isOsMetadataDir(name)) {
                    r.metadataSkipped << rel + u'/';
                    continue;
                }
                if (excluded(fi.absoluteFilePath()))
                    continue;
                Area next = area;
                bool listed = decryptedListed;
                const QString low = name.toLower();
                if (area == Area::Game && low == QStringLiteral("decrypted")) {
                    next = Area::Decrypted;
                    listed = rel.count(u'/') + 1 <= 4;
                } else if (area == Area::Game && low == QStringLiteral("fakelib")) {
                    next = Area::Fakelib;
                }
                walk(fi.absoluteFilePath(), rel, next, listed);
                continue;
            }
            visitFile(fi, rel, area, decryptedListed);
        }
    }

    void visitFile(const QFileInfo& fi, const QString& rel, Area area, bool decryptedListed)
    {
        const QString name = fi.fileName();
        const QString low = name.toLower();
        if (isOsMetadataFile(name)) {
            r.metadataSkipped << rel;
            return;
        }
        ++r.totalFiles;
        r.fileIndex.insert(relKey(rel), rel);
        if (low.endsWith(QStringLiteral(".bak"))) {
            r.leftoverBaks << rel;
            return;
        }
        if (area == Area::Decrypted && decryptedListed && fi.size() > 0) {
            r.decryptedFiles << rel;
            r.decryptedBytes += fi.size();
        }
        if (area == Area::Fakelib && rel.startsWith(QStringLiteral("fakelib/"), Qt::CaseInsensitive))
            r.fakelibFiles << rel;
        if (area == Area::Game && low == QStringLiteral("param.json"))
            paramJsonCandidates << rel;
        if (area == Area::Game && low == QStringLiteral("eboot.bin")) {
            if (!rel.contains(u'/')) {
                r.hasEboot = true;
                r.ebootRel = rel;
            } else {
                r.nestedEbootDirs << rel.section(u'/', 0, -2);
            }
        }

        auto head = readHead(fi.absoluteFilePath(), 4);
        const bool readable = head.ok() && head->size() == 4;
        if (readable && !hasExecutableMagic(head.value()))
            return; // a readable non-executable, whatever its name
        if (!readable && !nameLooksExecutable(low))
            return;

        ExecFile f;
        f.rel = rel;
        f.size = fi.size();
        f.area = area;
        if (!readable) {
            f.cls = FileClass::Unreadable;
            f.problemCode = Code::Unreadable;
            f.problem = head.ok() ? QStringLiteral("it is too short to be an executable (%1 bytes)").arg(fi.size())
                                  : head.error().message;
        } else if (area != Area::Game) {
            f.cls = elf::hasElfMagic(head.value()) ? FileClass::Elf : FileClass::Self;
            if (mode == ScanMode::Full) {
                auto h = sha256File(fi.absoluteFilePath(), token);
                if (h.ok())
                    f.sha256 = h.value();
            }
        } else if (mode == ScanMode::Full || (!rel.contains(u'/') && low == QStringLiteral("eboot.bin"))) {
            classifyFull(f, fi.absoluteFilePath());
            if (mode == ScanMode::Quick)
                f.sha256.clear();
        } else {
            classifyQuick(f, fi.absoluteFilePath(), head.value());
        }
        r.executables.push_back(std::move(f));
    }
};

} // namespace

Result<ScanResult> scanGame(const QString& root, const CancellationToken& token, ScanMode mode,
                            const QStringList& excludeDirs)
{
    const QFileInfo ri(root);
    if (!ri.exists() || !ri.isDir())
        return makeError(Code::NotAGame,
                         QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(root)));
    ScanResult r;
    r.root = QDir::cleanPath(ri.absoluteFilePath());
    Walker w{r, token, mode, excludeDirs, {}, 0, false};
    w.walk(r.root, QString(), Area::Game, false);
    if (w.cancelled || !token.checkpoint())
        return Error::cancelled();

    std::sort(r.executables.begin(), r.executables.end(),
              [](const ExecFile& a, const ExecFile& b) { return a.rel < b.rel; });
    r.leftoverBaks.sort();
    r.metadataSkipped.sort();
    r.nestedEbootDirs.removeDuplicates();
    r.nestedEbootDirs.sort();
    r.fakelibFiles.sort();
    r.decryptedFiles.sort();

    // param.json kept in the originals ZIP: sce_sys/, the root, then the
    // first one at most 3 levels deep.
    QString chosen;
    for (const QString& pref : {QStringLiteral("sce_sys/param.json"), QStringLiteral("param.json")}) {
        for (const QString& c : std::as_const(w.paramJsonCandidates))
            if (c.compare(pref, Qt::CaseInsensitive) == 0) {
                chosen = c;
                break;
            }
        if (!chosen.isEmpty())
            break;
    }
    if (chosen.isEmpty()) {
        QStringList sorted = w.paramJsonCandidates;
        sorted.sort();
        for (const QString& c : std::as_const(sorted))
            if (c.count(u'/') + 1 <= 3) {
                chosen = c;
                break;
            }
    }
    if (!chosen.isEmpty()) {
        r.paramJsonRel = chosen;
        r.paramJsonBytes = QFileInfo(joinPath(r.root, chosen)).size();
    }
    return r;
}

QStringList discoverGames(const QStringList& folders, int depth, const QStringList& excludeDirs,
                          const CancellationToken& token, QStringList* warnings)
{
    QStringList games;
    QStringList seen;
    const auto isExcluded = [&](const QString& abs) {
        for (const QString& ex : excludeDirs)
            if (!ex.isEmpty() && isSameOrInside(abs, ex))
                return true;
        return false;
    };
    const auto alreadySeen = [&](const QString& abs) {
        for (const QString& s : seen)
            if (isSameOrInside(abs, s) && isSameOrInside(s, abs))
                return true;
        return false;
    };
    std::function<void(const QString&, int)> visit = [&](const QString& dir, int level) {
        if (!token.checkpoint())
            return;
        if (QFileInfo(QDir(dir).filePath(QStringLiteral("eboot.bin"))).isFile()) {
            if (!alreadySeen(dir)) {
                seen << dir;
                games << dir;
            }
            return; // nested eboot.bin folders belong to this game
        }
        if (level >= depth)
            return;
        const QFileInfoList subs = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
        for (const QFileInfo& fi : subs) {
            if (fi.isSymLink() || fi.isJunction() || isOsMetadataDir(fi.fileName()))
                continue;
            const QString low = fi.fileName().toLower();
            if (low == QStringLiteral("decrypted") || low == QStringLiteral("fakelib"))
                continue;
            if (isExcluded(fi.absoluteFilePath()))
                continue;
            visit(QDir::cleanPath(fi.absoluteFilePath()), level + 1);
        }
    };
    QStringList unique;
    for (const QString& f : folders) {
        const QString abs = QDir::cleanPath(QFileInfo(f).absoluteFilePath());
        if (f.trimmed().isEmpty())
            continue;
        if (!QFileInfo(abs).isDir()) {
            if (warnings)
                *warnings << QStringLiteral("Skipped %1: the folder does not exist.").arg(QDir::toNativeSeparators(f));
            continue;
        }
        bool dup = false;
        for (const QString& u : unique)
            if (isSameOrInside(abs, u) && isSameOrInside(u, abs))
                dup = true;
        if (dup)
            continue;
        unique << abs;
        if (isExcluded(abs)) {
            if (warnings)
                *warnings << QStringLiteral("Skipped %1: it is a backup ZIP folder.").arg(QDir::toNativeSeparators(f));
            continue;
        }
        visit(abs, 0);
    }
    games.sort();
    return games;
}

} // namespace fw::backport
