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

        // Inside the game no name may change on the way into the image: a
        // zipped game with a name this computer cannot store is refused, not
        // built with the file renamed.
        const QString odd = m_root.filePath(QStringLiteral("odd-name.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(odd).ok());
            QVERIFY(w.addData(QStringLiteral("Game-app0/eboot.bin"), "e").ok());
            QVERIFY(w.addData(QStringLiteral("Game-app0/data/stage 1: intro.bin"), "x").ok());
            QVERIFY(w.finish().ok());
        }
        const Status st = archive::checkZipSource(odd);
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("\"stage 1: intro.bin\"")), qPrintable(st.error().message));
    }

    // Names stored without the UTF-8 flag: UTF-8 all the same (Info-ZIP on
    // Linux and macOS), or the format's code page 437; never Latin-1.
    void namesWithoutTheUtf8Flag()
    {
        const auto clearUtf8Flag = [](QByteArray& bytes) {
            // General-purpose flags: offset 6 of a local header, 8 of a central one.
            for (qsizetype at = bytes.indexOf("PK\x03\x04"); at >= 0; at = bytes.indexOf("PK\x03\x04", at + 4))
                bytes[at + 7] = char(bytes[at + 7] & ~0x08);
            for (qsizetype at = bytes.indexOf("PK\x01\x02"); at >= 0; at = bytes.indexOf("PK\x01\x02", at + 4))
                bytes[at + 9] = char(bytes[at + 9] & ~0x08);
        };
        const QString z = m_root.filePath(QStringLiteral("names.zip"));
        const QByteArray big(3 * 1024 * 1024 + 5, 'q'); // several chunks through addData
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QString::fromUtf8("Pok\xc3\xa9mon/d\xc3\xa9mo.bin"), "utf8").ok());
            QVERIFY(w.addData(QStringLiteral("XXXXXXX/big.bin"), big).ok());
            QVERIFY(w.finish().ok());
        }
        QByteArray bytes = readFile(z);
        clearUtf8Flag(bytes);
        const QByteArray cp437("M\x81nchen/\x82t\x82.bin"); // "München/été.bin" in code page 437
        QCOMPARE(cp437.size(), qsizetype(15));
        QCOMPARE(bytes.count("XXXXXXX/big.bin"), 2);
        bytes.replace("XXXXXXX/big.bin", cp437);
        QVERIFY(fwtest::writeTextFile(z, bytes));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        QCOMPARE(int(r.entries().size()), 2);
        QCOMPARE(r.entries()[0].name, QString::fromUtf8("Pok\xc3\xa9mon/d\xc3\xa9mo.bin"));
        QCOMPARE(r.entries()[1].name, QString::fromUtf8("M\xc3\xbcnchen/\xc3\xa9t\xc3\xa9.bin"));
        QVERIFY(r.verifyAll(CancellationToken()).ok());
        auto all = r.readAll(r.entries()[1]);
        QVERIFY(all.ok());
        QCOMPARE(all.value(), big);
        QCOMPARE(zip::crc32(big), r.entries()[1].crc32);
    }

    // A password-protected entry is named as such, not reported as damaged;
    // a zipped game with one is refused before the job starts.
    void passwordProtectedEntriesAreNamed()
    {
        const QString z = m_root.filePath(QStringLiteral("locked.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            QVERIFY(w.addData(QStringLiteral("game/eboot.bin"), QByteArray(1000, 'e')).ok());
            QVERIFY(w.finish().ok());
        }
        QByteArray bytes = readFile(z);
        bytes[6] = char(bytes[6] | 0x01);
        const qsizetype cd = bytes.indexOf("PK\x01\x02");
        QVERIFY(cd > 0);
        bytes[cd + 8] = char(bytes[cd + 8] | 0x01);
        QVERIFY(fwtest::writeTextFile(z, bytes));
        zip::Reader r;
        QVERIFY(r.open(z).ok());
        QVERIFY(r.entries().front().encrypted);
        const Status st = r.verifyAll(CancellationToken());
        QCOMPARE(st.error().code, ErrorCode::Unsupported);
        QVERIFY2(st.error().message.contains(QStringLiteral("password-protected")), qPrintable(st.error().message));
        const Status source = archive::checkZipSource(z);
        QCOMPARE(source.error().code, ErrorCode::Unsupported);
        QVERIFY2(source.error().message.contains(QStringLiteral("password-protected")), qPrintable(source.error().message));
    }

    // Exactly 65,535 entries fit the classic end record: such an archive has
    // no ZIP64 records (Python's zipfile writes it that way) and is valid.
    void archiveWith65535EntriesAndNoZip64Records()
    {
        const QString z = m_root.filePath(QStringLiteral("many.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(z).ok());
            for (int i = 0; i < 65535; ++i)
                QVERIFY(w.addData(QStringLiteral("f%1").arg(i), QByteArray(), zip::Method::Store).ok());
            QVERIFY(w.finish().ok());
        }
        // The writer added ZIP64 records (it does from 65,535 entries on):
        // take them out and write the real values into the classic record.
        QByteArray bytes = readFile(z);
        const qsizetype eocd = bytes.size() - 22;
        const qsizetype record = eocd - 20 - 56;
        QVERIFY(bytes.mid(eocd, 4) == QByteArray("PK\x05\x06") && bytes.mid(eocd - 20, 4) == QByteArray("PK\x06\x07")
                && bytes.mid(record, 4) == QByteArray("PK\x06\x06"));
        QByteArray classic = bytes.mid(eocd);
        classic.replace(12, 4, bytes.mid(record + 40, 4)); // size of the central directory
        classic.replace(16, 4, bytes.mid(record + 48, 4)); // its offset
        bytes = bytes.left(record) + classic;
        QVERIFY(fwtest::writeTextFile(z, bytes));
        zip::Reader r;
        const Status st = r.open(z);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QCOMPARE(int(r.entries().size()), 65535);
        QCOMPARE(r.entries().back().name, QStringLiteral("f65534"));
        // Without a locator, the 32-bit "see ZIP64" marks are still an error.
        bytes.replace(bytes.size() - 22 + 16, 4, QByteArray("\xFF\xFF\xFF\xFF"));
        QVERIFY(fwtest::writeTextFile(z, bytes));
        QVERIFY(!r.open(z).ok());
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
