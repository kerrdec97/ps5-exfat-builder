// ZIP writer/reader: round trips (store/deflate, UTF-8, empty, directories,
// multi-MiB), ZIP64 records, CRC corruption, truncation, zip-slip refusal,
// .part handling, cancellation and interoperability with Python's zipfile and
// Info-ZIP unzip when available.

#include "core/Zip.h"
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

    void interoperatesWithPythonAndUnzip()
    {
        const QString z64 = makeSample(QStringLiteral("interop64.zip"), true);
        const QString z = makeSample(QStringLiteral("interop.zip"));
        bool checked = false;
        const QString py = QStandardPaths::findExecutable(QStringLiteral("python3"));
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
