// Golden compatibility: the native builder must produce byte-identical images
// to the vendored Lazy_MkPFS for the same input and options.
//
// The references (SHA-256 + size) come from native/tests/data/pfs/make_golden.py,
// which runs vendor/lazy_mkpfs on trees generated from golden_cases.json with a
// pinned timestamp. This test regenerates the same trees, builds them natively
// and compares hashes.

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsSource.h"
#include "engines/pfs/PfscCodec.h"

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
        fwtest::isolateAppData(m_appData);
        m_spec = pfstest::loadJson(QStringLiteral("golden_cases.json"));
        m_golden = pfstest::loadJson(QStringLiteral("golden.json"));
        QVERIFY2(!m_spec.isEmpty(), "golden_cases.json missing");
        QVERIFY2(!m_golden.isEmpty(), "golden.json missing (run make_golden.py --write)");
        qInfo("zlib %s; references generated with Python %s, zlib %s", qPrintable(zlibVersionString()),
              qPrintable(m_golden.value(QStringLiteral("python")).toString()),
              qPrintable(m_golden.value(QStringLiteral("zlib")).toString()));
    }

    void golden_data()
    {
        QTest::addColumn<QJsonObject>("testCase");
        QTest::addColumn<int>("threads");
        const QJsonArray cases = m_spec.value(QStringLiteral("cases")).toArray();
        for (const auto& v : cases) {
            const QJsonObject c = v.toObject();
            const QByteArray name = c.value(QStringLiteral("name")).toString().toLatin1();
            QTest::newRow(name.constData()) << c << 3;
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

        const QString out = tmp.filePath(QStringLiteral("out.img"));
        auto st = writeImage(in, fmt, o, out, CancellationToken());
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        const qint64 size = QFileInfo(out).size();
        const QString digest = pfstest::sha256(out);
        QCOMPARE(size, expected.value(QStringLiteral("size")).toInteger());
        QCOMPARE(digest, expected.value(QStringLiteral("sha256")).toString());
        QCOMPARE(st->imageBytes, size);
    }

private:
    QTemporaryDir m_appData;
    QJsonObject m_spec;
    QJsonObject m_golden;
};

QTEST_GUILESS_MAIN(TestPfsGolden)
#include "tst_pfs_golden.moc"
