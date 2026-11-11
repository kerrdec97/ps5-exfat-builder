#include "engines/tools/DumpCheck.h"

#include "core/FileUtil.h"
#include "core/FolderAccess.h"
#include "engines/backport/CanonicalTrio.h"
#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SdkTable.h"
#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfReader.h"
#include "engines/gameinfo/GameInfo.h"
#include "engines/gameinfo/ParamJson.h"
#include "engines/gameinfo/Sfo.h"
#include "engines/gameinfo/Version.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>

namespace fw::tools {

namespace {

constexpr qint64 kFat32MaxFileBytes = 0xFFFFFFFFLL; // 4 GiB - 1
constexpr int kMaxFakelibNames = 200;
constexpr int kMaxSdkValues = 16;

template <typename T>
T rd(const QByteArray& d, qint64 off)
{
    return qFromLittleEndian<T>(d.constData() + off);
}

bool within(quint64 size, quint64 off, quint64 len)
{
    return off <= size && len <= size - off;
}

QByteArray readAt(QFile& f, qint64 offset, qint64 length)
{
    if (offset < 0 || length <= 0 || !f.seek(offset))
        return {};
    return f.read(length);
}

QString countOf(qint64 n, const QString& one, const QString& many)
{
    return QStringLiteral("%1 %2").arg(formatCount(n), n == 1 ? one : many);
}

ExecutableInfo verdict(ExecutableClass kind, const QString& problem = {})
{
    ExecutableInfo e;
    e.kind = kind;
    e.problem = problem;
    return e;
}

// The param struct of engines/backport/SceParam.h, read alone: its size, its
// magic and the PS5 SDK at +0x14.
void readParam(QFile& f, quint64 at, bool processParam, ExecutableInfo& out)
{
    const QByteArray raw = readAt(f, qint64(at), backport::kMinParamSize);
    if (raw.size() != qsizetype(backport::kMinParamSize))
        return;
    if (rd<quint32>(raw, 0x00) < backport::kMinParamSize)
        return;
    if (rd<quint32>(raw, 0x08) != (processParam ? backport::kProcParamMagic : backport::kModuleParamMagic))
        return;
    const quint32 ps5 = rd<quint32>(raw, 0x14);
    if (ps5 == 0 || !backport::bcdMajor(ps5))
        return;
    out.hasSdk = true;
    out.ps5Sdk = ps5;
}

// The program header of the param: the process param when there is one, else the module param.
const backport::ProgramHeader* paramHeader(const std::vector<backport::ProgramHeader>& phdrs)
{
    const backport::ProgramHeader* module = nullptr;
    for (const backport::ProgramHeader& ph : phdrs) {
        if (ph.type == backport::elf::PT_SCE_PROCPARAM)
            return &ph;
        if (ph.type == backport::elf::PT_SCE_MODULE_PARAM && !module)
            module = &ph;
    }
    return module;
}

ExecutableInfo inspectSelf(QFile& f, qint64 size, QByteArray head)
{
    namespace self = backport::self;
    namespace elf = backport::elf;
    if (head.size() < self::kCommonHeaderSize)
        return verdict(ExecutableClass::Truncated, QStringLiteral("the SELF header is cut short"));
    // The entry table, the ELF header copy and its program headers may reach
    // past the first read: one more read, of what the header itself asks for.
    const quint64 tableEnd = quint64(self::kCommonHeaderSize) + quint64(rd<quint16>(head, 0x18)) * self::kEntrySize;
    if (tableEnd > quint64(kDumpCheckMaxHeaderBytes))
        return verdict(ExecutableClass::Malformed, QStringLiteral("its entry table is larger than any executable's"));
    const quint64 want = self::alignUp(tableEnd, 16) + quint64(kDumpCheckHeaderBytes);
    if (want > quint64(head.size()) && size > head.size())
        head = readAt(f, 0, std::min<qint64>(size, std::min<qint64>(qint64(want), kDumpCheckMaxHeaderBytes)));
    const auto parsed = backport::selfio::inspect(head);
    if (!parsed.ok())
        return verdict(ExecutableClass::Truncated, parsed.error().message);
    const backport::SelfInfo& info = parsed.value();
    if (info.kind == backport::SelfKind::Encrypted)
        return verdict(ExecutableClass::Encrypted, info.problem);
    if (info.kind == backport::SelfKind::Compressed)
        return verdict(ExecutableClass::Compressed, info.problem);

    // The reader judged the bytes it was given. The segments of a fake-signed
    // SELF lie beyond them, so its bounds rules are applied here against the
    // size of the whole file.
    const quint64 fileSize = quint64(size);
    if (tableEnd > fileSize)
        return verdict(ExecutableClass::Truncated, QStringLiteral("the SELF entry table is cut short"));
    if (!info.elfValid) {
        if (!within(fileSize, info.elfOffset, elf::kEhdrSize))
            return verdict(ExecutableClass::Truncated, QStringLiteral("the ELF header copy is cut short"));
        return verdict(ExecutableClass::Malformed, info.problem.isEmpty() ? QStringLiteral("its ELF header copy is not valid") : info.problem);
    }
    int dataEntries = 0;
    for (const backport::SelfEntry& e : info.entries) {
        if (!e.isData())
            continue;
        ++dataEntries;
        if (!within(fileSize, e.offset, e.filesz))
            return verdict(ExecutableClass::Truncated, QStringLiteral("segment %1 ends after the end of the file").arg(e.index));
        const int si = e.segmentIndex();
        if (si < int(info.phdrs.size()) && e.filesz > info.phdrs[size_t(si)].filesz)
            return verdict(ExecutableClass::Malformed, QStringLiteral("segment entry %1 is larger than its program header").arg(e.index));
    }
    if (dataEntries == 0)
        return verdict(ExecutableClass::Malformed, QStringLiteral("it has no data segments"));

    ExecutableInfo out = verdict(ExecutableClass::FakeSigned);
    // The SDK it was built for. The param is a piece of the ELF; a SELF stores
    // each data entry's bytes where SelfReader's unwrap() places them (at the
    // entry's program header offset, later segments over earlier ones), so the
    // param is read from the entry that covers it.
    if (const backport::ProgramHeader* param = paramHeader(info.phdrs); param && param->filesz >= backport::kMinParamSize) {
        int covering = -1;
        quint64 at = 0;
        for (const backport::SelfEntry& e : info.entries) {
            if (!e.isData())
                continue;
            const int si = e.segmentIndex();
            if (si >= int(info.phdrs.size()) || si < covering)
                continue;
            const backport::ProgramHeader& ph = info.phdrs[size_t(si)];
            if (param->offset < ph.offset || !within(e.filesz, param->offset - ph.offset, backport::kMinParamSize))
                continue;
            covering = si;
            at = e.offset + (param->offset - ph.offset);
        }
        if (covering >= 0)
            readParam(f, at, param->type == elf::PT_SCE_PROCPARAM, out);
    }
    return out;
}

ExecutableInfo inspectElf(QFile& f, qint64 size, QByteArray head)
{
    namespace elf = backport::elf;
    if (head.size() < elf::kEhdrSize)
        return verdict(ExecutableClass::Truncated, QStringLiteral("the ELF header is cut short"));
    // The program header table normally follows the header; when it does not
    // fit the first read, read up to its end (within the header limit).
    const backport::ElfHeader h = backport::ElfImage::readHeader(head);
    const quint64 fileSize = quint64(size);
    const quint64 tableBytes = quint64(h.phnum) * elf::kPhdrSize;
    if (h.phnum > 0 && !within(fileSize, h.phoff, tableBytes))
        return verdict(ExecutableClass::Truncated, QStringLiteral("the program header table lies after the end of the file"));
    const quint64 tableEnd = h.phoff + tableBytes;
    if (tableEnd > quint64(head.size())) {
        if (tableEnd > quint64(kDumpCheckMaxHeaderBytes))
            return verdict(ExecutableClass::PlainElf); // an unusual layout: an ELF all the same, not looked into further
        head = readAt(f, 0, qint64(tableEnd));
    }
    const auto image = backport::ElfImage::parse(head, backport::Strictness::Lenient);
    if (!image.ok()) {
        // A sound ELF of a kind the console does not run (32-bit, big-endian...) is not a damaged file.
        const bool otherKind = backport::codeOf(image.error()) == backport::Code::SignUnsupported;
        return verdict(otherKind ? ExecutableClass::Unknown : ExecutableClass::Malformed, image.error().message);
    }
    for (const backport::ProgramHeader& ph : image->programHeaders())
        if (ph.type == elf::PT_LOAD && ph.filesz > 0 && !within(fileSize, ph.offset, ph.filesz))
            return verdict(ExecutableClass::Truncated, QStringLiteral("segment %1 ends after the end of the file").arg(ph.index));
    ExecutableInfo out = verdict(ExecutableClass::PlainElf);
    if (const backport::ProgramHeader* param = paramHeader(image->programHeaders());
        param && param->filesz >= backport::kMinParamSize && within(fileSize, param->offset, backport::kMinParamSize))
        readParam(f, param->offset, param->type == elf::PT_SCE_PROCPARAM, out);
    return out;
}

// A finding about many files: their number and the first of them.
struct Sample {
    qint64 count = 0;
    QStringList paths;
    void add(const QString& path, int keep)
    {
        ++count;
        if (paths.size() < keep)
            paths << path;
    }
};

// The findings, counted exactly and listed up to the caps.
class Findings {
public:
    explicit Findings(const DumpCheckOptions& options) : m_options(options) {}

