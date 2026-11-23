// Golden compatibility: the native builder must produce byte-identical images
// to Lazy_MkPFS for the same input and options.
//
// The references (SHA-256 + size, native/tests/data/pfs/golden.json) were made
// by native/tests/data/pfs/make_golden.py, which runs Lazy_MkPFS on trees
// generated from golden_cases.json with a pinned timestamp. Lazy_MkPFS is not
// part of this repository, so the references are a frozen record (the commit
// is in golden.json; the script says how to run it against a checkout). This
// test regenerates the same trees, builds them natively and compares hashes.

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsReader.h"
#include "engines/pfs/PfsSource.h"
#include "engines/pfs/PfsVerify.h"
#include "engines/pfs/PfscCodec.h"

#include "engines/pfs/PfsDirentBlocks.h"
#include "engines/pfs/PfsTestUtil.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::pfs;

class TestPfsGolden : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_appData.isValid());
        fwtest::isolateAppData(m_appData);
        m_spec = pfstest::loadJson(QStringLiteral("golden_cases.json"));
        m_golden = pfstest::loadJson(QStringLiteral("golden.json"));
        QVERIFY2(!m_spec.isEmpty(), "golden_cases.json missing");
        QVERIFY2(!m_golden.isEmpty(), "golden.json missing (it is made by make_golden.py --write, which needs Lazy_MkPFS)");
        qInfo("zlib %s; references generated with Python %s, zlib %s", qPrintable(zlibVersionString()),
              qPrintable(m_golden.value(QStringLiteral("python")).toString()),
              qPrintable(m_golden.value(QStringLiteral("zlib")).toString()));
    }

    void golden_data()
    {
        QTest::addColumn<QJsonObject>("testCase");
        QTest::addColumn<int>("threads");
        const QJsonArray cases = m_spec.value(QStringLiteral("cases")).toArray();
        QVERIFY(!cases.isEmpty());
        int index = 0;
        for (const auto& v : cases) {
            const QJsonObject c = v.toObject();
            const QByteArray name = c.value(QStringLiteral("name")).toString().toLatin1();
            // One to four threads in turn: the bytes must not depend on the count.
            QTest::newRow(name.constData()) << c << 1 + index++ % 4;
        }
    }

    void golden()
    {
        QFETCH(QJsonObject, testCase);
        QFETCH(int, threads);
        const QString name = testCase.value(QStringLiteral("name")).toString();
        const QJsonObject expected =
            m_golden.value(QStringLiteral("cases")).toObject().value(name).toObject();
        QVERIFY2(!expected.isEmpty(), qPrintable(QStringLiteral("no reference for ") + name));

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        const QJsonArray tree =
            m_spec.value(QStringLiteral("trees")).toObject().value(testCase.value(QStringLiteral("tree")).toString()).toArray();
        QVERIFY(pfstest::makeTree(src, tree, m_spec.value(QStringLiteral("file_mtime")).toInteger()));

        const qint64 fixedTime = m_spec.value(QStringLiteral("fixed_time")).toInteger();
        BuildOptions o;
        o.layout.blockSize = testCase.value(QStringLiteral("block_size")).toInt(65536);
        o.layout.caseInsensitive = testCase.value(QStringLiteral("case_insensitive")).toBool(true);
        o.layout.time = fixedTime;
        o.level = testCase.value(QStringLiteral("level")).toInt(6);
        o.threads = threads;
        const QString build = testCase.value(QStringLiteral("build")).toString();

        BuildInput in;
        OutputFormat fmt = OutputFormat::FolderImage;
        if (build == u"file_container") {
            in.isFolder = false;
            in.file = src + u'/' + testCase.value(QStringLiteral("file")).toString();
            fmt = OutputFormat::Container;
            o.container.minCompressMiB = testCase.value(QStringLiteral("min_compress_mb")).toDouble(1.0);
        } else {
            PlanOptions po;
            po.keepEmptyFolders = testCase.value(QStringLiteral("keep_empty")).toBool(false);
            po.ampr = AmprMode::Missing;
            po.time = fixedTime;
            auto plan = planFolder(src, po, CancellationToken());
            QVERIFY2(plan.ok(), plan.ok() ? "" : qPrintable(plan.error().toLogString()));
            in.isFolder = true;
            in.plan = std::move(plan).value();
            if (build == u"folder_compressed") {
                o.files.compress = true;
                o.files.minCompressMiB = testCase.value(QStringLiteral("min_compress_mb")).toDouble(5.0);
            } else if (build == u"folder_container") {
                fmt = OutputFormat::Container;
            }
        }

        // With the Lazy_MkPFS directory layout (entries may cross a block)
        // every case is byte-identical to Lazy_MkPFS.
        BuildOptions lazy = o;
        lazy.layout.lazyDirents = true;
        const QString lazyOut = tmp.filePath(QStringLiteral("lazy.img"));
        auto lst = writeImage(in, fmt, lazy, lazyOut, CancellationToken());
        QVERIFY2(lst.ok(), lst.ok() ? "" : qPrintable(lst.error().toLogString()));
        QCOMPARE(QFileInfo(lazyOut).size(), expected.value(QStringLiteral("size")).toInteger());
        QCOMPARE(pfstest::sha256(lazyOut), expected.value(QStringLiteral("sha256")).toString());
        const bool crossed = !direntProblems(lazyOut).isEmpty();

        // The images we build keep every entry inside its block: the same
        // bytes wherever no entry crossed one; where one did, the image
        // changes and must parse block by block.
        const QString out = tmp.filePath(QStringLiteral("out.img"));
        auto st = writeImage(in, fmt, o, out, CancellationToken());
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        const qint64 size = QFileInfo(out).size();
        const QString digest = pfstest::sha256(out);
        QCOMPARE(st->imageBytes, size);
        const QStringList problems = direntProblems(out);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(u'\n')));
        if (!crossed) {
            QCOMPARE(size, expected.value(QStringLiteral("size")).toInteger());
            QCOMPARE(digest, expected.value(QStringLiteral("sha256")).toString());
        } else {
            QVERIFY2(digest != expected.value(QStringLiteral("sha256")).toString(), "an entry crossed a block, yet nothing changed");
            qInfo("%s: a directory entry crossed a block in the Lazy_MkPFS layout; the image differs from it there",
                  qPrintable(name));
            if (in.isFolder) {
                VerifyExpectation ex;
                ex.format = fmt;
                ex.kind = VerifyExpectation::Kind::Folder;
                ex.plan = &in.plan;
                auto vr = verifyImage(out, ex, VerifyDepth::Full, CancellationToken());
                QVERIFY2(vr.ok(), vr.ok() ? "" : qPrintable(vr.error().toLogString()));
                QVERIFY2(vr->passed, qPrintable(vr->problems.join(u'\n')));
            }
        }
        m_crossed[name] = crossed;
    }

    // Which cases changed (the record of the departure from Lazy_MkPFS).
    void cleanupTestCase()
    {
        QStringList changed;
        for (auto it = m_crossed.cbegin(); it != m_crossed.cend(); ++it)
            if (it.value())
                changed << it.key();
        qInfo("cases whose directory entries crossed a block: %s", qPrintable(changed.isEmpty() ? QStringLiteral("none") : changed.join(u", ")));
    }

private:
    // Per-block dirent problems of the image at `path` and of a PFS image
    // nested in it (a folder container).
    static QStringList direntProblems(const QString& path)
    {
        auto file = std::make_shared<FileByteSource>(path);
        if (!file->open().ok())
            return {QStringLiteral("cannot open %1").arg(path)};
        auto img = PfsImage::open(file);
        if (!img.ok())
            return {img.error().toLogString()};
        QStringList problems = pfstest::scanDirentBlocks(**img).problems;
        const auto& files = (*img)->files();
        if (files.size() == 1 && files.front().path == kNestedImageName) {
            auto ns = (*img)->openFile((*img)->inodes()[files.front().inode]);
            if (!ns.ok())
                return problems << ns.error().toLogString();
            auto nested = PfsImage::open(*ns);
            if (nested.ok())
                problems += pfstest::scanDirentBlocks(**nested).problems;
        }
        return problems;
    }

    QTemporaryDir m_appData;
    QJsonObject m_spec;
    QJsonObject m_golden;
    QMap<QString, bool> m_crossed;
};

QTEST_GUILESS_MAIN(TestPfsGolden)
#include "tst_pfs_golden.moc"
