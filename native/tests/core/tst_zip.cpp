// ZIP writer/reader: round trips (store/deflate, UTF-8, empty, directories,
// multi-MiB), ZIP64 records, CRC corruption, truncation, zip-slip refusal,
// .part handling, cancellation and interoperability with Python's zipfile and
// Info-ZIP unzip when available.

#include "core/Zip.h"
#include "core/ArchiveSource.h"
#include "core/ProcessRunner.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {
QByteArray readFile(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
} // namespace

namespace {

// A Python that really runs. On Windows the Microsoft Store "App execution
// alias" stubs in ...\WindowsApps only print "Python was not found", so
// those folders are not searched, and the interpreter is probed first.
QString usablePython()
{
    QStringList dirs;
    for (const QString& d : qEnvironmentVariable("PATH").split(QDir::listSeparator(), Qt::SkipEmptyParts))
        if (!QDir::fromNativeSeparators(d).contains(QStringLiteral("/WindowsApps"), Qt::CaseInsensitive))
            dirs << d;
    for (const QString& name : {QStringLiteral("python3"), QStringLiteral("python")}) {
        const QString py = QStandardPaths::findExecutable(name, dirs);
        if (py.isEmpty())
            continue;
        ProcessSpec spec;
        spec.program = py;
        spec.arguments = {QStringLiteral("-I"), QStringLiteral("-c"), QStringLiteral("import zipfile")};
        auto r = runProcess(spec, CancellationToken());
        if (r.ok() && r->exitCode == 0)
            return py;
    }
    return {};
}

} // namespace