    void add(FindingSeverity severity, const QString& code, const QString& path, const QString& message)
    {
        count(severity, code, 1);
        DumpFinding f;
        f.severity = severity;
        f.code = code;
        f.path = path;
        f.message = message;
        list(std::move(f));
    }

    // One finding for `sample.count` files.
    void addGroup(FindingSeverity severity, const QString& code, const Sample& sample, const QString& message)
    {
        if (sample.count <= 0)
            return;
        (severity == FindingSeverity::Error ? errors : severity == FindingSeverity::Warning ? warnings : infos) += 1;
        byCode[code] += sample.count;
        DumpFinding f;
        f.severity = severity;
        f.code = code;
        f.message = message;
        f.count = sample.count;
        f.paths = sample.paths;
        list(std::move(f));
    }

    std::vector<DumpFinding> take()
    {
        std::vector<DumpFinding> out;
        for (auto* part : {&m_errors, &m_warnings, &m_infos})
            for (DumpFinding& f : *part) {
                if (int(out.size()) >= m_options.maxFindings) {
                    truncated = true;
                    break;
                }
                out.push_back(std::move(f));
            }
        return out;
    }

    qint64 errors = 0, warnings = 0, infos = 0;
    QMap<QString, qint64> byCode;
    bool truncated = false;

private:
    void count(FindingSeverity severity, const QString& code, qint64 n)
    {
        (severity == FindingSeverity::Error ? errors : severity == FindingSeverity::Warning ? warnings : infos) += n;
        byCode[code] += n;
    }
    void list(DumpFinding&& f)
    {
        std::vector<DumpFinding>& part = f.severity == FindingSeverity::Error ? m_errors
                                       : f.severity == FindingSeverity::Warning ? m_warnings : m_infos;
        int& ofCode = m_listed[f.code];
        if (ofCode >= m_options.maxPerCode || int(part.size()) >= m_options.maxFindings) {
            truncated = true;
            return;
        }
        ++ofCode;
        part.push_back(std::move(f));
    }

