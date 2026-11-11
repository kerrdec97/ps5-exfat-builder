// exfat.amprIndex: a game with fakelib/libSceAmpr.sprx gets ampr_emu.index in
// its image (added when the dump has none, rebuilt when the dump's no longer
// matches), the dump folder is never written, and a build that does not touch
// the index writes the same bytes as before.

#include "ExfatJobEnv.h"
#include "ExfatTestUtil.h"

#include "core/Log.h"
#include "engines/exfat/ExfatAmpr.h"
#include "engines/exfat/ExfatJobs.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/pfs/AmprIndex.h"
#include "support/TestSupport.h"

#include <QDirIterator>
#include <QTemporaryDir>
#include <QTest>

#include <mutex>

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

class TestExfatAmpr : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    JobEnv m_env;
    std::shared_ptr<CallbackSink> m_sink;
    std::mutex m_logMutex;
    QStringList m_amprLog; // the "ampr_emu.index: ..." lines of the job that ran last
    int m_run = 0;

    QString path(const QString& rel) const { return m_tmp.filePath(rel); }

    // Every file and folder under `root`: relative path -> SHA-256 of its content.
    static QMap<QString, QByteArray> hashes(const QString& root)
    {
        QMap<QString, QByteArray> out;
        const QDir base(root);
        QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo fi = it.nextFileInfo();
            const QString rel = base.relativeFilePath(fi.absoluteFilePath());
            out.insert(fi.isDir() ? rel + u'/' : rel, fi.isDir() ? QByteArray() : fileHash(fi.absoluteFilePath()));
        }
        return out;
    }

    // What the stand-alone index tool (ampr.index.local) builds for the folder as it is.
    static QByteArray toolIndex(const QString& folder, qint64 shiftTimes = 0)
    {
        auto scan = pfs::scanAmprFolder(folder);
        if (!scan.ok())
            return {};
        pfs::AmprToolRows rows = pfs::amprToolRows(scan->files);
        for (pfs::AmprRow& r : rows.rows)
            r.mtime += shiftTimes;
        return pfs::buildAmprIndex(rows.rows);
    }

    QString makeGame(const QString& name, bool amprLibrary)
    {
        const QString src = path(name);
        bool ok = makeSampleTree(src, 3);
        if (amprLibrary)
            ok &= writeRandom(src + QStringLiteral("/fakelib/libSceAmpr.sprx"), 4000, 77);
        if (!ok)
            QTest::qFail("the game folder could not be created", __FILE__, __LINE__);
        return src;
    }

    struct Built {
        bool ok = false;
        QString image;
        QMap<QString, QByteArray> files; // the image's content, as hashes()
        QByteArray index;                // the image's ampr_emu.index ("" when it has none)
        bool hasIndex = false;
        QStringList log;
        JobOutcome outcome;
    };

    // Builds `src` with exfat.amprIndex = `mode`, checks that the dump folder
    // is byte for byte what it was, and reads the image back.
    Built build(const QString& src, const QString& mode, const QVariantMap& more = {})
    {
        Built b;
        const QString tag = QStringLiteral("run%1").arg(++m_run);
        b.image = path(tag + QStringLiteral(".exfat"));
        const QMap<QString, QByteArray> before = hashes(src);
        {
            std::lock_guard lock(m_logMutex);
            m_amprLog.clear();
        }
        QVariantMap o = more;
        if (!mode.isEmpty())
            o.insert(keys::kAmprIndex, mode);
        auto r = m_env.run(kJobBuild, {{QStringLiteral("source"), src}, {QStringLiteral("output"), b.image}}, o);
        {
            std::lock_guard lock(m_logMutex);
            b.log = m_amprLog;
        }
        if (!r.ok()) {
            qWarning("build failed: %s", qPrintable(r.error().toLogString()));
            return b;
        }
        b.outcome = r.value();
        if (b.outcome.verification != Verification::Passed) {
            qWarning("verification: %s", qPrintable(b.outcome.verificationDetail));
            return b;
        }
        if (hashes(src) != before) {
            qWarning("the dump folder was changed by the build");
            return b;
        }
        const QString dest = path(tag + QStringLiteral("-out"));
        auto x = m_env.run(kJobExtract, {{QStringLiteral("image"), b.image}, {QStringLiteral("output"), dest}});
        if (!x.ok()) {
            qWarning("extract failed: %s", qPrintable(x.error().toLogString()));
            return b;
        }
        b.files = hashes(dest);
        b.hasIndex = QFileInfo::exists(dest + QStringLiteral("/ampr_emu.index"));
        b.index = b.hasIndex ? readAllOf(dest + QStringLiteral("/ampr_emu.index")) : QByteArray();
        b.ok = true;
        return b;
    }

    // The image holds the dump's files, apart from ampr_emu.index.
    static bool sameApartFromIndex(QMap<QString, QByteArray> image, QMap<QString, QByteArray> dump)
    {
        image.remove(QStringLiteral("ampr_emu.index"));
        dump.remove(QStringLiteral("ampr_emu.index"));
        return image == dump;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        m_sink = std::make_shared<CallbackSink>([this](const LogRecord& r) {
            if (!r.message.startsWith(QStringLiteral("ampr_emu.index")))
                return;
            std::lock_guard lock(m_logMutex);
            m_amprLog << r.message;
        });
        Logger::instance().addSink(m_sink);
    }

    void cleanupTestCase() { Logger::instance().removeSink(m_sink); }

    void settingIsDeclared()
    {
        const SettingDef* d = m_env.schema->find(keys::kAmprIndex);
        QVERIFY(d);
        QCOMPARE(d->defaultValue.toString(), QStringLiteral("auto"));
        QVERIFY(d->perJob);
        QStringList values;
        for (const auto& c : d->choices)
            values << c.value;
        QCOMPARE(values, (QStringList{QStringLiteral("auto"), QStringLiteral("missing"), QStringLiteral("refresh"), QStringLiteral("off")}));
        QVERIFY(d->description.contains(QStringLiteral("never changed")));
        QCOMPARE(readSettings(m_env.settings({})).ampr, pfs::AmprMode::Auto);
        QCOMPARE(readSettings(m_env.settings({{keys::kAmprIndex, QStringLiteral("missing")}})).ampr, pfs::AmprMode::Missing);
        QCOMPARE(readSettings(m_env.settings({{keys::kAmprIndex, QStringLiteral("refresh")}})).ampr, pfs::AmprMode::Refresh);
        QCOMPARE(readSettings(m_env.settings({{keys::kAmprIndex, QStringLiteral("off")}})).ampr, pfs::AmprMode::Off);
        QVERIFY(!m_env.schema->normalize(keys::kAmprIndex, QStringLiteral("sometimes")).ok());
    }

    // (a) no index in the dump: the image gets a correct one; (e) "off" adds none.
    void missingIndexIsAddedToTheImageOnly()
    {
        const QString src = makeGame(QStringLiteral("missing"), true);
        const QByteArray fresh = toolIndex(src);
        QVERIFY(fresh.startsWith("AMPRIDX3"));

        for (const QString& mode : {QString(), QStringLiteral("auto"), QStringLiteral("missing"), QStringLiteral("refresh")}) {
            const Built b = build(src, mode); // "" = the default, which is auto
            QVERIFY2(b.ok, qPrintable(mode));
            QVERIFY2(b.hasIndex, qPrintable(mode));
            QCOMPARE(b.index, fresh);
            QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: added (the dump has none)")});
            QVERIFY(sameApartFromIndex(b.files, hashes(src)));
            QCOMPARE(b.outcome.details.value(QStringLiteral("amprIndex")).toString(), QStringLiteral("added"));
            QVERIFY(!QFileInfo::exists(src + QStringLiteral("/ampr_emu.index")));
            // The image verifies against the folder it was built from, which has no index.
            auto v = m_env.run(kJobVerify, {{QStringLiteral("image"), b.image}, {QStringLiteral("source"), src}});
            QVERIFY(v.ok());
            QVERIFY2(v->verification == Verification::Passed, qPrintable(v->verificationDetail));
        }

        const Built off = build(src, QStringLiteral("off"));
        QVERIFY(off.ok);
        QVERIFY(!off.hasIndex);
        QVERIFY(off.log.isEmpty());
        QCOMPARE(off.files, hashes(src));
        QVERIFY(!off.outcome.details.contains(QStringLiteral("amprIndex")));
    }

    // (b) an index that no longer matches the game's files is rebuilt; (c) one that matches is kept.
    void outdatedIndexIsRebuiltAndACurrentOneKept()
    {
        const QString src = makeGame(QStringLiteral("outdated"), true);
        const QString own = src + QStringLiteral("/ampr_emu.index");
        // The dump's index was made before a file was added and another one grew.
        const QByteArray stale = toolIndex(src);
        QVERIFY(writeBytes(own, stale));
        QVERIFY(writeRandom(src + QStringLiteral("/data/added-later.bin"), 9000, 5));
        QVERIFY(writeRandom(src + QStringLiteral("/data/medium.bin"), 800 * 1024, 6));
        const QByteArray fresh = toolIndex(src);
        QVERIFY(fresh != stale);

        Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: rebuilt (it did not match the game's files)")});
        QVERIFY(sameApartFromIndex(b.files, hashes(src)));
        QCOMPARE(b.outcome.details.value(QStringLiteral("amprIndex")).toString(), QStringLiteral("rebuilt"));
        QCOMPARE(readAllOf(own), stale); // the dump keeps its own
        auto v = m_env.run(kJobVerify, {{QStringLiteral("image"), b.image}, {QStringLiteral("source"), src}});
        QVERIFY(v.ok());
        QVERIFY2(v->verification == Verification::Passed, qPrintable(v->verificationDetail));

        // Same size, other bytes: an index that only looks like one.
        QByteArray garbage = fresh;
        garbage.fill('x', garbage.size());
        QVERIFY(writeBytes(own, garbage));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(readAllOf(own), garbage);

        // "Add when the dump has none" and "off" copy the dump's index as it is.
        QVERIFY(writeBytes(own, stale));
        b = build(src, QStringLiteral("missing"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, stale);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (the dump has one)")});
        QCOMPARE(b.files, hashes(src));
        b = build(src, QStringLiteral("off"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, stale);
        QVERIFY(b.log.isEmpty());

        // (c) up to date: kept byte for byte.
        QVERIFY(writeBytes(own, fresh));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (up to date)")});
        QCOMPARE(b.files, hashes(src));
        QVERIFY(!b.outcome.details.contains(QStringLiteral("amprIndex")));

        // A dump that was copied has other file times: still up to date, and
        // the dump's own bytes go into the image.
        const QByteArray otherTimes = toolIndex(src, 3600);
        QVERIFY(otherTimes != fresh);
        QVERIFY(pfs::amprIndexesDifferOnlyInTimes(otherTimes, fresh));
        QVERIFY(writeBytes(own, otherTimes));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, otherTimes);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (up to date)")});
        // "Always rebuild it" replaces even that one.
        b = build(src, QStringLiteral("refresh"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: rebuilt (set to always rebuild it)")});
        QCOMPARE(readAllOf(own), otherTimes);
    }

    // A packed game's index lists the files inside its packs: "auto" leaves it alone.
    void packedGameKeepsItsIndex()
    {
        const QString src = makeGame(QStringLiteral("packed"), true);
        const QString own = src + QStringLiteral("/ampr_emu.index");
        QVERIFY(writeBytes(own, "the packer's index"));
        QVERIFY(writeBytes(src + QStringLiteral("/ampr_zstd.index"), "AMPRZPK1"));
        Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, QByteArray("the packer's index"));
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (the game's asset packs need the index they were made with)")});
        QCOMPARE(b.files, hashes(src));
        QVERIFY(QFile::remove(own));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QVERIFY(!b.hasIndex);
    }

    // (d) a game without the AMPR library: the default writes the bytes "off" writes.
    void gameWithoutTheLibraryBuildsAsBefore()
    {
        const QString src = makeGame(QStringLiteral("plain"), false);
        const Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QVERIFY(!b.hasIndex);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: not needed (no fakelib/libSceAmpr.sprx)")});
        QCOMPARE(b.files, hashes(src));

        // The same plan, so the same image: compared with a fixed serial number.
        ScanRules rules;
        rules.excludePatterns = defaultExcludePatterns();
        WriterOptions w;
        w.spare = SpareMode::Fixed;
        w.spareBytes = 4 * kMiB;
        w.label = QStringLiteral("OSFIMG");
        w.serial = 0x1234ABCD;
        w.syncToDisk = false;
        const CancellationToken token;
        QByteArray images[2];
        int at = 0;
        for (const pfs::AmprMode mode : {pfs::AmprMode::Off, pfs::AmprMode::Auto}) {
            auto tree = scanSource(src, rules, token);
            QVERIFY(tree.ok());
            const qsizetype nodes = qsizetype(tree->nodes.size());
            auto placed = applyAmprIndex(*tree, mode, path(QStringLiteral("plain-temp")));
            QVERIFY(placed.ok());
            QCOMPARE(placed->action, mode == pfs::AmprMode::Off ? pfs::AmprIndexAction::Off : pfs::AmprIndexAction::NotNeeded);
            QVERIFY(placed->indexFile.isEmpty());
            QCOMPARE(qsizetype(tree->nodes.size()), nodes);
            auto plan = planImage(*tree, w);
            QVERIFY(plan.ok());
            const QString image = path(QStringLiteral("plain-%1.exfat").arg(at));
            QVERIFY(writeImage(*tree, *plan, image, w, token).ok());
            images[at++] = readAllOf(image);
        }
        QVERIFY(!images[0].isEmpty());
        QVERIFY(images[0] == images[1]);
    }

    // The new entry sits where a scan of a folder that has the file puts it,
    // so the image is the one built from such a folder.
    void addedIndexIsPlacedLikeAFileOfTheFolder()
    {
        const QString src = makeGame(QStringLiteral("placed"), true);
        const QByteArray fresh = toolIndex(src);
        const CancellationToken token;
        auto injected = scanSource(src, {}, token);
        QVERIFY(injected.ok());
        auto placed = applyAmprIndex(*injected, pfs::AmprMode::Auto, path(QStringLiteral("placed-temp")));
        QVERIFY(placed.ok());
        QCOMPARE(placed->action, pfs::AmprIndexAction::Added);
        QCOMPARE(readAllOf(placed->indexFile), fresh);
        QVERIFY(!placed->indexFile.startsWith(src + u'/')); // not in the dump

        const QString copy = path(QStringLiteral("placed-copy"));
        QVERIFY(copyTree(src, copy));
        QVERIFY(writeBytes(copy + QStringLiteral("/ampr_emu.index"), fresh));
        auto real = scanSource(copy, {}, token);
        QVERIFY(real.ok());
        QCOMPARE(injected->fileCount, real->fileCount);
        QCOMPARE(injected->totalBytes, real->totalBytes);
        const auto rootNames = [](const SourceTree& t) {
            QStringList names;
            for (int c : t.nodes.front().children)
                names << t.nodes[size_t(c)].name;
            return names;
        };
        QCOMPARE(rootNames(*injected), rootNames(*real));
        // A second decision on the same tree finds the index it placed.
        QCOMPARE(decideAmprIndex(*injected, pfs::AmprMode::Auto).action, pfs::AmprIndexAction::KeptUpToDate);
    }

    void sourceIsDeletedAfterAVerifiedBuildWithANewIndex()
    {
        const QString src = makeGame(QStringLiteral("delete"), true);
        const QString image = path(QStringLiteral("delete.exfat"));
        auto r = m_env.run(kJobBuild, {{QStringLiteral("source"), src}, {QStringLiteral("output"), image}},
                           {{keys::kDeleteSourceAfterVerify, true}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(r->verification, Verification::Passed);
        QVERIFY(!QFileInfo::exists(src));
    }
};

QTEST_GUILESS_MAIN(TestExfatAmpr)
#include "tst_exfat_ampr.moc"