class TstZip : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

    QString makeSample(const QString& name, bool forceZip64 = false)
    {
        const QString src = m_root.filePath(QStringLiteral("src"));
        fwtest::writeRandomFile(src + QStringLiteral("/big.bin"), 5 * 1024 * 1024 + 17, 11);
        fwtest::writeTextFile(src + QStringLiteral("/text.txt"), QByteArray(200000, 'a'));
        fwtest::writeTextFile(src + QStringLiteral("/empty.bin"), QByteArray());
        const QString zipPath = m_root.filePath(name);
        zip::Writer w;
        if (forceZip64)
            w.setZip64ThresholdForTesting(4096);
        if (!w.open(zipPath).ok())
            return {};
        bool ok = w.addFile(QStringLiteral("game/big.bin"), src + QStringLiteral("/big.bin"), zip::Method::Store, CancellationToken()).ok()
            && w.addFile(QStringLiteral("game/text.txt"), src + QStringLiteral("/text.txt"), zip::Method::Deflate, CancellationToken()).ok()
            && w.addFile(QStringLiteral("game/empty.bin"), src + QStringLiteral("/empty.bin"), zip::Method::Deflate, CancellationToken()).ok()
            && w.addDirectory(QStringLiteral("game/empty folder")).ok()
            && w.addData(QStringLiteral("game/sce_sys/ゲーム名 — Ünïcode.json"), "{\"a\":1}").ok()
            && w.finish().ok();
        return ok ? zipPath : QString();
    }

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    // An entry that inflates to more than its declared size is damaged; the
    // reader stops at the declared size (readAll's limit cannot be bypassed).
    void entryLargerThanDeclaredIsRefused()
    {
        const QString z = m_root.filePath(QStringLiteral("liar.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("zeros.bin"), QByteArray(4 * 1024 * 1024, '\0')).ok());
            QVERIFY(w.finish().ok());
        }
        QByteArray bytes = readFile(z);
        // Declare 10 bytes in the local header (offset 22) and the central directory (offset 24).
        const auto put32 = [&](qsizetype at, quint32 v) {
            for (int i = 0; i < 4; ++i)
                bytes[at + i] = char((v >> (8 * i)) & 0xFF);
        };
        QVERIFY(bytes.startsWith("PK\x03\x04"));
        put32(22, 10);
        const qsizetype cd = bytes.indexOf(QByteArray("PK\x01\x02"));
        QVERIFY(cd > 0);
        put32(cd + 24, 10);
        QFile f(z);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        QCOMPARE(qint64(r.entries().front().uncompressedSize), qint64(10));
        auto all = r.readAll(r.entries().front(), 1024);
        QVERIFY(!all.ok());
        QCOMPARE(all.error().code, ErrorCode::CorruptData);
    }

    void roundTrip_data()
    {
        QTest::addColumn<bool>("zip64");
        QTest::newRow("classic") << false;
        QTest::newRow("zip64 records") << true;
    }

    void roundTrip()
    {
        QFETCH(bool, zip64);
        const QString z = makeSample(zip64 ? QStringLiteral("a64.zip") : QStringLiteral("a.zip"), zip64);
        QVERIFY(!z.isEmpty());
        QVERIFY(!QFile::exists(z + QStringLiteral(".part")));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        QCOMPARE(int(r.entries().size()), 5);
        QVERIFY(r.verifyAll(CancellationToken()).ok());
        const QString out = m_root.filePath(zip64 ? QStringLiteral("x64") : QStringLiteral("x"));
        QVERIFY(r.extractAll(out, CancellationToken()).ok());
        QCOMPARE(readFile(out + QStringLiteral("/game/big.bin")), readFile(m_root.filePath(QStringLiteral("src/big.bin"))));
        QCOMPARE(readFile(out + QStringLiteral("/game/text.txt")), QByteArray(200000, 'a'));
        QVERIFY(QFileInfo(out + QStringLiteral("/game/empty.bin")).size() == 0);
        QVERIFY(QFileInfo(out + QStringLiteral("/game/empty folder")).isDir());
        QCOMPARE(readFile(out + QStringLiteral("/game/sce_sys/ゲーム名 — Ünïcode.json")), QByteArray("{\"a\":1}"));
        const auto* text = r.find(QStringLiteral("game/text.txt"));
        QVERIFY(text && text->method == zip::Method::Deflate && text->compressedSize < 2000);
    }

    void verifyBeforeCommit()
    {
        const QString z = m_root.filePath(QStringLiteral("verified.zip"));
        zip::Writer w;
        QVERIFY(w.open(z).ok());
        QVERIFY(w.addData(QStringLiteral("a.txt"), QByteArray(100000, 'v')).ok());
        QVERIFY(w.finish(true).ok());
        QVERIFY(QFile::exists(z) && !QFile::exists(z + QStringLiteral(".part")));
    }

    void neverOverwritesWithoutConsent()
    {
        const QString z = makeSample(QStringLiteral("b.zip"));
        QVERIFY(!z.isEmpty());
        zip::Writer w;
        auto st = w.open(z);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::AlreadyExists);
    }

    void abandonedWriterLeavesNothing()
    {
        const QString z = m_root.filePath(QStringLiteral("abandoned.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("a.txt"), "hello").ok());
        }
        QVERIFY(!QFile::exists(z));
        QVERIFY(!QFile::exists(z + QStringLiteral(".part")));
    }

    void cancellationStopsAndLeavesNothing()
    {
        fwtest::writeRandomFile(m_root.filePath(QStringLiteral("c/big.bin")), 4 * 1024 * 1024, 3);
        const QString z = m_root.filePath(QStringLiteral("cancel.zip"));
        CancellationSource cs;
        cs.cancel();
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            auto st = w.addFile(QStringLiteral("big.bin"), m_root.filePath(QStringLiteral("c/big.bin")), zip::Method::Deflate, cs.token());
            QVERIFY(!st.ok() && st.error().isCancelled());
        }
        QVERIFY(!QFile::exists(z) && !QFile::exists(z + QStringLiteral(".part")));
    }

    void corruptionIsDetected()
    {
        const QString z = makeSample(QStringLiteral("corrupt.zip"));
        QByteArray bytes = readFile(z);
        // Flip a byte inside big.bin's stored data (just after its local header).
        const qsizetype at = bytes.indexOf("game/big.bin") + 12 + 1000;
        bytes[at] = char(bytes[at] ^ 0x5A);
        QVERIFY(fwtest::writeTextFile(z, bytes));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        auto st = r.verifyAll(CancellationToken());
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::CorruptData);
        // Truncated archive: no end record.
        QVERIFY(fwtest::writeTextFile(z, bytes.left(bytes.size() / 2)));
        QVERIFY(!r.open(z).ok());
    }

    void zipSlipIsRefusedBeforeWritingAnything()
    {
        for (const QString& evil : {QStringLiteral("../evil.txt"), QStringLiteral("a/../../evil.txt"), QStringLiteral("/abs.txt"),
                                    QStringLiteral("C:/win.txt"), QStringLiteral("..\\back.txt")}) {
            QVERIFY2(zip::Reader::safeRelativePath(evil).isEmpty(), qPrintable(evil));
        }
        QCOMPARE(zip::Reader::safeRelativePath(QStringLiteral("./a//b/./c.txt")), QStringLiteral("a/b/c.txt"));
        // Build an archive whose second entry escapes (written raw: the writer
        // itself strips leading slashes but keeps "..").
        const QString z = m_root.filePath(QStringLiteral("slip.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("ok.txt"), "fine").ok());
            QVERIFY(w.addData(QStringLiteral("../escaped.txt"), "bad").ok());
            QVERIFY(w.finish().ok());
        }
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        const QString out = m_root.filePath(QStringLiteral("slip-out"));
        auto st = r.extractAll(out, CancellationToken());
        QVERIFY(!st.ok());
        QVERIFY(!QFile::exists(out + QStringLiteral("/ok.txt")));
        QVERIFY(!QFile::exists(m_root.filePath(QStringLiteral("escaped.txt"))));
    }

    // A zipped game as a build source: only the game is unpacked, which is
    // exactly what the free-space check and the progress total counted.
    void zipSourceUnpacksOnlyTheGame()
    {
        const QString z = m_root.filePath(QStringLiteral("source.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("Extras/video.bin"), QByteArray(300000, 'v'), zip::Method::Store).ok());
            QVERIFY(w.addData(QStringLiteral("PPSA01234-app0/eboot.bin"), QByteArray(100000, 'e')).ok());
            QVERIFY(w.addData(QStringLiteral("PPSA01234-app0/sce_sys/param.json"), "{}").ok());
            QVERIFY(w.addData(QStringLiteral("ppsa01234-APP0/data/a.bin"), QByteArray(2000, 'a'), zip::Method::Store).ok());
            QVERIFY(w.addDirectory(QStringLiteral("Extras/empty")).ok());
            QVERIFY(w.addData(QStringLiteral("readme.txt"), "hello").ok());
            QVERIFY(w.finish().ok());
        }
        auto g = archive::inspectZip(z);
        QVERIFY(g.ok());
        QCOMPARE(g->rootPrefix, QStringLiteral("PPSA01234-app0/"));
        QCOMPARE(g->files, qint64(3));
        QCOMPARE(g->bytes, qint64(100000 + 2 + 2000));

        ProgressReporter progress;
        const QString temp = m_root.filePath(QStringLiteral("unpack"));
        auto root = archive::unpackZipSource(z, temp, CancellationToken(), &progress, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(readFile(root.value() + QStringLiteral("/eboot.bin")), QByteArray(100000, 'e'));
        QCOMPARE(readFile(root.value() + QStringLiteral("/data/a.bin")), QByteArray(2000, 'a'));
        QCOMPARE(QDir(temp + QStringLiteral("/zip-source")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("PPSA01234-app0")});
        // Every byte of the game counted once, compressed or not.
        const ProgressSnapshot s = progress.snapshot();
        QCOMPARE(s.total, g->bytes);
        QCOMPARE(s.done, g->bytes);

        // A name that would leave the folder is only a reason to refuse when
        // it belongs to what is unpacked.
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        const QString only = m_root.filePath(QStringLiteral("only-extras"));
        QVERIFY(r.extractAll(only, CancellationToken(), nullptr, false, QStringLiteral("extras/")).ok());
        QCOMPARE(QDir(only).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("Extras")});
        QVERIFY(QFileInfo(only + QStringLiteral("/Extras/empty")).isDir());
    }

    // Archives written with '\' as the separator (Compress-Archive of Windows
    // PowerShell 5, old .NET) hold the same folders as any other.
    void backslashSeparatorsAreFolders()
    {
        const QString z = m_root.filePath(QStringLiteral("backslash.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("game/eboot.bin"), QByteArray(1000, 'e')).ok());
            QVERIFY(w.addData(QStringLiteral("game/sce_sys/param.json"), "{}").ok());
            QVERIFY(w.addDirectory(QStringLiteral("game/empty")).ok());
            QVERIFY(w.finish().ok());
        }
        QByteArray bytes = readFile(z);
        for (const char* name : {"game/eboot.bin", "game/sce_sys/param.json", "game/empty/"}) {
            QByteArray with(name);
            with.replace('/', '\\');
            QCOMPARE(bytes.count(name), 2); // local header and central directory
            bytes.replace(name, with);
        }
        QVERIFY(fwtest::writeTextFile(z, bytes));

        zip::Reader r;
        QVERIFY(r.open(z).ok());
        QVERIFY(r.find(QStringLiteral("game/eboot.bin")));
        const auto* folder = r.find(QStringLiteral("game/empty/"));
        QVERIFY(folder && folder->isDirectory());
        auto g = archive::inspectZip(z);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().toLogString()));
        QCOMPARE(g->rootPrefix, QStringLiteral("game/"));
        QVERIFY(g->hasEboot && g->hasParam);
        const QString out = m_root.filePath(QStringLiteral("backslash-out"));
        QVERIFY(r.extractAll(out, CancellationToken()).ok());
        QCOMPARE(readFile(out + QStringLiteral("/game/eboot.bin")), QByteArray(1000, 'e'));
        QVERIFY(QFileInfo(out + QStringLiteral("/game/empty")).isDir());
    }

    // A ':' in a folder name (zipped on Linux or macOS) and a "./" entry are
    // no attempt to leave the destination: such archives are extracted.
    void harmlessNamesAreNotRefused()
    {
        QVERIFY(zip::Reader::extractedPath(QStringLiteral("./")).isEmpty());
        QVERIFY(zip::Reader::extractedPath(QStringLiteral("C:/win.txt")).isEmpty());
        QVERIFY(zip::Reader::extractedPath(QStringLiteral("a/../../evil.txt")).isEmpty());
        QVERIFY(zip::Reader::extractedPath(QStringLiteral("/abs.txt")).isEmpty());
        QCOMPARE(zip::Reader::extractedPath(QStringLiteral("a/C:/x.txt")), QStringLiteral("a/C_/x.txt"));
        QCOMPARE(zip::Reader::extractedPath(QStringLiteral("./a//b/./c.txt")), QStringLiteral("a/b/c.txt"));
        // The strict check other callers rely on is unchanged.
        QVERIFY(zip::Reader::safeRelativePath(QStringLiteral("Game: Two/eboot.bin")).isEmpty());

        const QString z = m_root.filePath(QStringLiteral("colon.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addDirectory(QStringLiteral("./")).ok());
            QVERIFY(w.addData(QStringLiteral("Game: Two-app0/eboot.bin"), QByteArray(500, 'e')).ok());
            QVERIFY(w.addData(QStringLiteral("Game: Two-app0/sce_sys/param.json"), "{}").ok());
            QVERIFY(w.finish().ok());
        }
        auto root = archive::unpackZipSource(z, m_root.filePath(QStringLiteral("colon-unpack")), CancellationToken(), nullptr, 0);
        QVERIFY2(root.ok(), root.ok() ? "" : qPrintable(root.error().toLogString()));
        QCOMPARE(QFileInfo(root.value()).fileName(), QStringLiteral("Game_ Two-app0"));
        QCOMPARE(readFile(root.value() + QStringLiteral("/eboot.bin")), QByteArray(500, 'e'));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        const QString all = m_root.filePath(QStringLiteral("colon-all"));
        QVERIFY(r.extractAll(all, CancellationToken()).ok()); // the "./" entry is the destination itself
        QVERIFY(QFileInfo(all + QStringLiteral("/Game_ Two-app0/sce_sys/param.json")).isFile());
    }

    // readAll's limit also holds for a declared size of 2^63 or more.
    void readAllLimitIsUnsigned()
    {
        const QString z = makeSample(QStringLiteral("limit.zip"));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        zip::Entry huge = *r.find(QStringLiteral("game/text.txt"));
        huge.uncompressedSize = ~quint64(0);
        auto all = r.readAll(huge, 1024);
        QVERIFY(!all.ok());
        QCOMPARE(all.error().code, ErrorCode::Unsupported);
        QVERIFY(r.readAll(*r.find(QStringLiteral("game/text.txt")), 200000).ok());
        QCOMPARE(r.readAll(*r.find(QStringLiteral("game/text.txt")), 199999).error().code, ErrorCode::Unsupported);
    }

    // A cancel during the read-back check is a cancel, not a damaged archive.
    void cancelDuringReadBackIsACancel()
    {
        const QString z = m_root.filePath(QStringLiteral("cancel-verify.zip"));
        CancellationSource cs;
        zip::Writer w;
        QVERIFY(w.open(z).ok());
        QVERIFY(w.addData(QStringLiteral("a.bin"), QByteArray(100000, 'v')).ok());
        cs.cancel();
        const Status st = w.finish(true, cs.token());
        QVERIFY(!st.ok());
        QVERIFY2(st.error().isCancelled(), qPrintable(st.error().toLogString()));
        QVERIFY(!QFile::exists(z) && !QFile::exists(z + QStringLiteral(".part")));
    }

    void interoperatesWithPythonAndUnzip()
    {
        const QString z64 = makeSample(QStringLiteral("interop64.zip"), true);
        const QString z = makeSample(QStringLiteral("interop.zip"));
        bool checked = false;
        const QString py = usablePython();
        if (!py.isEmpty()) {
            for (const QString& file : {z, z64}) {
                ProcessSpec spec;
                spec.program = py;
                spec.arguments = {QStringLiteral("-I"), QStringLiteral("-c"),
                                  QStringLiteral("import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); bad=z.testzip(); "
                                                 "print('names', len(z.namelist())); sys.exit(1 if bad else 0)"),
                                  file};
                auto r = runProcess(spec, CancellationToken());
                QVERIFY(r.ok());
                QVERIFY2(r->exitCode == 0, qPrintable(r->output));
            }
            checked = true;
        }
        const QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
        if (!unzip.isEmpty()) {
            ProcessSpec spec;
            spec.program = unzip;
            spec.arguments = {QStringLiteral("-t"), z};
            auto r = runProcess(spec, CancellationToken());
            QVERIFY(r.ok());
            QVERIFY2(r->exitCode == 0, qPrintable(r->output));
            checked = true;
        }
        if (!checked)
            QSKIP("Neither python3 nor unzip is available");
    }
};

QTEST_GUILESS_MAIN(TstZip)
#include "tst_zip.moc"
