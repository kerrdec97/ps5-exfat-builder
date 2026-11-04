// exFAT queue jobs: build (phases, verification, settings, cancellation,
// free space, replacing an image, deleting the source), verify, extract,
// and the settings schema with its legacy key table.

#include "ExfatJobEnv.h"
#include "ExfatTestUtil.h"

#include "core/Paths.h"
#include "engines/exfat/ExfatJobs.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

#include <mutex>

using namespace fw;
using namespace fw::exfat;
using namespace exfattest;

class TestExfatJobs : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    JobEnv m_env;
    QString path(const QString& rel) const { return m_tmp.filePath(rel); }
    static QString err(const Error& e) { return e.toLogString(); }

    QString makeGame(const QString& name, int smallFiles = 50)
    {
        const QString src = path(name);
        makeSampleTree(src, smallFiles);
        return src;
    }

    static QJsonObject buildParams(const QString& src, const QString& out)
    {
        return {{QStringLiteral("source"), src}, {QStringLiteral("output"), out}};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void jobKindsAreRegistered()
    {
        for (const QString& k : {kJobBuild, kJobVerify, kJobExtract, kJobEdit})
            QVERIFY2(m_env.jobs.contains(k), qPrintable(k));
    }

    void everySettingIsWellFormedAndRead()
    {
        int count = 0;
        for (const SettingDef* d : m_env.schema->all()) {
            if (!d->key.startsWith(QStringLiteral("exfat.")))
                continue;
            ++count;
            QVERIFY2(!d->label.isEmpty() && !d->section.isEmpty(), qPrintable(d->key));
            QVERIFY2(m_env.schema->normalize(d->key, d->defaultValue).ok(), qPrintable(d->key));
        }
        QCOMPARE(count, 18);

        // Every option changes what the jobs read.
        QVariantMap o;
        o.insert(keys::kClusterSize, QStringLiteral("131072"));
        o.insert(keys::kSectorSize, QStringLiteral("4096"));
        o.insert(keys::kImageSizeGiB, 2.5);
        o.insert(keys::kSpareSpace, QStringLiteral("fixed"));
        o.insert(keys::kSpareSpaceMiB, 77);
        o.insert(keys::kLabelPolicy, QStringLiteral("titleId"));
        o.insert(keys::kLabel, QStringLiteral("MYLABEL"));
        o.insert(keys::kRequireEboot, false);
        o.insert(keys::kExcludePatterns, QStringList{QStringLiteral("*.tmp")});
        o.insert(keys::kExcludeHidden, true);
        o.insert(keys::kOverwriteExisting, false);
        o.insert(keys::kAllocation, QStringLiteral("fatChain"));
        o.insert(keys::kOpenRetries, 7);
        o.insert(keys::kRetryWaitSeconds, 9);
        o.insert(keys::kEditStrategy, QStringLiteral("rebuild"));
        o.insert(keys::kExtractParallel, true);
        o.insert(keys::kExtractThreads, 5);
        o.insert(keys::kDeleteSourceAfterVerify, true);
        o.insert(general::keys::kVerifyMode, QStringLiteral("structure"));
        o.insert(general::keys::kVerifyAfterBuild, false);
        o.insert(general::keys::kIoBufferMiB, 8);
        o.insert(general::keys::kFreeSpaceMarginMiB, 123);
        o.insert(general::keys::kWorkerThreads, 1);
        const ExfatSettings x = readSettings(m_env.settings(o));
        QCOMPARE(x.writer.clusterBytes, 131072);
        QCOMPARE(x.writer.sectorBytes, 4096);
        QCOMPARE(x.writer.volumeBytes, qint64(2.5 * double(kGiB)));
        QCOMPARE(x.writer.spare, SpareMode::Fixed);
        QCOMPARE(x.writer.spareBytes, 77 * kMiB);
        QCOMPARE(x.labelPolicy, QStringLiteral("titleId"));
        QCOMPARE(x.label, QStringLiteral("MYLABEL"));
        QCOMPARE(x.requireEboot, false);
        QCOMPARE(x.rules.excludePatterns, QStringList{QStringLiteral("*.tmp")});
        QCOMPARE(x.rules.excludeHidden, true);
        QCOMPARE(x.overwrite, false);
        QCOMPARE(x.writer.fatChains, true);
        QCOMPARE(x.writer.openRetries, 7);
        QCOMPARE(x.writer.retryWaitMs, 9000);
        QCOMPARE(x.editStrategy, EditStrategy::Rebuild);
        QCOMPARE(x.extractParallel, true);
        QCOMPARE(x.extractThreads, 5);
        QCOMPARE(x.deleteSource, true);
        QCOMPARE(x.fullVerify, false);
        QCOMPARE(x.verify, false);
        QCOMPARE(x.writer.bufferBytes, 8 * kMiB);
        QCOMPARE(x.marginBytes, 123 * kMiB);
        QCOMPARE(x.threads, 1);
        QVERIFY(!m_env.schema->normalize(keys::kLabel, QStringLiteral("WAY TOO LONG LABEL")).ok());
    }

    void legacySettingsImport()
    {
        const QString legacy = path(QStringLiteral("legacy-settings.json"));
        QVERIFY(writeBytes(legacy, R"({"adv_cluster": "65536", "adv_sector": "4096", "adv_img_size_override": "90",
            "adv_excl_hidden": true, "adv_retries": "5", "adv_retry_wait": "2", "adv_delete_source": false,
            "exfat_extract_robocopy_mt": true, "exfat_extract_mt_threads": 16, "adv_skip_verify": true,
            "adv_checksum_verify": true})"));
        SettingsStore store(m_env.schema, path(QStringLiteral("store/settings.json")));
        QVERIFY(store.load().ok());
        auto n = store.importLegacy(legacy);
        QVERIFY2(n.ok(), qPrintable(n.ok() ? QString() : err(n.error())));
        QCOMPARE(n.value(), 11);
        QCOMPARE(store.value(keys::kClusterSize).toString(), QStringLiteral("65536"));
        QCOMPARE(store.value(keys::kSectorSize).toString(), QStringLiteral("4096"));
        QCOMPARE(store.value(keys::kImageSizeGiB).toDouble(), 90.0);
        QCOMPARE(store.value(keys::kExcludeHidden).toBool(), true);
        QCOMPARE(store.value(keys::kOpenRetries).toInt(), 5);
        QCOMPARE(store.value(keys::kRetryWaitSeconds).toInt(), 2);
        QCOMPARE(store.value(keys::kExtractParallel).toBool(), true);
        QCOMPARE(store.value(keys::kExtractThreads).toInt(), 16);
        QCOMPARE(store.value(general::keys::kVerifyAfterBuild).toBool(), false);
        QCOMPARE(store.value(general::keys::kVerifyMode).toString(), QStringLiteral("full"));

        // "Auto" and a blank size map to the automatic defaults.
        QVERIFY(writeBytes(legacy, R"({"adv_cluster": "Auto", "adv_img_size_override": ""})"));
        QVERIFY(store.importLegacy(legacy).ok());
        QCOMPARE(store.value(keys::kClusterSize).toString(), QStringLiteral("auto"));
        QCOMPARE(store.value(keys::kImageSizeGiB).toDouble(), 0.0);
    }

    void buildJobProducesAVerifiedImage()
    {
        const QString src = makeGame(QStringLiteral("Astro PPSA01325"));
        const QString outDir = path(QStringLiteral("out1"));
        QDir().mkpath(outDir);
        QList<ProgressSnapshot> snaps;
        std::mutex snapsMutex; // verification reports from several threads
        auto r = m_env.run(kJobBuild, buildParams(src, outDir), {}, nullptr, [&](const ProgressSnapshot& s) {
            std::lock_guard lock(snapsMutex);
            snaps << s;
        });
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        const JobOutcome& o = r.value();
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY2(o.verificationDetail.contains(QStringLiteral("byte for byte")), qPrintable(o.verificationDetail));
        QCOMPARE(o.outputs.size(), 1);
        QCOMPARE(o.outputs.first(), outDir + QStringLiteral("/Astro PPSA01325.exfat"));
        QVERIFY(QFileInfo::exists(o.outputs.first()));
        QVERIFY(!QFileInfo::exists(o.outputs.first() + QStringLiteral(".part")));
        QCOMPARE(o.details.value(QStringLiteral("label")).toString(), QStringLiteral("OSFIMG"));
        QCOMPARE(o.details.value(QStringLiteral("files")).toInt(), 61);
        // Phases ran in order; the write phase counted every file and byte.
        bool sawWrite = false, sawVerify = false;
        for (const ProgressSnapshot& s : snaps) {
            if (s.phaseIndex == 1 && s.filesDone == s.filesTotal && s.filesTotal == 61 && s.done == s.total)
                sawWrite = true;
            if (s.phaseIndex == 2)
                sawVerify = true;
        }
        QVERIFY(sawWrite);
        QVERIFY(sawVerify);
        const Fsck f = runFsck(o.outputs.first());
        if (f.available)
            QVERIFY2(f.exitCode == 0, qPrintable(f.output));
    }

    void buildJobSettingsAreHonoured()
    {
        const QString src = makeGame(QStringLiteral("opts"), 10);
        writeBytes(src + QStringLiteral("/skip.tmp"), "x");
        writeBytes(src + QStringLiteral("/data/also.skip"), "x");
        writeBytes(src + QStringLiteral("/.hidden"), "x");
        const QString out = path(QStringLiteral("opts.exfat"));
        QVariantMap o;
        o.insert(keys::kClusterSize, QStringLiteral("131072"));
        o.insert(keys::kSectorSize, QStringLiteral("4096"));
        o.insert(keys::kLabelPolicy, QStringLiteral("titleId"));
        o.insert(keys::kAllocation, QStringLiteral("fatChain"));
        o.insert(keys::kExcludePatterns, QStringList{QStringLiteral("*.tmp")});
        o.insert(keys::kExcludeHidden, true);
        o.insert(general::keys::kVerifyMode, QStringLiteral("structure"));
        QJsonObject p = buildParams(src, out);
        p.insert(QStringLiteral("excludes"), QJsonArray{QStringLiteral("*.skip")});
        auto r = m_env.run(kJobBuild, p, o);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed);
        QVERIFY(r->verificationDetail.contains(QStringLiteral("not compared")));
        auto reader = ExfatReader::open(out);
        QVERIFY(reader.ok());
        QCOMPARE(reader.value()->geometry().clusterBytes(), 131072);
        QCOMPARE(reader.value()->geometry().sectorBytes(), 4096);
        QCOMPARE(reader.value()->volume().label, QStringLiteral("PPSA01234")); // from sce_sys/param.json
        CancellationToken token;
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        QVERIFY(tree->find(QStringLiteral("skip.tmp")) < 0);
        QVERIFY(tree->find(QStringLiteral("data/also.skip")) < 0);
#ifndef Q_OS_WIN
        QVERIFY(tree->find(QStringLiteral(".hidden")) < 0);
#endif
        QVERIFY(!tree->entries[size_t(tree->find(QStringLiteral("data/big.bin")))].noFatChain);

        // An explicit label wins; "none" leaves the volume unnamed.
        p.insert(QStringLiteral("label"), QStringLiteral("CUSTOM"));
        QVERIFY(m_env.run(kJobBuild, p, o).ok());
        QCOMPARE(ExfatReader::open(out).value()->volume().label, QStringLiteral("CUSTOM"));
        p.remove(QStringLiteral("label"));
        o.insert(keys::kLabelPolicy, QStringLiteral("none"));
        QVERIFY(m_env.run(kJobBuild, p, o).ok());
        QCOMPARE(ExfatReader::open(out).value()->volume().label, QString());
        // A fixed image size.
        o.insert(keys::kImageSizeGiB, 0.25);
        QVERIFY(m_env.run(kJobBuild, p, o).ok());
        QCOMPARE(QFileInfo(out).size(), 256 * kMiB);
        o.insert(keys::kImageSizeGiB, 0.001); // 1 MiB: too small
        auto small = m_env.run(kJobBuild, p, o);
        QVERIFY(!small.ok());
        QVERIFY2(small.error().message.contains(QStringLiteral("too small")), qPrintable(small.error().message));
    }

    void buildJobValidation()
    {
        const QString src = makeGame(QStringLiteral("val"), 2);
        auto r = m_env.run(kJobBuild, buildParams(path(QStringLiteral("nope")), path(QStringLiteral("x.exfat"))));
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::NotFound);

        r = m_env.run(kJobBuild, buildParams(src, src + QStringLiteral("/inside.exfat")));
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);

        const QString noEboot = path(QStringLiteral("noeboot"));
        writeBytes(noEboot + QStringLiteral("/sub/eboot.bin"), "x");
        r = m_env.run(kJobBuild, buildParams(noEboot, path(QStringLiteral("noeboot.exfat"))));
        QVERIFY(!r.ok());
        QVERIFY2(r.error().message.contains(QStringLiteral("eboot.bin")), qPrintable(r.error().message));
        QVariantMap o{{keys::kRequireEboot, false}};
        QVERIFY(m_env.run(kJobBuild, buildParams(noEboot, path(QStringLiteral("noeboot.exfat"))), o).ok());

        const QString existing = path(QStringLiteral("existing.exfat"));
        writeBytes(existing, "OLD");
        r = m_env.run(kJobBuild, buildParams(src, existing), {{keys::kOverwriteExisting, false}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(readAllOf(existing), QByteArray("OLD"));

        r = m_env.run(kJobBuild, [&] { auto p = buildParams(src, path(QStringLiteral("l.exfat"))); p.insert(QStringLiteral("label"), QStringLiteral("TOO LONG LABEL")); return p; }());
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
    }

    void replacingAnImageKeepsTheOldOneUntilTheNewOneIsDone()
    {
        const QString src = makeGame(QStringLiteral("replace"), 2);
        const QString out = path(QStringLiteral("replace.exfat"));
        writeBytes(out, "OLD IMAGE");
        auto r = m_env.run(kJobBuild, buildParams(src, out));
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(ExfatReader::open(out).ok());
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".old")));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
    }

    void cancellationLeavesNoOutputAndNoPart()
    {
        const QString src = makeGame(QStringLiteral("cancel"), 5);
        QVERIFY(writeRandom(src + QStringLiteral("/data/large1.bin"), 48 * kMiB, 77));
        QVERIFY(writeRandom(src + QStringLiteral("/data/large2.bin"), 48 * kMiB, 78));
        const QString out = path(QStringLiteral("cancel.exfat"));
        writeBytes(out, "PREVIOUS IMAGE"); // must survive (audit M2)
        CancellationSource cancel;
        bool sawWriting = false;
        auto r = m_env.run(kJobBuild, buildParams(src, out), {}, &cancel, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.done > 16 * kMiB) {
                sawWriting = true;
                cancel.cancel();
            }
        });
        QVERIFY(sawWriting);
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
        QCOMPARE(readAllOf(out), QByteArray("PREVIOUS IMAGE"));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));

        // Cancelling during verification also keeps the previous image.
        CancellationSource cancel2;
        auto r2 = m_env.run(kJobBuild, buildParams(src, out), {}, &cancel2, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && s.done > 8 * kMiB)
                cancel2.cancel();
        });
        QVERIFY(!r2.ok());
        QVERIFY(r2.error().isCancelled());
        QCOMPARE(readAllOf(out), QByteArray("PREVIOUS IMAGE"));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
    }

    void insufficientSpaceIsReportedBeforeWriting()
    {
        const QString src = makeGame(QStringLiteral("space"), 2);
        const QString out = path(QStringLiteral("space.exfat"));
        auto r = m_env.run(kJobBuild, buildParams(src, out), {{keys::kImageSizeGiB, 100000.0}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::InsufficientSpace);
        QVERIFY2(r.error().message.startsWith(QStringLiteral("Not enough space on")), qPrintable(r.error().message));
        QVERIFY2(r.error().message.contains(QStringLiteral("Free ")) && r.error().message.contains(QStringLiteral("choose another output folder")),
                 qPrintable(r.error().message));
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
    }

    void sourceIsDeletedOnlyAfterFullVerification()
    {
        QVariantMap o{{keys::kDeleteSourceAfterVerify, true}};
        // Structure-only verification: kept.
        const QString a = makeGame(QStringLiteral("del-a"), 2);
        QVariantMap structure = o;
        structure.insert(general::keys::kVerifyMode, QStringLiteral("structure"));
        auto r = m_env.run(kJobBuild, buildParams(a, path(QStringLiteral("del-a.exfat"))), structure);
        QVERIFY(r.ok());
        QVERIFY(QFileInfo::exists(a + QStringLiteral("/eboot.bin")));
        QVERIFY(r->warnings.join(u' ').contains(QStringLiteral("kept")));
        // Verification off: kept.
        QVariantMap off = o;
        off.insert(general::keys::kVerifyAfterBuild, false);
        r = m_env.run(kJobBuild, buildParams(a, path(QStringLiteral("del-a2.exfat"))), off);
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Skipped);
        QVERIFY(QFileInfo::exists(a + QStringLiteral("/eboot.bin")));
        // Something was left out of the image: kept.
        writeBytes(a + QStringLiteral("/Thumbs.db"), "x");
        r = m_env.run(kJobBuild, buildParams(a, path(QStringLiteral("del-a3.exfat"))), o);
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Passed);
        QVERIFY(QFileInfo::exists(a + QStringLiteral("/eboot.bin")));
        QVERIFY(r->warnings.join(u' ').contains(QStringLiteral("left out")));
        // Full verification passed and nothing left out: deleted.
        const QString b = makeGame(QStringLiteral("del-b"), 2);
        r = m_env.run(kJobBuild, buildParams(b, path(QStringLiteral("del-b.exfat"))), o);
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Passed);
        QVERIFY(!QFileInfo::exists(b));
        QVERIFY(r->details.value(QStringLiteral("sourceDeleted")).toBool());
        QVERIFY(ExfatReader::open(path(QStringLiteral("del-b.exfat"))).ok());
    }

    void verifyJobReportsMismatches()
    {
        const QString src = makeGame(QStringLiteral("vjob"), 5);
        const QString out = path(QStringLiteral("vjob.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, out)).ok());
        const QJsonObject p{{QStringLiteral("image"), out}, {QStringLiteral("source"), src}};
        auto r = m_env.run(kJobVerify, p);
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Passed);

        flipByte(src + QStringLiteral("/data/medium.bin"), 1000);
        writeBytes(src + QStringLiteral("/extra-in-source.txt"), "x");
        r = m_env.run(kJobVerify, p);
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Failed);
        const QJsonArray mm = r->details.value(QStringLiteral("verification")).toObject().value(QStringLiteral("mismatches")).toArray();
        QCOMPARE(mm.size(), 2);
        QStringList kinds;
        for (const QJsonValue& v : mm)
            kinds << v.toObject().value(QStringLiteral("path")).toString() + u':' + v.toObject().value(QStringLiteral("kind")).toString();
        kinds.sort();
        QCOMPARE(kinds, (QStringList{QStringLiteral("data/medium.bin:content"), QStringLiteral("extra-in-source.txt:missing")}));
        QVERIFY(r->verificationDetail.contains(QStringLiteral("2 problems")));

        // A damaged image fails verification instead of erroring out.
        const QString bad = path(QStringLiteral("vjob-bad.exfat"));
        QVERIFY(QFile::copy(out, bad));
        QFile(bad).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        flipByte(bad, 100);
        r = m_env.run(kJobVerify, {{QStringLiteral("image"), bad}, {QStringLiteral("source"), src}});
        QVERIFY(r.ok());
        QCOMPARE(r->verification, Verification::Failed);
        QVERIFY(r->verificationDetail.contains(QStringLiteral("damaged")));
    }

    void extractJobRoundTrips()
    {
        const QString src = makeGame(QStringLiteral("xsrc"), 40);
        const QString img = path(QStringLiteral("xsrc.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        const QString dest = path(QStringLiteral("xdest"));
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed);
        QCOMPARE(r->outputs, QStringList{dest});
        QCOMPARE(listing(dest), listing(src));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".part")));

        // A non-empty destination is refused.
        auto again = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}});
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, ErrorCode::AlreadyExists);

        // An empty destination folder is fine; a stale .part of ours is replaced,
        // somebody else's is not touched.
        const QString dest2 = path(QStringLiteral("xdest2"));
        QDir().mkpath(dest2);
        QDir().mkpath(dest2 + QStringLiteral(".part"));
        writeBytes(dest2 + QStringLiteral(".part/.fw-extract-in-progress"), {});
        writeBytes(dest2 + QStringLiteral(".part/half.bin"), "partial");
        r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest2}},
                      {{keys::kExtractParallel, true}, {keys::kExtractThreads, 4}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(listing(dest2), listing(src));
        const QString dest3 = path(QStringLiteral("xdest3"));
        writeBytes(dest3 + QStringLiteral(".part/user-file.txt"), "mine");
        r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest3}});
        QVERIFY(!r.ok());
        QCOMPARE(readAllOf(dest3 + QStringLiteral(".part/user-file.txt")), QByteArray("mine"));
    }

    void extractCancellationLeavesNothing()
    {
        const QString src = makeGame(QStringLiteral("xcancel"), 5);
        QVERIFY(writeRandom(src + QStringLiteral("/data/large.bin"), 64 * kMiB, 5));
        const QString img = path(QStringLiteral("xcancel.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        const QString dest = path(QStringLiteral("xcancel-out"));
        CancellationSource cancel;
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}}, {}, &cancel,
                           [&](const ProgressSnapshot& s) {
                               if (s.phaseIndex == 1 && s.done > 8 * kMiB)
                                   cancel.cancel();
                           });
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".part")));
    }

    void extractRefusesADamagedImage()
    {
        const QString src = makeGame(QStringLiteral("xbad"), 2);
        const QString img = path(QStringLiteral("xbad.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        flipByte(img, 100);
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), path(QStringLiteral("xbad-out"))}});
        QVERIFY(!r.ok());
        QCOMPARE(r.error().code, ErrorCode::CorruptData);
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("xbad-out.part"))));
    }

    void titleIdDetection()
    {
        const QString a = path(QStringLiteral("tid/Some Game"));
        writeBytes(a + QStringLiteral("/sce_sys/param.json"), R"({"titleId": "ppsa09876"})");
        QCOMPARE(detectTitleId(a, QString()), QStringLiteral("PPSA09876"));
        const QString b = path(QStringLiteral("tid/CUSA-12345 Old Game"));
        QDir().mkpath(b);
        QCOMPARE(detectTitleId(b, QString()), QStringLiteral("CUSA12345"));
        const QString c = path(QStringLiteral("tid/nothing"));
        QDir().mkpath(c);
        QCOMPARE(detectTitleId(c, path(QStringLiteral("PPLH_00001 x.exfat"))), QStringLiteral("PPLH00001"));
        QCOMPARE(detectTitleId(c, QString()), QString());
    }
};

QTEST_GUILESS_MAIN(TestExfatJobs)
#include "tst_exfat_jobs.moc"
