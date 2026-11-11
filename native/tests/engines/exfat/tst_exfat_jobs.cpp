// exFAT queue jobs: build (phases, verification, settings, cancellation,
// free space, replacing an image, deleting the source), verify, extract,
// and the settings schema with its legacy key table.

#include "ExfatJobEnv.h"
#include "ExfatTestUtil.h"

#include "core/PathSafety.h"
#include "core/Paths.h"
#include "core/Zip.h"
#include "engines/exfat/ExfatExtract.h"
#include "engines/exfat/ExfatJobs.h"
#include "engines/exfat/ExfatVerify.h"
#include "engines/exfat/Pipeline.h"
#include "engines/exfat/PlatformFs.h"
#include "support/TestSupport.h"

#include <QDirIterator>
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
        if (!makeSampleTree(src, smallFiles))
            QTest::qFail("the sample tree could not be created", __FILE__, __LINE__);
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
        QCOMPARE(count, 19);

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
        QVERIFY(QDir().mkpath(outDir));
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
        QVERIFY(writeBytes(src + QStringLiteral("/skip.tmp"), "x"));
        QVERIFY(writeBytes(src + QStringLiteral("/data/also.skip"), "x"));
        // Hidden the Unix way (a leading dot) and the Windows way (the attribute).
        QVERIFY(writeBytes(src + QStringLiteral("/.hidden"), "x"));
        QVERIFY(writeBytes(src + QStringLiteral("/data/attribute.bin"), "x"));
#ifdef Q_OS_WIN
        QVERIFY(setHiddenAttribute(src + QStringLiteral("/data/attribute.bin")));
