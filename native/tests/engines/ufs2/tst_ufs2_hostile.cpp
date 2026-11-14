// Images made to hang a reader or to exhaust memory: superblock sizes that
// overflow, a block map that points back at itself, folders that share their
// blocks, folders nested without end. A small valid image is patched for each
// case; the reader must report damage, and quickly.

#include "Ufs2TestUtil.h"

#include "core/FsLinks.h"
#include "engines/ufs2/Ufs2Check.h"
#include "engines/ufs2/Ufs2Edit.h"
#include "engines/ufs2/Ufs2Verify.h"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using namespace ufs2test;

namespace {

// The guarded work takes milliseconds; unguarded it runs for hours or never
// ends. The bound only has to tell those apart, so it is wide enough for a
// machine whose disk and processors are all busy with other tests.
constexpr qint64 kQuickMs = 120000;
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
        fwtest::isolateAppData(m_tmp);
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
            FWTEST_COMPARE_ERROR(parsed, ErrorCode::CorruptData);

            const QString img = copyOf(QStringLiteral("overflow.ffpkg"));
            QVERIFY(patch(img, kSuperblockOffset, b));
            QElapsedTimer timer;
            timer.start();
            const auto opened = Ufs2Reader::open(img);
            QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
            QVERIFY(!opened.ok());
            FWTEST_COMPARE_ERROR(opened, ErrorCode::CorruptData);
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
            FWTEST_COMPARE_ERROR(parsed, ErrorCode::CorruptData);
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
        FWTEST_COMPARE_ERROR(blocks, ErrorCode::CorruptData);
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
        FWTEST_COMPARE_ERROR(entries, ErrorCode::CorruptData);
        CancellationToken token;
        const auto tree = r.value()->loadTree(token);
        QVERIFY(!tree.ok());
        FWTEST_COMPARE_ERROR(tree, ErrorCode::CorruptData);
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
        FWTEST_COMPARE_ERROR(tree, ErrorCode::CorruptData);
        QVERIFY2(tree.error().message.contains(QStringLiteral("folders take more space than the whole image")),
                 qPrintable(tree.error().message));
        const auto c = checkImage(*r.value(), token, nullptr, 100000);
        QVERIFY(c.ok());
        QVERIFY(!c->ok());
        QVERIFY2(c->problems.join(u'\n').contains(QStringLiteral("more blocks than the image holds")),
                 qPrintable(c->problems.join(u'\n')));
        QVERIFY2(timer.elapsed() < kQuickMs, qPrintable(QString::number(timer.elapsed())));
    }

    // A folder that lists one name twice: a kernel would use only one entry,
    // so neither the structure check nor the comparison may call it sound.
    void nameListedTwiceIsReported()
    {
        const QString src = m_tmp.filePath("twins-src");
        QVERIFY(mktext(src + "/eboot.bin", "eboot"));
        QVERIFY(mktext(src + "/twins/aa.txt", "one"));
        QVERIFY(mktext(src + "/twins/ab.txt", "two"));
        const QString img = m_tmp.filePath("twins.ffpkg");
        QVERIFY(buildImage(src, img).ok());
        qint64 dirBytes = 0;
        {
            auto r = Ufs2Reader::open(img);
            QVERIFY(r.ok());
            const auto dir = r.value()->resolve(QStringLiteral("/twins"));
            QVERIFY(dir.ok());
            const auto in = r.value()->readInode(dir.value());
            QVERIFY(in.ok());
            dirBytes = in->db[0] * r.value()->sb().fsize;
        }
        {
            QFile f(img);
            QVERIFY(f.open(QIODevice::ReadWrite) && f.seek(dirBytes));
            const QByteArray chunk = f.read(512);
            const qsizetype at = chunk.indexOf("ab.txt");
            QVERIFY(at > 0);
            QVERIFY(f.seek(dirBytes + at) && f.write("aa.txt", 6) == 6);
        }
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        const auto c = checkImage(*r.value(), token);
        QVERIFY(c.ok());
        QVERIFY2(c->problems.join(u'\n').contains(QStringLiteral("lists the name \"aa.txt\" twice")),
                 qPrintable(c->problems.join(u'\n')));
        const auto expected = scanLocalFolder(src, token);
        QVERIFY(expected.ok());
        const auto cmp = compareImage(*r.value(), expected.value(), CompareDepth::Full, token, nullptr);
        QVERIFY(cmp.ok());
        QVERIFY(!cmp->ok());
        QVERIFY2(cmp->extra.join(u'\n').contains(QStringLiteral("twins/aa.txt (listed twice)")), qPrintable(cmp->extra.join(u'\n')));
    }

    // UFS1 keeps its superblock at 8192: the image is named for what it is.
    void ufs1IsReportedAsUnsupported()
    {
        const QString img = m_tmp.filePath("ufs1.img");
        {
            QFile f(img);
            QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(400 * 1024));
        }
        QVERIFY(patch(img, 8192 + 0x55C, le32(0x00011954)));
        const auto r = Ufs2Reader::open(img);
        QVERIFY(!r.ok());
        FWTEST_COMPARE_ERROR(r, ErrorCode::Unsupported);
        QVERIFY2(r.error().message.contains(QStringLiteral("UFS1")), qPrintable(r.error().message));
    }

    // di_nlink is 16 bits: a folder with too many sub-folders is refused when
    // the image is planned, not after it was written and failed its check.
    void tooManySubFoldersAreRefusedWhenPlanning()
    {
        SourceTree t;
        TreeNode root;
        root.kind = NodeKind::Dir;
        t.nodes.push_back(root);
        for (int i = 1; i <= 32766; ++i) {
            TreeNode n;
            n.name = QStringLiteral("d%1").arg(i);
            n.rawName = n.name.toUtf8();
            n.kind = NodeKind::Dir;
            n.parent = 0;
            t.nodes[0].children.push_back(i);
            t.nodes.push_back(n);
        }
        t.recount();
        Ufs2Writer w(t, WriterOptions{});
        const auto plan = w.plan();
        QVERIFY(!plan.ok());
        FWTEST_COMPARE_ERROR(plan, ErrorCode::Unsupported);
        QVERIFY2(plan.error().message.contains(QStringLiteral("32,765")), qPrintable(plan.error().message));
    }

    // "Replace folder" matches files by the names stored in the image. A
    // name with a backslash must not reach outside the chosen folder.
    void replaceFolderIgnoresNamesThatLeaveTheFolder()
    {
        const QString base = m_tmp.filePath("replace");
        QVERIFY(mktext(base + "/original.txt", "in the image"));
        QVERIFY(mktext(base + "/local/d/ok.txt", "new ok"));
        QVERIFY(mktext(base + "/local/outside.txt", "OUTSIDE THE CHOSEN FOLDER"));
        SourceTree t;
        const auto addNode = [&t](int parent, const QString& name, NodeKind kind, const QString& local) {
            TreeNode n;
            n.name = name;
            n.rawName = name.toUtf8();
            n.kind = kind;
            n.parent = parent;
            n.localPath = local;
            n.size = local.isEmpty() ? 0 : QFileInfo(local).size();
            const int idx = int(t.nodes.size());
            t.nodes.push_back(n);
            if (parent >= 0)
                t.nodes[size_t(parent)].children.push_back(idx);
            return idx;
        };
        addNode(-1, QString(), NodeKind::Dir, QString());
        const int d = addNode(0, QStringLiteral("d"), NodeKind::Dir, QString());
        addNode(d, QStringLiteral("ok.txt"), NodeKind::File, base + "/original.txt");
        addNode(d, QStringLiteral("..\\outside.txt"), NodeKind::File, base + "/original.txt");
        t.recount();
        const QString img = m_tmp.filePath("replace.ffpkg");
        CancellationToken token;
        {
            Ufs2Writer w(t, WriterOptions{});
            QVERIFY(w.plan().ok());
            QVERIFY(w.write(img, token, nullptr).ok());
        }
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        EditOp op;
        op.kind = EditOp::Kind::Replace;
        op.path = QStringLiteral("/d");
        op.source = base + "/local/d";
        const Status st = applyEdits(tree.value(), {op}, token);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        int checked = 0;
        for (const TreeNode& n : tree->nodes) {
            if (n.name == QStringLiteral("ok.txt")) {
                QVERIFY(n.localPath.endsWith(QStringLiteral("/local/d/ok.txt")));
                ++checked;
            } else if (n.name.contains(QStringLiteral("outside"))) {
                QVERIFY2(n.localPath.isEmpty(), qPrintable(n.localPath));
                QVERIFY(n.imageIno != 0);
                ++checked;
            }
        }
        QCOMPARE(checked, 2);
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
            FWTEST_COMPARE_ERROR(tree, ErrorCode::CorruptData);
            QVERIFY2(tree.error().message.contains(QStringLiteral("nested more than 256 deep")),
                     qPrintable(tree.error().message));
        }
    }

    // A source folder the user may not list is not an empty folder: the scan
    // fails and names it instead of building an image without its files.
    void sourceFolderThatCannotBeListedFailsTheScan()
    {
        const QString src = m_tmp.filePath(QStringLiteral("denied"));
        QVERIFY(mktext(src + QStringLiteral("/a.txt"), "a"));
        QVERIFY(mktext(src + QStringLiteral("/locked/secret.txt"), "secret"));
        const QString locked = src + QStringLiteral("/locked");
        if (!fwtest::denyListing(locked, true))
            QSKIP("Access to a folder cannot be taken away here.");
        const auto restore = qScopeGuard([&] { (void)fwtest::denyListing(locked, false); });
        if (!QDir(locked).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty())
            QSKIP("This account can still list the folder (administrator or root).");
        CancellationToken token;
        const auto tree = scanLocalFolder(src, token);
        QVERIFY(!tree.ok());
        FWTEST_COMPARE_ERROR(tree, ErrorCode::PermissionDenied);
        QVERIFY2(tree.error().message.contains(QLatin1String("locked")), qPrintable(tree.error().toLogString()));
    }

    // A junction that leads back to a folder above it is an error at once
    // (Qt's canonical path does not show where a junction leads, so the scan
    // used to follow it until the path grew too long).
    void junctionBackIntoTheSourceIsRefused()
    {
        const QString src = m_tmp.filePath(QStringLiteral("looped"));
        QVERIFY(mktext(src + QStringLiteral("/data/near.txt"), "near"));
        const QString back = src + QStringLiteral("/data/back");
        if (!createDirectoryJunction(back, src).ok())
            QSKIP("Folder links cannot be created here.");
        const auto unlink = qScopeGuard([&] { (void)removeDirectoryJunction(back); });
        CancellationToken token;
        const auto tree = scanLocalFolder(src, token);
        QVERIFY(!tree.ok());
        FWTEST_COMPARE_ERROR(tree, ErrorCode::InvalidArgument);
        QVERIFY2(tree.error().message.contains(QLatin1String("links back into itself")), qPrintable(tree.error().toLogString()));
        // The first pass through the link already names it: nothing below it was listed twice.
        QVERIFY2(QDir::fromNativeSeparators(tree.error().message).endsWith(QLatin1String("data/back links back into itself, so it cannot be put into an image.")),
                 qPrintable(tree.error().message));
    }

    // A junction whose folder is gone is named as a broken link, not entered
    // and reported as a folder that cannot be read (Qt calls such a junction
    // an existing folder).
    void junctionToAFolderThatIsGoneIsABrokenLink()
    {
        const QString src = m_tmp.filePath(QStringLiteral("dangling"));
        QVERIFY(mktext(src + QStringLiteral("/data/near.txt"), "near"));
        const QString gone = m_tmp.filePath(QStringLiteral("dangling-target"));
        QVERIFY(QDir().mkpath(gone));
        const QString dead = src + QStringLiteral("/data/dead");
        if (!createDirectoryJunction(dead, gone).ok())
            QSKIP("Folder links cannot be created here.");
        const auto unlink = qScopeGuard([&] { (void)removeDirectoryJunction(dead); });
        QVERIFY(QDir(gone).removeRecursively());
        CancellationToken token;
        const auto tree = scanLocalFolder(src, token);
        QVERIFY(!tree.ok());
        FWTEST_COMPARE_ERROR(tree, ErrorCode::InvalidArgument);
        QVERIFY2(tree.error().message.contains(QLatin1String("broken link")), qPrintable(tree.error().toLogString()));
        QVERIFY2(tree.error().message.contains(QDir::toNativeSeparators(dead)), qPrintable(tree.error().toLogString()));
    }
};

QTEST_GUILESS_MAIN(TestUfs2Hostile)
#include "tst_ufs2_hostile.moc"
