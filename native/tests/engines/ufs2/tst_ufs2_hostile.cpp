// Images made to hang a reader or to exhaust memory: superblock sizes that
// overflow, a block map that points back at itself, folders that share their
// blocks, folders nested without end. A small valid image is patched for each
// case; the reader must report damage, and quickly.

#include "Ufs2TestUtil.h"

#include "engines/ufs2/Ufs2Check.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace ufs2test;

namespace {

constexpr qint64 kQuickMs = 1000;
constexpr int kLeafFolders = 96;

bool patch(const QString& path, qint64 off, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::ReadWrite) && f.seek(off) && f.write(bytes) == bytes.size();
}

QByteArray le32(qint32 v)
{
    QByteArray b(4, '\0');
    wr32s(b.data(), v);
    return b;
}

QByteArray le64(qint64 v)
{
    QByteArray b(8, '\0');
    wr64(b.data(), v);
    return b;
}

qint64 inodeOffset(const SuperblockInfo& sb, quint32 ino)
{
    const qint64 cg = ino / quint32(sb.ipg);
    const qint64 idx = ino % quint32(sb.ipg);
    return (cg * sb.fpg + sb.iblkno) * qint64(sb.fsize) + idx * kInodeSize;
}

// A chain of `depth` empty folders, each inside the one before.
SourceTree nestedFolders(int depth)
{
    SourceTree t;
    TreeNode root;
    root.kind = NodeKind::Dir;
    t.nodes.push_back(root);
    for (int i = 1; i <= depth; ++i) {
        TreeNode n;
        n.name = QStringLiteral("d");
        n.rawName = "d";
        n.kind = NodeKind::Dir;
        n.parent = i - 1;
        t.nodes[size_t(i - 1)].children.push_back(i);
        t.nodes.push_back(n);
    }
    t.recount();
    return t;
}

} // namespace