    DumpCheckOptions m_options;
    std::vector<DumpFinding> m_errors, m_warnings, m_infos;
    QHash<QString, int> m_listed;
};

QString versionText(const gameinfo::SystemVersion& v) { return v.isValid() ? v.toString() : QString(); }

// "it is ..." for a sentence about one executable.
QString classInWords(ExecutableClass c)
{
    switch (c) {
    case ExecutableClass::FakeSigned: return QStringLiteral("fake-signed");
    case ExecutableClass::Encrypted: return QStringLiteral("encrypted");
    case ExecutableClass::Compressed: return QStringLiteral("compressed");
    case ExecutableClass::PlainElf: return QStringLiteral("a plain ELF");
    case ExecutableClass::Truncated: return QStringLiteral("cut short");
    case ExecutableClass::Malformed: return QStringLiteral("not a valid executable");
    case ExecutableClass::Empty: return QStringLiteral("empty");
    case ExecutableClass::Unknown: return QStringLiteral("neither a SELF nor an ELF file");
    case ExecutableClass::Unreadable: break;
    }
    return QStringLiteral("unreadable");
}

bool sameName(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

} // namespace

QString findingSeverityName(FindingSeverity severity)
{
    switch (severity) {
    case FindingSeverity::Error: return QStringLiteral("error");
    case FindingSeverity::Warning: return QStringLiteral("warning");
    case FindingSeverity::Info: break;
    }
    return QStringLiteral("info");
}

QJsonObject DumpFinding::toJson() const
{
    QJsonObject o{{QStringLiteral("severity"), findingSeverityName(severity)}, {QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    if (!path.isEmpty())
        o.insert(QStringLiteral("path"), path);
    if (count > 0) {
        o.insert(QStringLiteral("count"), count);
        o.insert(QStringLiteral("paths"), QJsonArray::fromStringList(paths));
    }
    return o;
}

QString executableClassName(ExecutableClass c)
{
    switch (c) {
    case ExecutableClass::FakeSigned: return QStringLiteral("fake-signed");
    case ExecutableClass::Encrypted: return QStringLiteral("encrypted");
    case ExecutableClass::Compressed: return QStringLiteral("compressed");
    case ExecutableClass::PlainElf: return QStringLiteral("plain-elf");
    case ExecutableClass::Truncated: return QStringLiteral("truncated");
    case ExecutableClass::Malformed: return QStringLiteral("malformed");
    case ExecutableClass::Empty: return QStringLiteral("empty");
    case ExecutableClass::Unknown: return QStringLiteral("unknown");
    case ExecutableClass::Unreadable: break;
    }
    return QStringLiteral("unreadable");
}

ExecutableInfo inspectExecutable(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return verdict(ExecutableClass::Unreadable, f.errorString());
    const qint64 size = f.size();
    if (size == 0)
        return verdict(ExecutableClass::Empty);
    const QByteArray head = f.read(std::min(size, kDumpCheckHeaderBytes));
    if (head.isEmpty())
        return verdict(ExecutableClass::Unreadable, f.errorString());
    if (backport::selfio::hasSelfMagic(head))
        return inspectSelf(f, size, head);
    if (backport::elf::hasElfMagic(head))
        return inspectElf(f, size, head);
    return verdict(ExecutableClass::Unknown);
}

QString dumpNameProblem(const QString& name)
{
    if (name.size() > 255)
        return QStringLiteral("The name is longer than 255 characters, more than exFAT can store.");
    if (name.endsWith(u'.') || name.endsWith(u' '))
        return QStringLiteral("The name ends with a dot or a space, which Windows and exFAT drop.");
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == u'<' || u == u'>' || u == u':' || u == u'"' || u == u'/' || u == u'\\' || u == u'|' || u == u'?' || u == u'*')
            return QStringLiteral("The name has a character that exFAT and Windows cannot store (< > : \" / \\ | ? * or a control character).");
    }
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(name).hasMatch())
        return QStringLiteral("The name is reserved by Windows (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9).");
    return {};
}

QList<QStringList> caseCollisions(const QStringList& names)
{
    QList<QStringList> groups;
    QHash<QString, int> groupOf; // folded name -> index in `groups`
    for (const QString& name : names) {
        const QString key = name.toCaseFolded();
        const auto it = groupOf.constFind(key);
        if (it == groupOf.constEnd()) {
            groupOf.insert(key, int(groups.size()));
            groups << QStringList{name};
        } else if (!groups[*it].contains(name)) {
            groups[*it] << name;
        }
    }
    groups.removeIf([](const QStringList& g) { return g.size() < 2; });
    return groups;
}

bool isJunkFileName(const QString& name)
{
    const QString n = name.toLower();
    return n == QLatin1String("thumbs.db") || n == QLatin1String("desktop.ini") || n == QLatin1String(".ds_store")
        || n.endsWith(QLatin1String(".part")) || n.endsWith(QLatin1String(".tmp"));
}

bool isExecutableName(const QString& name)
{
    const QString n = name.toLower();
    return n == QLatin1String("eboot.bin") || n.endsWith(QLatin1String(".elf")) || n.endsWith(QLatin1String(".self"))
        || n.endsWith(QLatin1String(".prx")) || n.endsWith(QLatin1String(".sprx"));
}

QString DumpCheckResult::summary() const
{
    const QString inFiles = QStringLiteral("in %1").arg(countOf(files, QStringLiteral("file"), QStringLiteral("files")));
    if (errors == 0 && warnings == 0)
        return QStringLiteral("No problems found %1").arg(inFiles);
    QStringList parts;
    if (errors > 0)
        parts << countOf(errors, QStringLiteral("problem"), QStringLiteral("problems"));
    if (warnings > 0)
        parts << countOf(warnings, QStringLiteral("warning"), QStringLiteral("warnings"));
    return QStringLiteral("%1 %2").arg(parts.join(QStringLiteral(" and ")), inFiles);
}

QJsonObject DumpCheckResult::toJson() const
{
    QJsonArray list;
    for (const DumpFinding& f : findings)
        list.append(f.toJson());
    QJsonObject perCode;
    for (auto it = byCode.cbegin(); it != byCode.cend(); ++it)
        perCode.insert(it.key(), it.value());
    QJsonObject exec{{QStringLiteral("total"), executableCount}};
    for (auto it = executables.cbegin(); it != executables.cend(); ++it)
        exec.insert(it.key(), it.value());
    const QJsonObject counts{{QStringLiteral("files"), files},
                             {QStringLiteral("folders"), folders},
                             {QStringLiteral("bytes"), bytes},
                             {QStringLiteral("executables"), exec},
                             {QStringLiteral("errors"), errors},
                             {QStringLiteral("warnings"), warnings},
                             {QStringLiteral("infos"), infos},
                             {QStringLiteral("listed"), qint64(findings.size())},
                             {QStringLiteral("truncated"), truncated},
                             {QStringLiteral("byCode"), perCode}};
    return QJsonObject{{QStringLiteral("folder"), QDir::toNativeSeparators(folder)},
                       {QStringLiteral("findings"), list},
                       {QStringLiteral("counts"), counts},
                       {QStringLiteral("facts"), facts}};
}

Result<DumpCheckResult> checkDump(const QString& folderIn, const CancellationToken& token, ProgressReporter* progress,
                                  const DumpCheckOptions& options)
{
    const QString root = QDir::cleanPath(QFileInfo(folderIn).absoluteFilePath());
    if (folderIn.trimmed().isEmpty() || !QFileInfo(root).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(QDir::toNativeSeparators(folderIn)));
    using Severity = FindingSeverity;
    Findings found(options);
    DumpCheckResult result;
    result.folder = root;
    QJsonObject facts;
    if (progress)
        progress->begin(QStringLiteral("Checking"), 0, 0, true);

    // ---- where the game is --------------------------------------------------------------
    // The folder itself, or the one game below it. With several games only the
    // checks that make sense for any folder run.
    QString gameRoot = root, gameRel;
    bool severalGames = false;
    if (!gameinfo::hasEbootBin(root)) {
        const gameinfo::RootSearch search = gameinfo::findGameRoots(root, token);
        if (search.cancelled || token.isCancelled())
            return Error::cancelled();
        if (search.candidates.size() == 1) {
            gameRoot = search.candidates.first();
            gameRel = QDir(root).relativeFilePath(gameRoot);
            found.add(Severity::Info, QStringLiteral("game-nested"), gameRel,
                      QStringLiteral("The game is not in this folder but in %1 below it; it was checked as that game. Build from that folder.").arg(gameRel));
        } else if (search.candidates.size() > 1) {
            severalGames = true;
            QStringList names;
            for (const QString& c : search.candidates.mid(0, 10))
                names << QDir(root).relativeFilePath(c);
            QString listed = names.join(QStringLiteral(", "));
            if (search.candidates.size() > names.size())
                listed += QStringLiteral(", …");
            found.add(Severity::Info, QStringLiteral("several-games"), {},
                      QStringLiteral("This folder holds %1 games (%2). Only the file checks ran; check each game by itself.")
                          .arg(search.candidates.size())
                          .arg(listed));
        }
    }
    const auto inGame = [&gameRel](const QString& rel) { return gameRel.isEmpty() ? rel : gameRel + u'/' + rel; };
    const QString ebootRel = inGame(QStringLiteral("eboot.bin"));
    const QString fakelibRel = inGame(QStringLiteral("fakelib"));
    facts.insert(QStringLiteral("gameFolder"), gameRel);

    // ---- structure ------------------------------------------------------------------------
    bool ps5 = false, ps4 = false;
    gameinfo::SystemVersion paramSdk, paramFw;
    if (!severalGames) {
        const QDir game(gameRoot);
        const QFileInfo eboot(game.filePath(QStringLiteral("eboot.bin")));
        if (!eboot.isFile())
            found.add(Severity::Error, QStringLiteral("eboot-missing"), ebootRel, QStringLiteral("eboot.bin is missing: the game cannot start without it."));
        else if (eboot.size() == 0)
            found.add(Severity::Error, QStringLiteral("eboot-empty"), ebootRel, QStringLiteral("eboot.bin is empty (0 bytes): the game cannot start."));

        const QString paramJson = game.filePath(QStringLiteral("sce_sys/param.json"));
        const QString paramSfo = game.filePath(QStringLiteral("sce_sys/param.sfo"));
        const QString paramJsonRel = inGame(QStringLiteral("sce_sys/param.json"));
        if (QFileInfo(paramJson).isFile()) {
            ps5 = true;
            const auto param = gameinfo::readParamJson(paramJson);
            if (!param.ok()) {
                found.add(Severity::Error, QStringLiteral("param-unreadable"), paramJsonRel,
                          QStringLiteral("param.json cannot be read: %1").arg(param.error().message));
            } else {
                QStringList missing;
                if (param->titleId.trimmed().isEmpty())
                    missing << QStringLiteral("titleId");
                if (param->contentId.trimmed().isEmpty())
                    missing << QStringLiteral("contentId");
                if (param->contentVersion.trimmed().isEmpty())
                    missing << QStringLiteral("contentVersion");
                if (!missing.isEmpty())
                    found.add(Severity::Error, QStringLiteral("param-incomplete"), paramJsonRel,
                              QStringLiteral("param.json has no %1.").arg(missing.join(QStringLiteral(", "))));
                facts.insert(QStringLiteral("titleId"), param->titleId);
                facts.insert(QStringLiteral("contentId"), param->contentId);
                facts.insert(QStringLiteral("contentVersion"), param->contentVersion);
                facts.insert(QStringLiteral("title"), param->title);
                // The two fields as written, not the reader's fallback of one for the other.
                paramSdk = gameinfo::parseVersionField(param->raw.value(QStringLiteral("sdkVersion")));
                paramFw = gameinfo::parseVersionField(param->raw.value(QStringLiteral("requiredSystemSoftwareVersion")));
                facts.insert(QStringLiteral("paramSdk"), versionText(paramSdk));
                facts.insert(QStringLiteral("requiredFirmware"), versionText(paramFw));
            }
        } else if (QFileInfo(paramSfo).isFile()) {
            ps4 = true;
            found.add(Severity::Info, QStringLiteral("ps4-dump"), {}, QStringLiteral("PS4 dump: only basic checks apply."));
            const auto sfo = gameinfo::readSfo(paramSfo);
            if (!sfo.ok()) {
                found.add(Severity::Error, QStringLiteral("param-unreadable"), inGame(QStringLiteral("sce_sys/param.sfo")),
                          QStringLiteral("param.sfo cannot be read: %1").arg(sfo.error().message));
            } else {
                facts.insert(QStringLiteral("titleId"), sfo->string(QStringLiteral("TITLE_ID")));
                facts.insert(QStringLiteral("contentId"), sfo->string(QStringLiteral("CONTENT_ID")));
                const QString appVer = sfo->string(QStringLiteral("APP_VER"));
                facts.insert(QStringLiteral("contentVersion"), appVer.isEmpty() ? sfo->string(QStringLiteral("VERSION")) : appVer);
                facts.insert(QStringLiteral("title"), sfo->string(QStringLiteral("TITLE")));
            }
        } else {
            found.add(Severity::Error, QStringLiteral("param-missing"), paramJsonRel,
                      QStringLiteral("sce_sys/param.json is missing: the title ID and the version cannot be read."));
        }
        if (!QFileInfo(game.filePath(QStringLiteral("sce_sys/icon0.png"))).isFile())
            found.add(Severity::Warning, QStringLiteral("icon-missing"), inGame(QStringLiteral("sce_sys/icon0.png")),
                      QStringLiteral("sce_sys/icon0.png is missing: the game shows without its cover."));
    }
    facts.insert(QStringLiteral("platform"), ps5 ? QStringLiteral("PS5") : ps4 ? QStringLiteral("PS4") : QStringLiteral("Unknown"));

    // ---- the walk -------------------------------------------------------------------------
    Sample empty, junk, large, backups, fakeSigned, decryptedCopies;
    ExecutableInfo ebootInfo;
    bool ebootSeen = false;
    QMap<QString, qint64> sdks; // "4.50" -> executables built for it
    bool fakelibSeen = false;
    QStringList fakelibNames;   // capped
    qint64 fakelibFiles = 0, fakelibLibraries = 0;
    bool fakelibOnlyEmulators = true;
    int engineLibraries = 0;    // files that are the Backport engine's own, by size and SHA-256

    struct Folder {
        QString abs, rel; // rel: "" for the checked folder, else "a/b"
    };
    std::vector<Folder> pending{{root, QString()}};
    qint64 seen = 0;
    const QDir::Filters everything = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
    while (!pending.empty()) {
        const Folder folder = std::move(pending.back());
        pending.pop_back();
        const bool inFakelib = !severalGames && sameName(folder.rel, fakelibRel);
        // A decrypted folder holds plain copies of the executables on purpose.
        const bool inDecrypted = (u'/' + folder.rel + u'/').contains(QStringLiteral("/decrypted/"), Qt::CaseInsensitive);
        if (inFakelib)
            fakelibSeen = true;
        QStringList names;
        QDirIterator it(folder.abs, everything);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            // Paused jobs wait here; a cancelled one stops at the next entry.
            if ((++seen & 0x3F) == 0 ? !token.checkpoint() : token.isCancelled())
                return Error::cancelled();
            const QString name = fi.fileName();
            const QString rel = folder.rel.isEmpty() ? name : folder.rel + u'/' + name;
            names << name;
            if (const QString problem = dumpNameProblem(name); !problem.isEmpty())
                found.add(Severity::Error, QStringLiteral("bad-name"), rel, problem);
            if (fi.isSymbolicLink() || fi.isJunction()) {
                found.add(Severity::Warning, QStringLiteral("link"), rel,
                          QStringLiteral("A link or junction, not a real file or folder. It is not followed, so what it points at is not checked."));
                continue;
            }
            if (fi.isDir()) {
                ++result.folders;
                pending.push_back({fi.absoluteFilePath(), rel});
                continue;
            }
            const qint64 size = fi.size();
            ++result.files;
            result.bytes += size;
            if (progress) {
                progress->advance(size);
                progress->advanceFiles(1);
                if ((result.files & 0x3F) == 1)
                    progress->setCurrentItem(rel);
            }
            const bool executable = isExecutableName(name);
            if (size == 0 && !executable)
                empty.add(rel, options.maxSamplePaths);
            if (size > kFat32MaxFileBytes)
                large.add(rel, options.maxSamplePaths);
            if (isJunkFileName(name))
                junk.add(rel, options.maxSamplePaths);
            if (name.endsWith(QLatin1String(".bak"), Qt::CaseInsensitive))
                backups.add(rel, options.maxSamplePaths);
            if (inFakelib) {
                ++fakelibFiles;
                if (fakelibNames.size() < kMaxFakelibNames)
                    fakelibNames << name;
                if (name.endsWith(QLatin1String(".sprx"), Qt::CaseInsensitive) || name.endsWith(QLatin1String(".prx"), Qt::CaseInsensitive)) {
                    ++fakelibLibraries;
                    fakelibOnlyEmulators = fakelibOnlyEmulators && (sameName(name, QStringLiteral("libSceAmpr.sprx")) || sameName(name, QStringLiteral("libScePlayGo.sprx")));
                }
                // The Backport engine's own libraries are pinned by size and hash (small files; hashed only when the size fits).
                for (const backport::PinnedLibrary& lib : backport::kCanonicalTrio) {
                    if (!sameName(name, QString::fromLatin1(lib.name)) || size != lib.size)
                        continue;
                    const auto hash = sha256File(fi.absoluteFilePath(), token);
                    if (!hash.ok() && hash.error().isCancelled())
                        return Error::cancelled();
                    if (hash.ok() && hash.value().compare(QLatin1String(lib.sha256), Qt::CaseInsensitive) == 0)
                        ++engineLibraries;
                }
            }
            if (!executable)
                continue;

            if (progress)
                progress->setCurrentItem(rel);
            const ExecutableInfo info = size == 0 ? verdict(ExecutableClass::Empty) : inspectExecutable(fi.absoluteFilePath());
            const bool gameEboot = !severalGames && sameName(rel, ebootRel);
            if (gameEboot) {
                ebootInfo = info;
                ebootSeen = true;
            }
            ++result.executableCount;
            ++result.executables[executableClassName(info.kind)];
            if (info.hasSdk && !ps4) {
                const QString sdk = backport::sdkVersionText(info.ps5Sdk);
                if (sdks.contains(sdk) || sdks.size() < kMaxSdkValues)
                    ++sdks[sdk];
            }
            switch (info.kind) {
            case ExecutableClass::FakeSigned:
                fakeSigned.add(rel, 0);
                break;
            case ExecutableClass::Encrypted:
                found.add(Severity::Error, QStringLiteral("exec-encrypted"), rel,
                          QStringLiteral("Encrypted retail executable: it cannot run from a dump. The dump was not decrypted."));
                break;
            case ExecutableClass::Compressed:
                found.add(Severity::Error, QStringLiteral("exec-compressed"), rel,
                          QStringLiteral("Compressed executable, as retail packages hold them: it cannot run from a dump."));
                break;
            case ExecutableClass::Truncated:
                found.add(Severity::Error, QStringLiteral("exec-truncated"), rel, QStringLiteral("The file is cut short: %1.").arg(info.problem));
                break;
            case ExecutableClass::Malformed:
                found.add(Severity::Error, QStringLiteral("exec-malformed"), rel, QStringLiteral("Not a valid executable: %1.").arg(info.problem));
                break;
            case ExecutableClass::Empty:
                if (!gameEboot) // the game's own eboot.bin is reported as eboot-empty
                    found.add(Severity::Error, QStringLiteral("exec-empty"), rel, QStringLiteral("Empty executable (0 bytes)."));
                break;
            case ExecutableClass::Unreadable:
                found.add(Severity::Error, QStringLiteral("exec-unreadable"), rel, QStringLiteral("The file could not be opened: %1").arg(info.problem));
                break;
            case ExecutableClass::Unknown:
                if (gameEboot)
                    found.add(Severity::Error, QStringLiteral("exec-malformed"), rel,
                              info.problem.isEmpty() ? QStringLiteral("Not a valid executable: it is neither a SELF nor an ELF file.")
                                                     : QStringLiteral("Not a valid executable: %1.").arg(info.problem));
                else
                    found.add(Severity::Warning, QStringLiteral("exec-unknown"), rel,
                              info.problem.isEmpty() ? QStringLiteral("Named like an executable, but neither a SELF nor an ELF file.")
                                                     : QStringLiteral("An ELF of a kind the console does not load: %1.").arg(info.problem));
                break;
            case ExecutableClass::PlainElf:
                if (inDecrypted)
                    decryptedCopies.add(rel, options.maxSamplePaths);
                else
                    found.add(Severity::Warning, QStringLiteral("exec-unsigned"), rel,
                              QStringLiteral("Plain ELF, not signed: fine for a build, but it will not load on a console as it is."));
                break;
            }
        }
        if (names.isEmpty()) {
            // Empty, or not ours to open? An unreadable folder must not pass for an empty one.
            if (const Status listable = ensureFolderListable(folder.abs); !listable.ok())
                found.add(Severity::Error, QStringLiteral("folder-unreadable"), folder.rel,
                          QStringLiteral("This folder cannot be opened, so nothing in it was checked. Check its permissions."));
            continue;
        }
        for (const QStringList& twins : caseCollisions(names)) {
            QStringList quoted;
            for (const QString& t : twins.mid(0, 4))
                quoted << QStringLiteral("“%1”").arg(t);
            found.add(Severity::Error, QStringLiteral("case-collision"), folder.rel.isEmpty() ? twins.first() : folder.rel + u'/' + twins.first(),
                      QStringLiteral("%1 differ only in upper and lower case: exFAT and the console's game folders keep one of them.")
                          .arg(quoted.join(QStringLiteral(" and "))));
        }
    }

