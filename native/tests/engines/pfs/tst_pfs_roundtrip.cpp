// PFS engine: build → read → compare round trips, strict verification
// (changed / missing / extra files, exclusions), corruption detection,
// extraction, short (Lazy RAM-mode) images and thread-count determinism.

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsReader.h"
#include "engines/pfs/PfsSource.h"
#include "engines/pfs/PfsVerify.h"

#include "core/FileUtil.h"
#include "engines/pfs/PfsTestUtil.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::pfs;

namespace {

constexpr qint64 kTime = 1700000000;

BuildOptions options(int threads = 2, int level = 6)
{
    BuildOptions o;
    o.layout.time = kTime;
    o.threads = threads;
    o.level = level;
    return o;
}

PlanOptions planOptions()
{
    PlanOptions p;
    p.time = kTime;
    p.keepEmptyFolders = true;
    return p;
}

// The plan for `root`. A folder that cannot be planned fails the test here
// (the other test functions still run) and gives an empty plan.
SourcePlan plan(const QString& root, const PlanOptions& po = planOptions())
{
    auto r = planFolder(root, po, CancellationToken());
    if (!r.ok()) {
        QTest::qFail(qPrintable(QStringLiteral("plan failed: ") + r.error().toLogString()), __FILE__, __LINE__);
        return {};
    }
    return std::move(r).value();
}

QString errText(const Error& e) { return e.toLogString(); }

std::unique_ptr<PfsImage> openImage(const QString& path, std::shared_ptr<FileByteSource>& keep)
{
    keep = std::make_shared<FileByteSource>(path);
    if (!keep->open().ok())
        return {};
    auto img = PfsImage::open(keep);
    if (!img.ok()) {
        qWarning("%s", qPrintable(img.error().toLogString()));
        return {};
    }
    return std::move(img).value();
}

QByteArray readImageFile(const PfsImage& img, const PfsEntry& e)
{
    QByteArray out;
    const Status st = img.readFile(img.inodes()[e.inode], [&](const char* d, qint64 n) {
        out.append(d, n);
        return success();
    }, CancellationToken());
    if (!st.ok())
        qWarning("%s", qPrintable(st.error().toLogString()));
    return out;
}

void flipByte(const QString& path, qint64 offset)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(offset));
    char c = 0;
    QCOMPARE(f.read(&c, 1), qint64(1));
    c = char(c ^ 0x5A);
    QVERIFY(f.seek(offset));
    QCOMPARE(f.write(&c, 1), qint64(1));
}

} // namespace

