// The image.ffpkg.* jobs: settings reach the image, verification gates the
// final name (audit H6), cancellation and failures leave no output, an
// existing image survives a failed rebuild (M2), the danger-zone delete, the
// verify / extract / edit jobs, legacy settings import and a queue run.

#include "Ufs2TestUtil.h"

#include "core/Settings.h"
#include "engines/general/Module.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QTemporaryDir>

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

    Result<JobOutcome> runJob(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {},
                              const CancellationToken& token = {}, Sink sink = {})
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
                if (sink)
                    sink(s);
            },
            0);
        rep.setPhases(job->phases());
        TempDir temp(QStringLiteral("ufs2-test"), m_tmp.path());
        JobContext ctx;
        ctx.jobId = QStringLiteral("test");
        ctx.token = token;
        ctx.progress = &rep;
        ctx.log = Log(kind, QStringLiteral("test"));
        ctx.settings = sv;
        ctx.tempDir = temp.path();
        return job->run(ctx);
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
                                 ufs2::keys::kBytesPerInode, ufs2::keys::kDeleteSourceAfterBuild})
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
        QVERIFY(!QFileInfo::exists(out + ".part"));
        QCOMPARE(m_phasesSeen, (QStringList{"Scanning", "Writing image", "Verifying"}));
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
        QVERIFY(!QFileInfo::exists(out + ".part"));
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
        QVERIFY(!QFileInfo::exists(out + ".part"));
        // An existing image being replaced survives a cancelled rebuild (audit M2).
        QVERIFY(mktext(out, "previous image"));
        CancellationSource cs2;
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true), {}, cs2.token(), [&](const ProgressSnapshot& s) {
            if (s.phaseIndex == 1 && s.done > 4 * 1024 * 1024)
                cs2.cancel();
        });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QCOMPARE(readAll(out), QByteArray("previous image"));
        QVERIFY(!QFileInfo::exists(out + ".part"));
        // Cancelled during the scan.
        CancellationSource cs3;
        cs3.cancel();
        o = runJob(ufs2::kinds::kBuild, buildParams(src, m_tmp.filePath("cancel3.ffpkg")), {}, cs3.token());
        QVERIFY(!o.ok() && o.error().isCancelled());
        QVERIFY(!QFileInfo::exists(m_tmp.filePath("cancel3.ffpkg")));
    }

    void replaceExistingOnlyWhenAsked()
    {
        const QString src = freshGame("replace-src");
        const QString out = m_tmp.filePath("replace.ffpkg");
        QVERIFY(mktext(out, "old"));
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out));
        QVERIFY(!o.ok());
        QCOMPARE(o.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(out), QByteArray("old"));
        o = runJob(ufs2::kinds::kBuild, buildParams(src, out, true));
        QVERIFY(o.ok());
        QCOMPARE(o->verification, Verification::Passed);
        QVERIFY(Ufs2Reader::open(out).ok());
        QVERIFY(!QFileInfo::exists(out + ".old"));
    }

    void validationRefusesBadInput()
    {
        const QString src = freshGame("validate-src");
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, src + "/inside.ffpkg"));
        QCOMPARE(o.error().code, ErrorCode::InvalidArgument);
        o = runJob(ufs2::kinds::kBuild, buildParams(m_tmp.filePath("nope"), m_tmp.filePath("x.ffpkg")));
        QCOMPARE(o.error().code, ErrorCode::NotFound);
        o = runJob(ufs2::kinds::kBuild, buildParams(src, m_tmp.path()));
        QCOMPARE(o.error().code, ErrorCode::InvalidArgument);
        const QString empty = m_tmp.filePath("empty-src");
        QVERIFY(QDir().mkpath(empty));
        o = runJob(ufs2::kinds::kBuild, buildParams(empty, m_tmp.filePath("empty.ffpkg")));
        QCOMPARE(o.error().code, ErrorCode::InvalidArgument);
    }

    void freeSpaceIsChecked()
    {
        QStorageInfo si(m_tmp.path());
        if (si.bytesAvailable() > 60LL * 1024 * 1024 * 1024)
            QSKIP("more than 60 GiB free here; the 64 GiB reserve cannot be exceeded");
        const QString src = freshGame("space-src");
        const QString out = m_tmp.filePath("space.ffpkg");
        QVERIFY(m_store->setValue(general::keys::kFreeSpaceMarginMiB, 65536).ok());
        auto o = runJob(ufs2::kinds::kBuild, buildParams(src, out));
        m_store->reset(general::keys::kFreeSpaceMarginMiB);
        QVERIFY(!o.ok());
        QCOMPARE(o.error().code, ErrorCode::InsufficientSpace);
        QVERIFY(!QFileInfo::exists(out) && !QFileInfo::exists(out + ".part"));
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
        QCOMPARE(o.error().code, ErrorCode::AlreadyExists);
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
        QCOMPARE(m_phasesSeen, (QStringList{"Reading image", "Writing image", "Verifying", "Replacing original"}));
        QVERIFY(!QFileInfo::exists(img + ".part") && !QFileInfo::exists(img + ".old"));
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
        QCOMPARE(o.error().code, ErrorCode::AlreadyExists);
        QCOMPARE(sha(img), before);
        QVERIFY(!QFileInfo::exists(img + ".part"));
        // A cancelled edit too.
        CancellationSource cs;
        o = runJob(ufs2::kinds::kEdit,
                   QJsonObject{{"image", img}, {"operations", QJsonArray{QJsonObject{{"op", "mkdir"}, {"path", "/x"}}}}}, {}, cs.token(),
                   [&](const ProgressSnapshot& snap) {
                       if (snap.phaseIndex == 2)
                           cs.cancel();
                   });
        QVERIFY(!o.ok() && o.error().isCancelled());
        QCOMPARE(sha(img), before);
        QVERIFY(!QFileInfo::exists(img + ".part"));
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
        }, 120000));
        const auto rec = queue.record(id.value());
        QCOMPARE(rec->state, JobState::Succeeded);
        QVERIFY(rec->outcome.has_value());
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        QVERIFY(QFileInfo::exists(out));
    }
};

QTEST_GUILESS_MAIN(TestUfs2Jobs)
#include "tst_ufs2_jobs.moc"
