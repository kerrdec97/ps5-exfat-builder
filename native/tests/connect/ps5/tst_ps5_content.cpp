// Console-content tools: the ps5_autoloader editor (local autoload.txt), the
// ShadowMount+ fakelib manager, the recursive console folder walk and the
// PS5 Manager game list / folder compare — against the in-process
// FakeFtpServer (never a real console).

#include "FakePs5.h"
#include "Ps5TestUtil.h"

#include "connect/ps5/Autoloader.h"
#include "connect/ps5/ConsoleGames.h"
#include "connect/ps5/Fakelib.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/RemoteTree.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <algorithm>

using namespace fw;
using namespace fw::ps5;
using fake::FakeFtpServer;
using ps5test::Module;
using ps5test::quick;
using ps5test::readAll;

namespace {

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

const QByteArray kElf = QByteArray("\x7f" "ELF", 4) + QByteArray(60, '\1');
const QByteArray kSelf1 = QByteArray("\x4f\x15\x3d\x1d", 4) + QByteArray(60, '\2');
const QByteArray kSelf2 = QByteArray("\x54\x14\xf5\xee", 4) + QByteArray(60, '\3');

} // namespace

class TstPs5Content : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    int m_n = 0;

    QString fresh(const QString& name)
    {
        const QString p = m_tmp.filePath(QStringLiteral("%1-%2").arg(++m_n).arg(name));
        QDir().mkpath(p);
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    // ---- autoloader ---------------------------------------------------------------------

    void autoloaderNormalizesLikeTheLegacyEditor()
    {
        using namespace autoloader;
        QCOMPARE(normalize(QStringLiteral("a.elf\r\n!1000\r\n\r\n\r\n\r\nb.bin\r\n\r\n")), QStringLiteral("a.elf\n!1000\n\nb.bin\n"));
        QCOMPARE(normalize(QStringLiteral("x.js\ry.js")), QStringLiteral("x.js\ny.js\n"));
        QCOMPARE(normalize(QString()), QStringLiteral("\n"));
        QVERIFY(defaultHeader().startsWith(QStringLiteral("# ps5_autoloader\n")));
        QVERIFY(defaultHeader().contains(QStringLiteral("follow it with !4000.")));
        QCOMPARE(normalize(defaultHeader()), defaultHeader());
        QCOMPARE(sleepLine(5000), QStringLiteral("!5000"));
        QVERIFY(isPayloadName(QStringLiteral("A.ELF")) && isPayloadName(QStringLiteral("b.bin")));
        QVERIFY(!isPayloadName(QStringLiteral("c.js"))); // the autoloader does not run .js
        QVERIFY(!isPayloadName(QStringLiteral("autoload.txt")));
    }

    void autoloaderParsesAndValidates()
    {
        using namespace autoloader;
        const QString text = QStringLiteral("# header\n\nkstuff.elf\n!1500\nelfldr.elf\nreadme.txt\n!soon\n  spaced.bin  \n!0\n");
        const auto lines = parse(text);
        QCOMPARE(int(lines.size()), 9);
        QCOMPARE(lines[0].kind, LineKind::Comment);
        QCOMPARE(lines[1].kind, LineKind::Blank);
        QCOMPARE(lines[2].kind, LineKind::Payload);
        QCOMPARE(lines[3].kind, LineKind::Sleep);
        QCOMPARE(lines[3].sleepMs, 1500);
        QCOMPARE(lines[5].kind, LineKind::Invalid);
        QCOMPARE(lines[6].kind, LineKind::Invalid);
        QCOMPARE(lines[7].payload, QStringLiteral("spaced.bin"));
        const Validation v = validate(text, {QStringLiteral("kstuff.elf"), QStringLiteral("elfldr.elf")}, true);
        QCOMPARE(v.errors.size(), 2); // readme.txt, !soon
        QVERIFY(v.errors.join(u'\n').contains(QStringLiteral("Line 6")));
        QVERIFY(v.errors.join(u'\n').contains(QStringLiteral("Line 7")));
        QCOMPARE(v.payloads, 3);
        QCOMPARE(v.sleeps, 2);
        const QString w = v.warnings.join(u'\n');
        QVERIFY(w.contains(QStringLiteral("elfldr.elf must come before")));
        QVERIFY(w.contains(QStringLiteral("spaced.bin is not in the folder")));
        QVERIFY(w.contains(QStringLiteral("!0 does not wait")));
        // Loader first: no order warning; a case variant of a file is reported as such.
        const Validation ok = validate(QStringLiteral("elfldr.elf\n!1000\nKstuff.elf\n"), {QStringLiteral("elfldr.elf"), QStringLiteral("kstuff.elf")}, true);
        QVERIFY(ok.ok());
        QVERIFY(!ok.warnings.join(u'\n').contains(QStringLiteral("must come before")));
        QVERIFY(ok.warnings.join(u'\n').contains(QStringLiteral("differs in case")));
        QVERIFY(validate(defaultHeader()).ok());
        QVERIFY(validate(defaultHeader()).warnings.join(u' ').contains(QStringLiteral("no payloads")));
    }

    // Today's autoloader rules (ps5-unified-autoloader 0.1.7, research §9).
    void autoloaderFollowsTheUnifiedAutoloaderRules()
    {
        using namespace autoloader;
        const QString text = QStringLiteral("elfldr.elf   # LAN loader\n!4000\n@sync\n@later\n/data/payloads/kstuff.elf\nsub/x.bin\n"
                                            "script.js\nmy#file.elf\n\t ftpsrv.elf\n# only a comment\n");
        const auto lines = parse(text);
        QCOMPARE(int(lines.size()), 10);
        QCOMPARE(lines[0].kind, LineKind::Payload);
        QCOMPARE(lines[0].payload, QStringLiteral("elfldr.elf")); // inline comment
        QVERIFY(lines[0].warning.isEmpty());
        QCOMPARE(lines[1].sleepMs, 4000);
        QCOMPARE(lines[2].kind, LineKind::Directive);
        QCOMPARE(lines[2].directive, QStringLiteral("sync"));
        QVERIFY(lines[2].warning.isEmpty());
        QCOMPARE(lines[3].kind, LineKind::Directive);
        QVERIFY(lines[3].warning.contains(QStringLiteral("only @sync")));
        QCOMPARE(lines[4].kind, LineKind::Payload);
        QVERIFY(lines[4].absolute); // absolute paths are allowed now
        QCOMPARE(lines[5].kind, LineKind::Payload);
        QCOMPARE(lines[6].kind, LineKind::Payload); // sent to elfldr upstream, so it is listed, with a warning
        QVERIFY(lines[6].warning.contains(QStringLiteral(".js")));
        QCOMPARE(lines[7].kind, LineKind::Invalid); // "my" before the comment
        QVERIFY(lines[7].problem.contains(QStringLiteral("cannot contain #")));
        QCOMPARE(lines[8].kind, LineKind::Payload);
        QVERIFY(lines[8].warning.contains(QStringLiteral("starts with spaces")));
        QCOMPARE(lines[9].kind, LineKind::Comment);
        const Validation v = validate(text, {QStringLiteral("elfldr.elf")}, true);
        QCOMPARE(v.errors.size(), 1);
        const QString w = v.warnings.join(u'\n');
        QVERIFY(!w.contains(QStringLiteral("kstuff.elf is not in the folder"))); // absolute: not checked against the folder
        QVERIFY(!w.contains(QStringLiteral("is a path")));
        QVERIFY(w.contains(QStringLiteral("only @sync")));
        QVERIFY(w.contains(QStringLiteral("script.js")));
        QVERIFY(w.contains(QStringLiteral("ftpsrv.elf is not in the folder")));
        QVERIFY(!w.contains(QStringLiteral("must come before")));
        // Deleting a payload also finds its lines with an inline comment.
        QCOMPARE(removePayloadLines(QStringLiteral("a.elf # x\n!100\nb.elf\n"), QStringLiteral("a.elf")), QStringLiteral("b.elf\n"));
        // The header documents the four places and the LAN loader.
        const QString h = defaultHeader();
        QVERIFY(h.contains(QStringLiteral("/data/ps5_autoloader_<TITLEID>/")));
        QVERIFY(h.contains(QStringLiteral("@sync")));
        QVERIFY(!h.contains(QStringLiteral("savedata")));
        QVERIFY(!h.contains(QStringLiteral(".elf, .bin, .js")));
    }

    void autoloaderEditingHelpers()
    {
        using namespace autoloader;
        // Inserted at the end of the cursor's line, never splitting it.
        auto ins = insertLineAfter(QStringLiteral("a.elf\nb.elf\n"), 2, QStringLiteral("c.elf"));
        QCOMPARE(ins.text, QStringLiteral("a.elf\nc.elf\nb.elf\n"));
        QCOMPARE(ins.cursor, 11);
        ins = insertLineAfter(QStringLiteral("a.elf"), 5, sleepLine(250));
        QCOMPARE(ins.text, QStringLiteral("a.elf\n!250"));
        ins = insertLineAfter(QString(), 0, QStringLiteral("x.elf"));
        QCOMPARE(ins.text, QStringLiteral("x.elf"));
        // The legacy delete: the name's lines and the sleeps right after them.
        const QString before = QStringLiteral("# c\nold.elf\n!1000\n!2000\nkeep.elf\n!500\nold.elf\n# after\n!300\n");
        QCOMPARE(removePayloadLines(before, QStringLiteral("old.elf")), QStringLiteral("# c\nkeep.elf\n!500\n# after\n!300\n"));
    }

    void autoloaderLoadSaveAddDelete()
    {
        using namespace autoloader;
        const QString dir = fresh(QStringLiteral("al"));
        QVERIFY(!load(dir + QStringLiteral("/missing")).ok());
        auto first = load(dir);
        QVERIFY(first.ok());
        QVERIFY(!first->existed);
        QCOMPARE(first->text, defaultHeader());

        // First save: the header plus the user's lines, LF only, no backup.
        auto s1 = save(dir, first->text + QStringLiteral("elfldr.elf\r\n!1000\r\nkstuff.elf"));
        QVERIFY(s1.ok());
        QVERIFY(s1->changed);
        QVERIFY(s1->backupPath.isEmpty());
        const QByteArray onDisk = readAll(QDir(dir).filePath(kFileName));
        QVERIFY(!onDisk.contains('\r'));
        QVERIFY(onDisk.endsWith("kstuff.elf\n"));
        QVERIFY(onDisk.startsWith("# ps5_autoloader\n"));

        // Unchanged text: nothing written, no backup.
        auto same = save(dir, QString::fromUtf8(onDisk));
        QVERIFY(same.ok());
        QVERIFY(!same->changed);
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("autoload.txt.fw.bak"))));

        // Copies of the user's own (or an older version's backups) next to the
        // file: never counted as the app's backups, never removed.
        QStringList own;
        for (const char* name : {"autoload.txt.bak", "autoload.txt.bak.2", "autoload.txt.bak.old"}) {
            own << QDir(dir).filePath(QString::fromLatin1(name));
            QFile ownFile(own.last());
            QVERIFY(ownFile.open(QIODevice::WriteOnly) && ownFile.write("mine") == 4);
        }

        // Changes keep the previous file; a backup is never overwritten.
        auto s2 = save(dir, QStringLiteral("a.elf\n"));
        QVERIFY(s2.ok());
        QCOMPARE(QFileInfo(s2->backupPath).fileName(), QStringLiteral("autoload.txt.fw.bak"));
        QCOMPARE(readAll(s2->backupPath), onDisk);
        auto s3 = save(dir, QStringLiteral("b.elf\n"));
        QVERIFY(s3.ok());
        QCOMPARE(QFileInfo(s3->backupPath).fileName(), QStringLiteral("autoload.txt.fw.bak.2"));
        QCOMPARE(readAll(s3->backupPath), QByteArray("a.elf\n"));
        auto again = load(dir);
        QVERIFY(again.ok());
        QVERIFY(again->existed);
        QCOMPARE(again->text, QStringLiteral("b.elf\n"));

        // The newest three backups stay; older ones the saves made are removed,
        // the user's files next to them are not.
        auto s4 = save(dir, QStringLiteral("c.elf\n"));
        QVERIFY(s4.ok());
        QCOMPARE(QFileInfo(s4->backupPath).fileName(), QStringLiteral("autoload.txt.fw.bak.3"));
        QVERIFY(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("autoload.txt.fw.bak"))));
        auto s5 = save(dir, QStringLiteral("d.elf\n"));
        QVERIFY(s5.ok());
        QCOMPARE(QFileInfo(s5->backupPath).fileName(), QStringLiteral("autoload.txt.fw.bak.4"));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("autoload.txt.fw.bak"))));
        auto s6 = save(dir, QStringLiteral("e.elf\n"));
        QVERIFY(s6.ok());
        // The number goes on from the newest backup, not back to a freed one.
        QCOMPARE(QFileInfo(s6->backupPath).fileName(), QStringLiteral("autoload.txt.fw.bak.5"));
        QCOMPARE(readAll(s6->backupPath), QByteArray("d.elf\n"));
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("autoload.txt.fw.bak.2"))));
        QCOMPARE(QDir(dir).entryList({QStringLiteral("autoload.txt.*")}, QDir::Files, QDir::Name),
                 (QStringList{QStringLiteral("autoload.txt.bak"), QStringLiteral("autoload.txt.bak.2"), QStringLiteral("autoload.txt.bak.old"),
                              QStringLiteral("autoload.txt.fw.bak.3"), QStringLiteral("autoload.txt.fw.bak.4"),
                              QStringLiteral("autoload.txt.fw.bak.5")}));
        for (const QString& o : own) {
            QCOMPARE(readAll(o), QByteArray("mine"));
            QVERIFY(QFile::remove(o));
        }
        const auto newest = load(dir);
        QVERIFY(newest.ok());
        QCOMPARE(newest->text, QStringLiteral("e.elf\n"));

        // Payload files: copied in, listed, refused when not a payload, deleted.
        const QString src = fresh(QStringLiteral("al-src"));
        QVERIFY(writeBytes(QDir(src).filePath(QStringLiteral("kstuff.elf")), kElf));
        QVERIFY(writeBytes(QDir(src).filePath(QStringLiteral("notes.txt")), "x"));
        auto added = addPayload(dir, QDir(src).filePath(QStringLiteral("kstuff.elf")), false);
        QVERIFY(added.ok());
        COMPARE_VALUE(added, QStringLiteral("kstuff.elf"));
        QCOMPARE(readAll(QDir(dir).filePath(QStringLiteral("kstuff.elf"))), kElf);
        COMPARE_ERROR_CODE(addPayload(dir, QDir(src).filePath(QStringLiteral("kstuff.elf")), false), ErrorCode::AlreadyExists);
        QVERIFY(writeBytes(QDir(src).filePath(QStringLiteral("kstuff.elf")), kSelf1));
        QVERIFY(addPayload(dir, QDir(src).filePath(QStringLiteral("kstuff.elf")), true).ok());
        QCOMPARE(readAll(QDir(dir).filePath(QStringLiteral("kstuff.elf"))), kSelf1);
        COMPARE_ERROR_CODE(addPayload(dir, QDir(src).filePath(QStringLiteral("notes.txt")), false), ErrorCode::InvalidArgument);
        QVERIFY(writeBytes(QDir(dir).filePath(QStringLiteral("Alpha.bin")), "b"));
        QVERIFY(writeBytes(QDir(dir).filePath(QStringLiteral("zeta.js")), "j"));
        QCOMPARE(payloadFiles(dir), (QStringList{QStringLiteral("Alpha.bin"), QStringLiteral("kstuff.elf")})); // no .js
        COMPARE_ERROR_CODE(addPayload(dir, QDir(dir).filePath(QStringLiteral("zeta.js")), false), ErrorCode::InvalidArgument);
        QVERIFY(!deletePayload(dir, QStringLiteral("../outside.elf"), false).ok());
        QVERIFY(!deletePayload(dir, QStringLiteral("autoload.txt"), false).ok());
        bool trashed = true;
        QVERIFY(deletePayload(dir, QStringLiteral("zeta.js"), false, &trashed).ok()); // never the real recycle bin in tests
        QVERIFY(!trashed);
        QVERIFY(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("zeta.js"))));
        COMPARE_ERROR_CODE(deletePayload(dir, QStringLiteral("zeta.js"), false), ErrorCode::NotFound);
    }

    void autoloaderFolderSettingAndLegacyKey()
    {
        Module mod;
        const SettingDef* d = mod.schema->find(keys::kAutoloaderFolder);
        QVERIFY(d);
        QCOMPARE(d->type, SettingType::DirPath);
        QCOMPARE(mod.schema->legacyKeys().at(QStringLiteral("al_dir")).key, keys::kAutoloaderFolder);
        const QString legacy = QDir(fresh(QStringLiteral("al-legacy"))).filePath(QStringLiteral("s.json"));
        QVERIFY(fwtest::writeTextFile(legacy, "{\"al_dir\":\"D:/PS5/ps5_autoloader\"}"));
        SettingsStore store(mod.schema, QDir(fresh(QStringLiteral("al-store"))).filePath(QStringLiteral("s.json")));
        QVERIFY(store.load().ok());
        COMPARE_VALUE(store.importLegacy(legacy), 1);
        QCOMPARE(autoloader::folderFromSettings(store.snapshot()), QStringLiteral("D:/PS5/ps5_autoloader"));
    }

    // ---- fakelib manager ---------------------------------------------------------------

    void fakelibLocalFileRules()
    {
        using namespace fakelib;
        const QString dir = fresh(QStringLiteral("fl-local"));
        bool allWritten = true;
        auto put = [&](const QString& rel, const QByteArray& b) {
            const QString p = QDir(dir).filePath(rel);
            allWritten = writeBytes(p, b) && allWritten;
            return p;
        };
        QCOMPARE(checkLocalFile(put(QStringLiteral("libA.sprx"), kElf)).verdict, Verdict::Ok);
        QCOMPARE(checkLocalFile(put(QStringLiteral("libB.prx"), kSelf1)).verdict, Verdict::Ok);
        QCOMPARE(checkLocalFile(put(QStringLiteral("libC.SPRX"), kSelf2)).verdict, Verdict::Ok);
        const LocalCheck odd = checkLocalFile(put(QStringLiteral("odd.sprx"), "MZ\x90\x00 not an elf"));
        QCOMPARE(odd.verdict, Verdict::Confirm);
        QVERIFY(odd.message.contains(QStringLiteral("does not have an ELF/SELF header")));
        QCOMPARE(checkLocalFile(dir).verdict, Verdict::Reject); // a folder
        QVERIFY(checkLocalFile(put(QStringLiteral("eboot.bin"), kElf)).message.contains(QStringLiteral("eboot.bin")));
        QCOMPARE(checkLocalFile(put(QStringLiteral("eboot.bin"), kElf)).verdict, Verdict::Reject);
        QCOMPARE(checkLocalFile(put(QStringLiteral("sce_sys/about/right.sprx"), kElf)).verdict, Verdict::Reject);
        QCOMPARE(checkLocalFile(put(QStringLiteral("game.exfat"), "x")).verdict, Verdict::Reject);
        QCOMPARE(checkLocalFile(put(QStringLiteral("game.ffpfsc"), "x")).verdict, Verdict::Reject);
        QCOMPARE(checkLocalFile(put(QStringLiteral("notes.txt"), "x")).verdict, Verdict::Reject);
        // ShadowMount+ deletes libkernel.sprx from fakelib folders (research §1.2-10).
        const LocalCheck kernel = checkLocalFile(put(QStringLiteral("libkernel.sprx"), kElf));
        QCOMPARE(kernel.verdict, Verdict::Reject);
        QVERIFY(kernel.message.contains(QStringLiteral("deletes libkernel")));
        QVERIFY(allWritten); // no verdict above was about a file that is not there
        QVERIFY(isListedName(QStringLiteral("libA.sprx")));
        QVERIFY(!isListedName(QStringLiteral("libA.sprx.uploading")));
        QVERIFY(!isListedName(QStringLiteral("libA.sprx.replacing.bak")));
        QVERIFY(!isListedName(QStringLiteral("readme.txt")));
    }

    void fakelibManagerOverFtp()
    {
        using namespace fakelib;
        const QString root = fresh(QStringLiteral("fl-console"));
        const QString local = fresh(QStringLiteral("fl-files"));
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());

        // A console without the folder lists nothing.
        auto empty = list(c);
        QVERIFY2(empty.ok(), empty.ok() ? "" : qPrintable(empty.error().toLogString()));
        QVERIFY(empty->overlays.empty());
        QVERIFY(!empty->folderExists);

        // Add: creates the folder; a non-ELF file needs the user's confirmation.
        const QString libA = QDir(local).filePath(QStringLiteral("libSceAmpr.sprx"));
        const QString odd = QDir(local).filePath(QStringLiteral("libOdd.prx"));
        QVERIFY(writeBytes(libA, kElf));
        QVERIFY(writeBytes(odd, "not an elf at all"));
        QVERIFY(add(c, libA, false).ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))), kElf);
        COMPARE_ERROR_CODE(add(c, odd, false), ErrorCode::InvalidArgument);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libOdd.prx"))));
        QVERIFY(add(c, odd, true).ok());
        COMPARE_ERROR_CODE(add(c, QDir(local).filePath(QStringLiteral("eboot.bin")), true), ErrorCode::InvalidArgument);

        // Listing skips folders, other files and the temporaries of a safe replace.
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/libX.sprx.uploading")), "t"));
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/libY.sprx.replacing.bak")), "t"));
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/readme.txt")), "t"));
        QVERIFY(QDir().mkpath(srv.path(QStringLiteral("/data/shadowmount/fakelib/sub.sprx"))));
        auto two = list(c);
        QVERIFY(two.ok());
        QVERIFY(two->folderExists);
        QCOMPARE(int(two->overlays.size()), 2);
        QCOMPARE(two->overlays[0].name, QStringLiteral("libOdd.prx"));
        QCOMPARE(two->overlays[1].name, QStringLiteral("libSceAmpr.sprx"));
        QCOMPARE(two->overlays[1].size, qint64(kElf.size()));
        QVERIFY(two->overlays[1].enabled);
        QCOMPARE(two->overlays[1].path(), QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"));

        // Disable: moved into fakelib.disabled (created; outside the overlay folder), listed as disabled.
        Overlay ampr = two->overlays[1];
        QVERIFY(setEnabled(c, ampr, false).ok());
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))));
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"))), kElf);
        auto after = list(c);
        QVERIFY(after.ok());
        QCOMPARE(int(after->overlays.size()), 2);
        QCOMPARE(after->enabledCount(), 1);
        ampr = after->overlays[1];
        QVERIFY(!ampr.enabled);
        QCOMPARE(ampr.path(), QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"));

        // A copy of the same name in the other folder is never overwritten.
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx")), kSelf2));
        COMPARE_ERROR_CODE(setEnabled(c, ampr, true), ErrorCode::AlreadyExists);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))), kSelf2);
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libSceAmpr.sprx"))), kElf);
        QVERIFY(QFile::remove(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))));
        QVERIFY(setEnabled(c, ampr, true).ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))), kElf);
        ampr.enabled = true;

        // Replace: safe swap, no temporaries left.
        const QString newer = QDir(local).filePath(QStringLiteral("libSceAmpr-new.sprx"));
        QVERIFY(writeBytes(newer, kSelf1));
        QVERIFY(replace(c, ampr, newer, false).ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))), kSelf1);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx.uploading"))));
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx.replacing.bak"))));

        // Remove.
        QVERIFY(remove(c, ampr).ok());
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libSceAmpr.sprx"))));
        auto last = list(c);
        QVERIFY(last.ok());
        QCOMPARE(int(last->overlays.size()), 1);

        // Overlays disabled by older versions sit in fakelib/disabled, which
        // ShadowMount+ 1.7 copies into every title's cache: listed, then moved out.
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libOld.sprx")), kElf));
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libTwin.sprx")), kElf));
        QVERIFY(writeBytes(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libTwin.sprx")), kSelf1));
        auto legacy = list(c);
        QVERIFY(legacy.ok());
        QCOMPARE(legacy->legacyCount(), 2);
        const auto old = std::find_if(legacy->overlays.begin(), legacy->overlays.end(), [](const Overlay& x) { return x.name == u"libOld.sprx"; });
        QVERIFY(old != legacy->overlays.end() && old->legacyFolder && !old->enabled);
        QCOMPARE(old->path(), QStringLiteral("/data/shadowmount/fakelib/disabled/libOld.sprx"));
        auto moved = moveLegacyDisabled(c);
        COMPARE_ERROR_CODE(moved, ErrorCode::AlreadyExists); // libTwin is in both: that one stays
        QVERIFY(moved.error().message.contains(QStringLiteral("libTwin.sprx")));
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib.disabled/libOld.sprx"))), kElf);
        QVERIFY(QFile::remove(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled/libTwin.sprx"))));
        COMPARE_VALUE(moveLegacyDisabled(c), 0);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/disabled")))); // removed once empty
        const auto tidy = list(c);
        QVERIFY(tidy.ok());
        QCOMPARE(tidy->legacyCount(), 0);
    }

    void fakelibKeepsTheOriginalWhenTheServerCannotRename()
    {
        using namespace fakelib;
        const QString root = fresh(QStringLiteral("fl-norename"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/shadowmount/fakelib/libA.sprx"), kElf));
        FakeFtpServer::Options o;
        o.root = root;
        o.renameRefused = true;
        o.extraListNames = {QStringLiteral("..\\evil.sprx")};
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto l = list(c);
        QVERIFY(l.ok());
        QCOMPARE(int(l->overlays.size()), 1); // LIST mode, unsafe name skipped
        QCOMPARE(l->skipped, QStringList{QStringLiteral("..\\evil.sprx")});
        const QString newer = QDir(fresh(QStringLiteral("fl-norename-src"))).filePath(QStringLiteral("libA.sprx"));
        QVERIFY(writeBytes(newer, kSelf1));
        const Status st = replace(c, l->overlays[0], newer, false);
        QVERIFY(!st.ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libA.sprx"))), kElf);
        QVERIFY(!QFileInfo::exists(srv.path(QStringLiteral("/data/shadowmount/fakelib/libA.sprx.uploading"))));
        QVERIFY(!setEnabled(c, l->overlays[0], false).ok());
        QCOMPARE(readAll(srv.path(QStringLiteral("/data/shadowmount/fakelib/libA.sprx"))), kElf);
    }

    // ---- recursive walk ---------------------------------------------------------------

    void remoteTreeWalkWithMlsdAndList_data()
    {
        QTest::addColumn<bool>("mlsd");
        QTest::newRow("MLSD") << true;
        QTest::newRow("LIST + MDTM") << false;
    }

    void remoteTreeWalkWithMlsdAndList()
    {
        QFETCH(bool, mlsd);
        const QString root = fresh(QStringLiteral("walk"));
        QVERIFY(writeBytes(root + QStringLiteral("/g/app0/eboot.bin"), QByteArray(100, 'e')));
        QVERIFY(writeBytes(root + QStringLiteral("/g/app0/sce_sys/param.json"), "{}"));
        QVERIFY(writeBytes(root + QStringLiteral("/g/app0/Data/A b.bin"), QByteArray(7, 'a')));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/g/app0/empty")));
        const QDateTime when(QDate(2024, 3, 5), QTime(6, 7, 8), QTimeZone::utc());
        {
            QFile f(root + QStringLiteral("/g/app0/Data/A b.bin"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.setFileTime(when, QFileDevice::FileModificationTime));
        }
        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = mlsd;
        o.extraListNames = {QStringLiteral("..\\x.bin")};
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        RemoteWalkOptions opt;
        opt.exactTimes = true;
        opt.fillMissingSizes = true;
        int calls = 0;
        auto tree = walkRemoteTree(c, QStringLiteral("/g/app0/"), opt, {}, [&](int, int) { ++calls; });
        QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
        QCOMPARE(tree->root, QStringLiteral("/g/app0"));
        QStringList paths;
        for (const RemoteFile& f : tree->files)
            paths << f.relPath;
        // Per folder in name order, files of a folder before its sub-folders' files.
        QCOMPARE(paths, (QStringList{QStringLiteral("eboot.bin"), QStringLiteral("Data/A b.bin"), QStringLiteral("sce_sys/param.json")}));
        QCOMPARE(tree->files[1].size, qint64(7));
        QVERIFY(tree->files[1].exactTime);
        QCOMPARE(tree->files[1].modified, when);
        QCOMPARE(tree->folders, 4);
        QVERIFY(tree->dirs.contains(QStringLiteral("empty")));
        QCOMPARE(tree->totalBytes, qint64(100 + 2 + 7));
        // The unsafe name appears in every listed folder and is never followed.
        QCOMPARE(tree->skippedNames.size(), 4);
        QVERIFY(tree->skippedNames.contains(QStringLiteral("..\\x.bin")));
        QCOMPARE(calls, 4);
        QCOMPARE(srv.sawCommand(QStringLiteral("MDTM")), !mlsd);
        // The root must exist.
        QVERIFY(!walkRemoteTree(c, QStringLiteral("/nope")).ok());
    }

    // ---- PS5 Manager -------------------------------------------------------------------

    void siteAvailAndFolderParsing()
    {
        using Bytes = std::optional<qint64>;
        QCOMPARE(parseSiteAvail(QStringLiteral("123456789")), Bytes(123456789));
        QCOMPARE(parseSiteAvail(QStringLiteral("Available: 2 GB")), Bytes(qint64(2) * 1024 * 1024 * 1024));
        QCOMPARE(parseSiteAvail(QStringLiteral("1.5 TiB free")), Bytes(qint64(1.5 * 1024.0 * 1024 * 1024 * 1024)));
        QCOMPARE(parseSiteAvail(QStringLiteral("512 bytes")), Bytes(512));
        QVERIFY(!parseSiteAvail(QStringLiteral("not supported")).has_value());
        QCOMPARE(splitConsoleFolders(QStringLiteral(" /data/etaHEN/games/ ; /mnt/usb0/games\n\n/data/etaHEN/games")),
                 (QStringList{QStringLiteral("/data/etaHEN/games"), QStringLiteral("/mnt/usb0/games")}));
        QVERIFY(isConsoleImageName(QStringLiteral("A.FFPFSC")));
        QVERIFY(!isConsoleImageName(QStringLiteral("a.pkg")));
    }

    void gameListMergesConsoleAndLocalImages()
    {
        const QString root = fresh(QStringLiteral("games"));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/Same.exfat"), QByteArray(10, 's')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/Diff.ffpkg"), QByteArray(10, 'd')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/Ps5Only.ffpfsc"), QByteArray(3, 'p')));
        QVERIFY(writeBytes(root + QStringLiteral("/data/etaHEN/games/readme.txt"), "t"));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/data/etaHEN/games/PPSA01234-dump")));
        QVERIFY(writeBytes(root + QStringLiteral("/mnt/usb0/games/Usb.exfat"), QByteArray(4, 'u')));
        const QString localDir = fresh(QStringLiteral("games-local"));
        QVERIFY(writeBytes(QDir(localDir).filePath(QStringLiteral("Same.exfat")), QByteArray(10, 's')));
        QVERIFY(writeBytes(QDir(localDir).filePath(QStringLiteral("Diff.ffpkg")), QByteArray(12, 'd')));
        QVERIFY(writeBytes(QDir(localDir).filePath(QStringLiteral("LocalOnly.ffpfs")), QByteArray(5, 'l')));
        QVERIFY(writeBytes(QDir(localDir).filePath(QStringLiteral("notes.txt")), "n"));
        QVERIFY(writeBytes(QDir(localDir).filePath(QStringLiteral("sub/Nested.exfat")), "n")); // not recursive (legacy)

        FakeFtpServer::Options o;
        o.root = root;
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto list = listConsoleGames(c, {QStringLiteral("/data/etaHEN/games"), QStringLiteral("/mnt/usb0/games"), QStringLiteral("/mnt/usb1/games")});
        QVERIFY2(list.ok(), list.ok() ? "" : qPrintable(list.error().toLogString()));
        QCOMPARE(int(list->games.size()), 5); // 4 images + 1 dump folder, readme.txt left out
        QCOMPARE(list->folderErrors.size(), 1);
        QVERIFY(list->folderErrors.first().startsWith(QStringLiteral("/mnt/usb1/games: ")));
        QCOMPARE(list->freeBytes, std::optional<qint64>(123456789)); // the fake's SITE AVAIL
        QCOMPARE(list->imageBytes, qint64(10 + 10 + 3 + 4));
        QVERIFY(srv.sawCommand(QStringLiteral("SITE AVAIL")));

        const auto local = scanLocalImages(localDir);
        QCOMPARE(int(local.size()), 3);
        const auto rows = mergeGames(list->games, local);
        QCOMPARE(int(rows.size()), 6);
        for (const char* n : {"Same.exfat", "Diff.ffpkg", "Ps5Only.ffpfsc", "LocalOnly.ffpfs", "Usb.exfat", "PPSA01234-dump"})
            QVERIFY2(std::any_of(rows.begin(), rows.end(), [&](const GameRow& r) { return r.name == QLatin1String(n); }), n);
        auto find = [&](const QString& n) { return *std::find_if(rows.begin(), rows.end(), [&](const GameRow& r) { return r.name == n; }); };
        QCOMPARE(find(QStringLiteral("Same.exfat")).status, GameStatus::OnPs5);
        QCOMPARE(find(QStringLiteral("Diff.ffpkg")).status, GameStatus::SizeMismatch);
        QCOMPARE(find(QStringLiteral("Diff.ffpkg")).localSize, qint64(12));
        QCOMPARE(find(QStringLiteral("Diff.ffpkg")).remoteSize, qint64(10));
        QCOMPARE(find(QStringLiteral("Ps5Only.ffpfsc")).status, GameStatus::Ps5Only);
        QCOMPARE(find(QStringLiteral("LocalOnly.ffpfs")).status, GameStatus::LocalOnly);
        QCOMPARE(find(QStringLiteral("Usb.exfat")).remotePath, QStringLiteral("/mnt/usb0/games/Usb.exfat"));
        QVERIFY(find(QStringLiteral("PPSA01234-dump")).isDir);
        QCOMPARE(find(QStringLiteral("PPSA01234-dump")).status, GameStatus::Ps5Only);
        QCOMPARE(gameStatusLabel(GameStatus::SizeMismatch), QStringLiteral("Size mismatch"));
    }

    void folderCompareFindsMissingExtraAndDifferentFiles()
    {
        const QString root = fresh(QStringLiteral("cmp"));
        const QString remote = root + QStringLiteral("/data/etaHEN/games/GAME");
        const QString local = fresh(QStringLiteral("cmp-local"));
        for (const QString& base : {remote, local}) {
            QVERIFY(writeBytes(base + QStringLiteral("/eboot.bin"), QByteArray(64, 'e')));
            QVERIFY(writeBytes(base + QStringLiteral("/sce_sys/param.json"), "{}"));
        }
        QVERIFY(writeBytes(local + QStringLiteral("/Data/missing.bin"), QByteArray(9, 'm')));
        QVERIFY(writeBytes(local + QStringLiteral("/Data/size.bin"), QByteArray(20, 's')));
        QVERIFY(writeBytes(remote + QStringLiteral("/Data/size.bin"), QByteArray(19, 's')));
        QVERIFY(writeBytes(remote + QStringLiteral("/extra.log"), "x"));

        FakeFtpServer::Options o;
        o.root = root;
        o.mlsd = true;
        FakeFtpServer srv(o);
        FtpClient c(quick());
        QVERIFY(c.open(QStringLiteral("127.0.0.1"), srv.port()).ok());
        auto files = localFolderFiles(local);
        QVERIFY(files.ok());
        QCOMPARE(int(files->size()), 4);
        auto rep = compareConsoleFolder(c, QStringLiteral("/data/etaHEN/games/GAME"), files.value());
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QCOMPARE(rep->localFiles, 4);
        QCOMPARE(rep->remoteFiles, 4);
        QCOMPARE(rep->matching, 2);
        QCOMPARE(rep->count(DiffKind::MissingOnConsole), 1);
        QCOMPARE(rep->count(DiffKind::OnlyOnConsole), 1);
        QCOMPARE(rep->count(DiffKind::SizeDiffers), 1);
        QCOMPARE(int(rep->diffs.size()), 3);
        QCOMPARE(rep->diffs[0].path, QStringLiteral("Data/missing.bin"));
        QCOMPARE(rep->diffs[1].path, QStringLiteral("Data/size.bin"));
        QCOMPARE(rep->diffs[1].localSize, qint64(20));
        QCOMPARE(rep->diffs[1].remoteSize, qint64(19));
        QCOMPARE(rep->diffs[2].kind, DiffKind::OnlyOnConsole);
        QVERIFY(!rep->identical());
        QVERIFY(rep->summary().contains(QStringLiteral("1 missing on the PS5")));

        // Make them equal: identical report.
        QVERIFY(writeBytes(remote + QStringLiteral("/Data/missing.bin"), QByteArray(9, 'm')));
        QVERIFY(writeBytes(remote + QStringLiteral("/Data/size.bin"), QByteArray(20, 's')));
        QVERIFY(QFile::remove(remote + QStringLiteral("/extra.log")));
        auto same = compareConsoleFolder(c, QStringLiteral("/data/etaHEN/games/GAME"), files.value());
        QVERIFY(same.ok());
        QVERIFY(same->identical());
        QVERIFY(same->summary().startsWith(QStringLiteral("All 4 file(s)")));

        // Pure compare: a size the console did not report is not a difference.
        const auto r = compareFiles({{QStringLiteral("a"), 1}, {QStringLiteral("b"), 2}}, {{QStringLiteral("a"), -1}, {QStringLiteral("b"), 2}});
        QCOMPARE(r.sizeUnknown, 1);
        QCOMPARE(r.matching, 1);
        QVERIFY(r.diffs.empty());
        QVERIFY(!r.identical());
    }
};

QTEST_GUILESS_MAIN(TstPs5Content)
#include "tst_ps5_content.moc"