class TestPfsRoundTrip : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid());
        fwtest::isolateAppData(m_appData);
        m_spec = pfstest::loadJson(QStringLiteral("golden_cases.json"));
        QVERIFY(!m_spec.isEmpty());
    }

    void roundTrip_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("folder-raw") << QStringLiteral("folder-raw");
        QTest::newRow("folder-compressed") << QStringLiteral("folder-compressed");
        QTest::newRow("folder-container") << QStringLiteral("folder-container");
        QTest::newRow("file-container") << QStringLiteral("file-container");
    }

    // Build, read every file back through the reader, compare with the source.
    void roundTrip()
    {
        QFETCH(QString, mode);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::makeTree(src, tree("game"), 1600000000));
        QVERIFY(QDir().mkpath(src + QStringLiteral("/data/empty_folder")));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/image.bin"),
                                   pfstest::genContent(QStringLiteral("mixed"), 1600000, 7), -1));

        BuildOptions o = options();
        BuildInput in;
        OutputFormat fmt = OutputFormat::FolderImage;
        if (mode == u"file-container") {
            in.isFolder = false;
            in.file = src + QStringLiteral("/image.bin");
            fmt = OutputFormat::Container;
        } else {
            in.plan = plan(src);
            if (mode == u"folder-compressed") {
                o.files.compress = true;
                o.files.minCompressMiB = 0.0;
            }
            if (mode == u"folder-container")
                fmt = OutputFormat::Container;
        }
        const QString out = tmp.filePath(QStringLiteral("out.img"));
        auto st = writeImage(in, fmt, o, out, CancellationToken());
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(errText(st.error())));

        std::shared_ptr<FileByteSource> keep;
        auto img = openImage(out, keep);
        QVERIFY(img);
        QVERIFY2(img->warnings().isEmpty(), qPrintable(img->warnings().join(u';')));

        if (mode == u"file-container") {
            QCOMPARE(img->files().size(), size_t(1));
            QCOMPARE(img->files().front().path, QByteArray("image.bin"));
            const QByteArray got = readImageFile(*img, img->files().front());
            const QByteArray want = pfstest::readAll(in.file);
            QVERIFY(got.size() >= want.size());
            QCOMPARE(got.left(want.size()), want);
            QVERIFY(st->containerCompressed);
        } else {
            const PfsImage* target = img.get();
            std::shared_ptr<ByteSource> nestedSrc;
            std::unique_ptr<PfsImage> nested;
            if (mode == u"folder-container") {
                QCOMPARE(img->files().size(), size_t(1));
                QCOMPARE(img->files().front().path, kNestedImageName);
                auto ns = img->openFile(img->inodes()[img->files().front().inode]);
                QVERIFY(ns.ok());
                nestedSrc = *ns;
                auto ni = PfsImage::open(nestedSrc);
                QVERIFY2(ni.ok(), ni.ok() ? "" : qPrintable(errText(ni.error())));
                nested = std::move(ni).value();
                target = nested.get();
            }
            QCOMPARE(target->files().size(), in.plan.files.size());
            for (const SourceFile& f : in.plan.files) {
                const PfsEntry* e = target->findFile(f.relPath);
                QVERIFY2(e, f.relPath.constData());
                const QByteArray got = readImageFile(*target, *e);
                const QByteArray want = f.generated ? f.content : pfstest::readAll(f.absPath);
                const Inode& ino = target->inodes()[e->inode];
                if (ino.isCompressed()) {
                    QCOMPARE(got.size(), qsizetype(ceilDiv(want.size(), target->header().blockSize) * target->header().blockSize));
                    QCOMPARE(got.left(want.size()), want);
                    QVERIFY(got.mid(want.size()).count('\0') == got.size() - want.size());
                } else {
                    QCOMPARE(got, want);
                }
            }
            QVERIFY(target->findFile("data/empty_folder/.pfskeep"));
        }

        VerifyExpectation ex;
        ex.format = fmt;
        if (in.isFolder) {
            ex.kind = VerifyExpectation::Kind::Folder;
            ex.plan = &in.plan;
        } else {
            ex.kind = VerifyExpectation::Kind::File;
            ex.file = in.file;
            ex.fileName = "image.bin";
        }
        for (VerifyDepth d : {VerifyDepth::Full, VerifyDepth::Structure}) {
            auto vr = verifyImage(out, ex, d, CancellationToken());
            QVERIFY2(vr.ok(), vr.ok() ? "" : qPrintable(errText(vr.error())));
            QVERIFY2(vr->passed, qPrintable(vr->problems.join(u'\n')));
        }
        auto none = verifyImage(out, VerifyExpectation{}, VerifyDepth::Structure, CancellationToken());
        QVERIFY(none.ok() && none->passed);
        QCOMPARE(none->nested, mode == u"folder-container");
    }

    // Strict verification: changed, missing and extra files all fail.
    void verifyDetectsDifferences_data()
    {
        QTest::addColumn<bool>("container");
        QTest::newRow("folder image") << false;
        QTest::newRow("container") << true;
    }

    void verifyDetectsDifferences()
    {
        QFETCH(bool, container);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::makeTree(src, tree("game"), 1600000000));
        const SourcePlan p0 = plan(src);
        BuildInput in;
        in.plan = p0;
        const OutputFormat fmt = container ? OutputFormat::Container : OutputFormat::FolderImage;
        const QString out = tmp.filePath(QStringLiteral("out.img"));
        QVERIFY(writeImage(in, fmt, options(), out, CancellationToken()).ok());

        auto verifyWith = [&](const SourcePlan& p, VerifyDepth depth) {
            VerifyExpectation ex;
            ex.kind = VerifyExpectation::Kind::Folder;
            ex.format = fmt;
            ex.plan = &p;
            auto r = verifyImage(out, ex, depth, CancellationToken());
            if (r.ok())
                return std::move(r).value();
            // A check that could not run fails the test here, whatever the caller expects of the report.
            QTest::qFail(qPrintable(errText(r.error())), __FILE__, __LINE__);
            VerifyReport failed;
            failed.fail(errText(r.error()));
            return failed;
        };
        QVERIFY(verifyWith(p0, VerifyDepth::Full).passed);

        // 1. same size, one byte different
        const QString changed = src + QStringLiteral("/data/level_02.pak");
        flipByte(changed, 12345);
        auto r1 = verifyWith(plan(src), VerifyDepth::Full);
        QVERIFY(!r1.passed);
        QCOMPARE(r1.problemCount, qint64(1));
        QVERIFY2(r1.problems.front().contains(QStringLiteral("Different content: data/level_02.pak (first difference at byte 12345)")),
                 qPrintable(r1.problems.front()));
        // Structure depth does not read the source: same names and sizes pass.
        QVERIFY(verifyWith(plan(src), VerifyDepth::Structure).passed);
        flipByte(changed, 12345);

        // 2. a file the image does not have
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/new.bin"), "new", -1));
        auto r2 = verifyWith(plan(src), VerifyDepth::Full);
        QVERIFY(!r2.passed);
        QCOMPARE(r2.problems, QStringList{QStringLiteral("Missing from the image: data/new.bin")});
        QVERIFY(QFile::remove(src + QStringLiteral("/data/new.bin")));

        // 3. a file the source no longer has
        QVERIFY(QFile::remove(src + QStringLiteral("/audio_cfg.ini")));
        auto r3 = verifyWith(plan(src), VerifyDepth::Structure);
        QVERIFY(!r3.passed);
        QCOMPARE(r3.problems, QStringList{QStringLiteral("Not in the source: audio_cfg.ini")});

        // 4. different size
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/audio_cfg.ini"), "short", -1));
        auto r4 = verifyWith(plan(src), VerifyDepth::Structure);
        QVERIFY(!r4.passed);
        QVERIFY(r4.problems.front().startsWith(QStringLiteral("Different size: audio_cfg.ini")));
    }

    // Audit H2: files the build leaves out never make verification fail.
    void exclusionsDoNotCauseFalseFailures()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::makeTree(src, tree("game"), 1600000000));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/old_build.ffpkg"), "leftover", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/OLD.FFPFSC"), "leftover", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/decrypted/eboot.bin"), "x", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/sce_sys/backup/param.json"), "x", -1));
        PlanOptions po = planOptions();
        po.excludes = {QStringLiteral("*.exfat"), QStringLiteral("*.ffpkg"), QStringLiteral("*.ffpfs"),
                       QStringLiteral("*.ffpfsc"), QStringLiteral("decrypted"), QStringLiteral("sce_sys/backup/")};
        const SourcePlan p = plan(src, po);
        QCOMPARE(p.excludedFiles, 2);
        QCOMPARE(p.excludedFolders, 2);
        for (const SourceFile& f : p.files) {
            QVERIFY(!f.relPath.endsWith(".ffpkg") && !f.relPath.startsWith("decrypted"));
            QVERIFY(!f.relPath.startsWith("sce_sys/backup"));
        }
        BuildInput in;
        in.plan = p;
        const QString out = tmp.filePath(QStringLiteral("out.ffpfsc"));
        QVERIFY(writeImage(in, OutputFormat::Container, options(), out, CancellationToken()).ok());
        VerifyExpectation ex;
        ex.kind = VerifyExpectation::Kind::Folder;
        ex.format = OutputFormat::Container;
        ex.plan = &p;
        auto r = verifyImage(out, ex, VerifyDepth::Full, CancellationToken());
        QVERIFY(r.ok());
        QVERIFY2(r->passed, qPrintable(r->problems.join(u'\n')));
        QCOMPARE(r->filesChecked, qint64(p.files.size()));
    }

    // Damage is reported as CorruptData, both in metadata and in compressed blocks.
    void corruptionDetected()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString file = tmp.filePath(QStringLiteral("game.exfat"));
        QVERIFY(pfstest::writeFile(file, pfstest::genContent(QStringLiteral("text"), 3 * 1024 * 1024, 5), -1));
        BuildInput in;
        in.isFolder = false;
        in.file = file;
        const QString out = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(writeImage(in, OutputFormat::Container, options(), out, CancellationToken()).ok());
        const QByteArray good = pfstest::readAll(out);
        VerifyExpectation ex;
        ex.kind = VerifyExpectation::Kind::File;
        ex.file = file;
        ex.fileName = "game.exfat";
        const auto intact = verifyImage(out, ex, VerifyDepth::Full, CancellationToken());
        QVERIFY2(intact.ok(), intact.ok() ? "" : qPrintable(errText(intact.error())));
        QVERIFY(intact->passed);

        // A byte inside the 3rd compressed block (payload starts at block 6, PFSC data at +64 KiB).
        const qint64 payload = 6 * 0x10000;
        const qint64 table = payload + 0x400;
        const qint64 block2 = qint64(pfs::getU64(good.constData() + table + 2 * 8));
        flipByte(out, payload + block2 + 40);
        std::shared_ptr<FileByteSource> keep;
        auto img = openImage(out, keep);
        QVERIFY(img); // metadata is intact
        const Status rs = img->readFile(img->inodes()[img->files().front().inode],
                                        [](const char*, qint64) { return success(); }, CancellationToken());
        QVERIFY(!rs.ok());
        FWTEST_COMPARE_ERROR(rs, ErrorCode::CorruptData);
        for (VerifyDepth d : {VerifyDepth::Full, VerifyDepth::Structure}) {
            auto vr = verifyImage(out, ex, d, CancellationToken());
            QVERIFY(!vr.ok());
            FWTEST_COMPARE_ERROR(vr, ErrorCode::CorruptData);
        }
        img.reset();
        keep.reset();

        // Metadata damage: inode table (uroot's first block pointer), dirents, PFSC header.
        struct Damage {
            const char* what;
            qint64 offset;
        };
        const Damage damages[] = {
            {"uroot db[0]", 0x10000 + 2 * kInodeD32Size + 0x64},
            {"super root dirent", 0x20000 + 12},
            {"PFSC magic", payload + 1},
            {"PFS magic", 8},
        };
        for (const Damage& d : damages) {
            QVERIFY(writeFileAtomic(out, good).ok());
            flipByte(out, d.offset);
            auto r = verifyImage(out, ex, VerifyDepth::Structure, CancellationToken());
            QVERIFY2(!r.ok(), d.what);
            QVERIFY2(r.error().code == ErrorCode::CorruptData, qPrintable(errText(r.error())));
        }
        // Truncated image.
        QVERIFY(writeFileAtomic(out, good.left(0x30000)).ok());
        auto t = verifyImage(out, ex, VerifyDepth::Structure, CancellationToken());
        QVERIFY(!t.ok());
        FWTEST_COMPARE_ERROR(t, ErrorCode::CorruptData);
    }

    // Lazy's RAM mode leaves the last block short; such images still read correctly.
    void shortImageReadable()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), pfstest::genContent(QStringLiteral("random"), 1000, 1), -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/z.bin"), pfstest::genContent(QStringLiteral("text"), 70000, 2), -1));
        BuildInput in;
        in.plan = plan(src);
        const QString out = tmp.filePath(QStringLiteral("out.ffpfs"));
        QVERIFY(writeImage(in, OutputFormat::FolderImage, options(), out, CancellationToken()).ok());
        std::shared_ptr<FileByteSource> keep;
        auto full = openImage(out, keep);
        QVERIFY(full);
        const PfsEntry* lastEntry = full->findFile("z.bin");
        QVERIFY(lastEntry);
        const Inode& last = full->inodes()[lastEntry->inode];
        const qint64 end = last.db[0] * 0x10000 + 70000;
        full.reset();
        keep.reset();
        QFile f(out);
        QVERIFY(f.resize(end));
        auto img = openImage(out, keep);
        QVERIFY(img);
        QCOMPARE(img->missingTailBytes(), qint64(2 * 0x10000 - 70000));
        const PfsEntry* shortEntry = img->findFile("z.bin");
        QVERIFY(shortEntry);
        QCOMPARE(readImageFile(*img, *shortEntry), pfstest::readAll(src + QStringLiteral("/z.bin")));
    }

    void extractRoundTrip()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::makeTree(src, tree("game"), 1600000000));
        QVERIFY(QDir().mkpath(src + QStringLiteral("/empty")));
        BuildInput in;
        in.plan = plan(src);
        const QString folderImg = tmp.filePath(QStringLiteral("game.ffpfs"));
        const QString container = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(writeImage(in, OutputFormat::FolderImage, options(), folderImg, CancellationToken()).ok());
        QVERIFY(writeImage(in, OutputFormat::Container, options(4, 9), container, CancellationToken()).ok());

        auto sameTree = [&](const QString& dir) {
            for (const SourceFile& f : in.plan.files) {
                const QByteArray want = f.generated ? f.content : pfstest::readAll(f.absPath);
                if (pfstest::readAll(dir + u'/' + QString::fromLatin1(f.relPath)) != want)
                    return false;
            }
            return QFileInfo(dir + QStringLiteral("/empty")).isDir();
        };
        ExtractOptions eo;
        auto r1 = extractImage(folderImg, tmp.filePath(QStringLiteral("x1")), eo, CancellationToken());
        QVERIFY2(r1.ok(), r1.ok() ? "" : qPrintable(errText(r1.error())));
        QCOMPARE(r1->files, qint64(in.plan.files.size()));
        QVERIFY(sameTree(tmp.filePath(QStringLiteral("x1"))));
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x1.part"))));

        // Deep: the container's nested image is unpacked directly.
        auto r2 = extractImage(container, tmp.filePath(QStringLiteral("x2")), eo, CancellationToken());
        QVERIFY(r2.ok());
        QVERIFY(r2->nested);
        QVERIFY(sameTree(tmp.filePath(QStringLiteral("x2"))));

        // Not deep: pfs_image.dat is exactly the uncompressed folder image.
        eo.deep = false;
        auto r3 = extractImage(container, tmp.filePath(QStringLiteral("x3")), eo, CancellationToken());
        QVERIFY(r3.ok());
        QCOMPARE(pfstest::readAll(tmp.filePath(QStringLiteral("x3/pfs_image.dat"))), pfstest::readAll(folderImg));

        // A non-empty destination needs overwrite.
        eo.deep = true;
        auto r4 = extractImage(folderImg, tmp.filePath(QStringLiteral("x1")), eo, CancellationToken());
        QVERIFY(!r4.ok());
        FWTEST_COMPARE_ERROR(r4, ErrorCode::AlreadyExists);
        QVERIFY(pfstest::writeFile(tmp.filePath(QStringLiteral("x1/eboot.bin")), "modified", -1));
        eo.overwrite = true;
        auto r5 = extractImage(folderImg, tmp.filePath(QStringLiteral("x1")), eo, CancellationToken());
        QVERIFY(r5.ok());
        QVERIFY(sameTree(tmp.filePath(QStringLiteral("x1"))));
    }

    void cancelledExtractLeavesNothing()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString file = tmp.filePath(QStringLiteral("big.exfat"));
        QVERIFY(pfstest::writeFile(file, pfstest::genContent(QStringLiteral("mixed"), 12 * 1024 * 1024, 9), -1));
        BuildInput in;
        in.isFolder = false;
        in.file = file;
        const QString out = tmp.filePath(QStringLiteral("big.ffpfsc"));
        QVERIFY(writeImage(in, OutputFormat::Container, options(), out, CancellationToken()).ok());
        CancellationSource cs;
        ProgressHooks hooks;
        qint64 seen = 0;
        hooks.advance = [&](qint64 n) {
            seen += n;
            if (seen > 2 * 1024 * 1024)
                cs.cancel();
        };
        auto r = extractImage(out, tmp.filePath(QStringLiteral("x")), ExtractOptions{}, cs.token(), hooks);
        QVERIFY(!r.ok());
        QVERIFY(r.error().isCancelled());
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x"))));
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x.part"))));
    }

    // The output does not depend on the number of compression threads.
    void threadDeterminism()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::makeTree(src, tree("game"), 1600000000));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/big/blob.bin"),
                                   pfstest::genContent(QStringLiteral("mixed"), 24 * 1024 * 1024 + 12345, 77), -1));
        BuildInput in;
        in.plan = plan(src);
        QStringList hashes;
        for (int threads : {1, 4, 7}) {
            for (int variant = 0; variant < 2; ++variant) {
                BuildOptions o = options(threads, 6);
                o.files.compress = variant == 1;
                o.files.minCompressMiB = 0.0;
                const OutputFormat fmt = variant == 0 ? OutputFormat::Container : OutputFormat::FolderImage;
                const QString out = tmp.filePath(QStringLiteral("t%1_%2.img").arg(threads).arg(variant));
                auto st = writeImage(in, fmt, o, out, CancellationToken());
                QVERIFY(st.ok());
                QCOMPARE(st->threads, threads);
                hashes << QStringLiteral("%1:%2").arg(variant).arg(pfstest::sha256(out));
            }
        }
        QCOMPARE(hashes[0], hashes[2]);
        QCOMPARE(hashes[0], hashes[4]);
        QCOMPARE(hashes[1], hashes[3]);
        QCOMPARE(hashes[1], hashes[5]);
    }

    void sourceRules()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "x", -1));
        QVERIFY(pfstest::writeFile(src + QString::fromUtf8("/d\xc3\xa9mo.bin"), "x", -1));
        auto bad = planFolder(src, planOptions(), CancellationToken());
        QVERIFY(!bad.ok());
        FWTEST_COMPARE_ERROR(bad, ErrorCode::InvalidArgument);
        PlanOptions po = planOptions();
        po.excludes = {QString::fromUtf8("d\xc3\xa9mo.bin")};
        QVERIFY(planFolder(src, po, CancellationToken()).ok());

        const QString empty = tmp.filePath(QStringLiteral("empty"));
        QVERIFY(QDir().mkpath(empty + QStringLiteral("/sub")));
        auto e = planFolder(empty, planOptions(), CancellationToken());
        QVERIFY(!e.ok());

        // AMPR index: missing / refresh / off.
        const QString ampr = tmp.filePath(QStringLiteral("ampr"));
        QVERIFY(pfstest::writeFile(ampr + QStringLiteral("/eboot.bin"), "x", 1600000000));
        QVERIFY(pfstest::writeFile(ampr + QStringLiteral("/fakelib/libSceAmpr.sprx"), "prx", 1600000000));
        PlanOptions pa = planOptions();
        const SourcePlan generated = plan(ampr, pa);
        QVERIFY(generated.amprGenerated);
        auto idx = std::find_if(generated.files.begin(), generated.files.end(),
                                [](const SourceFile& f) { return f.relPath == "ampr_emu.index"; });
        QVERIFY(idx != generated.files.end());
        QVERIFY(idx->content.startsWith("AMPRIDX3"));
        QVERIFY(pfstest::writeFile(ampr + QStringLiteral("/ampr_emu.index"), "user index", 1600000000));
        const SourcePlan kept = plan(ampr, pa);
        QVERIFY(!kept.amprGenerated);
        pa.ampr = AmprMode::Refresh;
        const SourcePlan refreshed = plan(ampr, pa);
        QVERIFY(refreshed.amprGenerated);
        QCOMPARE(std::count_if(refreshed.files.begin(), refreshed.files.end(),
                               [](const SourceFile& f) { return f.relPath == "ampr_emu.index"; }),
                 std::ptrdiff_t(1));
        pa.ampr = AmprMode::Off;
        QVERIFY(!plan(ampr, pa).amprGenerated);
    }

private:
    QJsonArray tree(const char* name) const
    {
        return m_spec.value(QStringLiteral("trees")).toObject().value(QLatin1String(name)).toArray();
    }

    QTemporaryDir m_appData;
    QJsonObject m_spec;
};

QTEST_GUILESS_MAIN(TestPfsRoundTrip)
#include "tst_pfs_roundtrip.moc"
