// tools.dump.check: a clean dump passes; every finding code has a crafted
// dump that raises it; the check changes nothing in the dump, can be
// cancelled, and stays small whatever it is given.

#include "core/FileUtil.h"
#include "core/FsLinks.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SdkTable.h"
#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfReader.h"
#include "engines/tools/DumpCheck.h"
#include "engines/tools/Module.h"
#include "support/SparseFile.h"
#include "support/TestSupport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

using namespace fw;
using namespace fw::tools;
namespace elf = fw::backport::elf;
namespace self = fw::backport::self;

namespace {

template <typename T>
void put(QByteArray& b, qsizetype at, T v)
{
    qToLittleEndian<T>(v, b.data() + at);
}

constexpr quint64 kSegmentAt = 0x4000;
constexpr quint32 kSdk900 = 0x09000001;
constexpr quint32 kSdk450 = 0x04500001;

struct Segment {
    quint32 type;
    quint64 offset; // in the ELF
    quint64 filesz;
};

QByteArray elfHeaderAndTable(const std::vector<Segment>& segments)
{
    QByteArray h(elf::kEhdrSize + qsizetype(segments.size()) * elf::kPhdrSize, '\0');
    std::memcpy(h.data(), "\x7F" "ELF", 4);
    h[4] = 2; // 64-bit
    h[5] = 1; // little endian
    h[6] = 1;
    put<quint16>(h, 0x10, elf::ET_SCE_EXEC);
    put<quint16>(h, 0x12, elf::EM_X86_64);
    put<quint32>(h, 0x14, 1);
    put<quint64>(h, 0x20, elf::kEhdrSize); // e_phoff
    put<quint16>(h, 0x34, elf::kEhdrSize);
    put<quint16>(h, 0x36, elf::kPhdrSize);
    put<quint16>(h, 0x38, quint16(segments.size()));
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const qsizetype p = elf::kEhdrSize + qsizetype(i) * elf::kPhdrSize;
        put<quint32>(h, p + 0x00, segments[i].type);
        put<quint64>(h, p + 0x08, segments[i].offset);
        put<quint64>(h, p + 0x20, segments[i].filesz);
        put<quint64>(h, p + 0x28, segments[i].filesz);
        put<quint64>(h, p + 0x30, 0x4000);
    }
    return h;
}

// The one data segment: some bytes, then a process param for `sdk` at +0x100.
constexpr quint64 kParamInSegment = 0x100;
QByteArray segmentData(quint32 sdk)
{
    QByteArray d(0x400, '\x5A');
    std::memset(d.data() + kParamInSegment, 0, 0x40);
    put<quint64>(d, kParamInSegment + 0x00, 0x40);
    put<quint32>(d, kParamInSegment + 0x08, backport::kProcParamMagic);
    put<quint32>(d, kParamInSegment + 0x10, 0x09008001);
    put<quint32>(d, kParamInSegment + 0x14, sdk);
    return d;
}

std::vector<Segment> segments(qsizetype dataSize)
{
    return {{elf::PT_LOAD, kSegmentAt, quint64(dataSize)}, {elf::PT_SCE_PROCPARAM, kSegmentAt + kParamInSegment, 0x40}};
}

// A fake-signed SELF as make_fself writes one: common header, one data entry
// for program header 0, the ELF header copy, then the segment's bytes at
// `dataAt` (0 = right after the header).
QByteArray fakeSelf(quint32 sdk = kSdk900, quint64 entryProps = self::props::kHasBlocks, qsizetype dataAt = 0)
{
    const QByteArray stored = segmentData(sdk);
    const QByteArray elfPart = elfHeaderAndTable(segments(stored.size()));
    const qsizetype elfAt = 0x40; // common header + one entry
    if (dataAt == 0)
        dataAt = qsizetype(self::alignUp(quint64(elfAt + elfPart.size() + self::kExInfoSize), 0x100));
    QByteArray s(dataAt, '\0');
    put<quint32>(s, 0x00, self::kMagicFake);
    s[4] = 0x00;
    s[5] = 0x01;
    s[6] = 0x01;
    s[7] = 0x12;
    put<quint32>(s, 0x08, 0x101);
    put<quint64>(s, 0x10, quint64(dataAt + stored.size())); // file size
    put<quint16>(s, 0x18, 1);                               // one entry
    put<quint16>(s, 0x1A, 0x22);
    put<quint64>(s, 0x20, entryProps);
    put<quint64>(s, 0x28, quint64(dataAt));
    put<quint64>(s, 0x30, quint64(stored.size()));
    put<quint64>(s, 0x38, quint64(stored.size()));
    std::memcpy(s.data() + elfAt, elfPart.constData(), size_t(elfPart.size()));
    return s + stored;
}

QByteArray plainElf(quint32 sdk = kSdk900)
{
    const QByteArray stored = segmentData(sdk);
    QByteArray e = elfHeaderAndTable(segments(stored.size()));
    e.resize(qsizetype(kSegmentAt), '\0');
    return e + stored;
}

QByteArray paramJson(const QString& sdk = QStringLiteral("0x0900000000000000"))
{
    return QStringLiteral("{\n"
                          "  \"contentId\": \"UP9000-PPSA00026_00-ABCDEFGHIJKLMNOP\",\n"
                          "  \"contentVersion\": \"01.002.000\",\n"
                          "  \"titleId\": \"PPSA00026\",\n"
                          "  \"sdkVersion\": \"%1\",\n"
                          "  \"requiredSystemSoftwareVersion\": \"%1\",\n"
                          "  \"localizedParameters\": {\"defaultLanguage\": \"en-US\", \"en-US\": {\"titleName\": \"Check Me\"}}\n"
                          "}\n")
        .arg(sdk)
        .toUtf8();
}

// An independent param.sfo writer (two string fields are enough here).
QByteArray paramSfo()
{
    struct Field {
        QByteArray key, value;
    };
    const std::vector<Field> fields = {{"CONTENT_ID", "UP0000-CUSA00026_00-ABCDEFGHIJKLMNOP"}, {"TITLE", "Old Game"}, {"TITLE_ID", "CUSA00026"}};
    QByteArray keys, data, table;
    for (const Field& f : fields) {
        QByteArray value = f.value + '\0';
        const quint32 maxLen = quint32((value.size() + 3) & ~3);
        QByteArray e(16, '\0');
        put<quint16>(e, 0, quint16(keys.size()));
        put<quint16>(e, 2, quint16(0x0204));
        put<quint32>(e, 4, quint32(value.size()));
        put<quint32>(e, 8, maxLen);
        put<quint32>(e, 12, quint32(data.size()));
        table += e;
        keys += f.key + '\0';
        value.resize(maxLen, '\0');
        data += value;
    }
    while (keys.size() % 4)
        keys.append('\0');
    QByteArray h(20, '\0');
    put<quint32>(h, 0, 0x46535000u);
    put<quint32>(h, 4, 0x101u);
    put<quint32>(h, 8, 20 + quint32(table.size()));
    put<quint32>(h, 12, 20 + quint32(table.size()) + quint32(keys.size()));
    put<quint32>(h, 16, quint32(fields.size()));
    return h + table + keys + data;
}