#endif
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
        {   // closed before the image is rebuilt (Windows cannot replace an open file)
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
            // Each platform's own kind of hidden file is left out, the other kind is an ordinary file.
#ifdef Q_OS_WIN
            QVERIFY(tree->find(QStringLiteral("data/attribute.bin")) < 0);
            QVERIFY(tree->find(QStringLiteral(".hidden")) > 0);
#else
            QVERIFY(tree->find(QStringLiteral(".hidden")) < 0);
            QVERIFY(tree->find(QStringLiteral("data/attribute.bin")) > 0);
#endif
            const int big = tree->find(QStringLiteral("data/big.bin"));
            QVERIFY(big > 0);
            QVERIFY(!tree->entries[size_t(big)].noFatChain);
        }

        // The label of the image at `out`; an image that cannot be opened has
        // none of the labels compared below.
        const auto labelOf = [](const QString& image) {
            auto reader = ExfatReader::open(image);
            return reader.ok() ? reader.value()->volume().label : QStringLiteral("<unreadable image>");
        };
        // An explicit label wins; "none" leaves the volume unnamed.
        p.insert(QStringLiteral("label"), QStringLiteral("CUSTOM"));
        QVERIFY(m_env.run(kJobBuild, p, o).ok());
        QCOMPARE(labelOf(out), QStringLiteral("CUSTOM"));
        p.remove(QStringLiteral("label"));
        o.insert(keys::kLabelPolicy, QStringLiteral("none"));
        QVERIFY(m_env.run(kJobBuild, p, o).ok());
        QCOMPARE(labelOf(out), QString());
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
        FWTEST_COMPARE_ERROR(r, ErrorCode::NotFound);

        r = m_env.run(kJobBuild, buildParams(src, src + QStringLiteral("/inside.exfat")));
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::InvalidArgument);

        const QString noEboot = path(QStringLiteral("noeboot"));
        QVERIFY(writeBytes(noEboot + QStringLiteral("/sub/eboot.bin"), "x"));
        r = m_env.run(kJobBuild, buildParams(noEboot, path(QStringLiteral("noeboot.exfat"))));
        QVERIFY(!r.ok());
        QVERIFY2(r.error().message.contains(QStringLiteral("eboot.bin")), qPrintable(r.error().message));
        QVariantMap o{{keys::kRequireEboot, false}};
        QVERIFY(m_env.run(kJobBuild, buildParams(noEboot, path(QStringLiteral("noeboot.exfat"))), o).ok());

        const QString existing = path(QStringLiteral("existing.exfat"));
        QVERIFY(writeBytes(existing, "OLD"));
        r = m_env.run(kJobBuild, buildParams(src, existing), {{keys::kOverwriteExisting, false}});
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::AlreadyExists);
        QCOMPARE(readAllOf(existing), QByteArray("OLD"));

        r = m_env.run(kJobBuild, [&] { auto p = buildParams(src, path(QStringLiteral("l.exfat"))); p.insert(QStringLiteral("label"), QStringLiteral("TOO LONG LABEL")); return p; }());
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::InvalidArgument);
    }

    void replacingAnImageKeepsTheOldOneUntilTheNewOneIsDone()
    {
        const QString src = makeGame(QStringLiteral("replace"), 2);
        const QString out = path(QStringLiteral("replace.exfat"));
        QVERIFY(writeBytes(out, "OLD IMAGE"));
        auto r = m_env.run(kJobBuild, buildParams(src, out));
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QVERIFY(ExfatReader::open(out).ok());
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".old")));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
    }

    // The old image is open in another program (mounted, being uploaded), so
    // the new one cannot take its name. The finished, verified image is kept
    // under a free name next to it instead of being deleted.
    void finishedImageIsKeptWhenTheOldOneCannotBeReplaced()
    {
#ifndef Q_OS_WIN
        QSKIP("An open file can be replaced on this platform.");
#endif
        const QString src = makeGame(QStringLiteral("locked"), 2);
        const QString out = path(QStringLiteral("locked.exfat"));
        QVERIFY(writeBytes(out, "OLD IMAGE"));
        QFile holder(out);
        QVERIFY(holder.open(QIODevice::ReadOnly)); // opened without delete sharing
        auto r = m_env.run(kJobBuild, buildParams(src, out));
        QVERIFY(!r.ok());
        const QString kept = path(QStringLiteral("locked (new).exfat"));
        QVERIFY2(r.error().message.contains(QStringLiteral("built and verified")), qPrintable(err(r.error())));
        QVERIFY2(r.error().message.contains(QStringLiteral("in use")), qPrintable(err(r.error())));
        QVERIFY2(r.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(err(r.error())));
        QCOMPARE(readAllOf(out), QByteArray("OLD IMAGE"));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        {
            CancellationToken token;
            auto reader = ExfatReader::open(kept);
            QVERIFY(reader.ok());
            auto tree = reader.value()->readTree(token);
            QVERIFY(tree.ok() && tree->problems.isEmpty());
            QVERIFY(tree->find(QStringLiteral("eboot.bin")) > 0);
        }
        // A second attempt keeps its image too, without touching the first.
        const QByteArray first = fileHash(kept);
        r = m_env.run(kJobBuild, buildParams(src, out));
        holder.close();
        QVERIFY(!r.ok());
        QCOMPARE(fileHash(kept), first);
        QVERIFY(ExfatReader::open(path(QStringLiteral("locked (new) (2).exfat"))).ok());
        QCOMPARE(readAllOf(out), QByteArray("OLD IMAGE"));
    }

    void cancellationLeavesNoOutputAndNoPart()
    {
        const QString src = makeGame(QStringLiteral("cancel"), 5);
        QVERIFY(writeRandom(src + QStringLiteral("/data/large1.bin"), 48 * kMiB, 77));
        QVERIFY(writeRandom(src + QStringLiteral("/data/large2.bin"), 48 * kMiB, 78));
        const QString out = path(QStringLiteral("cancel.exfat"));
        QVERIFY(writeBytes(out, "PREVIOUS IMAGE")); // must survive the cancelled rebuild
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

    // A file changes (same size, other bytes) after it was written into the
    // image: the check fails, the new image is not saved, the image that was
    // there stays byte for byte, and the source is kept although "delete the
    // source" is on.
    void failedVerificationKeepsThePreviousImageAndTheSource()
    {
        const QString src = makeGame(QStringLiteral("verifyfail"), 5);
        const QString out = path(QStringLiteral("verifyfail.exfat"));
        QVERIFY(writeBytes(out, "PREVIOUS IMAGE"));
        // The first report of the verification phase comes from the job's own
        // thread before any file is compared; later ones come from several.
        std::once_flag once;
        bool tampered = false;
        auto r = m_env.run(kJobBuild, buildParams(src, out), {{keys::kDeleteSourceAfterVerify, true}}, nullptr,
                           [&](const ProgressSnapshot& s) {
                               if (s.phaseIndex != 2)
                                   return;
                               std::call_once(once, [&] {
                                   QFile f(src + QStringLiteral("/data/medium.bin"));
                                   char c = 0;
                                   tampered = f.open(QIODevice::ReadWrite) && f.seek(1000) && f.read(&c, 1) == 1 && f.seek(1000)
                                              && f.putChar(char(c ^ 0x55));
                               });
                           });
        QVERIFY(tampered);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Failed);
        QVERIFY2(r->verificationDetail.contains(QStringLiteral("1 problem")), qPrintable(r->verificationDetail));
        QVERIFY(r->outputs.isEmpty());
        QCOMPARE(readAllOf(out), QByteArray("PREVIOUS IMAGE"));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QVERIFY(QFileInfo::exists(src + QStringLiteral("/eboot.bin")));
        QVERIFY(!r->details.contains(QStringLiteral("sourceDeleted")));
    }

    void insufficientSpaceIsReportedBeforeWriting()
    {
        const QString src = makeGame(QStringLiteral("space"), 2);
        const QString out = path(QStringLiteral("space.exfat"));
        auto r = m_env.run(kJobBuild, buildParams(src, out), {{keys::kImageSizeGiB, 100000.0}});
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::InsufficientSpace);
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
        QVERIFY(writeBytes(a + QStringLiteral("/Thumbs.db"), "x"));
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
        QVERIFY(writeBytes(src + QStringLiteral("/extra-in-source.txt"), "x"));
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
        FWTEST_COMPARE_ERROR(again, ErrorCode::AlreadyExists);

        // An empty destination folder is fine; a stale .part of ours is replaced,
        // somebody else's is not touched.
        const QString dest2 = path(QStringLiteral("xdest2"));
        QVERIFY(QDir().mkpath(dest2));
        QVERIFY(QDir().mkpath(dest2 + QStringLiteral(".part")));
        QVERIFY(writeBytes(dest2 + QStringLiteral(".part/.fw-extract-in-progress"), {}));
        QVERIFY(writeBytes(dest2 + QStringLiteral(".part/half.bin"), "partial"));
        r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest2}},
                      {{keys::kExtractParallel, true}, {keys::kExtractThreads, 4}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(listing(dest2), listing(src));
        const QString dest3 = path(QStringLiteral("xdest3"));
        QVERIFY(writeBytes(dest3 + QStringLiteral(".part/user-file.txt"), "mine"));
        r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest3}});
        QVERIFY(!r.ok());
        QCOMPARE(readAllOf(dest3 + QStringLiteral(".part/user-file.txt")), QByteArray("mine"));
    }

    // The chosen (empty) folder cannot be replaced because a program has it
    // open: the extracted, verified files stay, under "<folder>.new".
    void extractedFilesAreKeptWhenTheFolderCannotBeUsed()
    {
#ifndef Q_OS_WIN
        QSKIP("A folder in use can be removed on this platform.");
#endif
        const QString src = makeGame(QStringLiteral("heldsrc"), 5);
        const QString img = path(QStringLiteral("heldsrc.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        const QString dest = path(QStringLiteral("held"));
        QVERIFY(QDir().mkpath(dest));
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dest)); // the current folder of a process cannot be removed
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}});
        QVERIFY(QDir::setCurrent(before));
        QVERIFY(!r.ok());
        const QString kept = dest + QStringLiteral(".new");
        QVERIFY2(r.error().message.contains(QStringLiteral("extracted and verified")), qPrintable(err(r.error())));
        QVERIFY2(r.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(err(r.error())));
        QCOMPARE(listing(kept), listing(src));
        QVERIFY(!QFileInfo::exists(kept + QStringLiteral("/.fw-extract-in-progress")));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".part")));
        QVERIFY(QDir(dest).isEmpty());
    }

    // exFAT allows names Win32 rewrites or opens as devices (trailing dots or
    // spaces, CON, NUL...). Extraction stores them under a name this drive
    // keeps as it is, reports the change, verifies against the image's names,
    // and never writes outside the output folder.
    void extractMapsNamesTheDriveCannotStore()
    {
        const QString src = makeGame(QStringLiteral("xnames"), 2);
        const QString img = path(QStringLiteral("xnames.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        struct Added {
            const char* imagePath;
            const char* content;
        };
        const Added added[] = {{"CON", "console"}, {"nul.txt", "null"}, {"end.", "trailing dot"},
                               {"dup", "plain"},   {"dup.", "dotted"},  {"...", "only dots"},
                               {"dir. /f.bin", "in a folder"}};
        QJsonArray ops;
        for (const Added& a : added) {
            const QString local = path(QStringLiteral("xnames-local/%1.bin").arg(ops.size()));
            QVERIFY(writeBytes(local, a.content));
            ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("add")},
                                   {QStringLiteral("imagePath"), QString::fromLatin1(a.imagePath)},
                                   {QStringLiteral("localPath"), local}});
        }
        auto edit = m_env.run(kJobEdit, {{QStringLiteral("image"), img}, {QStringLiteral("operations"), ops}});
        QVERIFY2(edit.ok(), qPrintable(edit.ok() ? QString() : err(edit.error())));

        const QString dest = path(QStringLiteral("xnames-out"));
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed); // renamed entries are compared under their image names
        const bool win = hostUsesWindowsNames();
        const QList<QPair<QString, QByteArray>> expected = {
            {win ? QStringLiteral("_CON") : QStringLiteral("CON"), "console"},
            {win ? QStringLiteral("_nul.txt") : QStringLiteral("nul.txt"), "null"},
            {win ? QStringLiteral("end") : QStringLiteral("end."), "trailing dot"},
            {QStringLiteral("dup"), "plain"},
            {win ? QStringLiteral("dup (2)") : QStringLiteral("dup."), "dotted"},
            {QStringLiteral("_"), "only dots"},
            {win ? QStringLiteral("dir/f.bin") : QStringLiteral("dir. /f.bin"), "in a folder"},
        };
        for (const auto& [name, content] : expected)
            QCOMPARE(readAllOf(dest + u'/' + name), content);
        const QStringList renamed = r->warnings.filter(QStringLiteral("Renamed for this drive"));
        QCOMPARE(renamed.size(), win ? 6 : 1);
        QVERIFY(renamed.contains(QStringLiteral("Renamed for this drive: ... → _")));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("CON"))));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("_"))));
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
        FWTEST_COMPARE_ERROR(r, ErrorCode::CorruptData);
        QVERIFY(!QFileInfo::exists(path(QStringLiteral("xbad-out.part"))));
    }

    // "Check the image on its own" treats boot-region damage like the check
    // against a folder does: a failed verification, never a pass or a job error.
    void standaloneCheckFailsOnBootDamage()
    {
        const QString src = makeGame(QStringLiteral("alone"), 5);
        const QString img = path(QStringLiteral("alone.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        auto good = m_env.run(kJobVerify, {{QStringLiteral("image"), img}});
        QVERIFY2(good.ok(), qPrintable(good.ok() ? QString() : err(good.error())));
        QCOMPARE(good->verification, Verification::Passed);

        const QString badBackup = path(QStringLiteral("alone-backup.exfat"));
        QVERIFY(QFile::copy(img, badBackup));
        flipByte(badBackup, 12 * 512 + 100);
        auto r = m_env.run(kJobVerify, {{QStringLiteral("image"), badBackup}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Failed);
        QVERIFY2(r->verificationDetail.contains(QStringLiteral("backup boot region")), qPrintable(r->verificationDetail));

        const QString badMain = path(QStringLiteral("alone-main.exfat"));
        QVERIFY(QFile::copy(img, badMain));
        flipByte(badMain, 100);
        r = m_env.run(kJobVerify, {{QStringLiteral("image"), badMain}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Failed);
        QVERIFY2(r->verificationDetail.contains(QStringLiteral("main boot sector")), qPrintable(r->verificationDetail));
        QCOMPARE(r->summary, QStringLiteral("The image is damaged."));
    }

    // The extraction marker is compared the way the destination drive
    // compares names: an entry with that name in another case is refused too.
    void extractRefusesTheMarkerNameInAnyCase()
    {
        const QString src = makeGame(QStringLiteral("markersrc"), 2);
        QVERIFY(writeBytes(src + QStringLiteral("/.FW-Extract-In-Progress"), "a real file of the game"));
        const QString img = path(QStringLiteral("markersrc.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        const QString dest = path(QStringLiteral("markerdest"));
        auto r = m_env.run(kJobExtract, {{QStringLiteral("image"), img}, {QStringLiteral("output"), dest}});
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::Unsupported);
        QVERIFY2(r.error().message.contains(QStringLiteral(".FW-Extract-In-Progress")), qPrintable(err(r.error())));
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".part")));
    }

    // A zipped dump: the image is named after the ZIP without ".zip", and
    // "delete the source" neither deletes the ZIP nor claims it did.
    void zippedSourceNamesTheImageAndIsNeverDeleted()
    {
        const QString src = path(QStringLiteral("zipgame"));
        QVERIFY(writeBytes(src + QStringLiteral("/eboot.bin"), QByteArray("\x7F" "ELF fake eboot", 15)));
        QVERIFY(writeBytes(src + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA01234","contentVersion":"01.000.000"})"));
        QVERIFY(writeRandom(src + QStringLiteral("/data/content.bin"), 300 * 1024, 5));
        const QString zipPath = path(QStringLiteral("ZippedGame.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(zipPath, true).ok());
            QDirIterator it(src, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString p = it.next();
                QVERIFY(w.addFile(QDir(src).relativeFilePath(p), p, zip::Method::Store, {}).ok());
            }
            QVERIFY(w.finish(false).ok());
        }
        const QString outDir = path(QStringLiteral("zip-out"));
        QVERIFY(QDir().mkpath(outDir));
        QCOMPARE(resolveBuildOutput(zipPath, outDir), outDir + QStringLiteral("/ZippedGame.exfat"));
        QCOMPARE(resolveBuildOutput(src + u'/', outDir), outDir + QStringLiteral("/zipgame.exfat"));
        QCOMPARE(resolveBuildOutput(src, outDir + u'/'), outDir + QStringLiteral("/zipgame.exfat"));

        auto r = m_env.run(kJobBuild, buildParams(zipPath, outDir), {{keys::kDeleteSourceAfterVerify, true}});
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed);
        QCOMPARE(r->outputs, QStringList{outDir + QStringLiteral("/ZippedGame.exfat")});
        QVERIFY(QFileInfo::exists(zipPath));
        QVERIFY(!r->details.contains(QStringLiteral("sourceDeleted")));
        QVERIFY2(r->warnings.join(u'\n').contains(QStringLiteral("was kept: only folders are deleted")),
                 qPrintable(r->warnings.join(u'\n')));
    }

    void summariesCountInTheSingularAndEndWithOneFullStop()
    {
        QCOMPARE(counted(1, "file", "files"), QStringLiteral("1 file"));
        QCOMPARE(counted(0, "file", "files"), QStringLiteral("0 files"));
        QCOMPARE(counted(1234, "file", "files"), QStringLiteral("1,234 files"));
        VerifyReport one;
        one.full = true;
        one.filesChecked = 1;
        one.dirsChecked = 1;
        QVERIFY2(one.summary().startsWith(QStringLiteral("1 file and 1 folder compared byte for byte")), qPrintable(one.summary()));
        one.full = false;
        one.filesChecked = 2;
        QVERIFY2(one.summary().startsWith(QStringLiteral("2 files and 1 folder checked")), qPrintable(one.summary()));
        // A problem that is a whole sentence does not get a second full stop.
        VerifyReport damaged;
        damaged.structureProblems << corruptImage(QStringLiteral("its boot checksum does not match")).message;
        QVERIFY(!damaged.passed());
        QVERIFY2(damaged.summary().endsWith(QStringLiteral("game folder.")), qPrintable(damaged.summary()));
        VerifyReport mismatch;
        mismatch.structureProblems << QStringLiteral("the name hash of a.bin does not match");
        QVERIFY2(mismatch.summary().endsWith(QStringLiteral("does not match.")), qPrintable(mismatch.summary()));
    }

    void fat32LimitIsKnownByFileSystemName()
    {
        for (const char* fs : {"FAT32", "FAT", "fat16", "vfat", "msdos"})
            QVERIFY2(limitsFilesTo4GiB(QString::fromLatin1(fs)), fs);
        for (const char* fs : {"NTFS", "exFAT", "ReFS", "ext4", "apfs", ""})
            QVERIFY2(!limitsFilesTo4GiB(QString::fromLatin1(fs)), fs);
        QVERIFY(!tooLargeForDrive(m_tmp.path(), 4 * kGiB - 1)); // below the limit on any drive
    }

    // More buffers than a queue's default capacity used to block the constructor.
    void pipelineTakesMoreThanEightBuffers()
    {
        ChunkPipeline pipe(12, 4096);
        for (int i = 0; i < 12; ++i)
            QVERIFY(pipe.takeFree() != nullptr);
    }

    // A buffer that cannot be allocated inside a worker thread used to end
    // the whole app (an exception leaving a std::thread).
    void workerThreadsReportAllocationFailureAsAnError()
    {
        const QString src = makeGame(QStringLiteral("oom"), 5);
        const QString img = path(QStringLiteral("oom.exfat"));
        QVERIFY(m_env.run(kJobBuild, buildParams(src, img)).ok());
        CancellationToken token;
        VerifyOptions vo;
        vo.full = true;
        vo.threads = 4;
        vo.bufferBytes = qint64(1) << 46;
        auto v = verifyImageAgainstFolder(img, src, {}, vo, token, nullptr);
        QVERIFY(!v.ok());
        FWTEST_COMPARE_ERROR(v, ErrorCode::Internal);
        QVERIFY2(v.error().message.contains(QStringLiteral("Not enough memory")), qPrintable(err(v.error())));

        auto reader = ExfatReader::open(img);
        QVERIFY(reader.ok());
        auto tree = reader.value()->readTree(token);
        QVERIFY(tree.ok());
        const QString dest = path(QStringLiteral("oom-out"));
        QVERIFY(QDir().mkpath(dest));
        ExtractOptions eo;
        eo.parallel = true;
        eo.threads = 4;
        eo.bufferBytes = qint64(1) << 46;
        auto x = extractTree(*reader.value(), tree.value(), dest, eo, token, nullptr);
        QVERIFY(!x.ok());
        FWTEST_COMPARE_ERROR(x, ErrorCode::Internal);
        QVERIFY2(x.error().message.contains(QStringLiteral("Not enough memory")), qPrintable(err(x.error())));
    }

    // A Windows shortcut is a small file to copy as it is, whatever it points
    // at: not a link to follow, and not an error when its target is gone.
    void windowsShortcutsAreCopiedAsFiles()
    {
#ifndef Q_OS_WIN
        QSKIP("Shortcut files are a Windows matter.");
#endif
        const QString src = makeGame(QStringLiteral("lnk"), 2);
        const QString elsewhere = path(QStringLiteral("lnk-target-folder"));
        QVERIFY(writeRandom(elsewhere + QStringLiteral("/big.bin"), 100 * 1024, 7));
        if (!QFile::link(elsewhere, src + QStringLiteral("/folder.lnk"))
            || !QFile::link(path(QStringLiteral("no-such-file.bin")), src + QStringLiteral("/dead.lnk")))
            QSKIP("Shortcuts cannot be created here.");
        CancellationToken token;
        auto tree = scanSource(src, {}, token);
        QVERIFY2(tree.ok(), qPrintable(tree.ok() ? QString() : err(tree.error())));
        int seen = 0;
        for (const SourceNode& n : tree->nodes) {
            QVERIFY2(!n.relativePath.startsWith(QStringLiteral("folder.lnk/")), qPrintable(n.relativePath));
            if (n.relativePath != QStringLiteral("folder.lnk") && n.relativePath != QStringLiteral("dead.lnk"))
                continue;
            ++seen;
            QVERIFY(!n.isDir);
            QFile raw(n.absolutePath);
            QVERIFY(raw.open(QIODevice::ReadOnly));
            QVERIFY(raw.size() > 0);
            QCOMPARE(n.size, raw.size());
        }
        QCOMPARE(seen, 2);
        const QString img = path(QStringLiteral("lnk.exfat"));
        auto r = m_env.run(kJobBuild, buildParams(src, img));
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : err(r.error())));
        QCOMPARE(r->verification, Verification::Passed);
    }

    void titleIdDetection()
    {
        const QString a = path(QStringLiteral("tid/Some Game"));
        QVERIFY(writeBytes(a + QStringLiteral("/sce_sys/param.json"), R"({"titleId": "ppsa09876"})"));
        QCOMPARE(detectTitleId(a, QString()), QStringLiteral("PPSA09876"));
        const QString b = path(QStringLiteral("tid/CUSA-12345 Old Game"));
        QVERIFY(QDir().mkpath(b));
        QCOMPARE(detectTitleId(b, QString()), QStringLiteral("CUSA12345"));
        const QString c = path(QStringLiteral("tid/nothing"));
        QVERIFY(QDir().mkpath(c));
        QCOMPARE(detectTitleId(c, path(QStringLiteral("PPLH_00001 x.exfat"))), QStringLiteral("PPLH00001"));
        QCOMPARE(detectTitleId(c, QString()), QString());
    }
};

QTEST_GUILESS_MAIN(TestExfatJobs)
#include "tst_exfat_jobs.moc"