    // ---- executables and what a tool left behind -----------------------------------------------
    found.addGroup(Severity::Info, QStringLiteral("exec-fake-signed"), fakeSigned,
                   fakeSigned.count == 1 ? QStringLiteral("1 executable is fake-signed.")
                                         : QStringLiteral("%1 executables are fake-signed.").arg(formatCount(fakeSigned.count)));
    found.addGroup(Severity::Info, QStringLiteral("decrypted-copies"), decryptedCopies,
                   QStringLiteral("%1 in a decrypted folder: a dump extra the game does not need.")
                       .arg(countOf(decryptedCopies.count, QStringLiteral("unsigned copy"), QStringLiteral("unsigned copies"))));
    QJsonObject sdkCounts;
    for (auto it = sdks.cbegin(); it != sdks.cend(); ++it)
        sdkCounts.insert(it.key(), it.value());
    facts.insert(QStringLiteral("executableSdks"), sdkCounts);

    // ---- backport state (PS5 games) ------------------------------------------------------------
    if (ps5 && !severalGames) {
        const QString ebootSdk = ebootInfo.hasSdk ? backport::sdkVersionText(ebootInfo.ps5Sdk) : QString();
        facts.insert(QStringLiteral("ebootClass"), ebootSeen ? executableClassName(ebootInfo.kind) : QStringLiteral("missing"));
        facts.insert(QStringLiteral("ebootSdk"), ebootSdk);
        facts.insert(QStringLiteral("fakelib"), QJsonObject{{QStringLiteral("present"), fakelibSeen},
                                                           {QStringLiteral("files"), fakelibFiles},
                                                           {QStringLiteral("libraries"), fakelibLibraries},
                                                           {QStringLiteral("names"), QJsonArray::fromStringList(fakelibNames)},
                                                           {QStringLiteral("engineLibraries"), engineLibraries}});
        if (fakelibSeen) {
            QString holds = fakelibFiles == 0 ? QStringLiteral("The fakelib folder is empty.")
                                              : QStringLiteral("fakelib holds %1: %2%3.")
                                                    .arg(countOf(fakelibFiles, QStringLiteral("file"), QStringLiteral("files")),
                                                         fakelibNames.mid(0, 6).join(QStringLiteral(", ")),
                                                         fakelibFiles > 6 ? QStringLiteral(" and %1 more").arg(formatCount(fakelibFiles - 6)) : QString());
            if (engineLibraries > 0)
                holds += QStringLiteral(" %1 this app's Backport engine.")
                             .arg(engineLibraries == 1 ? QStringLiteral("One of them is a library of") : QStringLiteral("%1 of them are libraries of").arg(engineLibraries));
            found.add(Severity::Info, QStringLiteral("fakelib-present"), fakelibRel, holds);
        }
        // The SDK in the executable against the one param.json names: they differ once a tool re-tagged the executables.
        const auto ebootMajor = ebootInfo.hasSdk ? backport::bcdMajor(ebootInfo.ps5Sdk) : std::nullopt;
        const auto ebootMinor = ebootInfo.hasSdk ? backport::bcdMinor(ebootInfo.ps5Sdk) : std::nullopt;
        const bool sdkDiffers = ebootMajor && paramSdk.isValid()
            && (*ebootMajor != paramSdk.majorVersion || (ebootMinor && *ebootMinor != paramSdk.minorVersion));
        if (sdkDiffers)
            found.add(Severity::Info, QStringLiteral("sdk-differs"), ebootRel,
                      QStringLiteral("eboot.bin targets SDK %1, param.json says SDK %2: the executables were re-tagged (a backport or an SDK fix).")
                          .arg(ebootSdk, versionText(paramSdk)));
        const QString targets = ebootSdk.isEmpty() ? QStringLiteral("the SDK of eboot.bin could not be read")
                                                   : QStringLiteral("executables target SDK %1").arg(ebootSdk);
        QString state, text;
        const bool ebootUsable = ebootSeen && (ebootInfo.kind == ExecutableClass::FakeSigned || ebootInfo.kind == ExecutableClass::PlainElf);
        if (fakelibLibraries > 0 && (!fakelibOnlyEmulators || sdkDiffers || engineLibraries > 0)) {
            state = QStringLiteral("backported");
            text = QStringLiteral("Looks backported (fakelib present; %1).").arg(targets);
        } else if (fakelibLibraries > 0) {
            state = QStringLiteral("emulator-libraries");
            text = QStringLiteral("Has emulator libraries in fakelib (%1), as a backport or a packed folder does; %2.")
                       .arg(fakelibNames.mid(0, 2).join(QStringLiteral(", ")), targets);
        } else if (sdkDiffers) {
            state = QStringLiteral("retagged");
            text = QStringLiteral("Looks re-tagged (no fakelib, but eboot.bin targets SDK %1 and param.json says %2).").arg(ebootSdk, versionText(paramSdk));
        } else if (ebootUsable) {
            state = QStringLiteral("original");
            text = QStringLiteral("Looks original (no fakelib; %1).").arg(targets);
        } else {
            state = QStringLiteral("unknown");
            text = QStringLiteral("The backport state cannot be told: eboot.bin could not be read as an executable.");
        }
        facts.insert(QStringLiteral("backportState"), state);
        facts.insert(QStringLiteral("backportSummary"), text);
        found.add(Severity::Info, QStringLiteral("backport-state"), {}, text);
        if (fakelibLibraries > 0 && ebootSeen && ebootInfo.kind != ExecutableClass::FakeSigned)
            found.add(Severity::Warning, QStringLiteral("fakelib-eboot-not-signed"), ebootRel,
                      QStringLiteral("fakelib is present but eboot.bin is not fake-signed (it is %1): a half-finished or half-restored backport.")
                          .arg(classInWords(ebootInfo.kind)));
    }
    found.addGroup(Severity::Info, QStringLiteral("backup-files"), backups,
                   QStringLiteral("%1 left by a tool that changed files in place.").arg(countOf(backups.count, QStringLiteral(".bak file"), QStringLiteral(".bak files"))));

    // ---- hygiene ---------------------------------------------------------------------------------
    found.addGroup(Severity::Info, QStringLiteral("empty-files"), empty,
                   empty.count == 1 ? QStringLiteral("1 file is empty (0 bytes).") : QStringLiteral("%1 files are empty (0 bytes).").arg(formatCount(empty.count)));
    found.addGroup(Severity::Info, QStringLiteral("junk-files"), junk,
                   QStringLiteral("%1 the game does not need (Thumbs.db, desktop.ini, .DS_Store, *.part, *.tmp).")
                       .arg(countOf(junk.count, QStringLiteral("file"), QStringLiteral("files"))));
    found.addGroup(Severity::Info, QStringLiteral("large-files"), large,
                   QStringLiteral("%1 larger than 4 GiB: that needs exFAT or PFS; a FAT32 USB drive cannot hold it.")
                       .arg(countOf(large.count, QStringLiteral("file is"), QStringLiteral("files are"))));

    result.findings = found.take();
    result.errors = found.errors;
    result.warnings = found.warnings;
    result.infos = found.infos;
    result.byCode = found.byCode;
    result.truncated = found.truncated;
    result.facts = facts;
    if (progress)
        progress->flush();
    return result;
}

} // namespace fw::tools