bool write(const QString& path, const QByteArray& bytes) { return fwtest::writeTextFile(path, bytes); }

// Every file and folder of `dir` with its size, time stamp and content hash.
QMap<QString, QString> fingerprint(const QString& dir)
{
    QMap<QString, QString> out;
    QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi = it.nextFileInfo();
        const QString rel = QDir(dir).relativeFilePath(fi.filePath());
        if (fi.isDir()) {
            out.insert(rel + u'/', QStringLiteral("dir"));
            continue;
        }
        QFile f(fi.filePath());
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (f.open(QIODevice::ReadOnly))
            hash.addData(&f);
        out.insert(rel, QStringLiteral("%1|%2|%3").arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()).arg(QString::fromLatin1(hash.result().toHex())));
    }
    return out;
}

// A file whose name Windows itself would not let a program create the usual way.
bool writeOddName(const QString& dir, const QString& name, const QByteArray& bytes)
{
#ifdef Q_OS_WIN
    const std::wstring native = (QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(QDir(dir).absolutePath()) + u'\\' + name).toStdWString();
    const HANDLE h = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.constData(), DWORD(bytes.size()), &written, nullptr) && written == DWORD(bytes.size());
    CloseHandle(h);
    return ok;
#else
    return write(dir + u'/' + name, bytes);
#endif
}

void removeOddName(const QString& dir, const QString& name)
{
#ifdef Q_OS_WIN
    const std::wstring native = (QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(QDir(dir).absolutePath()) + u'\\' + name).toStdWString();
    DeleteFileW(native.c_str());
#else
    QFile::remove(dir + u'/' + name);
#endif
}

} // namespace

