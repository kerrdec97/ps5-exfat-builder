// Backport scanner (S15, S16, S32, S33), fakelib set rules (S8, S6), game
// metadata and ZIP prefix, manifest schema-1 reading and hash verdict (S7),
// settings registration and legacy keys.

#include "BackportTestEnv.h"

#include "core/Settings.h"
#include "engines/backport/BackportManifest.h"
#include "engines/backport/BackportSettings.h"
#include "engines/backport/ExecutableScanner.h"
#include "engines/backport/FakelibSet.h"
#include "engines/backport/GameMetadata.h"
#include "engines/backport/Module.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

QStringList rels(const ScanResult& s, bool processableOnly = false)
{
    QStringList out;
    for (const ExecFile& e : s.executables)
        if (!processableOnly || e.isProcessable())
            out << e.rel;
    return out;
}

} // namespace

class TstBackportScan : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    // ---- ExecutableScanner -------------------------------------------------------
    void scanClassifiesSelfAndElf()
    {
        Env e(Mixed);
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        QVERIFY(s->hasEboot);
        QCOMPARE(rels(*s), QStringList({QStringLiteral("eboot.bin"), QStringLiteral("sce_module/libc.prx")}));
        QCOMPARE(s->find(QStringLiteral("eboot.bin"))->cls, FileClass::Self);
        QCOMPARE(s->find(QStringLiteral("sce_module/libc.prx"))->cls, FileClass::Elf);
        QCOMPARE(s->gameSdk().value_or(-1), 7);                        // D7: from eboot.bin
        QCOMPARE(s->find(QStringLiteral("sce_module/libc.prx"))->sdk.value_or(-1), 8);
        QCOMPARE(s->find(QStringLiteral("eboot.bin"))->ps4.value_or(0), quint32(0x09590001));
        QCOMPARE(s->paramJsonRel, QStringLiteral("sce_sys/param.json"));
        QVERIFY(!s->find(QStringLiteral("eboot.bin"))->sha256.isEmpty());
    }

    void osMetadataIsNeverAnExecutable() // S15 / G1(f)
    {
        Env e(OsMetadata);
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        QCOMPARE(rels(*s), QStringList({QStringLiteral("eboot.bin"), QStringLiteral("sce_module/libc.prx")}));
        QVERIFY(s->metadataSkipped.contains(QStringLiteral("._eboot.bin")));
        QVERIFY(s->metadataSkipped.contains(QStringLiteral(".DS_Store")));
        QVERIFY(s->metadataSkipped.contains(QStringLiteral("__MACOSX/")));
        QVERIFY(s->metadataSkipped.contains(QStringLiteral("sce_module/._libc.prx")));
    }

    void bakFilesAreReportedNotExecutables() // S16 / G1(g)
    {
        Env e(LeftoverBak);
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        QCOMPARE(s->leftoverBaks, QStringList({QStringLiteral("eboot.bin.bak")}));
        QVERIFY(!s->find(QStringLiteral("eboot.bin.bak")));
    }

    void fakelibAndDecryptedAreNeverProcessed() // S33 / G1(d)
    {
        Env e(OtherFakelib);
        writeBytes(e.game + "/decrypted/eboot.bin", f1());
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        QCOMPARE(rels(*s, true), QStringList({QStringLiteral("eboot.bin"), QStringLiteral("sce_module/libc.prx")}));
        QVERIFY(s->find(QStringLiteral("fakelib/libSceAmpr.sprx")));
        QCOMPARE(s->find(QStringLiteral("fakelib/libSceAmpr.sprx"))->area, Area::Fakelib);
        QCOMPARE(s->find(QStringLiteral("decrypted/eboot.bin"))->area, Area::Decrypted);
        QCOMPARE(s->fakelibFiles.size(), 2);
        QCOMPARE(s->decryptedFiles, QStringList({QStringLiteral("decrypted/eboot.bin")}));
    }

    void encryptedSelfDetectedUpFront() // S32 / G1(e)
    {
        Env e(EncryptedRight);
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        const ExecFile* r = s->find(QStringLiteral("sce_sys/about/right.sprx"));
        QVERIFY(r);
        QCOMPARE(r->cls, FileClass::SelfEncrypted);
        QCOMPARE(s->encryptedOrCompressed().size(), size_t(1));
        auto q = scanGame(e.game, CancellationToken(), ScanMode::Quick);
        QVERIFY(q.ok());
        QCOMPARE(q->find(QStringLiteral("sce_sys/about/right.sprx"))->cls, FileClass::SelfEncrypted);
        QCOMPARE(q->gameSdk().value_or(-1), 7); // quick scans still read eboot.bin
    }

    void unreadableHeaderFallsBackToName()
    {
        Env e;
        writeBytes(e.game + "/sce_module/tiny.prx", QByteArray("ab"));
        writeBytes(e.game + "/notes.prx", QByteArray("hello world")); // readable, not ELF/SELF
        writeBytes(e.game + "/short.txt", QByteArray("ab"));
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        QCOMPARE(s->find(QStringLiteral("sce_module/tiny.prx"))->cls, FileClass::Unreadable);
        QVERIFY(!s->find(QStringLiteral("notes.prx")));
        QVERIFY(!s->find(QStringLiteral("short.txt")));
    }

    void paramProblemsAreReportedPerFile()
    {
        Env e;
        writeBytes(e.game + "/sce_module/bad.prx", selfOf(f4()));
        auto s = scanGame(e.game, CancellationToken());
        QVERIFY(s.ok());
        const ExecFile* b = s->find(QStringLiteral("sce_module/bad.prx"));
        QCOMPARE(b->problemCode, Code::ParamMagicInvalid);
        QVERIFY(b->problem.contains(QStringLiteral("0xDEADBEEF")));
        QVERIFY(b->problem.contains(QStringLiteral("may still be encrypted")));
    }

    void nestedEbootAndExcludedFolders() // G1(h), S38
    {
        Env e(NestedEboot);
        QDir().mkpath(e.game + "/backups");
        writeBytes(e.game + "/backups/x.prx", f2());
        auto s = scanGame(e.game, CancellationToken(), ScanMode::Full, {e.game + "/backups"});
        QVERIFY(s.ok());
        QCOMPARE(s->nestedEbootDirs, QStringList({QStringLiteral("sub")}));
        QVERIFY(!s->find(QStringLiteral("backups/x.prx")));
    }

    void discoverGamesHonoursDepthAndExcludes() // backport.scanDepth, S38
    {
        QTemporaryDir lib;
        makeGame(lib.filePath("a/PPSA00001-app"), SelfOnly);
        makeGame(lib.filePath("b/c/d/PPSA00002-app"), SelfOnly);
        makeGame(lib.filePath("zips/PPSA00003-app"), SelfOnly);
        QStringList warnings;
        auto all = discoverGames({lib.path(), lib.filePath("missing")}, 6, {lib.filePath("zips")}, CancellationToken(), &warnings);
        QCOMPARE(all.size(), 2);
        QCOMPARE(warnings.size(), 1);
        auto shallow = discoverGames({lib.path()}, 2, {}, CancellationToken());
        QCOMPARE(shallow.size(), 2); // a/ and zips/, not b/c/d/
        QVERIFY(!shallow.join(u'|').contains(QStringLiteral("PPSA00002")));
    }

    void excludeThatContainsTheRootIsIgnored() // review bug 3
    {
        Env e;
        // The ZIP folder is the game's parent, grandparent, or the game itself.
        for (const QString& ex : {e.tmp.filePath("games"), e.tmp.path(), e.game}) {
            auto s = scanGame(e.game, CancellationToken(), ScanMode::Full, {ex});
            QVERIFY(s.ok());
            QVERIFY2(s->find(QStringLiteral("sce_module/libc.prx")), qPrintable(ex));
        }
        QCOMPARE(excludesBelow(e.game, {e.tmp.filePath("games"), e.game + "/backups", QString()}),
                 QStringList({QDir::cleanPath(e.game + "/backups")}));
        // Library discovery: a ZIP folder that holds the library, or is it.
        QTemporaryDir lib;
        makeGame(lib.filePath("PPSA00001-app"), SelfOnly);
        makeGame(lib.filePath("sub/PPSA00002-app"), SelfOnly);
        makeGame(lib.filePath("zips/PPSA00003-app"), SelfOnly);
        QCOMPARE(discoverGames({lib.path()}, 6, {lib.path()}, CancellationToken()).size(), 3);
        QCOMPARE(discoverGames({lib.filePath("sub")}, 6, {lib.path()}, CancellationToken()).size(), 1);
        QCOMPARE(discoverGames({lib.path()}, 6, {lib.filePath("zips")}, CancellationToken()).size(), 2); // S38 kept
    }

    void hostileSelfInALibraryScan() // review bug 1: reached through scanGame on eboot.bin
    {
        QTemporaryDir d;
        for (quint64 off : {0xFFFFFFFFFFFFFFC0ULL, 0x800000000ULL}) {
            QByteArray s = selfOf(f1());
            put64(s, 0x20 + 6 * 0x20 + 0x40 + 0x08, off); // ELF copy: phdr 0 p_offset
            writeBytes(d.filePath("lib/PPSA00001-app/eboot.bin"), s);
            for (ScanMode mode : {ScanMode::Quick, ScanMode::Full}) {
                auto r = scanGame(d.filePath("lib/PPSA00001-app"), CancellationToken(), mode);
                QVERIFY(r.ok());
                QCOMPARE(r->eboot()->cls, FileClass::SelfMalformed);
                QVERIFY(!r->gameSdk().has_value());
            }
        }
    }

    // ---- FakelibSet (port of tests/test_backport_safety.py FakelibSource) -----------
    void fakelibGoodSet()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("set/libSceAgc.sprx"), fakeLib(1));
        writeBytes(d.filePath("set/readme.txt"), "notes");
        writeBytes(d.filePath("set/._libSceAgc.sprx"), QByteArray("\x00\x05\x16\x07", 4));
        FakelibSource src;
        QCOMPARE(fakelibProblems(d.filePath("set"), QString(), &src), QStringList());
        QCOMPARE(int(src.libs.size()), 1);
        QVERIFY(src.ignored.contains(QStringLiteral("readme.txt")));
    }

    void fakelibNestedSetRejected()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("parent/fakelib/libSceAgc.sprx"), fakeLib(1));
        const QStringList p = fakelibProblems(d.filePath("parent"), QString());
        QCOMPARE(p.size(), 1);
        QVERIFY(p[0].contains(QStringLiteral("sub-folder")));
    }

    void fakelibUndersizedAndNonElfRejected()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("set/libSceAgc.sprx"), QByteArray());
        writeBytes(d.filePath("set/libSceA.sprx"), "version https://git-lfs.github.com/spec/v1\n");
        writeBytes(d.filePath("set/libSceB.prx"), QByteArray("MZ") + QByteArray(4000, '\0'));
        const QStringList p = fakelibProblems(d.filePath("set"), QString());
        QCOMPARE(p.size(), 3);
        QVERIFY(p.join(u'|').contains(QStringLiteral("0 bytes")));
        QVERIFY(p.join(u'|').contains(QStringLiteral("not an ELF/SELF")));
    }

    void fakelibEmptyOrMissing()
    {
        QTemporaryDir d;
        QDir().mkpath(d.filePath("empty"));
        QVERIFY(fakelibProblems(d.filePath("empty"), QString())[0].contains(QStringLiteral("no .sprx/.prx")));
        QVERIFY(fakelibProblems(d.filePath("nope"), QString())[0].contains(QStringLiteral("does not exist")));
    }

    void fakelibInsideGameRejected()
    {
        QTemporaryDir d;
        writeBytes(d.filePath("game/eboot.bin"), f1());
        writeBytes(d.filePath("game/fakelib/libSceAgc.sprx"), fakeLib(1));
        const QStringList p = fakelibProblems(d.filePath("game/fakelib"), d.filePath("game"));
        QVERIFY(p[0].contains(QStringLiteral("outside the game folder")));
        // and a set that contains the game
        QVERIFY(fakelibProblems(d.path(), d.filePath("game"))[0].contains(QStringLiteral("outside the game folder")));
    }

    void fakelibStates() // port of FakelibState
    {
        QTemporaryDir d;
        const QString game = d.filePath("game"), src = d.filePath("fakelib_set");
        writeBytes(game + "/eboot.bin", QByteArray());
        writeBytes(src + "/libSceAgc.sprx", QByteArray("agc").repeated(100));
        const FakelibSource set = listFakelibSource(src);
        QCOMPARE(fakelibState(game, &set), FakelibState::None);
        writeBytes(game + "/fakelib/libSceAgc.sprx", "other tool");
        QCOMPARE(fakelibState(game, &set), FakelibState::Other);
        writeBytes(game + "/fakelib/libSceAgc.sprx", QByteArray("agc").repeated(100));
        QCOMPARE(fakelibState(game, &set), FakelibState::Same);
        QCOMPARE(fakelibState(game, nullptr), FakelibState::Other);
        writeBytes(game + "/fakelib/libSceAmpr.sprx", "extra");
        QCOMPARE(fakelibExtras(game, set), QStringList({QStringLiteral("fakelib/libSceAmpr.sprx")}));
    }

    // ---- GameMetadata / ZIP prefix ---------------------------------------------------
    void zipPrefixFromParamJson()
    {
        Env e;
        const GameMetadata m = readGameMetadata(e.game);
        QCOMPARE(m.titleId, QStringLiteral("PPSA01234"));
        QCOMPARE(m.title, QStringLiteral("Game"));
        QCOMPARE(m.version, QStringLiteral("01.000.000"));
        QCOMPARE(m.paramSdk.value_or(-1), 7);
        QCOMPARE(m.requiredFwText(), QStringLiteral("7.50"));
        QCOMPARE(zipPrefix(m, QStringLiteral("PPSA01234-app")), QStringLiteral("PPSA01234 Game (01.000.000)"));
        QCOMPARE(manifestTitleId(m, e.game), QStringLiteral("PPSA01234"));
    }

    void zipPrefixFolderNameFallbacks_data()
    {
        QTest::addColumn<QString>("folder");
        QTest::addColumn<QString>("expected");
        QTest::newRow("full") << QStringLiteral("PPSA15552 Dead Cells (01.000.000)")
                              << QStringLiteral("PPSA15552 Dead Cells (01.000.000)");
        QTest::newRow("app") << QStringLiteral("PPSA03598-app") << QStringLiteral("PPSA03598");
        QTest::newRow("site tag") << QStringLiteral("[DLPSGAME.COM]-Phantom Breaker 01.000 PPSA13794")
                                  << QStringLiteral("PPSA13794 Phantom Breaker 01.000");
        QTest::newRow("nothing") << QString() << QStringLiteral("Unknown Game");
        QTest::newRow("no id") << QStringLiteral("My Folder") << QStringLiteral("My Folder");
    }
    void zipPrefixFolderNameFallbacks()
    {
        QFETCH(QString, folder);
        QFETCH(QString, expected);
        QCOMPARE(zipPrefix(GameMetadata{}, folder), expected);
    }

    void titleSanitisedAndSfoRead()
    {
        GameMetadata m;
        m.titleId = QStringLiteral("ppsa00001");
        m.title = QStringLiteral("Bad: Name?\u2122");
        QCOMPARE(zipPrefix(m, QString()), QStringLiteral("PPSA00001 Bad Name"));
        // param.sfo (PSF) fallback
        QTemporaryDir d;
        QByteArray sfo(0x200, '\0');
        std::memcpy(sfo.data(), "\0PSF", 4);
        put32(sfo, 8, 0x40);   // key table
        put32(sfo, 12, 0x80);  // data table
        put32(sfo, 16, 2);     // entries
        // entry 0: TITLE_ID
        put16(sfo, 20, 0);
        put32(sfo, 24, 10);
        put32(sfo, 28, 16);
        put32(sfo, 32, 0);
        // entry 1: TITLE
        put16(sfo, 36, 9);
        put32(sfo, 40, 6);
        put32(sfo, 44, 16);
        put32(sfo, 48, 16);
        std::memcpy(sfo.data() + 0x40, "TITLE_ID\0TITLE\0", 15);
        std::memcpy(sfo.data() + 0x80, "CUSA12345", 9);
        std::memcpy(sfo.data() + 0x90, "Hello", 5);
        writeBytes(d.filePath("g/sce_sys/param.sfo"), sfo);
        const GameMetadata g = readGameMetadata(d.filePath("g"));
        QCOMPARE(g.titleId, QStringLiteral("CUSA12345"));
        QCOMPARE(g.title, QStringLiteral("Hello"));
        // Folder fallback for the manifest ID, sanitised.
        QCOMPARE(manifestTitleId(GameMetadata{}, QStringLiteral("/x/My Game!")), QStringLiteral("MyGame"));
        QCOMPARE(manifestTitleId(GameMetadata{}, QStringLiteral("/x/!!")), QStringLiteral("UNKNOWN"));
    }

    void ps5VersionFields()
    {
        QCOMPARE(parsePs5VersionField(QStringLiteral("0x1270000000000000"))->first, 12);
        QCOMPARE(parsePs5VersionField(QStringLiteral("0x1270000000000000"))->second, 70);
        QCOMPARE(parsePs5VersionField(QStringLiteral("0x1000000000000000"))->first, 10);
        QCOMPARE(parsePs5VersionField(QStringLiteral("0x0A00000000000000"))->first, 10); // non-BCD: top byte
        QCOMPARE(parsePs5VersionField(QStringLiteral("7"))->first, 7);
        QVERIFY(!parsePs5VersionField(QStringLiteral("zz")).has_value());
    }

    // ---- Manifest (port of ManifestDetection) ----------------------------------------
    void manifestVerdicts()
    {
        QTemporaryDir d;
        const QString game = d.filePath("game");
        writeBytes(game + "/eboot.bin", "patched");
        writeBytes(game + "/sce_module/libc.prx", "libc-patched");
        const QString origSha = QString::fromLatin1(sha("orig"));
        auto scan = scanGame(game, CancellationToken());
        QVERIFY(scan.ok());

        Manifest m;
        QJsonObject files{{"eboot.bin", origSha}, {"sce_sys/param.json", "ignored"}, {"decrypted/eboot.bin", "ignored"}};
        auto doc = fw::json::parse(QJsonDocument(QJsonObject{
                                       {"originals", QJsonObject{{"files", files}}},
                                       {"targets", QJsonObject{{"4xx", QJsonObject{{"post_hashes",
                                                                                    QJsonObject{{"EBOOT.BIN", QString::fromLatin1(sha("patched"))}}}}}}}})
                                       .toJson());
        QVERIFY(doc.ok());
        m.doc = doc.value();
        auto [v, f] = manifestVerdict(*scan, &m);
        QCOMPARE(v, Verdict::Patched); // keys compare case-insensitively
        QCOMPARE(f, QStringList({QStringLiteral("eboot.bin")}));

        writeBytes(game + "/eboot.bin", "orig");
        auto scan2 = scanGame(game, CancellationToken());
        QCOMPARE(manifestVerdict(*scan2, &m).first, Verdict::Original);

        // An old manifest without post hashes: differs.
        auto old = fw::json::parse(QJsonDocument(QJsonObject{{"originals", QJsonObject{{"files", files}}},
                                                             {"targets", QJsonObject{{"4xx", QJsonObject{}}}}})
                                       .toJson());
        Manifest m2;
        m2.doc = old.value();
        writeBytes(game + "/eboot.bin", "patched");
        auto scan3 = scanGame(game, CancellationToken());
        auto [v3, f3] = manifestVerdict(*scan3, &m2);
        QCOMPARE(v3, Verdict::Differs);
        QCOMPARE(f3, QStringList({QStringLiteral("eboot.bin")}));
        QCOMPARE(manifestVerdict(*scan3, nullptr).first, Verdict::Unknown);
    }

    void pythonSchema1ManifestReadAndUpgraded() // D8
    {
        QTemporaryDir d;
        const QString path = manifestPathFor(d.path(), QStringLiteral("PPSA01234"));
        QCOMPARE(QDir::fromNativeSeparators(path), d.filePath("BackportBackup/PPSA01234/manifest.json"));
        const QByteArray py =
            "{\n  \"schema_version\": 1,\n  \"title_id\": \"PPSA01234\",\n  \"title\": \"Game\",\n"
            "  \"game_folder\": \"D:\\\\Games\\\\PPSA01234-app\",\n  \"tool_version\": \"4.1.0\",\n"
            "  \"created_at\": \"2026-11-04 05:00:00\",\n  \"updated_at\": \"2026-11-04 05:00:00\",\n"
            "  \"current_target\": \"4xx\",\n  \"originals\": {\n    \"zip\": \"E:\\\\Zips\\\\x.zip\",\n"
            "    \"zip_abs\": \"E:\\\\Zips\\\\x.zip\",\n    \"zip_sha256\": \"ab\",\n"
            "    \"files\": {\"eboot.bin\": \"11\"},\n    \"snapshot_at\": \"2026-11-04 05:00:00\"\n  },\n"
            "  \"targets\": {\"4xx\": {\"built_at\": \"x\", \"tool_version\": \"4.1.0\", \"post_hashes\": "
            "{\"eboot.bin\": \"22\", \"fakelib/libSceAgc.sprx\": \"33\"}}},\n"
            "  \"history\": [{\"action\": \"apply_target\", \"target\": \"4xx\", \"timestamp\": \"x\", "
            "\"tool_version\": \"4.1.0\"}],\n  \"python_only_key\": 5\n}";
        QVERIFY(writeBytes(path, py));
        auto r = readManifest(path);
        QVERIFY(r.ok() && r.value());
        Manifest m = *r.value();
        QCOMPARE(m.schemaVersion(), 1);
        QCOMPARE(m.currentTarget().value_or(QString()), QStringLiteral("4xx"));
        QCOMPARE(m.originalsFiles().value(QStringLiteral("eboot.bin")), QStringLiteral("11"));
        QCOMPARE(m.postHashes().size(), 2);
        TargetRecord t;
        t.sdk = 5;
        t.applied = true;
        t.addedFiles = {QStringLiteral("fakelib/libSceAgc.sprx")};
        t.filesAfter.insert(QStringLiteral("fakelib/libSceAgc.sprx"), qMakePair(QStringLiteral("44"), qint64(9)));
        recordTarget(m, t);
        QVERIFY(writeManifest(m).ok());
        auto back = readManifest(path);
        QVERIFY(back.ok() && back.value());
        QCOMPARE(back.value()->schemaVersion(), 2);
        QCOMPARE(back.value()->currentTarget().value_or(QString()), QStringLiteral("5xx"));
        QCOMPARE(back.value()->addedFileHashes().value(QStringLiteral("fakelib/libSceAgc.sprx")), QStringLiteral("44"));
        QCOMPARE(back.value()->doc.value(QStringLiteral("python_only_key")).toInt(), 5); // unknown keys kept
        QCOMPARE(back.value()->doc.keys().front(), QStringLiteral("schema_version"));  // order kept
        // A damaged manifest is an error, a missing one is not.
        writeBytes(path, "{not json");
        QVERIFY(!readManifest(path).ok());
        QVERIFY(readManifest(d.filePath("none.json")).ok());
    }

    // ---- Settings ----------------------------------------------------------------------
    void settingsRegisteredAndWellFormed()
    {
        SettingsSchema schema;
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext ctx{schema, deps, jobs};
        registerModule(ctx);
        const QStringList expected = {keys::kOriginalsZipFolder, keys::kBackportZipFolder, keys::kTargetSdk,
                                      keys::kFakelibFolder, keys::kApplyToGameFolder, keys::kNidPatchMode,
                                      keys::kEncryptedSelfPolicy, keys::kVerifyBuild, keys::kScanFolders,
                                      keys::kScanDepth, keys::kKeepStagingOnFailure, keys::kJournalRetentionDays,
                                      keys::kNotes, keys::kAmprPlaygoFolder};
        int count = 0;
        for (const auto* d : schema.all()) {
            QVERIFY2(d->key.startsWith(QStringLiteral("backport.")), qPrintable(d->key));
            QVERIFY2(d->section == u"Backport" || d->section == u"Firmware and SDK", qPrintable(d->key));
            QVERIFY2(schema.normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
            QVERIFY2(!d->description.isEmpty(), qPrintable(d->key));
            ++count;
        }
        QCOMPARE(count, expected.size());
        for (const QString& k : expected)
            QVERIFY2(schema.contains(k), qPrintable(k));
        QCOMPARE(schema.find(keys::kTargetSdk)->section, QStringLiteral("Firmware and SDK"));
        QCOMPARE(schema.find(keys::kTargetSdk)->choices.size(), 13); // pairs 1-13
        QCOMPARE(schema.find(keys::kTargetSdk)->choices.last().label, QStringLiteral("SDK 13 · loads on FW 13.00+"));
        QCOMPARE(schema.find(keys::kTargetSdk)->defaultValue.toString(), QStringLiteral("4"));
        QCOMPARE(schema.find(keys::kApplyToGameFolder)->defaultValue.toBool(), true);
        QVERIFY(schema.normalize(keys::kTargetSdk, QStringLiteral("11")).ok());
        QVERIFY(!schema.normalize(keys::kTargetSdk, QStringLiteral("14")).ok());
        QVERIFY(!schema.normalize(keys::kNotes, QString(501, u'x')).ok());
        QVERIFY(!schema.normalize(keys::kScanDepth, 13).ok());
        QVERIFY(jobs.contains(QStringLiteral("backport.run")));
        QVERIFY(jobs.contains(QStringLiteral("backport.restore")));
        QVERIFY(jobs.contains(QStringLiteral("backport.scan")));
        QVERIFY(jobs.contains(QStringLiteral("backport.sdkFix")));
        QVERIFY(jobs.contains(QStringLiteral("backport.audioScan")));
        QVERIFY(jobs.contains(QStringLiteral("backport.pinProviders")));
        QCOMPARE(schema.find(keys::kAmprPlaygoFolder)->type, SettingType::DirPath);
        QVERIFY(schema.find(keys::kAmprPlaygoFolder)->perJob);
        // No settings for blocked features.
        for (const auto* d : schema.all())
            QVERIFY2(!d->key.contains(QStringLiteral("pro"), Qt::CaseInsensitive) || d->key == keys::kTargetSdk,
                     qPrintable(d->key));
    }

    void everySettingReachesTheOptions()
    {
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
        QTemporaryDir d;
        QDir().mkpath(d.filePath("o"));
        QDir().mkpath(d.filePath("b"));
        QDir().mkpath(d.filePath("f"));
        QDir().mkpath(d.filePath("p"));
        QVariantMap values{{keys::kOriginalsZipFolder, d.filePath("o")},
                           {keys::kAmprPlaygoFolder, d.filePath("p")},
                           {keys::kBackportZipFolder, d.filePath("b")},
                           {keys::kTargetSdk, QStringLiteral("5")},
                           {keys::kFakelibFolder, d.filePath("f")},
                           {keys::kApplyToGameFolder, false},
                           {keys::kNidPatchMode, QStringLiteral("off")},
                           {keys::kEncryptedSelfPolicy, QStringLiteral("leaveUnchanged")},
                           {keys::kVerifyBuild, false},
                           {keys::kKeepStagingOnFailure, false},
                           {keys::kJournalRetentionDays, 7},
                           {keys::kNotes, QStringLiteral("tested on 4.03")}};
        const SettingsValues v(schema, values, {});
        const BackportOptions o = optionsFromSettings(v, QStringLiteral("/g"));
        QCOMPARE(o.originalsDir, d.filePath("o"));
        QCOMPARE(o.backportDir, d.filePath("b"));
        QCOMPARE(o.targetSdk, 5);
        QCOMPARE(o.fakelibDir, d.filePath("f"));
        QCOMPARE(o.providerDir, d.filePath("p"));
        QCOMPARE(o.applyToGame, false);
        QCOMPARE(o.nidMode, NidMode::Off);
        QCOMPARE(o.encryptedPolicy, EncryptedPolicy::LeaveUnchanged);
        QCOMPARE(o.verifyBuild, false);
        QCOMPARE(o.keepStagingOnFailure, false);
        QCOMPARE(o.journalRetentionDays, 7);
        QCOMPARE(o.notes, QStringLiteral("tested on 4.03"));
    }

    void legacyKeysImport()
    {
        auto schema = std::make_shared<SettingsSchema>();
        DependencyRegistry deps;
        JobFactory jobs;
        ModuleContext ctx{*schema, deps, jobs};
        registerModule(ctx);
        QTemporaryDir d;
        QDir().mkpath(d.filePath("o"));
        writeBytes(d.filePath("lib.sprx"), "x");
        const QJsonObject legacy{{"abp_zip_orig_dest", d.filePath("o")},
                                 {"abp_target_sdk", 11}, // dropped: the old app wrote SDK 10 for "11"
                                 {"abp_last_sdk", "5"},
                                 {"abp_fakelib", d.filePath("lib.sprx")}, // a file: dropped
                                 {"abp_notes", QString(600, u'n')},
                                 {"bp_folders", QJsonArray{d.path()}}};
        writeBytes(d.filePath("legacy.json"), QJsonDocument(legacy).toJson());
        SettingsStore store(schema, d.filePath("settings.json"));
        auto n = store.importLegacy(d.filePath("legacy.json"));
        QVERIFY(n.ok());
        QCOMPARE(store.value(keys::kOriginalsZipFolder).toString(), d.filePath("o"));
        QCOMPARE(store.value(keys::kTargetSdk).toString(), QStringLiteral("5"));
        QCOMPARE(store.value(keys::kFakelibFolder).toString(), QString());
        QCOMPARE(store.value(keys::kNotes).toString().size(), kMaxNotesLength);
        QCOMPARE(store.value(keys::kScanFolders).toStringList(), QStringList({d.path()}));
    }
};

QTEST_GUILESS_MAIN(TstBackportScan)
#include "tst_backport_scan.moc"
