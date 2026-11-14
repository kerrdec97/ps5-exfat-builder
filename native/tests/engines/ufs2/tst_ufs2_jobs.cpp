// The image.ffpkg.* jobs: settings reach the image, verification gates the
// final name (audit H6), cancellation and failures leave no output, an
// existing image survives a failed rebuild (M2), the danger-zone delete, the
// verify / extract / edit jobs, legacy settings import and a queue run.

#include "Ufs2TestUtil.h"

#include "core/Settings.h"
#include "core/Zip.h"
#include "engines/general/Module.h"
#include "engines/ufs2/Module.h"
#include "engines/ufs2/Ufs2Check.h"
#include "engines/ufs2/Ufs2Verify.h"
#include "jobs/JobQueue.h"
#include "support/ProgressAudit.h"
#include "support/SparseFile.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>

using namespace ufs2test;

class TestUfs2Jobs : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appData;
    QTemporaryDir m_tmp;
    std::shared_ptr<SettingsSchema> m_schema = std::make_shared<SettingsSchema>();
    DependencyRegistry m_deps;
    JobFactory m_jobs;
    std::unique_ptr<SettingsStore> m_store;
    QStringList m_phasesSeen;

    using Sink = std::function<void(const ProgressSnapshot&)>;

    // Runs a job as the queue's worker does (phases and weights declared as
    // the queue declares them); `audit` watches the progress.
    Result<JobOutcome> runJob(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {},
                              const CancellationToken& token = {}, Sink sink = {}, fwtest::ProgressAudit* audit = nullptr)
    {
        FW_TRY_ASSIGN(auto job, m_jobs.create(kind, params, overrides));
        FW_TRY_ASSIGN(QVariantMap normalized, m_store->normalizeOverrides(overrides));
        const SettingsValues sv = m_store->snapshot(normalized);
        FW_TRY(job->validate(sv));
        m_phasesSeen.clear();
        ProgressReporter rep(
            [&](const ProgressSnapshot& s) {
                if (m_phasesSeen.isEmpty() || m_phasesSeen.last() != s.phase)
                    m_phasesSeen << s.phase;
                if (audit)
                    audit->report(s);
                if (sink)
                    sink(s);
            },
            0);
        if (audit)
            audit->watch(&rep);
        rep.setPhases(job->phases(), job->phaseWeights(sv));
        TempDir temp(QStringLiteral("ufs2-test"), m_tmp.path());
        JobContext ctx;
        ctx.jobId = QStringLiteral("test");
        ctx.token = token;
        ctx.progress = &rep;
        ctx.log = Log(kind, QStringLiteral("test"));
        ctx.settings = sv;
        ctx.tempDir = temp.path();
        auto r = job->run(ctx);
        if (audit) {
            rep.finish();
            audit->finish(rep.snapshot(), r.ok() && r->verification != Verification::Failed);
            audit->watch(nullptr);
        }
        return r;
    }

    static QByteArray sha(const QString& path) { return QCryptographicHash::hash(readAll(path), QCryptographicHash::Sha256); }

    QString freshGame(const QString& name, qint64 big = 2 * 1024 * 1024)
    {
        const QString src = m_tmp.filePath(name);
        QDir(src).removeRecursively();
        return makeGameTree(src, big) ? src : QString();
    }

    QJsonObject buildParams(const QString& src, const QString& out, bool replace = false)
    {
        return QJsonObject{{"source", src}, {"output", out}, {"replaceExisting", replace}};
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
    }

    void registersSettingsAndJobKinds()
    {
        for (const QString& k : {ufs2::keys::kBlockSize, ufs2::keys::kFragmentSize, ufs2::keys::kMinFreePercent,
                                 ufs2::keys::kBytesPerInode, ufs2::keys::kAmprIndex, ufs2::keys::kDeleteSourceAfterBuild})
            QVERIFY2(m_schema->contains(k), qPrintable(k));
        QCOMPARE(m_store->value(ufs2::keys::kBlockSize).toString(), QStringLiteral("65536"));
        QCOMPARE(m_store->value(ufs2::keys::kFragmentSize).toString(), QStringLiteral("65536"));
        QCOMPARE(m_store->value(ufs2::keys::kMinFreePercent).toInt(), 0);
        QCOMPARE(m_store->value(ufs2::keys::kBytesPerInode).toString(), QStringLiteral("262144"));
        QCOMPARE(m_store->value(ufs2::keys::kDeleteSourceAfterBuild).toBool(), false);
        // Only values UFS2Tool accepts.
        QVERIFY(!m_store->setValue(ufs2::keys::kBlockSize, QStringLiteral("131072")).ok());
        QVERIFY(!m_store->setValue(ufs2::keys::kMinFreePercent, 100).ok());
        for (const QString& k : {ufs2::kinds::kBuild, ufs2::kinds::kVerify, ufs2::kinds::kExtract, ufs2::kinds::kEdit})
            QVERIFY2(m_jobs.contains(k), qPrintable(k));
        QCOMPARE(formatParams(m_store->snapshot()).blockSize, 65536);
    }

    void importsLegacySettings()
    {
        const QString legacy = m_tmp.filePath("legacy-settings.json");
        QVERIFY(mktext(legacy, QJsonDocument(QJsonObject{{"adv_ffpkg_block", "32768"},
                                                         {"adv_ffpkg_frag", "65536"},
                                                         {"adv_ffpkg_minfree", "8"},
                                                         {"adv_ffpkg_inode", "524288"},
                                                         {"adv_delete_source_ffpkg", true}})
                                   .toJson()));
        SettingsStore store(m_schema, m_tmp.filePath("imported.json"));
        auto n = store.importLegacy(legacy);
        QVERIFY(n.ok());
        QCOMPARE(n.value(), 5);
        QCOMPARE(store.value(ufs2::keys::kBlockSize).toString(), QStringLiteral("32768"));
        QCOMPARE(store.value(ufs2::keys::kMinFreePercent).toInt(), 8);
        QCOMPARE(store.value(ufs2::keys::kBytesPerInode).toString(), QStringLiteral("524288"));
        QCOMPARE(store.value(ufs2::keys::kDeleteSourceAfterBuild).toBool(), true);
        // The 32K block with a 64K fragment is reduced to 32K/32K, as the legacy app did.
        QStringList notes;
        const FormatParams p = formatParams(store.snapshot(), &notes);
        QCOMPARE(p.fragmentSize, 32768);
        QCOMPARE(notes.size(), 1);
        // 128 KiB blocks (offered by the legacy UI, rejected by UFS2Tool) are not imported.
        QVERIFY(mktext(legacy, QJsonDocument(QJsonObject{{"adv_ffpkg_block", "131072"}}).toJson()));
        SettingsStore store2(m_schema, m_tmp.filePath("imported2.json"));
        QCOMPARE(store2.importLegacy(legacy).value(), 0);
        QCOMPARE(store2.value(ufs2::keys::kBlockSize).toString(), QStringLiteral("65536"));
    }

    void buildProducesVerifiedImage()
    {
        const QString src = freshGame("build-src");
        const QString out = m_tmp.filePath("out/Test Game.ffpkg");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out));
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("byte for byte")), qPrintable(o->verificationDetail));
        QCOMPARE(o->outputs, QStringList{out});
        QVERIFY(QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        // The structure check is the first part of Verifying, the compare the rest.
        QCOMPARE(m_phasesSeen, (QStringList{"Scanning", "Writing image", "Verifying", "Checking structure", "Verifying"}));
        QVERIFY(o->details.value("writeBytesPerSec").toDouble() > 0);
        QCOMPARE(o->details.value("blockSize").toInt(), 65536);
        auto r = Ufs2Reader::open(out);
        QVERIFY(r.ok());
        QCOMPARE(r.value()->sb().fsize, 65536);
        QCOMPARE(QFileInfo(out).size(), r.value()->sb().bytes());
    }

    void settingsReachTheImage()
    {
        const QString src = freshGame("settings-src");
        // Large enough (sparse) for -i to change the inode count: below
        // ~64 MiB every density rounds up to one block of inodes per group.
        {
            QFile f(src + "/data/sparse.bin");
            QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(80LL * 1024 * 1024));
        }
        const QString out = m_tmp.filePath("settings.ffpkg");
        const QVariantMap ov{{ufs2::keys::kBlockSize, "32768"},
                             {ufs2::keys::kFragmentSize, "65536"}, // larger than the block: reduced
                             {ufs2::keys::kMinFreePercent, 5},
                             {ufs2::keys::kBytesPerInode, "131072"}};
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out), ov);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->warnings.size(), 1);
        auto r = Ufs2Reader::open(out);
        QVERIFY(r.ok());
        const SuperblockInfo& sb = r.value()->sb();
        QCOMPARE(sb.bsize, 32768);
        QCOMPARE(sb.fsize, 32768);
        QCOMPARE(sb.minfree, 5);
        QCOMPARE(sb.ipg, inodesPerGroup(FormatParams{32768, 32768, 5, 131072}, sb.bytes()));
        QVERIFY(sb.ipg != inodesPerGroup(FormatParams{32768, 32768, 5, 524288}, sb.bytes()));
    }

    void verificationModes()
    {
        const QString src = freshGame("modes-src");
        const QString out = m_tmp.filePath("modes.ffpkg");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out), {{general::keys::kVerifyMode, "structure"}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("name, type and size")), qPrintable(o->verificationDetail));
        QVERIFY(QFile::remove(out));
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out), {{general::keys::kVerifyAfterBuild, false}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Skipped);
        QVERIFY(o->verificationDetail.contains(QStringLiteral("Structure check passed")));
        QVERIFY(QFileInfo::exists(out));
    }

    void failedVerificationNeverReachesTheFinalName()
    {
        // The source changes (same size, other bytes) after the image was
        // written: verification fails, nothing is saved, no .part is left.
        const QString src = freshGame("verifyfail-src");
        const QString out = m_tmp.filePath("verifyfail.ffpkg");
        bool tampered = false;
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out), {}, {}, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && !tampered) {
                tampered = true;
                QFile f(src + "/eboot.bin");
                if (f.open(QIODevice::ReadWrite)) {
                    f.seek(100);
                    f.write("tampered");
                }
            }
        });
        QVERIFY(tampered);
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Failed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("eboot.bin")), qPrintable(o->verificationDetail));
        QVERIFY(o->outputs.isEmpty());
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));

        // The same while replacing an image: the one that was there stays,
        // byte for byte.
        QVERIFY(mktext(out, "previous image"));
        tampered = false;
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {}, {}, [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && !tampered) {
                tampered = true;
                QFile f(src + "/eboot.bin");
                if (f.open(QIODevice::ReadWrite)) {
                    f.seek(200);
                    f.write("tampered again");
                }
            }
        });
        QVERIFY(tampered);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Failed);
        QVERIFY(o->outputs.isEmpty());
        QCOMPARE(readAll(out), QByteArray("previous image"));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
    }

    void cancelLeavesNoOutputAndKeepsTheOldImage()
    {
        const QString src = freshGame("cancel-src", 64 * 1024 * 1024);
        const QString out = m_tmp.filePath("cancel.ffpkg");
        // No previous image: nothing is left behind.
        CancellationSource cs;
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out), {}, cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.done > 4 * 1024 * 1024)
                cs.cancel();
        });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        // An existing image being replaced survives a cancelled rebuild (audit M2).
        QVERIFY(mktext(out, "previous image"));
        CancellationSource cs2;
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {}, cs2.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.done > 4 * 1024 * 1024)
                cs2.cancel();
        });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QCOMPARE(readAll(out), QByteArray("previous image"));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        // Cancelled during the scan.
        CancellationSource cs3;
        cs3.cancel();
        o = runJob(ufs2::kinds::kBuild, buildParams(src, m_tmp.filePath("cancel3.ffpkg")), {}, cs3.token());
        QVERIFY(!o.ok() && o.error().isCancelled());
        QVERIFY(!QFileInfo::exists(m_tmp.filePath("cancel3.ffpkg")));
    }

    // GitHub #97: Cancel while the finished image is verified saves it as
    // with verification turned off: the structure check still passes (it is
    // finished when the cancel came during it), the image replaces the old one
    // only now, the job does not fail and says the image is not verified. The
    // game folder is never deleted for it.
    void cancelDuringVerificationSavesTheImage()
    {
        const QString src = freshGame("vcancel-src", 64 * 1024 * 1024);
        const QString out = m_tmp.filePath("vcancel.ffpkg");
        // During the structure check (the first report of the stage), and while the bytes are compared.
        for (const qint64 after : {qint64(-1), qint64(4 * 1024 * 1024)}) {
            QVERIFY(mktext(out, "previous image"));
            CancellationSource cs;
            std::atomic<bool> cancelled{false};
            auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {{ufs2::keys::kDeleteSourceAfterBuild, true}}, cs.token(),
                            [&](const ProgressSnapshot& s) {
                                if (s.phaseIndex == 2 && s.done > after && !cancelled.exchange(true))
                                    cs.cancel();
                            });
            QVERIFY(cancelled);
            QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
            QCOMPARE(o->verification, Verification::Skipped);
            QVERIFY2(o->verificationDetail.contains(QStringLiteral("cancelled")), qPrintable(o->verificationDetail));
            QVERIFY(o->details.value("verificationCancelled").toBool());
            QCOMPARE(o->outputs, QStringList{out});
            QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
            QVERIFY(QFileInfo(src).isDir());
            QVERIFY(!o->details.contains("sourceDeleted"));
            auto check = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", out}, {"source", src}});
            QVERIFY2(check.ok() && check->verification == Verification::Passed,
                     check.ok() ? qPrintable(check->verificationDetail) : qPrintable(check.error().toLogString()));
        }
    }

    // Closing the app is no Cancel the user chose: while the new image is
    // verified it is not saved unverified, the previous one stays as it was
    // and the .part goes. The same when the user had cancelled and the
    // structure check that then still runs is under way: closing the app stops it.
    void closingTheAppDuringVerificationKeepsNothing()
    {
        const QString src = freshGame("vclose-src", 64 * 1024 * 1024);
        const QString out = m_tmp.filePath("vclose.ffpkg");
        for (const bool cancelledFirst : {false, true}) {
            QVERIFY(mktext(out, "previous image"));
            CancellationSource cs;
            std::atomic<bool> closed{false};
            auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {}, cs.token(), [&](const ProgressSnapshot& s) {
                if (s.phaseIndex != 2 || closed)
                    return;
                if (!cancelledFirst && s.done > 4 * 1024 * 1024) {
                    closed = true;
                    cs.shutdown();
                } else if (cancelledFirst && !cs.isCancelled()) {
                    cs.cancel(); // during the structure check, which then runs again to its end
                } else if (cancelledFirst && cs.token().finishedWorkKept()) {
                    closed = true;
                    cs.shutdown();
                }
            });
            QVERIFY2(closed, cancelledFirst ? "after a cancel" : "plain");
            QVERIFY(!o.ok());
            QVERIFY2(o.error().isCancelled(), qPrintable(o.error().toLogString()));
            QCOMPARE(readAll(out), QByteArray("previous image"));
            QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        }
    }

    // A difference found before the cancel still fails the build: the new
    // image is not saved (not even unverified) and the previous one stays.
    void aMismatchFoundBeforeTheCancelFailsTheBuild()
    {
        const QString src = freshGame("vcancel-bad-src", 64 * 1024 * 1024);
        const QString out = m_tmp.filePath("vcancel-bad.ffpkg");
        QVERIFY(mktext(out, "previous image"));
        CancellationSource cs;
        std::atomic<bool> changed{false}, cancelled{false};
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {}, cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex != 2)
                return;
            // Before anything is compared, eboot.bin (compared first, at the
            // top of the tree) changes; the cancel comes while data/big.pak is.
            if (!changed.exchange(true)) {
                QFile f(src + "/eboot.bin");
                char c = 0;
                if (f.open(QIODevice::ReadWrite) && f.seek(1000) && f.getChar(&c) && f.seek(1000))
                    f.putChar(char(c ^ 0x55));
            } else if (s.done > 4 * 1024 * 1024 && !cancelled.exchange(true)) {
                cs.cancel();
            }
        });
        QVERIFY(cancelled);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Failed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("eboot.bin")), qPrintable(o->verificationDetail));
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("cancelled")), qPrintable(o->verificationDetail));
        QVERIFY(!o->details.contains("verificationCancelled"));
        QVERIFY(o->outputs.isEmpty());
        QCOMPARE(readAll(out), QByteArray("previous image"));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
    }

    void extractCancelDuringVerificationKeepsTheFiles()
    {
        const QString src = freshGame("xvcancel-src", 64 * 1024 * 1024);
        const QString img = m_tmp.filePath("xvcancel.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString dest = m_tmp.filePath("xvcancel-out/Game");
        CancellationSource cs;
        std::atomic<bool> cancelled{false};
        auto o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest}}, {}, cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 2 && s.done > 4 * 1024 * 1024 && !cancelled.exchange(true))
                cs.cancel();
        });
        QVERIFY(cancelled);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Skipped);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("not verified")), qPrintable(o->verificationDetail));
        QCOMPARE(o->outputs, QStringList{dest});
        QCOMPARE(snapshotFolder(dest), snapshotFolder(src));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
    }

    // What a power cut leaves: a staging folder with the job's marker in it
    // is replaced by the next run; one without the marker is refused, not touched.
    void extractLeftoverStagingIsOursOnlyWithTheMarker()
    {
        const QString src = freshGame("leftover-src");
        const QString img = m_tmp.filePath("leftover.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString ours = m_tmp.filePath("leftover-out/ours");
        QVERIFY(mktext(ours + ".part/half.bin", "half of an earlier run"));
        QVERIFY(mktext(ours + ".part/.fw-extract-staging", ""));
        auto o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", ours}});
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(snapshotFolder(ours), snapshotFolder(src));
        QVERIFY(!QFileInfo::exists(ours + ".part"));
        const QString theirs = m_tmp.filePath("leftover-out/theirs");
        QVERIFY(mktext(theirs + ".part/theirs.txt", "theirs"));
        o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", theirs}});
        FWTEST_COMPARE_ERROR(o, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(theirs + ".part/theirs.txt"), QByteArray("theirs"));
        QVERIFY(!QFileInfo::exists(theirs));
    }

    // An edit whose verification is cancelled is applied: the names and sizes
    // of the edited tree are still checked (what runs with verification
    // turned off), the contents are not, and the outcome says so.
    void editCancelDuringVerificationIsApplied()
    {
        const QString src = freshGame("evcancel-src", 64 * 1024 * 1024);
        const QString img = m_tmp.filePath("evcancel.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        CancellationSource cs;
        std::atomic<bool> cancelled{false};
        auto o = runJob(ufs2::kinds::kEdit, QJsonObject{{"image", img}, {"operations", QJsonArray{QJsonObject{{"op", "mkdir"}, {"path", "/added"}}}}},
                        {}, cs.token(), [&](const ProgressSnapshot& s) {
                            if (s.phaseIndex == 2 && s.done > 4 * 1024 * 1024 && !cancelled.exchange(true))
                                cs.cancel();
                        });
        QVERIFY(cancelled);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Skipped);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("cancelled")), qPrintable(o->verificationDetail));
        QCOMPARE(o->outputs, QStringList{img});
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(img)));
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        QVERIFY(tree->find("/added") >= 0);
        QCOMPARE(tree->nodes[size_t(tree->find("/data/big.pak"))].size, qint64(64 * 1024 * 1024));
    }

    void replaceExistingOnlyWhenAsked()
    {
        const QString src = freshGame("replace-src");
        const QString out = m_tmp.filePath("replace.ffpkg");
        QVERIFY(mktext(out, "old"));
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out));
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(out), QByteArray("old"));
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true));
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY(Ufs2Reader::open(out).ok());
        QVERIFY(!QFileInfo::exists(out + ".old"));
    }

    // The old image is open in another program, so the new one cannot take
    // its name. The finished, verified image is kept next to it.
    void finishedImageIsKeptWhenTheOldOneCannotBeReplaced()
    {
#ifndef Q_OS_WIN
        QSKIP("An open file can be replaced on this platform.");
#endif
        const QString src = freshGame("locked-src");
        const QString out = m_tmp.filePath("locked.ffpkg");
        QVERIFY(mktext(out, "old"));
        QFile holder(out);
        QVERIFY(holder.open(QIODevice::ReadOnly)); // opened without delete sharing
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true));
        holder.close();
        QVERIFY(!o.ok());
        const QString kept = m_tmp.filePath("locked (new).ffpkg");
        QVERIFY2(o.error().message.contains("built and verified"), qPrintable(o.error().toLogString()));
        QVERIFY2(o.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(o.error().toLogString()));
        QCOMPARE(readAll(out), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(out)));
        auto r = Ufs2Reader::open(kept);
        QVERIFY(r.ok());
        CancellationToken token;
        const auto c = checkImage(*r.value(), token);
        QVERIFY(c.ok() && c->ok());

        // The same for an edit, which always rebuilds.
        r = Error(ErrorCode::Internal, QString());
        const QByteArray before = sha(kept);
        holder.setFileName(kept);
        QVERIFY(holder.open(QIODevice::ReadOnly));
        o = runJob(ufs2::kinds::kEdit,
                   QJsonObject{{"image", kept}, {"operations", QJsonArray{QJsonObject{{"op", "mkdir"}, {"path", "/added"}}}}});
        holder.close();
        QVERIFY(!o.ok());
        const QString keptEdit = m_tmp.filePath("locked (new) (new).ffpkg");
        QVERIFY2(o.error().message.contains("edited image was built and verified"), qPrintable(o.error().toLogString()));
        QVERIFY2(o.error().message.contains(QDir::toNativeSeparators(keptEdit)), qPrintable(o.error().toLogString()));
        QCOMPARE(sha(kept), before);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(kept)));
        r = Ufs2Reader::open(keptEdit);
        QVERIFY(r.ok());
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        QVERIFY(tree->find("/added") >= 0);
    }

    // The chosen (empty) folder cannot be replaced because a program has it
    // open: the extracted, verified files stay, under "<folder>.new".
    void extractedFilesAreKeptWhenTheFolderCannotBeUsed()
    {
#ifndef Q_OS_WIN
        QSKIP("A folder in use can be removed on this platform.");
#endif
        const QString src = freshGame("held-src");
        const QString img = m_tmp.filePath("held.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString dest = m_tmp.filePath("held-out");
        QVERIFY(QDir().mkpath(dest));
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dest)); // the current folder of a process cannot be removed
        auto o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest}});
        QVERIFY(QDir::setCurrent(before));
        QVERIFY(!o.ok());
        const QString kept = dest + ".new";
        QVERIFY2(o.error().message.contains("extracted and verified"), qPrintable(o.error().toLogString()));
        QVERIFY2(o.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(o.error().toLogString()));
        QCOMPARE(snapshotFolder(kept), snapshotFolder(src));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
    }

    void validationRefusesBadInput()
    {
        const QString src = freshGame("validate-src");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, src + "/inside.ffpkg"));
        FWTEST_COMPARE_ERROR(o, ErrorCode::InvalidArgument);
        o = runJob(ufs2::kinds::kBuild, buildParams(m_tmp.filePath("nope"), m_tmp.filePath("x.ffpkg")));
        FWTEST_COMPARE_ERROR(o, ErrorCode::NotFound);
        o = runJob(ufs2::kinds::kBuild, buildParams(src, m_tmp.path()));
        FWTEST_COMPARE_ERROR(o, ErrorCode::InvalidArgument);
        const QString empty = m_tmp.filePath("empty-src");
        QVERIFY(QDir().mkpath(empty));
        o = runJob(ufs2::kinds::kBuild, buildParams(empty, m_tmp.filePath("empty.ffpkg")));
        FWTEST_COMPARE_ERROR(o, ErrorCode::InvalidArgument);
    }

    // The image plus the reserve must fit the drive before anything is
    // written; an interrupted build's leftover is removed first. The reserve
    // is set to its largest value (64 GiB); on a drive
    // with more room than that, a sparse file makes the image large enough to
    // exceed the rest, so the refusal does not depend on how full this PC is.
    void freeSpaceIsChecked()
    {
        const qint64 gib = 1024LL * 1024 * 1024;
        const qint64 available = QStorageInfo(m_tmp.path()).bytesAvailable();
        const qint64 filler = available - 60 * gib; // the reserve covers the last 60 GiB with room to spare
        if (filler > 1024 * gib)
            QSKIP("more than a terabyte is free here: planning an image that large takes too long for a test");
        const QString src = freshGame("space-src");
        QVERIFY(!src.isEmpty());
        const QString out = m_tmp.filePath("space.ffpkg");
        // Leaves no sparse file behind for the tests that follow.
        const auto cleanup = qScopeGuard([&] { QFile::remove(src + "/data/filler.bin"); });
        if (filler > 0) {
            const qint64 allocated = fwtest::makeSparseFile(src + "/data/filler.bin", filler);
            if (allocated < 0 || allocated > 64 * 1024 * 1024)
                QSKIP("this file system does not create sparse files");
        }
        // What an interrupted build of this image left goes before the check,
        // which must count its space as free (no file is left after it).
        QVERIFY(mktext(PartFile::partPathFor(out), "left by a crash"));
        QVERIFY(m_store->setValue(general::keys::kFreeSpaceMarginMiB, 65536).ok());
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out));
        m_store->reset(general::keys::kFreeSpaceMarginMiB);
        FWTEST_COMPARE_ERROR(o, ErrorCode::InsufficientSpace);
        QVERIFY2(o.error().message.startsWith(QStringLiteral("Not enough space on")), qPrintable(o.error().message));
        QVERIFY(!QFileInfo::exists(out) && !QFileInfo::exists(PartFile::partPathFor(out)));
    }

    void deleteSourceOnlyAfterFullVerification()
    {
        const QString src = freshGame("delete-src");
        const QString out = m_tmp.filePath("delete.ffpkg");
        // Structure-only verification: the folder is kept.
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out),
                        {{ufs2::keys::kDeleteSourceAfterBuild, true}, {general::keys::kVerifyMode, "structure"}});
        QVERIFY(o.ok());
        QVERIFY(QFileInfo(src).isDir());
        QVERIFY(o->warnings.join(u' ').contains(QStringLiteral("kept")));
        // No eboot.bin: kept.
        QVERIFY(QFile::rename(src + "/eboot.bin", src + "/eboot.bak"));
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {{ufs2::keys::kDeleteSourceAfterBuild, true}});
        QVERIFY(o.ok());
        QVERIFY(QFileInfo(src).isDir());
        QVERIFY(QFile::rename(src + "/eboot.bak", src + "/eboot.bin"));
        // Full verification passed and eboot.bin present: deleted.
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {{ufs2::keys::kDeleteSourceAfterBuild, true}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY(!QFileInfo::exists(src));
        QVERIFY(o->details.value("sourceDeleted").toBool());
        QVERIFY(Ufs2Reader::open(out).ok());
    }

    void verifyJob()
    {
        const QString src = freshGame("vj-src");
        const QString img = m_tmp.filePath("vj.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        auto o = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", img}, {"source", src}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        // Without a source: structure check and every file read back.
        o = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", img}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("read back")), qPrintable(o->verificationDetail));
        // A changed, a missing and an extra file.
        QVERIFY(mktext(src + "/sce_sys/param.json", "{}"));
        QVERIFY(QFile::remove(src + "/.hidden"));
        QVERIFY(mktext(src + "/extra.txt", "extra"));
        o = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", img}, {"source", src}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Failed);
        QVERIFY2(o->verificationDetail.contains(QStringLiteral("param.json")) && o->verificationDetail.contains(QStringLiteral("extra.txt"))
                     && o->verificationDetail.contains(QStringLiteral(".hidden")),
                 qPrintable(o->verificationDetail));
        // A damaged image is a failed verification, not a crashed job.
        const QString bad = m_tmp.filePath("vj-bad.ffpkg");
        QFile::remove(bad);
        QVERIFY(QFile::copy(img, bad));
        {
            QFile f(bad);
            QVERIFY(f.open(QIODevice::ReadWrite) && f.seek(kSuperblockOffset + 0x55C));
            f.write("XXXX");
        }
        o = runJob(ufs2::kinds::kVerify, QJsonObject{{"image", bad}});
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Failed);
    }

    void extractJob()
    {
        const QString src = freshGame("ex-src");
        QVERIFY(mkfile(src + QStringLiteral("/Ünïcödé/日本.bin"), 5000, 4));
        const QString img = m_tmp.filePath("ex.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString dest = m_tmp.filePath("ex-out/Game");
        auto o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest}});
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
        QCOMPARE(m_phasesSeen, (QStringList{"Reading image", "Extracting", "Verifying"}));
        QCOMPARE(snapshotFolder(dest), snapshotFolder(src));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
        // An existing, non-empty destination is refused.
        o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest}});
        FWTEST_COMPARE_ERROR(o, ErrorCode::AlreadyExists);
        // One folder only.
        const QString sub = m_tmp.filePath("ex-out/sce_sys");
        o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", sub}, {"path", "/sce_sys"}});
        QVERIFY(o.ok());
        QCOMPARE(readAll(sub + "/param.json"), readAll(src + "/sce_sys/param.json"));
        // Cancelled: no destination and no staging folder.
        const QString dest2 = m_tmp.filePath("ex-out/Cancelled");
        CancellationSource cs;
        o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest2}}, {}, cs.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.filesDone >= 2)
                cs.cancel();
        });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QVERIFY(!QFileInfo::exists(dest2));
        QVERIFY(!QFileInfo::exists(dest2 + ".part"));
    }

    void editJob()
    {
        const QString src = freshGame("edit-src");
        const QString img = m_tmp.filePath("edit.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString local = m_tmp.filePath("edit-local");
        QVERIFY(mktext(local + "/param.json", "{\"edited\":true}"));
        QVERIFY(mkfile(local + "/dlc/content.bin", 777777, 3));
        const QJsonArray ops{
            QJsonObject{{"op", "replace"}, {"path", "/sce_sys/param.json"}, {"source", local + "/param.json"}},
            QJsonObject{{"op", "add"}, {"path", "/dlc"}, {"source", local + "/dlc"}},
            QJsonObject{{"op", "delete"}, {"path", "/data/big.pak"}},
            QJsonObject{{"op", "rename"}, {"path", "/emptydir"}, {"newName", "renamed"}},
            QJsonObject{{"op", "mkdir"}, {"path", "/sce_sys/about"}},
        };
        auto o = runJob(ufs2::kinds::kEdit, QJsonObject{{"image", img}, {"operations", ops}});
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
        QCOMPARE(m_phasesSeen, (QStringList{"Reading image", "Writing image", "Verifying", "Checking structure", "Verifying", "Replacing original"}));
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(img)) && !QFileInfo::exists(img + ".old"));
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        QVERIFY(tree->find("/data/big.pak") < 0);
        QVERIFY(tree->find("/renamed") >= 0 && tree->find("/emptydir") < 0);
        QVERIFY(tree->find("/sce_sys/about") >= 0);
        QCOMPARE(tree->nodes[size_t(tree->find("/dlc/content.bin"))].size, qint64(777777));
        auto s = r.value()->openFile(r.value()->resolve("/sce_sys/param.json").value());
        QByteArray buf(100, '\0');
        buf.resize(int(s.value()->read(buf.data(), 100).value()));
        QCOMPARE(buf, QByteArray("{\"edited\":true}"));
        r = Error(ErrorCode::Internal, QString());
        // A failing edit leaves the original byte-identical and no .part.
        const QByteArray before = sha(img);
        o = runJob(ufs2::kinds::kEdit, QJsonObject{{"image", img},
                                                    {"operations", QJsonArray{QJsonObject{{"op", "delete"}, {"path", "/sce_sys/icon0.png"}},
                                                                              QJsonObject{{"op", "add"}, {"path", "/eboot.bin"}, {"source", local + "/param.json"}}}}});
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::AlreadyExists);
        QCOMPARE(sha(img), before);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(img)));
        // An edit cancelled while the new image is written too (a cancel
        // during its verification keeps the edit: editCancelDuringVerificationIsApplied).
        CancellationSource cs;
        o = runJob(ufs2::kinds::kEdit,
                   QJsonObject{{"image", img}, {"operations", QJsonArray{QJsonObject{{"op", "mkdir"}, {"path", "/x"}}}}}, {}, cs.token(),
                   [&](const ProgressSnapshot& snap) {
                       if (snap.phaseIndex == 1)
                           cs.cancel();
                   });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QCOMPARE(sha(img), before);
        QVERIFY(!QFileInfo::exists(PartFile::partPathFor(img)));
    }

    // What the job monitor is given for every job kind here, with every
    // verification setting: the bar never falls back, never stands full while
    // a stage still has work to do, and has reached the end of the work when
    // the job ends (a verification that is off, or compares names and sizes
    // only, takes no or only a small share). When the structure check and the
    // compare share Verifying, the check is its first half and the compare,
    // the long part, fills the rest instead of running at "99 %".
    void progressRunsToTheEndAndNeverFallsBack()
    {
        const QString src = freshGame("progress-src", 8 * 1024 * 1024);
        const auto audited = [&](const QString& kind, const QJsonObject& params, const QVariantMap& overrides,
                                 fwtest::ProgressAudit& audit) -> QString {
            auto r = runJob(kind, params, overrides, {}, {}, &audit);
            if (!r.ok())
                return r.error().toLogString();
            if (r->verification == Verification::Failed)
                return r->verificationDetail;
            return audit.problems().join(u'\n');
        };
        // The bar and the stage's own share when the compare began after the check.
        const auto compareStart = [](const fwtest::ProgressAudit& audit) {
            bool checked = false;
            for (const ProgressSnapshot& s : audit.seen()) {
                checked = checked || s.phase == QLatin1String("Checking structure");
                if (checked && s.phase == QLatin1String("Verifying"))
                    return std::pair{s.overallFraction, s.phaseProgress};
            }
            return std::pair{-1.0, -1.0};
        };
        const QVariantMap off{{general::keys::kVerifyAfterBuild, false}};
        const QVariantMap structure{{general::keys::kVerifyMode, QStringLiteral("structure")}};
        const QString img = m_tmp.filePath("progress/full.ffpkg");
        for (const auto& [name, overrides] : {std::pair{"full", QVariantMap()}, std::pair{"off", off}, std::pair{"structure", structure}}) {
            fwtest::ProgressAudit audit;
            const QString out = m_tmp.filePath(QStringLiteral("progress/%1.ffpkg").arg(QLatin1String(name)));
            const QString problems = audited(ufs2::kinds::kBuild, buildParams(src, out), overrides, audit);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("build, verification %1: %2").arg(QLatin1String(name), problems)));
            if (overrides.isEmpty()) {
                const auto [bar, stage] = compareStart(audit);
                QVERIFY2(stage >= 0.0 && stage <= 0.51, qPrintable(QString::number(stage)));
                QVERIFY2(bar > 0.6 && bar < 0.83, qPrintable(QString::number(bar)));
            }
        }
        for (const auto& [name, overrides] : {std::pair{"full", QVariantMap()}, std::pair{"off", off}, std::pair{"structure", structure}}) {
            fwtest::ProgressAudit audit;
            const QJsonObject p{{"image", img}, {"output", m_tmp.filePath(QStringLiteral("progress/x-%1").arg(QLatin1String(name)))}};
            const QString problems = audited(ufs2::kinds::kExtract, p, overrides, audit);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("extract, verification %1: %2").arg(QLatin1String(name), problems)));
        }
        for (const auto& [name, overrides] : {std::pair{"full", QVariantMap()}, std::pair{"structure", structure}}) {
            fwtest::ProgressAudit withFolder, alone;
            QString problems = audited(ufs2::kinds::kVerify, QJsonObject{{"image", img}, {"source", src}}, overrides, withFolder);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("verify, %1: %2").arg(QLatin1String(name), problems)));
            problems = audited(ufs2::kinds::kVerify, QJsonObject{{"image", img}}, overrides, alone);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("verify alone, %1: %2").arg(QLatin1String(name), problems)));
        }
        for (const auto& [name, overrides] : {std::pair{"full", QVariantMap()}, std::pair{"off", off}}) {
            const QString local = m_tmp.filePath(QStringLiteral("progress-local/%1.bin").arg(QLatin1String(name)));
            QVERIFY(mkfile(local, 300000, 7));
            const QJsonArray ops{QJsonObject{{"op", "add"}, {"path", QStringLiteral("/%1.bin").arg(QLatin1String(name))}, {"source", local}}};
            fwtest::ProgressAudit audit;
            const QString problems = audited(ufs2::kinds::kEdit, QJsonObject{{"image", img}, {"operations", ops}}, overrides, audit);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("edit, verification %1: %2").arg(QLatin1String(name), problems)));
            if (overrides.isEmpty()) {
                const auto [bar, stage] = compareStart(audit);
                QVERIFY2(stage >= 0.0 && stage <= 0.51, qPrintable(QString::number(stage)));
                QVERIFY2(bar > 0.5 && bar < 0.76, qPrintable(QString::number(bar)));
            }
        }
    }

    // The queue declares a job's shares for its own settings before run(),
    // and a job that runs an image build (zstd packs) takes the build's shares
    // from it: they are the shares the job runs with, for every verification
    // setting. Each stage the job enters begins where those shares put it.
    void declaredSharesAreTheOnesTheJobRuns()
    {
        const QString src = freshGame("shares-src");
        const QString img = m_tmp.filePath("shares/img.ffpkg");
        {
            auto built = runJob(ufs2::kinds::kBuild, buildParams(src, img));
            QVERIFY2(built.ok(), built.ok() ? "" : qPrintable(built.error().toLogString()));
        }
        const QVariantMap off{{general::keys::kVerifyAfterBuild, false}};
        const QVariantMap structure{{general::keys::kVerifyMode, QStringLiteral("structure")}};
        struct Case {
            QString what, kind;
            QJsonObject params;
        };
        int n = 0;
        const auto next = [&](const QString& name) { return m_tmp.filePath(QStringLiteral("shares/%1-%2").arg(name).arg(++n)); };
        for (const auto& [name, overrides] : {std::pair{"full", QVariantMap()}, std::pair{"off", off}, std::pair{"structure", structure}}) {
            const QJsonArray ops{QJsonObject{{"op", "mkdir"}, {"path", QStringLiteral("/added-%1").arg(QLatin1String(name))}}};
            const QString editable = next(QStringLiteral("edit")) + QStringLiteral(".ffpkg");
            QVERIFY(QFile::copy(img, editable));
            const std::vector<Case> cases = {
                {QStringLiteral("build"), ufs2::kinds::kBuild, buildParams(src, next(QStringLiteral("build")) + QStringLiteral(".ffpkg"))},
                {QStringLiteral("extract"), ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", next(QStringLiteral("x"))}}},
                {QStringLiteral("verify"), ufs2::kinds::kVerify, QJsonObject{{"image", img}, {"source", src}}},
                {QStringLiteral("verify alone"), ufs2::kinds::kVerify, QJsonObject{{"image", img}}},
                {QStringLiteral("edit"), ufs2::kinds::kEdit, QJsonObject{{"image", editable}, {"operations", ops}}},
            };
            for (const Case& c : cases) {
                const QString what = QStringLiteral("%1, verification %2").arg(c.what, QLatin1String(name));
                auto job = m_jobs.create(c.kind, c.params, overrides);
                QVERIFY2(job.ok(), qPrintable(what));
                auto normalized = m_store->normalizeOverrides(overrides);
                QVERIFY(normalized.ok());
                const QList<double> declared = (*job)->phaseWeights(m_store->snapshot(*normalized));
                QCOMPARE(declared.size(), (*job)->phases().size());
                double sum = 0.0;
                for (const double d : declared)
                    sum += d;
                QVERIFY(sum > 0.0);
                fwtest::ProgressAudit audit;
                auto r = runJob(c.kind, c.params, overrides, {}, {}, &audit);
                QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(what + u' ' + r.error().toLogString()));
                QVERIFY2(audit.problems().isEmpty(), qPrintable(what + QStringLiteral(": ") + audit.problems().join(u'\n')));
                const QStringList names = (*job)->phases();
                double before = 0.0;
                for (int i = 0; i < names.size(); ++i) {
                    const double at = audit.fractionWhen([&](const ProgressSnapshot& s) { return s.phaseNames == names && s.phaseIndex == i; });
                    if (at >= 0.0)
                        QVERIFY2(std::abs(at - before / sum) < 0.005,
                                 qPrintable(QStringLiteral("%1: stage %2 began at %3, declared %4").arg(what).arg(i).arg(at).arg(before / sum)));
                    before += declared[i];
                }
            }
        }
    }

    void summariesCountInTheSingular()
    {
        CheckReport check;
        check.files = 1;
        check.dirs = 1;
        QCOMPARE(check.summary(), QStringLiteral("File system consistent: 1 file and 1 folder."));
        check.files = 2414;
        check.dirs = 12;
        QCOMPARE(check.summary(), QStringLiteral("File system consistent: 2,414 files and 12 folders."));
        CompareReport cmp;
        cmp.filesCompared = 1;
        cmp.dirsCompared = 3;
        QCOMPARE(cmp.summary(CompareDepth::Structure), QStringLiteral("1 file and 3 folders match by name, type and size."));
    }

    // The staging marker is compared the way the destination drive compares
    // names: an entry with that name in another case is refused too.
    void extractRefusesTheMarkerNameInAnyCase()
    {
        const QString src = freshGame("marker-src");
        QVERIFY(mktext(src + "/.FW-EXTRACT-STAGING", "a real file of the game"));
        const QString img = m_tmp.filePath("marker.ffpkg");
        QVERIFY(runJob(ufs2::kinds::kBuild, buildParams(src, img)).ok());
        const QString dest = m_tmp.filePath("marker-out");
        auto o = runJob(ufs2::kinds::kExtract, QJsonObject{{"image", img}, {"output", dest}});
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::Unsupported);
        QVERIFY2(o.error().message.contains(".FW-EXTRACT-STAGING"), qPrintable(o.error().toLogString()));
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(!QFileInfo::exists(dest + ".part"));
    }

    // "Delete the game folder" with a zipped dump neither deletes the ZIP nor
    // reports the temporary unpacked copy as the deleted source.
    void zippedSourceIsNeverDeleted()
    {
        const QString src = freshGame("zip-src");
        const QString zipPath = m_tmp.filePath("ZippedGame.zip");
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
        const QString img = m_tmp.filePath("zipped.ffpkg");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(zipPath, img), {{ufs2::keys::kDeleteSourceAfterBuild, true}});
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY(QFileInfo::exists(zipPath));
        QVERIFY(!o->details.contains("sourceDeleted"));
        QVERIFY2(o->warnings.join(u'\n').contains("was kept: only folders are deleted"), qPrintable(o->warnings.join(u'\n')));
    }

    // A Windows shortcut is a small file to copy as it is, whatever it points at.
    void windowsShortcutsAreCopiedAsFiles()
    {
#ifndef Q_OS_WIN
        QSKIP("Shortcut files are a Windows matter.");
#endif
        const QString src = freshGame("lnk-src");
        const QString elsewhere = m_tmp.filePath("lnk-target-folder");
        QVERIFY(mkfile(elsewhere + "/big.bin", 100 * 1024, 7));
        if (!QFile::link(elsewhere, src + "/folder.lnk") || !QFile::link(m_tmp.filePath("no-such-file.bin"), src + "/dead.lnk"))
            QSKIP("Shortcuts cannot be created here.");
        CancellationToken token;
        auto tree = scanLocalFolder(src, token);
        QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
        for (const char* name : {"/folder.lnk", "/dead.lnk"}) {
            const int idx = tree->find(name);
            QVERIFY2(idx > 0, name);
            const TreeNode& n = tree->nodes[size_t(idx)];
            QVERIFY(!n.isDir());
            QFile raw(src + name);
            QVERIFY(raw.open(QIODevice::ReadOnly));
            QVERIFY(raw.size() > 0);
            QCOMPARE(n.size, raw.size());
        }
        QVERIFY(tree->find("/folder.lnk/big.bin") < 0);
        const QString img = m_tmp.filePath("lnk.ffpkg");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, img));
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
        QCOMPARE(o->verification, Verification::Passed);
    }

    void runsThroughTheQueue()
    {
        const QString src = freshGame("queue-src");
        const QString out = m_tmp.filePath("queue.ffpkg");
        JobQueue queue(*m_store, m_jobs);
        auto id = queue.enqueue(ufs2::kinds::kBuild, buildParams(src, out));
        QVERIFY2(id.ok(), id.ok() ? "" : qPrintable(id.error().toLogString()));
        QVERIFY(QTest::qWaitFor([&] {
            auto rec = queue.record(id.value());
            return rec && isTerminal(rec->state);
        }, 300000));
        const auto rec = queue.record(id.value());
        QVERIFY(rec.has_value());
        QCOMPARE(rec->state, JobState::Succeeded);
        QVERIFY(rec->outcome.has_value());
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        QVERIFY(QFileInfo::exists(out));
    }
};

QTEST_GUILESS_MAIN(TestUfs2Jobs)
#include "tst_ufs2_jobs.moc"