class TestUfs2Hostile : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_image;
    SuperblockInfo m_sb;
    qint64 m_imageBytes = 0;

    QString copyOf(const QString& name)
    {
        const QString p = m_tmp.filePath(name);
        QFile::remove(p);
        return QFile::copy(m_image, p) ? p : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        const QString src = m_tmp.filePath("src");
        QVERIFY(makeGameTree(src));
        for (int i = 0; i < kLeafFolders; ++i)
            QVERIFY(mktext(src + QStringLiteral("/leaf%1/f.txt").arg(i), "leaf"));
        m_image = m_tmp.filePath("good.ffpkg");
        const auto built = buildImage(src, m_image);
        QVERIFY2(built.ok(), built.ok() ? "" : qPrintable(built.error().toLogString()));
        auto r = Ufs2Reader::open(m_image);
        QVERIFY(r.ok());
        m_sb = r.value()->sb();
        m_imageBytes = r.value()->fileSize();
        CancellationToken token;
        const auto c = checkImage(*r.value(), token, nullptr, 100000);
        QVERIFY(c.ok() && c->ok());
    }

    // fs_size x fsize = 2^64 wraps to 0 and used to pass the "image is
    // incomplete" test for a file of any size; ncg x 16 used to overflow an
    // int and void the summary-area test.
    void superblockSizesThatOverflowAreRefused()
    {
        QByteArray sb(kSuperblockSize, '\0');
        {
            QFile f(m_image);
            QVERIFY(f.open(QIODevice::ReadOnly) && f.seek(kSuperblockOffset));
            QCOMPARE(f.read(sb.data(), sb.size()), qint64(sb.size()));
        }
        QVERIFY(parseSuperblock(sb.constData(), m_imageBytes, kSuperblockOffset).ok());
        const auto set32 = [](QByteArray& b, int off, qint32 v) { wr32s(b.data() + off, v); };
        const auto set64 = [](QByteArray& b, int off, qint64 v) { wr64(b.data() + off, v); };
        // 64 KiB blocks without fragments, in both cases.
        const auto geometry = [&](QByteArray& b) {
            set32(b, 0x030, 65536); // bsize
            set32(b, 0x034, 65536); // fsize
            set32(b, 0x038, 1);     // frag
            set32(b, 0x050, 16);    // bshift
            set32(b, 0x054, 16);    // fshift
            set32(b, 0x060, 0);     // fragshift
            set32(b, 0x074, 8192);  // nindir
            set32(b, 0x078, 256);   // inopb
            set32(b, 0x008, 2);     // sblkno
            set32(b, 0x00C, 3);     // cblkno
            set32(b, 0x010, 4);     // iblkno
            set32(b, 0x0A0, 32768); // cgsize
        };
        {
            QByteArray b = sb;
            geometry(b);
            set32(b, 0x02C, 1 << 18);          // ncg
            set32(b, 0x0B8, 8192);             // ipg
            set32(b, 0x0BC, 1 << 30);          // fpg
            set32(b, 0x014, 36);               // dblkno
            set64(b, 0x438, qint64(1) << 48);  // size, in 64 KiB fragments
            set32(b, 0x09C, 1 << 22);          // cssize
            set64(b, 0x448, 36);               // csaddr
            const auto parsed = parseSuperblock(b.constData(), m_imageBytes, kSuperblockOffset);
            QVERIFY(!parsed.ok());
            QCOMPARE(parsed.error().code, ErrorCode::CorruptData);

            const QString img = copyOf(QStringLiteral("overflow.ffpkg"));
            QVERIFY(patch(img, kSuperblockOffset, b));
            QElapsedTimer timer;
            timer.start();
            const auto opened = Ufs2Reader::open(img);
            QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
            QVERIFY(!opened.ok());
            QCOMPARE(opened.error().code, ErrorCode::CorruptData);
        }
        {
            QByteArray b = sb;
            geometry(b);
            set32(b, 0x02C, 1 << 27);          // ncg: ncg x 16 = 2^31
            set32(b, 0x0B8, 16);               // ipg
            set32(b, 0x0BC, 64);               // fpg
            set32(b, 0x014, 5);                // dblkno
            set64(b, 0x438, qint64(1) << 33);  // size
            set32(b, 0x09C, 65536);            // cssize: far too small for 2^27 groups
            set64(b, 0x448, 5);                // csaddr
            const auto parsed = parseSuperblock(b.constData(), -1, kSuperblockOffset);
            QVERIFY(!parsed.ok());
            QCOMPARE(parsed.error().code, ErrorCode::CorruptData);
            QVERIFY2(parsed.error().message.contains(QStringLiteral("summary area")), qPrintable(parsed.error().message));
        }
    }

    // A triple-indirect pointer to a block whose pointers all lead back to
    // that block: one block used to expand to nindir^3 references.
    void blockMapThatPointsAtItselfIsRefused()
    {
        const QString img = copyOf(QStringLiteral("self-map.ffpkg"));
        QVERIFY(!img.isEmpty());
        quint32 ino = 0;
        qint64 pointerBlock = 0;
        {
            auto r = Ufs2Reader::open(img);
            QVERIFY(r.ok());
            const auto found = r.value()->resolve(QStringLiteral("/data/big.pak"));
            QVERIFY(found.ok());
            ino = found.value();
            const auto in = r.value()->readInode(ino);
            QVERIFY(in.ok());
            pointerBlock = in->ib[0];
            QVERIFY(pointerBlock > 0);
        }
        QByteArray pointers;
        for (int i = 0; i < m_sb.nindir; ++i)
            pointers += le64(pointerBlock);
        QVERIFY(patch(img, pointerBlock * m_sb.fsize, pointers));
        const qint64 at = inodeOffset(m_sb, ino);
        QVERIFY(patch(img, at + 0x10, le64(std::min(m_sb.maxfilesize, qint64(1) << 44)))); // di_size
        QVERIFY(patch(img, at + 0xD0 + 16, le64(pointerBlock)));                         // di_ib[2]

        QElapsedTimer timer;
        timer.start();
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        const auto in = r.value()->readInode(ino);
        QVERIFY(in.ok());
        const auto blocks = r.value()->blocks(in.value(), ino);
        QVERIFY(!blocks.ok());
        QCOMPARE(blocks.error().code, ErrorCode::CorruptData);
        QVERIFY2(blocks.error().message.contains(QStringLiteral("maps more blocks than the image holds")),
                 qPrintable(blocks.error().message));
        QVERIFY(!r.value()->openFile(ino).ok());
        CancellationToken token;
        const auto c = checkImage(*r.value(), token, nullptr, 100000);
        QVERIFY(c.ok());
        QVERIFY(!c->ok());
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
    }

    // The block-map walk can be cancelled (it used to have no token).
    void blockMapWalkCanBeCancelled()
    {
        auto r = Ufs2Reader::open(m_image);
        QVERIFY(r.ok());
        const auto found = r.value()->resolve(QStringLiteral("/data/big.pak"));
        QVERIFY(found.ok());
        const auto in = r.value()->readInode(found.value());
        QVERIFY(in.ok());
        CancellationSource source;
        const CancellationToken token = source.token();
        QVERIFY(r.value()->blocks(in.value(), found.value(), &token).ok());
        source.cancel();
        const auto cancelled = r.value()->blocks(in.value(), found.value(), &token);
        QVERIFY(!cancelled.ok());
        QVERIFY(cancelled.error().isCancelled());
    }

    void folderLargerThanTheImageIsRefused()
    {
        const QString img = copyOf(QStringLiteral("big-dir.ffpkg"));
        QVERIFY(!img.isEmpty());
        QVERIFY(m_imageBytes < 128 * 1024 * 1024);
        QVERIFY(patch(img, inodeOffset(m_sb, kRootIno) + 0x10, le64(128 * 1024 * 1024)));
        QElapsedTimer timer;
        timer.start();
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        const auto entries = r.value()->readDirectory(kRootIno);
        QVERIFY(!entries.ok());
        QCOMPARE(entries.error().code, ErrorCode::CorruptData);
        CancellationToken token;
        QCOMPARE(r.value()->loadTree(token).error().code, ErrorCode::CorruptData);
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
    }

    // Many folders whose twelve direct blocks are all the same block: each
    // lists that block's entries again, so a small image used to yield a tree
    // many times its own size.
    void foldersSharingTheirBlocksStopTheWalk()
    {
        const QString img = copyOf(QStringLiteral("shared-dirs.ffpkg"));
        QVERIFY(!img.isEmpty());
        QVERIFY2(qint64(kLeafFolders) * kNDirect * m_sb.bsize > m_imageBytes, qPrintable(QString::number(m_imageBytes)));
        quint32 fileIno = 0;
        qint64 shared = 0;
        std::vector<quint32> leaves;
        {
            auto r = Ufs2Reader::open(img);
            QVERIFY(r.ok());
            const auto big = r.value()->resolve(QStringLiteral("/data/big.pak"));
            QVERIFY(big.ok());
            fileIno = big.value();
            const auto in = r.value()->readInode(fileIno);
            QVERIFY(in.ok());
            shared = in->db[0]; // a whole block of the big file becomes directory data
            QVERIFY(shared > 0 && shared % m_sb.frag == 0);
            for (int i = 0; i < kLeafFolders; ++i) {
                const auto leaf = r.value()->resolve(QStringLiteral("/leaf%1").arg(i));
                QVERIFY(leaf.ok());
                leaves.push_back(leaf.value());
            }
        }
        // One entry "x" in the first 512-byte chunk, empty records in the others.
        QByteArray block(m_sb.bsize, '\0');
        for (int chunk = 0; chunk < m_sb.bsize; chunk += 512)
            wr16(block.data() + chunk + 4, 512);
        wr32(block.data(), fileIno);
        block[6] = char(kDtReg);
        block[7] = 1;
        block[8] = 'x';
        QVERIFY(patch(img, shared * m_sb.fsize, block));
        for (const quint32 leaf : leaves) {
            const qint64 at = inodeOffset(m_sb, leaf);
            QVERIFY(patch(img, at + 0x10, le64(qint64(kNDirect) * m_sb.bsize)));
            for (int i = 0; i < kNDirect; ++i)
                QVERIFY(patch(img, at + 0x70 + 8 * i, le64(shared)));
        }

        QElapsedTimer timer;
        timer.start();
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        QVERIFY(r.value()->readDirectory(leaves.front()).ok()); // each folder alone is readable
        CancellationToken token;
        const auto tree = r.value()->loadTree(token);
        QVERIFY(!tree.ok());
        QCOMPARE(tree.error().code, ErrorCode::CorruptData);
        QVERIFY2(tree.error().message.contains(QStringLiteral("folders take more space than the whole image")),
                 qPrintable(tree.error().message));
        const auto c = checkImage(*r.value(), token, nullptr, 100000);
        QVERIFY(c.ok());
        QVERIFY(!c->ok());
        QVERIFY2(c->problems.join(u'\n').contains(QStringLiteral("more blocks than the image holds")),
                 qPrintable(c->problems.join(u'\n')));
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
    }

    void foldersNestedDeeperThanTheLimitAreRefused_data()
    {
        QTest::addColumn<int>("depth");
        QTest::addColumn<bool>("readable");
        QTest::newRow("200 deep reads completely") << 200 << true;
        QTest::newRow("300 deep is refused") << 300 << false;
    }

    void foldersNestedDeeperThanTheLimitAreRefused()
    {
        QFETCH(int, depth);
        QFETCH(bool, readable);
        const SourceTree src = nestedFolders(depth);
        const QString img = m_tmp.filePath(QStringLiteral("deep-%1.ffpkg").arg(depth));
        CancellationToken token;
        {
            Ufs2Writer w(src, WriterOptions{});
            const auto plan = w.plan();
            QVERIFY2(plan.ok(), plan.ok() ? "" : qPrintable(plan.error().toLogString()));
            const auto written = w.write(img, token, nullptr);
            QVERIFY2(written.ok(), written.ok() ? "" : qPrintable(written.error().toLogString()));
        }
        QElapsedTimer timer;
        timer.start();
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        const auto tree = r.value()->loadTree(token);
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
        if (readable) {
            QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
            QCOMPARE(tree->dirCount, qint64(depth));
        } else {
            QVERIFY(!tree.ok());
            QCOMPARE(tree.error().code, ErrorCode::CorruptData);
            QVERIFY2(tree.error().message.contains(QStringLiteral("nested more than 256 deep")),
                     qPrintable(tree.error().message));
        }
    }
};

QTEST_GUILESS_MAIN(TestUfs2Hostile)
#include "tst_ufs2_hostile.moc"
