// Byte-identity with stored UFS2Tool images, no .NET needed.
//
// tests/data/ufs2/*.ffpkg.zz were built by the real UFS2Tool 4.0.0 — the
// UFS2Tool.dll the legacy app embeds (sha256 08f2f2a1...8e3a, commit c123639)
// running on .NET 8.0.33 — from the tree makeReferenceTree() writes:
//   UFS2Tool newfs -O 2 -b 65536 -f 65536 -m 0 -S 512 -i 262144 -D <tree> ...
//     -> ufs2tool-legacy-params.ffpkg.zz      (the legacy app's parameters)
//   UFS2Tool newfs -O 2 -b 65536 -f 32768 -m 8 -S 512 -i 131072 -D <tree> ...
//     -> ufs2tool-64k-32k-m8-i128k.ffpkg.zz   (fragments, freed tails)
// and stored with qCompress() framing (4-byte big-endian length + zlib).
//
// UFS2Tool adds entries in the order the OS lists them, which differs between
// file systems, so the test takes the directory order from the stored image;
// the build time and fs_id are taken from it as well. Everything else must
// match byte for byte.

#include "Ufs2TestUtil.h"

#include "engines/ufs2/Ufs2Check.h"

#include <QTemporaryDir>

#include <map>

using namespace ufs2test;

namespace {

QByteArray pattern(qint64 size, int seed)
{
    QByteArray b(size, '\0');
    for (qint64 j = 0; j < size; ++j)
        b[qsizetype(j)] = char((j * 7 + seed * 13 + (j >> 9)) & 0xFF);
    return b;
}

bool makeReferenceTree(const QString& root)
{
    bool ok = true;
    ok &= mktext(root + "/eboot.bin", pattern(200000, 1));
    ok &= mktext(root + "/sce_sys/param.json", "{\"titleId\":\"PPSA99999\",\"contentVersion\":\"01.000.000\"}");
    ok &= mktext(root + "/sce_sys/icon0.png", pattern(70000, 2));
    ok &= mktext(root + "/data/big.pak", pattern(1500000, 3));
    ok &= mktext(root + "/data/sub/deep/x.bin", pattern(100, 4));
    ok &= mktext(root + "/data/empty.bin", QByteArray());
    ok &= mktext(root + "/.hidden", pattern(10, 5));
    ok &= mktext(root + "/odd/a.bin", pattern(33000, 6));
    ok &= mktext(root + "/odd/b.bin", pattern(65537, 7));
    for (int j = 0; j < 40; ++j)
        ok &= mktext(root + QStringLiteral("/many/f%1.dat").arg(j, 2, 10, QLatin1Char('0')), pattern(j * 97, 10 + j));
    ok &= QDir().mkpath(root + "/emptydir");
    return ok;
}

// Puts the children of every directory of `tree` in the order `ref` lists them.
bool adoptOrder(SourceTree& tree, const SourceTree& ref)
{
    std::vector<std::pair<int, int>> stack{{0, 0}};
    while (!stack.empty()) {
        const auto [t, r] = stack.back();
        stack.pop_back();
        std::map<QByteArray, int> pos;
        const auto& rk = ref.nodes[size_t(r)].children;
        for (size_t i = 0; i < rk.size(); ++i)
            pos[ref.nodes[size_t(rk[i])].rawName] = int(i);
        auto& kids = tree.nodes[size_t(t)].children;
        if (kids.size() != rk.size())
            return false;
        for (int k : kids)
            if (!pos.count(tree.nodes[size_t(k)].rawName))
                return false;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) {
            return pos[tree.nodes[size_t(a)].rawName] < pos[tree.nodes[size_t(b)].rawName];
        });
        for (int k : kids)
            if (tree.nodes[size_t(k)].isDir())
                stack.emplace_back(k, rk[size_t(pos[tree.nodes[size_t(k)].rawName])]);
    }
    return true;
}

} // namespace

class TestUfs2Reference : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
        QVERIFY(makeReferenceTree(m_tmp.filePath("src")));
    }

    void matchesUfs2Tool_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("block");
        QTest::addColumn<int>("frag");
        QTest::addColumn<int>("minfree");
        QTest::addColumn<int>("bpi");
        QTest::newRow("legacy parameters") << "ufs2tool-legacy-params.ffpkg.zz" << 65536 << 65536 << 0 << 262144;
        QTest::newRow("64K/32K -m 8 -i 128K") << "ufs2tool-64k-32k-m8-i128k.ffpkg.zz" << 65536 << 32768 << 8 << 131072;
    }

    void matchesUfs2Tool()
    {
        QFETCH(QString, file);
        QFETCH(int, block);
        QFETCH(int, frag);
        QFETCH(int, minfree);
        QFETCH(int, bpi);
        const QByteArray reference = qUncompress(readAll(fwtest::dataDir() + "/ufs2/" + file));
        QVERIFY2(reference.size() > 1024 * 1024, qPrintable(file));
        const QString refPath = m_tmp.filePath(file + ".img");
        QVERIFY(mktext(refPath, reference));
        auto ref = Ufs2Reader::open(refPath);
        QVERIFY(ref.ok());
        CancellationToken token;
        auto refTree = ref.value()->loadTree(token);
        QVERIFY(refTree.ok());

        auto tree = scanLocalFolder(m_tmp.filePath("src"), token);
        QVERIFY(tree.ok());
        QVERIFY(adoptOrder(tree.value(), refTree.value()));
        WriterOptions opt;
        opt.params = FormatParams{block, frag, minfree, bpi};
        opt.formatTime = ref.value()->sb().time;
        const auto refRoot = ref.value()->readInode(kRootIno);
        QVERIFY2(refRoot.ok(), refRoot.ok() ? "" : qPrintable(refRoot.error().toLogString()));
        opt.populateTime = refRoot->mtime;
        opt.fsId1 = ref.value()->sb().id[1];
        Ufs2Writer w(tree.value(), opt);
        const QString mine = m_tmp.filePath(file + ".native");
        auto st = w.write(mine, token, nullptr);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        const QByteArray ours = readAll(mine);
        QCOMPARE(ours.size(), reference.size());
        const qint64 diff = firstDifference(ours, reference);
        QVERIFY2(diff < 0, qPrintable(QStringLiteral("first difference at byte %1").arg(diff)));

        // The stored image itself passes the native structure check.
        auto c = checkImage(*ref.value(), token);
        QVERIFY(c.ok());
        QVERIFY2(c->ok(), qPrintable(c->problems.join(u'\n')));
        QCOMPARE(c->files, qint64(49));
    }
};

QTEST_GUILESS_MAIN(TestUfs2Reference)
#include "tst_ufs2_reference.moc"
