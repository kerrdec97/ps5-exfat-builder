// Backport and originals ZIPs on the shared core ZIP code: .part, read-back
// (CRC, size, count, SHA-256 per member), rename, never overwrite (S17, S23),
// destination rules (S10, S28, S30), case-insensitive de-duplication.

#include "BackportTestEnv.h"

#include "core/FileUtil.h"
#include "core/Zip.h"
#include "engines/backport/BackupArchive.h"
#include "engines/backport/Common.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

ZipMember member(const QString& arc, const QString& path)
{
    return {arc, path, QString::fromLatin1(sha(readBytes(path)))};
}

} // namespace

class TstBackportZip : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void namesAndUniquePaths()
    {
        QCOMPARE(originalsZipName(QStringLiteral("PPSA01234 Game")), QStringLiteral("PPSA01234 Game original files.zip"));
        QCOMPARE(backportZipName(QStringLiteral("PPSA01234 Game"), 4), QStringLiteral("FW 4xx PPSA01234 Game backport files.zip"));
        QCOMPARE(backportZipName(QStringLiteral("X"), 10), QStringLiteral("FW 10xx X backport files.zip"));
        QTemporaryDir d;
        const QString first = d.filePath("G original files.zip");
        QCOMPARE(uniqueZipPath(first), first);
        writeBytes(first, "earlier originals");
        QCOMPARE(uniqueZipPath(first), d.filePath("G original files (2).zip"));
        writeBytes(d.filePath("G original files (2).zip.part"), "a run in progress");
        QCOMPARE(uniqueZipPath(first), d.filePath("G original files (3).zip"));
    }

    void writesVerifiesAndNeverOverwrites() // port of AtomicZip.test_writes_and_never_overwrites
    {
        QTemporaryDir d;
        writeBytes(d.filePath("src/eboot.bin"), selfOf(f1()));
        writeBytes(d.filePath("src/sce_module/libc.prx"), selfOf(f2()));
        const QString target = d.filePath("G original files.zip");
        writeBytes(target, "earlier originals");
        const QString dest = uniqueZipPath(target);
        {
            StagedZip z(dest);
            QVERIFY(z.write({member("eboot.bin", d.filePath("src/eboot.bin")),
                             member("sce_module/libc.prx", d.filePath("src/sce_module/libc.prx"))},
                            CancellationToken())
                        .ok());
            QVERIFY(QFileInfo::exists(dest + ".part"));
            QVERIFY(!QFileInfo::exists(dest)); // not renamed before the read-back
            QVERIFY(z.verify(CancellationToken()).ok());
            QVERIFY(z.commit().ok());
            QCOMPARE(z.result().count, 2);
            QCOMPARE(z.result().sha256.value("eboot.bin"), QString::fromLatin1(sha(selfOf(f1()))));
            QCOMPARE(z.result().zipSha256, QString::fromLatin1(sha(readBytes(dest))));
        }
        QCOMPARE(readBytes(target), QByteArray("earlier originals"));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
        QVERIFY(!QFileInfo::exists(dest + ".part.part"));
        zip::Reader r;
        QVERIFY(r.open(dest).ok());
        QCOMPARE(int(r.entries().size()), 2);
        QCOMPARE(r.entries()[0].name, QStringLiteral("eboot.bin")); // sorted
        QVERIFY(r.verifyAll(CancellationToken()).ok());
        // Handles are closed: the archive can be renamed and deleted (S23).
        QVERIFY(QFile::rename(dest, dest + ".moved"));
        QVERIFY(QFile::remove(dest + ".moved"));
    }

    void failureLeavesNothing() // port of AtomicZip.test_failure_leaves_nothing
    {
        QTemporaryDir d;
        const QString dest = d.filePath("x.zip");
        {
            StagedZip z(dest);
            QVERIFY(!z.write({ZipMember{"a", d.filePath("missing"), "00"}}, CancellationToken()).ok());
        }
        QCOMPARE(filesIn(d.path()), QStringList());
        // A hook failing mid-archive: nothing left either.
        writeBytes(d.filePath("s/a.bin"), "aaaa");
        writeBytes(d.filePath("s/b.bin"), "bbbb");
        {
            StagedZip z(dest);
            const ZipHook hook = [](const QString& point, const QString& item) -> Status {
                if (point == u"zip.member" && item == u"b.bin")
                    return makeError(Code::ZipWriteFailed, QStringLiteral("disk full (injected)"));
                return success();
            };
            QVERIFY(!z.write({member("a.bin", d.filePath("s/a.bin")), member("b.bin", d.filePath("s/b.bin"))},
                             CancellationToken(), nullptr, hook)
                         .ok());
        }
        QCOMPARE(filesIn(d.path()), QStringList());
    }

    void readBackDetectsChangedSource()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("s/a.bin"), "aaaa");
        const QString dest = d.filePath("x.zip");
        StagedZip z(dest);
        // The recorded hash is of other content: the read-back must refuse.
        QVERIFY(z.write({ZipMember{"a.bin", d.filePath("s/a.bin"), QString::fromLatin1(sha("different"))}}, CancellationToken()).ok());
        auto st = z.verify(CancellationToken());
        QVERIFY(!st.ok());
        QCOMPARE(codeOf(st.error()), Code::ZipVerifyFailed);
        QVERIFY(!z.commit().ok());
        z.discard();
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
    }

    void corruptPartIsCaught()
    {
        QTemporaryDir d;
        QByteArray big;
        for (int i = 0; i < 200000; ++i)
            big.append(char(i * 31));
        writeBytes(d.filePath("s/a.bin"), big);
        const QString dest = d.filePath("x.zip");
        StagedZip z(dest);
        QVERIFY(z.write({member("a.bin", d.filePath("s/a.bin"))}, CancellationToken()).ok());
        // Flip a byte inside the stored data.
        QFile f(dest + ".part");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.seek(200);
        char c = 0;
        f.getChar(&c);
        f.seek(200);
        f.putChar(char(c ^ 0x55));
        f.close();
        QVERIFY(!z.verify(CancellationToken()).ok());
    }

    void caseInsensitiveDedupeFirstWins()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("one"), "first");
        writeBytes(d.filePath("two"), "second");
        auto m = normalizeMembers({member("Fakelib/LIB.sprx", d.filePath("one")), member("fakelib/lib.sprx", d.filePath("two")),
                                   member("a\\b.bin", d.filePath("two"))});
        QCOMPARE(int(m.size()), 2);
        QCOMPARE(m[0].arcName, QStringLiteral("Fakelib/LIB.sprx"));
        QCOMPARE(m[1].arcName, QStringLiteral("a/b.bin"));
    }

    void unicodeNames()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("s/x"), "data");
        const QString dest = d.filePath(QStringLiteral("PPSA01234 Jéu ゲーム original files.zip"));
        StagedZip z(dest);
        QVERIFY(z.write({member(QStringLiteral("sce_module/été.prx"), d.filePath("s/x"))}, CancellationToken()).ok());
        QVERIFY(z.verify(CancellationToken()).ok());
        QVERIFY(z.commit().ok());
        zip::Reader r;
        QVERIFY(r.open(dest).ok());
        QCOMPARE(r.entries()[0].name, QStringLiteral("sce_module/été.prx"));
    }

    // ---- S10 / S28 / S30 ----------------------------------------------------------------
    void destinationRules() // port of ZipDestination
    {
        QTemporaryDir d;
        const QString game = d.filePath("game");
        writeBytes(game + "/eboot.bin", f1());
        const QString out = d.filePath("out");
        QDir().mkpath(out);
        QCOMPARE(zipDestinationProblem(out, game, "PPSA01234 original files.zip"), QString());
        QCOMPARE(filesIn(out), QStringList()); // probe removed
        for (const QString& dest : {game, game + "/sce_sys"}) {
            QDir().mkpath(dest);
            QVERIFY(zipDestinationProblem(dest, game, "x.zip").contains(QStringLiteral("inside the game folder")));
        }
        const QString sib = d.filePath("game2"); // common prefix, not inside
        QDir().mkpath(sib);
        QCOMPARE(zipDestinationProblem(sib, game, "x.zip"), QString());
        QVERIFY(zipDestinationProblem(d.filePath("nope"), game, "x.zip").contains(QStringLiteral("does not exist")));
        QVERIFY(zipDestinationProblem(QString(), game, "x.zip").contains(QStringLiteral("No folder")));
        const QString msg = zipDestinationProblem(out, game, QString(250, u'x') + ".zip");
        QVERIFY2(msg.contains(QStringLiteral("Windows only guarantees 259")), qPrintable(msg));
    }

    void unwritableDestinationRefused()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX permissions");
#else
        QTemporaryDir d;
        const QString ro = d.filePath("ro");
        QDir().mkpath(ro);
        QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (QFile(ro + "/probe").open(QIODevice::WriteOnly)) { // running as root
            QFile::remove(ro + "/probe");
            QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            QSKIP("running with permissions that ignore read-only folders");
        }
        QVERIFY(zipDestinationProblem(ro, QString(), "x.zip").contains(QStringLiteral("cannot be written")));
        QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    }

    void cancellationLeavesNoPart()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("s/a.bin"), QByteArray(3 * 1024 * 1024, 'a'));
        CancellationSource src;
        src.cancel();
        const QString dest = d.filePath("x.zip");
        {
            StagedZip z(dest);
            auto st = z.write({member("a.bin", d.filePath("s/a.bin"))}, src.token());
            QVERIFY(!st.ok());
            QVERIFY(st.error().isCancelled());
        }
        QCOMPARE(filesIn(d.path()), QStringList());
    }
};

QTEST_GUILESS_MAIN(TstBackportZip)
#include "tst_backport_zip.moc"
