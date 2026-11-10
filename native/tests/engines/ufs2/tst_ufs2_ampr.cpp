// ufs2.amprIndex: a game with fakelib/libSceAmpr.sprx gets ampr_emu.index in
// its .ffpkg (added when the dump has none, rebuilt when the dump's no longer
// matches), the dump folder is never written, and a build that does not touch
// the index writes the same bytes as before.

#include "Ufs2TestUtil.h"

#include "core/Log.h"
#include "core/Settings.h"
#include "engines/general/Module.h"
#include "engines/pfs/AmprIndex.h"
#include "engines/ufs2/Module.h"
#include "engines/ufs2/Ufs2Ampr.h"

#include <QTemporaryDir>

#include <mutex>

using namespace ufs2test;

class TestUfs2Ampr : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appData;
    QTemporaryDir m_tmp;
    std::shared_ptr<SettingsSchema> m_schema = std::make_shared<SettingsSchema>();
    DependencyRegistry m_deps;
    JobFactory m_jobs;
    std::unique_ptr<SettingsStore> m_store;
    std::shared_ptr<CallbackSink> m_sink;
    std::mutex m_logMutex;
    QStringList m_amprLog; // the "ampr_emu.index: ..." lines of the job that ran last
    int m_run = 0;

    Result<JobOutcome> runJob(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {})
    {
        FW_TRY_ASSIGN(auto job, m_jobs.create(kind, params, overrides));
        FW_TRY_ASSIGN(QVariantMap normalized, m_store->normalizeOverrides(overrides));
        const SettingsValues sv = m_store->snapshot(normalized);
        FW_TRY(job->validate(sv));
        ProgressReporter rep([](const ProgressSnapshot&) {}, 0);
        rep.setPhases(job->phases());
        TempDir temp(QStringLiteral("ufs2-ampr-test"), m_tmp.path());
        JobContext ctx;
        ctx.jobId = QStringLiteral("test");
        ctx.progress = &rep;
        ctx.log = Log(kind, QStringLiteral("test"));
        ctx.settings = sv;
        ctx.tempDir = temp.path();
        return job->run(ctx);
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
        const QString src = m_tmp.filePath(name);
        bool ok = makeGameTree(src, 300 * 1024);
        if (amprLibrary)
            ok &= mkfile(src + QStringLiteral("/fakelib/libSceAmpr.sprx"), 4000, 77);
        if (!ok)
            QTest::qFail("the game folder could not be created", __FILE__, __LINE__);
        return src;
    }

    struct Built {
        bool ok = false;
        QString image;
        QMap<QString, QByteArray> files; // the image's content, as snapshotFolder()
        QByteArray index;                // the image's ampr_emu.index ("" when it has none)
        bool hasIndex = false;
        QStringList log;
        JobOutcome outcome;
    };

    // Builds `src` with ufs2.amprIndex = `mode`, checks that the dump folder
    // is byte for byte what it was, and reads the image back.
    Built build(const QString& src, const QString& mode, const QVariantMap& more = {})
    {
        Built b;
        const QString tag = QStringLiteral("run%1").arg(++m_run);
        b.image = m_tmp.filePath(tag + QStringLiteral(".ffpkg"));
        const QMap<QString, QByteArray> before = snapshotFolder(src);
        {
            std::lock_guard lock(m_logMutex);
            m_amprLog.clear();
        }
        QVariantMap o = more;
        if (!mode.isEmpty())
            o.insert(ufs2::keys::kAmprIndex, mode);
        auto r = runJob(ufs2::kinds::kBuild, QJsonObject{{"source", src}, {"output", b.image}}, o);
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
        if (snapshotFolder(src) != before) {
            qWarning("the dump folder was changed by the build");
            return b;
        }
        const QString dest = m_tmp.filePath(tag + QStringLiteral("-out"));
        auto x = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", b.image}, {"output", dest}});
        if (!x.ok()) {
            qWarning("extract failed: %s", qPrintable(x.error().toLogString()));
            return b;
        }
        b.files = snapshotFolder(dest);
        b.hasIndex = b.files.contains(QStringLiteral("ampr_emu.index"));
        b.index = b.files.value(QStringLiteral("ampr_emu.index"));
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

    bool verifies(const QString& image, const QString& src)
    {
        auto v = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", image}, {"source", src}});
        if (v.ok() && v->verification != Verification::Passed)
            qWarning("verification: %s", qPrintable(v->verificationDetail));
        return v.ok() && v->verification == Verification::Passed;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid() && m_tmp.isValid());
        fwtest::isolateAppData(m_appData);
        ModuleContext ctx{*m_schema, m_deps, m_jobs};
        general::registerModule(ctx);
        ufs2::registerModule(ctx);
        m_store = std::make_unique<SettingsStore>(m_schema, m_appData.filePath("settings.json"));
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
        const SettingDef* d = m_schema->find(ufs2::keys::kAmprIndex);
        QVERIFY(d);
        QCOMPARE(d->defaultValue.toString(), QStringLiteral("auto"));
        QVERIFY(d->perJob);
        QStringList values;
        for (const auto& c : d->choices)
            values << c.value;
        QCOMPARE(values, (QStringList{QStringLiteral("auto"), QStringLiteral("missing"), QStringLiteral("refresh"), QStringLiteral("off")}));
        QVERIFY(d->description.contains(QStringLiteral("never changed")));
        QVERIFY(!m_store->setValue(ufs2::keys::kAmprIndex, QStringLiteral("sometimes")).ok());
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
            QVERIFY(sameApartFromIndex(b.files, snapshotFolder(src)));
            QCOMPARE(b.outcome.details.value(QStringLiteral("amprIndex")).toString(), QStringLiteral("added"));
            QVERIFY(!QFileInfo::exists(src + QStringLiteral("/ampr_emu.index")));
            // The image verifies against the folder it was built from, which has no index.
            QVERIFY(verifies(b.image, src));
        }

        const Built off = build(src, QStringLiteral("off"));
        QVERIFY(off.ok);
        QVERIFY(!off.hasIndex);
        QVERIFY(off.log.isEmpty());
        QCOMPARE(off.files, snapshotFolder(src));
        QVERIFY(!off.outcome.details.contains(QStringLiteral("amprIndex")));
    }

    // (b) an index that no longer matches the game's files is rebuilt; (c) one that matches is kept.
    void outdatedIndexIsRebuiltAndACurrentOneKept()
    {
        const QString src = makeGame(QStringLiteral("outdated"), true);
        const QString own = src + QStringLiteral("/ampr_emu.index");
        // The dump's index was made before a file was added and another one grew.
        const QByteArray stale = toolIndex(src);
        QVERIFY(mktext(own, stale));
        QVERIFY(mkfile(src + QStringLiteral("/data/added-later.bin"), 9000, 5));
        QVERIFY(mkfile(src + QStringLiteral("/sce_sys/icon0.png"), 90000, 6));
        const QByteArray fresh = toolIndex(src);
        QVERIFY(fresh != stale);

        Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: rebuilt (it did not match the game's files)")});
        QVERIFY(sameApartFromIndex(b.files, snapshotFolder(src)));
        QCOMPARE(b.outcome.details.value(QStringLiteral("amprIndex")).toString(), QStringLiteral("rebuilt"));
        QCOMPARE(readAll(own), stale); // the dump keeps its own
        QVERIFY(verifies(b.image, src));

        // Same size, other bytes: an index that only looks like one.
        const QByteArray garbage(fresh.size(), 'x');
        QVERIFY(mktext(own, garbage));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(readAll(own), garbage);

        // "Add when the dump has none" and "off" copy the dump's index as it is.
        QVERIFY(mktext(own, stale));
        b = build(src, QStringLiteral("missing"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, stale);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (the dump has one)")});
        QCOMPARE(b.files, snapshotFolder(src));
        b = build(src, QStringLiteral("off"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, stale);
        QVERIFY(b.log.isEmpty());

        // (c) up to date: kept byte for byte.
        QVERIFY(mktext(own, fresh));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (up to date)")});
        QCOMPARE(b.files, snapshotFolder(src));
        QVERIFY(!b.outcome.details.contains(QStringLiteral("amprIndex")));

        // A dump that was copied has other file times: still up to date, and
        // the dump's own bytes go into the image.
        const QByteArray otherTimes = toolIndex(src, 3600);
        QVERIFY(otherTimes != fresh);
        QVERIFY(pfs::amprIndexesDifferOnlyInTimes(otherTimes, fresh));
        QVERIFY(mktext(own, otherTimes));
        b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, otherTimes);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (up to date)")});
        // "Always rebuild it" replaces even that one.
        b = build(src, QStringLiteral("refresh"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, fresh);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: rebuilt (set to always rebuild it)")});
        QCOMPARE(readAll(own), otherTimes);
    }

    // A packed game's index lists the files inside its packs: "auto" leaves it alone.
    void packedGameKeepsItsIndex()
    {
        const QString src = makeGame(QStringLiteral("packed"), true);
        QVERIFY(mktext(src + QStringLiteral("/ampr_emu.index"), "the packer's index"));
        QVERIFY(mktext(src + QStringLiteral("/ampr_zstd.index"), "AMPRZPK1"));
        const Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QCOMPARE(b.index, QByteArray("the packer's index"));
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: kept (the game's asset packs need the index they were made with)")});
        QCOMPARE(b.files, snapshotFolder(src));
    }

    // (d) a game without the AMPR library: the default writes the bytes "off" writes.
    void gameWithoutTheLibraryBuildsAsBefore()
    {
        const QString src = makeGame(QStringLiteral("plain"), false);
        const Built b = build(src, QStringLiteral("auto"));
        QVERIFY(b.ok);
        QVERIFY(!b.hasIndex);
        QCOMPARE(b.log, QStringList{QStringLiteral("ampr_emu.index: not needed (no fakelib/libSceAmpr.sprx)")});
        QCOMPARE(b.files, snapshotFolder(src));

        // The same tree, so the same image: compared with fixed times and file-system id.
        WriterOptions w;
        w.formatTime = 1700000000;
        w.populateTime = 1700000100;
        w.fsId1 = 0x1234;
        const CancellationToken token;
        QByteArray images[2];
        int at = 0;
        for (const pfs::AmprMode mode : {pfs::AmprMode::Off, pfs::AmprMode::Auto}) {
            auto tree = scanLocalFolder(src, token);
            QVERIFY(tree.ok());
            const qsizetype nodes = qsizetype(tree->nodes.size());
            auto placed = applyAmprIndex(*tree, mode, m_tmp.filePath(QStringLiteral("plain-temp")));
            QVERIFY(placed.ok());
            QCOMPARE(placed->action, mode == pfs::AmprMode::Off ? pfs::AmprIndexAction::Off : pfs::AmprIndexAction::NotNeeded);
            QVERIFY(placed->indexFile.isEmpty());
            QCOMPARE(qsizetype(tree->nodes.size()), nodes);
            Ufs2Writer writer(*tree, w);
            QVERIFY(writer.plan().ok());
            const QString image = m_tmp.filePath(QStringLiteral("plain-%1.ffpkg").arg(at));
            QVERIFY(writer.write(image, token, nullptr).ok());
            images[at++] = readAll(image);
        }
        QVERIFY(!images[0].isEmpty());
        QVERIFY(images[0] == images[1]);
    }

    // The new entry sits where a listing of a folder that has the file puts
    // it, so the image is the one built from such a folder.
    void addedIndexGivesTheImageOfAFolderThatHasIt()
    {
        const QString src = makeGame(QStringLiteral("placed"), true);
        const QByteArray fresh = toolIndex(src);
        WriterOptions w;
        w.formatTime = 1700000000;
        w.populateTime = 1700000100;
        w.fsId1 = 0x1234;
        const CancellationToken token;

        auto injected = scanLocalFolder(src, token);
        QVERIFY(injected.ok());
        auto placed = applyAmprIndex(*injected, pfs::AmprMode::Auto, m_tmp.filePath(QStringLiteral("placed-temp")));
        QVERIFY(placed.ok());
        QCOMPARE(placed->action, pfs::AmprIndexAction::Added);
        QCOMPARE(readAll(placed->indexFile), fresh);
        QVERIFY(!placed->indexFile.startsWith(src + u'/')); // not in the dump
        QVERIFY(!QFileInfo::exists(src + QStringLiteral("/ampr_emu.index")));
        // A second decision on the same tree finds the index it placed.
        QCOMPARE(decideAmprIndex(*injected, pfs::AmprMode::Auto).action, pfs::AmprIndexAction::KeptUpToDate);
        const QString a = m_tmp.filePath(QStringLiteral("placed-a.ffpkg"));
        {
            Ufs2Writer writer(*injected, w);
            QVERIFY(writer.plan().ok());
            QVERIFY(writer.write(a, token, nullptr).ok());
        }

#ifdef Q_OS_WIN // NTFS lists a folder in the order the new entry is placed by
        const QString copy = m_tmp.filePath(QStringLiteral("placed-copy"));
        QVERIFY(QDir().mkpath(copy));
        const QMap<QString, QByteArray> files = snapshotFolder(src);
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            if (it.key().endsWith(u'/'))
                QVERIFY(QDir().mkpath(copy + u'/' + it.key()));
            else
                QVERIFY(mktext(copy + u'/' + it.key(), it.value()));
        }
        QVERIFY(mktext(copy + QStringLiteral("/ampr_emu.index"), fresh));
        auto real = scanLocalFolder(copy, token);
        QVERIFY(real.ok());
        const QString b = m_tmp.filePath(QStringLiteral("placed-b.ffpkg"));
        Ufs2Writer writer(*real, w);
        QVERIFY(writer.plan().ok());
        QVERIFY(writer.write(b, token, nullptr).ok());
        const QByteArray one = readAll(a), two = readAll(b);
        QVERIFY(!one.isEmpty());
        QCOMPARE(firstDifference(one, two), qint64(-1));
#endif
    }

    void sourceIsDeletedAfterAVerifiedBuildWithANewIndex()
    {
        const QString src = makeGame(QStringLiteral("delete"), true);
        const QString image = m_tmp.filePath(QStringLiteral("delete.ffpkg"));
        auto r = runJob(ufs2::kinds::kBuild, QJsonObject{{"source", src}, {"output", image}},
                        {{ufs2::keys::kDeleteSourceAfterBuild, true}});
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        QCOMPARE(r->verification, Verification::Passed);
        QVERIFY2(!QFileInfo::exists(src), qPrintable(r->warnings.join(u' ')));
    }
};

QTEST_GUILESS_MAIN(TestUfs2Ampr)
#include "tst_ufs2_ampr.moc"