class TstDumpCheck : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_n = 0;

    // A sound PS5 dump: fake-signed executables, param.json, a cover, data.
    QString makeDump(const QString& name = {})
    {
        const QString g = m_root.filePath(name.isEmpty() ? QStringLiteral("dump%1").arg(++m_n) : name);
        write(g + QStringLiteral("/eboot.bin"), fakeSelf());
        write(g + QStringLiteral("/sce_sys/param.json"), paramJson());
        write(g + QStringLiteral("/sce_sys/icon0.png"), QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(64, 'i'));
        write(g + QStringLiteral("/sce_module/libc.prx"), fakeSelf());
        write(g + QStringLiteral("/sce_module/libSceFios2.sprx"), fakeSelf());
        fwtest::writeRandomFile(g + QStringLiteral("/data/assets.bin"), 70000, 7);
        write(g + QStringLiteral("/data/config/settings.ini"), "[game]\nlanguage=en\n");
        return g;
    }

    static DumpCheckResult check(const QString& folder, const DumpCheckOptions& options = {})
    {
        auto r = checkDump(folder, CancellationToken(), nullptr, options);
        if (!r.ok()) {
            qWarning("checkDump failed: %s", qPrintable(r.error().toLogString()));
            return {};
        }
        return r.value();
    }

    static const DumpFinding* find(const DumpCheckResult& r, const QString& code, const QString& path = {})
    {
        for (const DumpFinding& f : r.findings)
            if (f.code == code && (path.isEmpty() || f.path == path))
                return &f;
        return nullptr;
    }

    static QStringList codes(const DumpCheckResult& r, FindingSeverity severity)
    {
        QStringList out;
        for (const DumpFinding& f : r.findings)
            if (f.severity == severity)
                out << f.code;
        return out;
    }

    static JobContext context(ProgressReporter& progress, const CancellationToken& token = {})
    {
        JobContext ctx;
        ctx.jobId = QStringLiteral("t");
        ctx.token = token;
        ctx.progress = &progress;
        ctx.log = Log(QStringLiteral("test"));
        return ctx;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- the pieces without file access ---------------------------------------------------
    void namesThatCannotBeStored()
    {
        QVERIFY(dumpNameProblem(QStringLiteral("eboot.bin")).isEmpty());
        QVERIFY(dumpNameProblem(QStringLiteral("Ghost of Yōtei (01.000).bin")).isEmpty());
        QVERIFY(dumpNameProblem(QString(255, u'a')).isEmpty());
        QVERIFY(dumpNameProblem(QString(256, u'a')).contains(QStringLiteral("255")));
        QVERIFY(dumpNameProblem(QStringLiteral("readme.")).contains(QStringLiteral("dot or a space")));
        QVERIFY(dumpNameProblem(QStringLiteral("readme ")).contains(QStringLiteral("dot or a space")));
        QVERIFY(dumpNameProblem(QStringLiteral("NUL")).contains(QStringLiteral("reserved")));
        QVERIFY(dumpNameProblem(QStringLiteral("com3.txt")).contains(QStringLiteral("reserved")));
        // Windows reads the superscript digits of Latin-1 as digits in device names.
        QVERIFY(dumpNameProblem(QStringLiteral("COM¹")).contains(QStringLiteral("reserved")));
        QVERIFY(dumpNameProblem(QStringLiteral("lpt³.log")).contains(QStringLiteral("reserved")));
        // The console devices, and a device name with spaces before its extension.
        QVERIFY(dumpNameProblem(QStringLiteral("CONIN$")).contains(QStringLiteral("reserved")));
        QVERIFY(dumpNameProblem(QStringLiteral("conout$.txt")).contains(QStringLiteral("reserved")));
        QVERIFY(dumpNameProblem(QStringLiteral("AUX  .dat")).contains(QStringLiteral("reserved")));
        // The message's list names the device the user typed.
        QVERIFY(dumpNameProblem(QStringLiteral("CONIN$")).contains(QStringLiteral("CONIN$,")));
        QVERIFY(dumpNameProblem(QStringLiteral("conout$.txt")).contains(QStringLiteral("CONOUT$,")));
        QVERIFY(dumpNameProblem(QStringLiteral("COM¹")).contains(QStringLiteral("COM¹ to COM³")));
        QVERIFY(dumpNameProblem(QStringLiteral("COM10")).isEmpty());
        QVERIFY(dumpNameProblem(QStringLiteral("CONIN")).isEmpty());
        QVERIFY(dumpNameProblem(QStringLiteral("CON$")).isEmpty());
        QVERIFY(dumpNameProblem(QStringLiteral("console.txt")).isEmpty());
        QVERIFY(dumpNameProblem(QStringLiteral("a:b")).contains(QStringLiteral("character")));
        QVERIFY(dumpNameProblem(QStringLiteral("what?.bin")).contains(QStringLiteral("character")));
        QVERIFY(dumpNameProblem(QStringLiteral("tab\there")).contains(QStringLiteral("character")));
    }

    void caseTwinsAreGrouped()
    {
        QCOMPARE(caseCollisions({QStringLiteral("data"), QStringLiteral("eboot.bin"), QStringLiteral("Data"), QStringLiteral("DATA"),
                                 QStringLiteral("readme.txt"), QStringLiteral("README.TXT")}),
                 (QList<QStringList>{{QStringLiteral("data"), QStringLiteral("Data"), QStringLiteral("DATA")},
                                     {QStringLiteral("readme.txt"), QStringLiteral("README.TXT")}}));
        QVERIFY(caseCollisions({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a1")}).isEmpty());
        // A name listed twice is one name, not a twin of itself.
        QVERIFY(caseCollisions({QStringLiteral("a"), QStringLiteral("a")}).isEmpty());
        // exFAT compares names one UTF-16 unit at a time through its up-case
        // table: "ß" stays "ß" there and "ﬁ" stays one letter, so these are
        // two files each, not twins.
        QVERIFY(caseCollisions({QStringLiteral("straße.bin"), QStringLiteral("STRASSE.bin")}).isEmpty());
        QVERIFY(caseCollisions({QStringLiteral("ﬁle.dat"), QStringLiteral("FILE.dat")}).isEmpty());
        // Letters outside ASCII with a case pair are twins as well.
        QCOMPARE(caseCollisions({QStringLiteral("Ärger"), QStringLiteral("ärger")}).size(), 1);
        // Its table, not Unicode case folding: the Kelvin sign (U+212A), the
        // long s (U+017F) and the micro sign (U+00B5) keep their own names
        // there, and a letter outside the BMP (Deseret, two UTF-16 units) has
        // no up-case at all, so each pair is two files.
        QVERIFY(caseCollisions({QStringLiteral("k.bin"), QString(QChar(0x212A)) + QStringLiteral(".bin")}).isEmpty());
        QVERIFY(caseCollisions({QStringLiteral("s.bin"), QString(QChar(0x017F)) + QStringLiteral(".bin")}).isEmpty());
        QVERIFY(caseCollisions({QString(QChar(0x00B5)) + QStringLiteral(".bin"), QString(QChar(0x03BC)) + QStringLiteral(".bin")}).isEmpty());
        QVERIFY(caseCollisions({QString::fromUcs4(U"\U00010400.bin"), QString::fromUcs4(U"\U00010428.bin")}).isEmpty());
        // Greek and Cyrillic letters with a case pair are twins.
        QCOMPARE(caseCollisions({QString(QChar(0x03A3)) + QStringLiteral(".bin"), QString(QChar(0x03C3)) + QStringLiteral(".bin")}).size(), 1);
        QCOMPARE(caseCollisions({QString(QChar(0x0416)) + QStringLiteral(".bin"), QString(QChar(0x0436)) + QStringLiteral(".bin")}).size(), 1);
    }

    void junkAndExecutableNames()
    {
        for (const char* n : {"Thumbs.db", "desktop.ini", ".DS_Store", "download.part", "x.TMP"})
            QVERIFY2(isJunkFileName(QString::fromLatin1(n)), n);
        for (const char* n : {"thumbs.dbx", "part", "tmp.bin", "eboot.bin"})
            QVERIFY2(!isJunkFileName(QString::fromLatin1(n)), n);
        for (const char* n : {"eboot.bin", "EBOOT.BIN", "libc.prx", "libSceAgc.sprx", "game.elf", "x.self"})
            QVERIFY2(isExecutableName(QString::fromLatin1(n)), n);
        for (const char* n : {"eboot.bin.bak", "right.sprx.txt", "param.json", "self"})
            QVERIFY2(!isExecutableName(QString::fromLatin1(n)), n);
    }

    // ---- executables: the header alone says what the file is -----------------------------------
    void executablesAreClassifiedFromTheirHeaders()
    {
        const QString dir = m_root.filePath(QStringLiteral("exec"));
        const auto classOf = [&dir](const QString& name, const QByteArray& bytes) {
            write(dir + u'/' + name, bytes);
            return inspectExecutable(dir + u'/' + name);
        };
        ExecutableInfo e = classOf(QStringLiteral("fake.bin"), fakeSelf(kSdk450));
        QCOMPARE(e.kind, ExecutableClass::FakeSigned);
        QVERIFY(e.hasSdk);
        QCOMPARE(backport::sdkVersionText(e.ps5Sdk), QStringLiteral("4.50"));
        // The same answer as the whole-file reader gives (unwrap, then the process param).
        {
            const QByteArray whole = fakeSelf(kSdk450);
            const auto elfBytes = backport::selfio::unwrap(whole);
            QVERIFY(elfBytes.ok());
            quint32 ps5 = 0;
            QVERIFY(backport::elfSdkMajor(elfBytes.value(), &ps5).has_value());
            QCOMPARE(e.ps5Sdk, ps5);
        }
        // A header that ends long before the segment's bytes: nothing in between is needed.
        e = classOf(QStringLiteral("far.bin"), fakeSelf(kSdk900, self::props::kHasBlocks, 0x30000));
        QCOMPARE(e.kind, ExecutableClass::FakeSigned);
        QCOMPARE(backport::sdkVersionText(e.ps5Sdk), QStringLiteral("9.00"));

        QCOMPARE(classOf(QStringLiteral("enc.bin"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kEncrypted)).kind, ExecutableClass::Encrypted);
        QCOMPARE(classOf(QStringLiteral("comp.bin"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kCompressed)).kind, ExecutableClass::Compressed);
        // Encrypted is told from the entry table, even when the rest of the file is missing.
        QCOMPARE(classOf(QStringLiteral("enc-cut.bin"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kEncrypted).left(0x80)).kind,
                 ExecutableClass::Encrypted);

        QByteArray cut = fakeSelf();
        cut.chop(0x180);
        e = classOf(QStringLiteral("cut.bin"), cut);
        QCOMPARE(e.kind, ExecutableClass::Truncated);
        QVERIFY2(e.problem.contains(QStringLiteral("after the end of the file")), qPrintable(e.problem));
        QCOMPARE(classOf(QStringLiteral("stub.bin"), fakeSelf().left(0x10)).kind, ExecutableClass::Truncated);
        QCOMPARE(classOf(QStringLiteral("table-cut.bin"), fakeSelf().left(0x30)).kind, ExecutableClass::Truncated);
        QCOMPARE(classOf(QStringLiteral("no-entries.bin"), [] {
                     QByteArray s = fakeSelf();
                     put<quint64>(s, 0x20, 0); // the one entry is not a data entry any more
                     return s;
                 }()).kind,
                 ExecutableClass::Malformed);

        // A header that claims an entry table of megabytes is not read to its end.
        e = classOf(QStringLiteral("huge-table.bin"), [] {
            QByteArray s = fakeSelf();
            put<quint16>(s, 0x18, quint16(0xFFFF));
            return s;
        }());
        QCOMPARE(e.kind, ExecutableClass::Malformed);

        e = classOf(QStringLiteral("plain.elf"), plainElf(kSdk450));
        QCOMPARE(e.kind, ExecutableClass::PlainElf);
        QVERIFY(e.hasSdk);
        QCOMPARE(backport::sdkVersionText(e.ps5Sdk), QStringLiteral("4.50"));
        QByteArray shortElf = plainElf();
        shortElf.chop(0x100);
        QCOMPARE(classOf(QStringLiteral("short.elf"), shortElf).kind, ExecutableClass::Truncated);
        QCOMPARE(classOf(QStringLiteral("tiny.elf"), plainElf().left(0x20)).kind, ExecutableClass::Truncated);
        QByteArray elf32 = plainElf();
        elf32[4] = 1; // a sound file of another kind: not "damaged"
        QCOMPARE(classOf(QStringLiteral("32.elf"), elf32).kind, ExecutableClass::Unknown);

        QCOMPARE(classOf(QStringLiteral("text.prx"), "just some text").kind, ExecutableClass::Unknown);
        QCOMPARE(classOf(QStringLiteral("empty.prx"), QByteArray()).kind, ExecutableClass::Empty);
        QCOMPARE(inspectExecutable(dir + QStringLiteral("/not-there.prx")).kind, ExecutableClass::Unreadable);
    }

    // ---- a clean dump ---------------------------------------------------------------------
    void aCleanDumpPassesAndIsLeftAsItWas()
    {
        const QString g = makeDump();
        const auto before = fingerprint(g);
        QVERIFY(before.size() >= 9);

        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        QVERIFY(jobs.contains(QStringLiteral("tools.dump.check")));
        auto job = jobs.create(QStringLiteral("tools.dump.check"), {{QStringLiteral("folder"), g}, {QStringLiteral("title"), QStringLiteral("Check Me")}});
        QVERIFY(job.ok());
        QCOMPARE(job.value()->title(), QStringLiteral("Check Check Me"));
        QVERIFY(job.value()->validate(SettingsValues(schema, {}, {})).ok());
        qint64 filesSeen = 0;
        bool countedAsBytes = false;
        ProgressReporter progress([&](const ProgressSnapshot& p) {
            filesSeen = std::max(filesSeen, p.filesDone);
            // The walk lists files: no byte count, so no read speed made from the listing.
            countedAsBytes = countedAsBytes || p.unitIsBytes || p.done > 0 || !p.indeterminate;
        }, 0);
        JobContext ctx = context(progress);
        ctx.settings = SettingsValues(schema, {}, {});
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
        QCOMPARE(o->summary, QStringLiteral("No problems found in 7 files"));
        QVERIFY(o->warnings.isEmpty());
        QVERIFY(o->outputs.isEmpty());
        QCOMPARE(filesSeen, qint64(7));
        QVERIFY(!countedAsBytes);

        const QJsonObject d = o->details;
        const QJsonObject counts = d.value(QStringLiteral("counts")).toObject();
        QCOMPARE(counts.value(QStringLiteral("files")).toInteger(), qint64(7));
        QCOMPARE(counts.value(QStringLiteral("folders")).toInteger(), qint64(4)); // sce_sys, sce_module, data, data/config
        QCOMPARE(counts.value(QStringLiteral("errors")).toInteger(), qint64(0));
        QCOMPARE(counts.value(QStringLiteral("warnings")).toInteger(), qint64(0));
        QVERIFY(!counts.value(QStringLiteral("truncated")).toBool());
        const QJsonObject exec = counts.value(QStringLiteral("executables")).toObject();
        QCOMPARE(exec.value(QStringLiteral("total")).toInteger(), qint64(3));
        QCOMPARE(exec.value(QStringLiteral("fake-signed")).toInteger(), qint64(3));
        const QJsonObject facts = d.value(QStringLiteral("facts")).toObject();
        QCOMPARE(facts.value(QStringLiteral("platform")).toString(), QStringLiteral("PS5"));
        QCOMPARE(facts.value(QStringLiteral("titleId")).toString(), QStringLiteral("PPSA00026"));
        QCOMPARE(facts.value(QStringLiteral("contentVersion")).toString(), QStringLiteral("01.002.000"));
        QCOMPARE(facts.value(QStringLiteral("title")).toString(), QStringLiteral("Check Me"));
        QCOMPARE(facts.value(QStringLiteral("paramSdk")).toString(), QStringLiteral("9.00"));
        QCOMPARE(facts.value(QStringLiteral("ebootSdk")).toString(), QStringLiteral("9.00"));
        QCOMPARE(facts.value(QStringLiteral("ebootClass")).toString(), QStringLiteral("fake-signed"));
        QCOMPARE(facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("original"));
        QCOMPARE(facts.value(QStringLiteral("backportSummary")).toString(), QStringLiteral("Looks original (no fakelib; executables target SDK 9.00)."));
        QCOMPARE(facts.value(QStringLiteral("executableSdks")).toObject().value(QStringLiteral("9.00")).toInteger(), qint64(3));
        // Nothing above a note.
        for (const auto& v : d.value(QStringLiteral("findings")).toArray())
            QCOMPARE(v.toObject().value(QStringLiteral("severity")).toString(), QStringLiteral("info"));

        // Byte for byte, name for name, time stamp for time stamp.
        QCOMPARE(fingerprint(g), before);
    }

    // ---- structure --------------------------------------------------------------------------
    void missingAndEmptyEboot()
    {
        const QString g = makeDump();
        QVERIFY(QFile::remove(g + QStringLiteral("/eboot.bin")));
        DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("eboot-missing"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(f->path, QStringLiteral("eboot.bin"));
        QCOMPARE(r.errors, qint64(1));
        QCOMPARE(r.summary(), QStringLiteral("1 problem in 6 files"));
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("unknown"));

        QVERIFY(write(g + QStringLiteral("/eboot.bin"), QByteArray()));
        QVERIFY(write(g + QStringLiteral("/sce_module/empty.prx"), QByteArray()));
        r = check(g);
        QVERIFY(find(r, QStringLiteral("eboot-empty"), QStringLiteral("eboot.bin")));
        QVERIFY(!find(r, QStringLiteral("eboot-missing")));
        // The game's eboot.bin is reported once; another empty executable has its own finding.
        QVERIFY(!find(r, QStringLiteral("exec-empty"), QStringLiteral("eboot.bin")));
        const DumpFinding* empty = find(r, QStringLiteral("exec-empty"), QStringLiteral("sce_module/empty.prx"));
        QVERIFY(empty);
        QCOMPARE(empty->severity, FindingSeverity::Error);
        QCOMPARE(r.errors, qint64(2));
        QVERIFY(!find(r, QStringLiteral("empty-files"))); // executables are not counted twice
        QCOMPARE(r.executables.value(QStringLiteral("empty")), qint64(2));
    }

    void paramJsonMissingUnreadableIncomplete()
    {
        const QString g = makeDump();
        const QString param = g + QStringLiteral("/sce_sys/param.json");
        QVERIFY(QFile::remove(param));
        DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("param-missing"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(f->path, QStringLiteral("sce_sys/param.json"));
        QCOMPARE(r.facts.value(QStringLiteral("platform")).toString(), QStringLiteral("Unknown"));
        QVERIFY(!find(r, QStringLiteral("backport-state"))); // nothing to compare the executables with

        QVERIFY(write(param, QByteArray("\x01\x02\x03 not json at all", 20)));
        r = check(g);
        f = find(r, QStringLiteral("param-unreadable"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QVERIFY(!find(r, QStringLiteral("param-missing")));

        QVERIFY(write(param, R"({"titleId":"PPSA00026"})"));
        r = check(g);
        f = find(r, QStringLiteral("param-incomplete"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(f->message, QStringLiteral("param.json has no contentId, contentVersion."));
        QCOMPARE(r.facts.value(QStringLiteral("titleId")).toString(), QStringLiteral("PPSA00026"));
    }

    void aPs4DumpGetsTheBasicChecksOnly()
    {
        const QString g = makeDump();
        QVERIFY(QFile::remove(g + QStringLiteral("/sce_sys/param.json")));
        QVERIFY(write(g + QStringLiteral("/sce_sys/param.sfo"), paramSfo()));
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("ps4-dump"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QCOMPARE(f->message, QStringLiteral("PS4 dump: only basic checks apply."));
        QVERIFY(!find(r, QStringLiteral("param-missing")));
        QVERIFY(!find(r, QStringLiteral("backport-state")));
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(0));
        QCOMPARE(r.facts.value(QStringLiteral("platform")).toString(), QStringLiteral("PS4"));
        QCOMPARE(r.facts.value(QStringLiteral("titleId")).toString(), QStringLiteral("CUSA00026"));
        QCOMPARE(r.facts.value(QStringLiteral("title")).toString(), QStringLiteral("Old Game"));
        QCOMPARE(r.executables.value(QStringLiteral("fake-signed")), qint64(3)); // the executables are still looked at
    }

    void aMissingCoverIsAWarning()
    {
        const QString g = makeDump();
        QVERIFY(QFile::remove(g + QStringLiteral("/sce_sys/icon0.png")));
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("icon-missing"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Warning);
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(1));
        QCOMPARE(r.summary(), QStringLiteral("1 warning in 6 files"));
    }

    void aGameOneLevelDownAndAFolderOfGames()
    {
        const QString wrapper = m_root.filePath(QStringLiteral("wrapper"));
        makeDump(QStringLiteral("wrapper/PPSA00026-app"));
        QVERIFY(write(wrapper + QStringLiteral("/readme.txt"), "notes"));
        DumpCheckResult r = check(wrapper);
        const DumpFinding* f = find(r, QStringLiteral("game-nested"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QCOMPARE(f->path, QStringLiteral("PPSA00026-app"));
        // Checked as that game: nothing is "missing", its facts are read.
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(0));
        QCOMPARE(r.facts.value(QStringLiteral("gameFolder")).toString(), QStringLiteral("PPSA00026-app"));
        QCOMPARE(r.facts.value(QStringLiteral("titleId")).toString(), QStringLiteral("PPSA00026"));
        QCOMPARE(r.facts.value(QStringLiteral("ebootSdk")).toString(), QStringLiteral("9.00"));

        makeDump(QStringLiteral("wrapper/Second game"));
        r = check(wrapper);
        f = find(r, QStringLiteral("several-games"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QVERIFY2(f->message.contains(QStringLiteral("2 games")) && f->message.contains(QStringLiteral("PPSA00026-app"))
                     && f->message.contains(QStringLiteral("Second game")),
                 qPrintable(f->message));
        QVERIFY(!find(r, QStringLiteral("eboot-missing")));
        QVERIFY(!find(r, QStringLiteral("param-missing")));
        QCOMPARE(r.executables.value(QStringLiteral("fake-signed")), qint64(6)); // the file checks ran for all of them
    }

    // ---- executables in a dump ------------------------------------------------------------------
    void executableFindings()
    {
        const QString g = makeDump();
        QVERIFY(write(g + QStringLiteral("/sce_module/retail.prx"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kEncrypted)));
        QVERIFY(write(g + QStringLiteral("/sce_module/packed.sprx"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kCompressed)));
        QByteArray cut = fakeSelf();
        cut.chop(0x200);
        QVERIFY(write(g + QStringLiteral("/sce_module/cut.prx"), cut));
        QByteArray noData = fakeSelf();
        put<quint64>(noData, 0x20, 0);
        QVERIFY(write(g + QStringLiteral("/sce_module/odd.self"), noData));
        QVERIFY(write(g + QStringLiteral("/tools/helper.elf"), plainElf()));
        QVERIFY(write(g + QStringLiteral("/tools/notes.prx"), "not an executable"));
        const DumpCheckResult r = check(g);

        const auto expect = [&r](const char* code, const char* path, FindingSeverity severity) {
            const DumpFinding* f = find(r, QString::fromLatin1(code), QString::fromLatin1(path));
            if (!f) {
                qWarning("no finding %s for %s", code, path);
                return false;
            }
            return f->severity == severity;
        };
        QVERIFY(expect("exec-encrypted", "sce_module/retail.prx", FindingSeverity::Error));
        QVERIFY(find(r, QStringLiteral("exec-encrypted"))->message.contains(QStringLiteral("cannot run from a dump")));
        QVERIFY(expect("exec-compressed", "sce_module/packed.sprx", FindingSeverity::Error));
        QVERIFY(expect("exec-truncated", "sce_module/cut.prx", FindingSeverity::Error));
        QVERIFY(expect("exec-malformed", "sce_module/odd.self", FindingSeverity::Error));
        QVERIFY(expect("exec-unsigned", "tools/helper.elf", FindingSeverity::Warning));
        QVERIFY(find(r, QStringLiteral("exec-unsigned"))->message.contains(QStringLiteral("will not load on a console")));
        QVERIFY(expect("exec-unknown", "tools/notes.prx", FindingSeverity::Warning));
        QCOMPARE(r.errors, qint64(4));
        QCOMPARE(r.warnings, qint64(2));
        QCOMPARE(r.summary(), QStringLiteral("4 problems and 2 warnings in 13 files"));
        // The fake-signed ones are counted, not listed one by one.
        const DumpFinding* signedOnes = find(r, QStringLiteral("exec-fake-signed"));
        QVERIFY(signedOnes);
        QCOMPARE(signedOnes->severity, FindingSeverity::Info);
        QCOMPARE(signedOnes->count, qint64(3));
        QCOMPARE(r.executableCount, qint64(9));
        QCOMPARE(r.executables.value(QStringLiteral("fake-signed")), qint64(3));
        QCOMPARE(r.executables.value(QStringLiteral("encrypted")), qint64(1));
        QCOMPARE(r.executables.value(QStringLiteral("compressed")), qint64(1));
        QCOMPARE(r.executables.value(QStringLiteral("truncated")), qint64(1));
        QCOMPARE(r.executables.value(QStringLiteral("malformed")), qint64(1));
        QCOMPARE(r.executables.value(QStringLiteral("plain-elf")), qint64(1));
        QCOMPARE(r.executables.value(QStringLiteral("unknown")), qint64(1));
        // Errors come first in the list, then warnings, then notes.
        QCOMPARE(r.findings.front().severity, FindingSeverity::Error);
        QCOMPARE(r.findings.back().severity, FindingSeverity::Info);
    }

    // A file another program holds open for itself cannot be looked into: said, not skipped.
    void anExecutableThatCannotBeOpened()
    {
        const QString g = makeDump();
        const QString held = g + QStringLiteral("/sce_module/held.prx");
        QVERIFY(write(held, fakeSelf()));
#ifdef Q_OS_WIN
        const HANDLE h = CreateFileW(QDir::toNativeSeparators(held).toStdWString().c_str(), GENERIC_READ, 0 /* no sharing */, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([h] { CloseHandle(h); });
#else
        QVERIFY(QFile::setPermissions(held, QFileDevice::Permissions()));
        const auto release = qScopeGuard([&] { QFile::setPermissions(held, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
        if (QFile(held).open(QIODevice::ReadOnly))
            QSKIP("This account opens the file all the same (root).");
#endif
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("exec-unreadable"), QStringLiteral("sce_module/held.prx"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(r.executables.value(QStringLiteral("unreadable")), qint64(1));
        QCOMPARE(r.errors, qint64(1));
    }

    void aTruncatedOrForeignEbootIsAProblem()
    {
        const QString g = makeDump();
        QByteArray cut = fakeSelf();
        cut.chop(0x300);
        QVERIFY(write(g + QStringLiteral("/eboot.bin"), cut));
        DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("exec-truncated"), QStringLiteral("eboot.bin"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(r.facts.value(QStringLiteral("ebootClass")).toString(), QStringLiteral("truncated"));
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("unknown"));

        QVERIFY(write(g + QStringLiteral("/eboot.bin"), "MZ this is something else"));
        r = check(g);
        f = find(r, QStringLiteral("exec-malformed"), QStringLiteral("eboot.bin"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
    }

    // ---- backport state ---------------------------------------------------------------------------
    void backportStates()
    {
        // Backported: fakelib with libraries, executables re-tagged below what param.json says.
        QString g = makeDump();
        QVERIFY(write(g + QStringLiteral("/eboot.bin"), fakeSelf(kSdk450)));
        QVERIFY(write(g + QStringLiteral("/fakelib/libSceAgc.sprx"), fakeSelf(kSdk450)));
        QVERIFY(write(g + QStringLiteral("/fakelib/libSceAmpr.sprx"), fakeSelf(kSdk450)));
        QVERIFY(write(g + QStringLiteral("/fakelib/libScePlayGo.sprx"), fakeSelf(kSdk450)));
        QVERIFY(write(g + QStringLiteral("/sce_module/libc.prx.bak"), fakeSelf()));
        DumpCheckResult r = check(g);
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(0));
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("backported"));
        QCOMPARE(r.facts.value(QStringLiteral("backportSummary")).toString(), QStringLiteral("Looks backported (fakelib present; executables target SDK 4.50)."));
        QCOMPARE(r.facts.value(QStringLiteral("ebootSdk")).toString(), QStringLiteral("4.50"));
        QCOMPARE(r.facts.value(QStringLiteral("paramSdk")).toString(), QStringLiteral("9.00"));
        const QJsonObject fakelib = r.facts.value(QStringLiteral("fakelib")).toObject();
        QVERIFY(fakelib.value(QStringLiteral("present")).toBool());
        QCOMPARE(fakelib.value(QStringLiteral("libraries")).toInteger(), qint64(3));
        QCOMPARE(fakelib.value(QStringLiteral("engineLibraries")).toInteger(), qint64(0)); // the names alone prove nothing
        const DumpFinding* f = find(r, QStringLiteral("fakelib-present"), QStringLiteral("fakelib"));
        QVERIFY(f);
        QVERIFY2(f->message.contains(QStringLiteral("3 files")) && f->message.contains(QStringLiteral("libSceAmpr.sprx"))
                     && f->message.contains(QStringLiteral("libScePlayGo.sprx")),
                 qPrintable(f->message));
        f = find(r, QStringLiteral("sdk-differs"), QStringLiteral("eboot.bin"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QVERIFY(find(r, QStringLiteral("backport-state")));
        f = find(r, QStringLiteral("backup-files"));
        QVERIFY(f);
        QCOMPARE(f->count, qint64(1));
        QCOMPARE(f->paths, QStringList{QStringLiteral("sce_module/libc.prx.bak")});
        QCOMPARE(r.facts.value(QStringLiteral("executableSdks")).toObject().value(QStringLiteral("4.50")).toInteger(), qint64(4));

        // Mixed: fakelib is there but eboot.bin is not fake-signed.
        QVERIFY(write(g + QStringLiteral("/eboot.bin"), plainElf(kSdk450)));
        r = check(g);
        f = find(r, QStringLiteral("fakelib-eboot-not-signed"), QStringLiteral("eboot.bin"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Warning);
        QVERIFY2(f->message.startsWith(QStringLiteral("fakelib is present but eboot.bin is not fake-signed")), qPrintable(f->message));
        QVERIFY(r.warnings >= 2); // that, and the unsigned eboot.bin itself

        // Only the emulator libraries, executables as param.json says: a packed folder has that too.
        g = makeDump();
        QVERIFY(write(g + QStringLiteral("/fakelib/libSceAmpr.sprx"), fakeSelf()));
        r = check(g);
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("emulator-libraries"));
        QVERIFY(!find(r, QStringLiteral("sdk-differs")));
        QCOMPARE(r.warnings, qint64(0));

        // No fakelib, but the executables were re-tagged.
        g = makeDump();
        QVERIFY(write(g + QStringLiteral("/eboot.bin"), fakeSelf(kSdk450)));
        r = check(g);
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("retagged"));
        QVERIFY(find(r, QStringLiteral("sdk-differs")));

        // An empty fakelib folder is said, and is not a backport.
        g = makeDump();
        QVERIFY(QDir().mkpath(g + QStringLiteral("/fakelib")));
        r = check(g);
        f = find(r, QStringLiteral("fakelib-present"));
        QVERIFY(f);
        QCOMPARE(f->message, QStringLiteral("The fakelib folder is empty."));
        QCOMPARE(r.facts.value(QStringLiteral("backportState")).toString(), QStringLiteral("original"));

        // Unsigned copies in a decrypted folder are one note, not a warning each.
        g = makeDump();
        QVERIFY(write(g + QStringLiteral("/decrypted/eboot.bin"), plainElf()));
        QVERIFY(write(g + QStringLiteral("/decrypted/sce_module/libc.prx"), plainElf()));
        r = check(g);
        QCOMPARE(r.warnings, qint64(0));
        f = find(r, QStringLiteral("decrypted-copies"));
        QVERIFY(f);
        QCOMPARE(f->count, qint64(2));
        QCOMPARE(r.facts.value(QStringLiteral("ebootClass")).toString(), QStringLiteral("fake-signed")); // the game's own, not the copy
    }

    // ---- hygiene ------------------------------------------------------------------------------------
    void junkLargeAndBackupFiles()
    {
        const QString g = makeDump();
        QVERIFY(write(g + QStringLiteral("/Thumbs.db"), "x"));
        QVERIFY(write(g + QStringLiteral("/data/.DS_Store"), "x"));
        QVERIFY(write(g + QStringLiteral("/data/desktop.ini"), "x"));
        QVERIFY(write(g + QStringLiteral("/data/download.part"), "x"));
        QVERIFY(write(g + QStringLiteral("/data/scratch.tmp"), "x"));
        QVERIFY(write(g + QStringLiteral("/data/empty.dat"), QByteArray()));
        DumpCheckResult r = check(g);
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(0));
        const DumpFinding* f = find(r, QStringLiteral("junk-files"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QCOMPARE(f->count, qint64(5));
        QCOMPARE(f->paths.size(), 5);
        QVERIFY(f->paths.contains(QStringLiteral("Thumbs.db")) && f->paths.contains(QStringLiteral("data/scratch.tmp")));
        QCOMPARE(r.byCode.value(QStringLiteral("junk-files")), qint64(5));
        f = find(r, QStringLiteral("empty-files"));
        QVERIFY(f);
        QCOMPARE(f->count, qint64(1));
        QCOMPARE(f->paths, QStringList{QStringLiteral("data/empty.dat")});
        QVERIFY(!find(r, QStringLiteral("large-files")));

        // A file over 4 GiB (sparse: it takes no room).
        const QString big = g + QStringLiteral("/data/big.pak");
        const qint64 onDisk = fwtest::makeSparseFile(big, 0x100000000LL + 4096);
        if (onDisk < 0 || onDisk > 64LL * 1024 * 1024) {
            QFile::remove(big);
            QSKIP("This drive has no sparse files; a file over 4 GiB would be written for real.");
        }
        r = check(g);
        f = find(r, QStringLiteral("large-files"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QCOMPARE(f->count, qint64(1));
        QCOMPARE(f->paths, QStringList{QStringLiteral("data/big.pak")});
        QVERIFY2(f->message.contains(QStringLiteral("FAT32")), qPrintable(f->message));
        QVERIFY(QFile::remove(big));
    }

    void tenThousandEmptyFilesAreOneCappedFinding()
    {
        const QString g = makeDump();
        const QString dir = g + QStringLiteral("/data/empties");
        QVERIFY(QDir().mkpath(dir));
        for (int i = 0; i < 10000; ++i) {
            QFile f(dir + QStringLiteral("/e%1.dat").arg(i, 5, 10, QLatin1Char('0')));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("empty-files"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Info);
        QCOMPARE(f->count, qint64(10000));
        QCOMPARE(f->paths.size(), 20);
        QVERIFY2(f->message.contains(QStringLiteral("10,000")), qPrintable(f->message));
        QCOMPARE(r.byCode.value(QStringLiteral("empty-files")), qint64(10000));
        QCOMPARE(r.files, qint64(10007));
        QCOMPARE(r.errors, qint64(0));
        QCOMPARE(r.warnings, qint64(0));
        const QJsonObject json = r.toJson();
        QVERIFY(json.value(QStringLiteral("findings")).toArray().size() < 10);
        QCOMPARE(json.value(QStringLiteral("counts")).toObject().value(QStringLiteral("byCode")).toObject().value(QStringLiteral("empty-files")).toInteger(), qint64(10000));
        QCOMPARE(r.summary(), QStringLiteral("No problems found in 10,007 files"));
    }

    // A flood of one kind of finding neither grows the result nor hides the others.
    void theListIsCappedAndTheCountsAreExact()
    {
        const QString g = makeDump();
        for (int i = 0; i < 700; ++i)
            QVERIFY(write(g + QStringLiteral("/junk/x%1.prx").arg(i), "nothing"));
        for (int i = 0; i < 150; ++i)
            QVERIFY(write(g + QStringLiteral("/broken/b%1.sprx").arg(i), fakeSelf().left(0x30)));
        QVERIFY(QFile::remove(g + QStringLiteral("/sce_sys/icon0.png")));
        DumpCheckResult r = check(g);
        QCOMPARE(r.byCode.value(QStringLiteral("exec-unknown")), qint64(700));
        QCOMPARE(r.byCode.value(QStringLiteral("exec-truncated")), qint64(150));
        QCOMPARE(r.errors, qint64(150));
        QCOMPARE(r.warnings, qint64(701));
        QVERIFY(r.truncated);
        QVERIFY(int(r.findings.size()) <= 500);
        int unknownListed = 0, truncatedListed = 0;
        for (const DumpFinding& f : r.findings) {
            unknownListed += f.code == QLatin1String("exec-unknown");
            truncatedListed += f.code == QLatin1String("exec-truncated");
        }
        QCOMPARE(unknownListed, 100);
        QCOMPARE(truncatedListed, 100);
        QVERIFY(find(r, QStringLiteral("icon-missing"))); // still there among 700 warnings of another kind
        QVERIFY(find(r, QStringLiteral("backport-state")));
        QCOMPARE(r.summary(), QStringLiteral("150 problems and 701 warnings in 856 files"));
        QVERIFY(r.toJson().value(QStringLiteral("counts")).toObject().value(QStringLiteral("truncated")).toBool());

        // The caps are options.
        DumpCheckOptions small;
        small.maxFindings = 5;
        r = check(g, small);
        QCOMPARE(int(r.findings.size()), 5);
        QCOMPARE(r.errors, qint64(150));
        for (const DumpFinding& f : r.findings)
            QCOMPARE(f.severity, FindingSeverity::Error); // errors first
    }

    void namesThatDifferOnlyInCase()
    {
        const QString g = makeDump();
        const QString dir = g + QStringLiteral("/data/textures");
        QVERIFY(QDir().mkpath(dir));
#ifdef Q_OS_WIN
        // NTFS keeps such twins only in a folder marked case-sensitive.
        QProcess p;
        p.start(QStringLiteral("fsutil.exe"), {QStringLiteral("file"), QStringLiteral("setCaseSensitiveInfo"), QDir::toNativeSeparators(dir), QStringLiteral("enable")});
        if (!p.waitForFinished(20000) || p.exitCode() != 0)
            QSKIP("This machine cannot mark a folder case-sensitive (fsutil file setCaseSensitiveInfo).");
#endif
        QVERIFY(write(dir + QStringLiteral("/Wall.dds"), "a"));
        QVERIFY(write(dir + QStringLiteral("/wall.dds"), "b"));
        if (QDir(dir).entryList(QDir::Files).size() != 2)
            QSKIP("This file system does not keep two names that differ only in case.");
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("case-collision"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QVERIFY2(f->path.compare(QStringLiteral("data/textures/wall.dds"), Qt::CaseInsensitive) == 0, qPrintable(f->path));
        QVERIFY2(f->message.contains(QStringLiteral("“Wall.dds”")) && f->message.contains(QStringLiteral("“wall.dds”")), qPrintable(f->message));
        QCOMPARE(r.errors, qint64(1));
    }

    void namesExfatCannotStore()
    {
        const QString g = makeDump();
        const QString odd = QStringLiteral("notes.");
        if (!writeOddName(g + QStringLiteral("/data"), odd, "x"))
            QSKIP("This file system does not take a name that ends with a dot.");
        const auto cleanup = qScopeGuard([&] { removeOddName(g + QStringLiteral("/data"), odd); }); // QTemporaryDir could not remove it
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("bad-name"));
        if (!f && !QDir(g + QStringLiteral("/data")).entryList(QDir::Files).contains(odd))
            QSKIP("The odd name is not listed back on this system.");
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(f->path, QStringLiteral("data/notes."));
        QVERIFY2(f->message.contains(QStringLiteral("dot or a space")), qPrintable(f->message));
    }

    void aLinkIsReportedAndNotFollowed()
    {
        const QString g = makeDump();
        const QString outside = m_root.filePath(QStringLiteral("outside-target"));
        // What a followed link would drag in: a broken executable and many files.
        QVERIFY(write(outside + QStringLiteral("/eboot.bin"), fakeSelf().left(0x30)));
        QVERIFY(write(outside + QStringLiteral("/more/retail.prx"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kEncrypted)));
        const QString link = g + QStringLiteral("/data/linked");
        if (!createDirectoryJunction(link, outside).ok())
            QSKIP("This machine cannot create a junction or directory link.");
        const auto unlink = qScopeGuard([&] { (void)removeDirectoryJunction(link); });
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("link"), QStringLiteral("data/linked"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Warning);
        QCOMPARE(r.warnings, qint64(1));
        QCOMPARE(r.errors, qint64(0));        // nothing behind the link was read
        QCOMPARE(r.files, qint64(7));
        QCOMPARE(r.executableCount, qint64(3));
        // The link and its target are as they were.
        QVERIFY(QFileInfo::exists(outside + QStringLiteral("/eboot.bin")));
    }

    void aFolderThatCannotBeOpenedIsAProblem()
    {
        const QString g = makeDump();
        const QString locked = g + QStringLiteral("/data/config");
        if (!fwtest::denyListing(locked, true))
            QSKIP("This machine cannot take the right to list a folder away.");
        const auto restore = qScopeGuard([&] { (void)fwtest::denyListing(locked, false); });
        if (!QDir(locked).entryList(QDir::Files).isEmpty())
            QSKIP("This account lists the folder all the same (administrator, root).");
        const DumpCheckResult r = check(g);
        const DumpFinding* f = find(r, QStringLiteral("folder-unreadable"), QStringLiteral("data/config"));
        QVERIFY(f);
        QCOMPARE(f->severity, FindingSeverity::Error);
        QCOMPARE(r.errors, qint64(1));
    }

    // ---- the job ----------------------------------------------------------------------------------------
    void aDumpWithProblemsStillSucceedsAsAJob()
    {
        const QString g = makeDump();
        QVERIFY(write(g + QStringLiteral("/sce_module/retail.prx"), fakeSelf(kSdk900, self::props::kHasBlocks | self::props::kEncrypted)));
        QVERIFY(QFile::remove(g + QStringLiteral("/sce_sys/icon0.png")));
        const auto before = fingerprint(g);
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        auto job = jobs.create(QStringLiteral("tools.dump.check"), {{QStringLiteral("folder"), g}});
        QVERIFY(job.ok());
        QCOMPARE(job.value()->title(), QStringLiteral("Check %1").arg(QFileInfo(g).fileName()));
        ProgressReporter progress;
        JobContext ctx = context(progress);
        ctx.settings = SettingsValues(schema, {}, {});
        auto o = job.value()->run(ctx);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString())); // the check did its job
        QCOMPARE(o->verification, Verification::Inconclusive);
        QCOMPARE(o->summary, QStringLiteral("1 problem and 1 warning in 7 files"));
        QCOMPARE(o->warnings.size(), 2);
        QVERIFY2(o->warnings.first().startsWith(QStringLiteral("sce_module/retail.prx: Encrypted retail executable")), qPrintable(o->warnings.first()));
        const QJsonArray findings = o->details.value(QStringLiteral("findings")).toArray();
        QCOMPARE(findings.first().toObject().value(QStringLiteral("code")).toString(), QStringLiteral("exec-encrypted"));
        QCOMPARE(findings.first().toObject().value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
        QCOMPARE(findings.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("sce_module/retail.prx"));
        QCOMPARE(fingerprint(g), before);

        // A folder that is not there is refused before the job is queued, and fails when run all the same.
        auto gone = jobs.create(QStringLiteral("tools.dump.check"), {{QStringLiteral("folder"), m_root.filePath(QStringLiteral("no such dump"))}});
        QVERIFY(gone.ok());
        QVERIFY(!gone.value()->validate(SettingsValues(schema, {}, {})).ok());
        ProgressReporter p2;
        JobContext c2 = context(p2);
        QVERIFY(!gone.value()->run(c2).ok());
    }

    void cancelMidWalkEndsCancelled()
    {
        const QString g = makeDump();
        for (int i = 0; i < 3000; ++i)
            QVERIFY(write(g + QStringLiteral("/data/many/f%1.bin").arg(i), "x"));
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext mc{*schema, deps, jobs};
        registerModule(mc);
        auto job = jobs.create(QStringLiteral("tools.dump.check"), {{QStringLiteral("folder"), g}});
        QVERIFY(job.ok());
        CancellationSource cancel;
        qint64 seenAtCancel = -1, seenAtEnd = 0;
        ProgressReporter progress([&](const ProgressSnapshot& p) {
            seenAtEnd = p.filesDone;
            if (p.filesDone >= 100 && seenAtCancel < 0) {
                seenAtCancel = p.filesDone;
                cancel.cancel();
            }
        }, 0);
        JobContext ctx = context(progress, cancel.token());
        ctx.settings = SettingsValues(schema, {}, {});
        auto o = job.value()->run(ctx);
        QVERIFY(!o.ok());
        QVERIFY(o.error().isCancelled());
        QVERIFY(seenAtCancel >= 100);
        QVERIFY2(seenAtEnd < 3007, qPrintable(QString::number(seenAtEnd))); // it stopped, it did not finish the walk first

        // Cancelled before it starts.
        CancellationSource early;
        early.cancel();
        const auto r = checkDump(g, early.token());
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
    }
};

QTEST_MAIN(TstDumpCheck)
#include "tst_dumpcheck.moc"
