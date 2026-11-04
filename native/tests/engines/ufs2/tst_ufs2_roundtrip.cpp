// Writer -> reader round trips on synthetic trees, the structure checker,
// comparison (changed / missing / extra), corruption detection, extraction,
// edits, cancellation and a throughput benchmark.

#include "Ufs2TestUtil.h"

#include "engines/ufs2/Ufs2Check.h"
#include "engines/ufs2/Ufs2Edit.h"
#include "engines/ufs2/Ufs2Verify.h"

#include <QElapsedTimer>
#include <QTemporaryDir>

using namespace ufs2test;

namespace {

// Reads the whole image back into a snapshot like snapshotFolder().
QMap<QString, QByteArray> snapshotImage(const QString& image)
{
    QMap<QString, QByteArray> out;
    auto r = Ufs2Reader::open(image);
    if (!r.ok())
        return out;
    CancellationToken token;
    auto tree = r.value()->loadTree(token);
    if (!tree.ok())
        return out;
    for (size_t i = 1; i < tree->nodes.size(); ++i) {
        const TreeNode& n = tree->nodes[i];
        const QString rel = tree->relativePath(int(i));
        if (n.isDir()) {
            out.insert(rel + u'/', QByteArray());
            continue;
        }
        auto s = r.value()->openFile(n.imageIno);
        QByteArray data(n.size, '\0');
        if (!s.ok() || s.value()->read(data.data(), n.size).valueOr(-1) != n.size)
            data = "<unreadable>";
        out.insert(rel, data);
    }
    return out;
}

Result<CheckReport> check(const QString& image)
{
    FW_TRY_ASSIGN(auto r, Ufs2Reader::open(image));
    CancellationToken token;
    return checkImage(*r, token);
}

QString problems(const Result<CheckReport>& c)
{
    if (!c.ok())
        return c.error().toLogString();
    return c->problems.join(QStringLiteral("\n"));
}

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

} // namespace

class TestUfs2RoundTrip : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;
    QString m_src;

    void makeRichTree(const QString& root)
    {
        QVERIFY(makeGameTree(root, 3 * 1024 * 1024 + 123));
        // Unicode, 4-byte UTF-8, long names (255 bytes in ASCII and in 3-byte characters).
        QVERIFY(mkfile(root + QStringLiteral("/Ünïcödé/日本語のファイル.bin"), 12345, 31));
        QVERIFY(mkfile(root + QStringLiteral("/Ünïcödé/emoji 🎮.txt"), 77, 32));
        QVERIFY(mkfile(root + QStringLiteral("/Ünïcödé/ελληνικά"), 0, 33));
        QVERIFY(mkfile(root + u'/' + QString(255, u'L'), 999, 34));
        QVERIFY(mkfile(root + u'/' + QString(85, QChar(0x6587)), 1001, 35));
        // Empty folders, nested empty folders.
        QVERIFY(QDir().mkpath(root + "/empty/a/b/c"));
        // Many small files across folders (more than one directory block).
        for (int d = 0; d < 6; ++d)
            for (int i = 0; i < 400; ++i)
                QVERIFY(mkfile(root + QStringLiteral("/small/d%1/file_%2_with_a_longer_name.dat").arg(d).arg(i), (i * 13) % 700, quint32(d * 1000 + i)));
        // Multi-MiB random data and an exact block multiple.
        QVERIFY(mkfile(root + "/media/movie.bin", 9 * 1024 * 1024 + 7, 40));
        QVERIFY(mkfile(root + "/media/exact64k.bin", 65536 * 3, 41));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_src = m_tmp.filePath("rich");
        makeRichTree(m_src);
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("block");
        QTest::addColumn<int>("frag");
        QTest::addColumn<int>("minfree");
        QTest::addColumn<int>("bpi");
        QTest::newRow("legacy 64K/64K") << 65536 << 65536 << 0 << 262144;
        QTest::newRow("32K/32K -i 128K") << 32768 << 32768 << 0 << 131072;
        QTest::newRow("64K/8K -m 8") << 65536 << 8192 << 8 << 524288;
        QTest::newRow("32K/4K") << 32768 << 4096 << 0 << 262144;
    }

    void roundTrip()
    {
        QFETCH(int, block);
        QFETCH(int, frag);
        QFETCH(int, minfree);
        QFETCH(int, bpi);
        const QString img = m_tmp.filePath(QStringLiteral("rt-%1-%2.ffpkg").arg(block).arg(frag));
        WriterOptions opt;
        opt.params = FormatParams{block, frag, minfree, bpi};
        PlanSummary s;
        auto st = buildImage(m_src, img, opt, &s);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QCOMPARE(QFileInfo(img).size(), s.imageBytes);
        const auto c = check(img);
        QVERIFY2(c.ok() && c->ok(), qPrintable(problems(c)));
        QCOMPARE(c->files, qint64(2414));
        QCOMPARE(snapshotImage(img), snapshotFolder(m_src));

        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        QCOMPARE(r.value()->sb().bsize, block);
        QCOMPARE(r.value()->sb().fsize, frag);
        QCOMPARE(r.value()->sb().minfree, minfree);
        CancellationToken token;
        auto tree = scanLocalFolder(m_src, token);
        QVERIFY(tree.ok());
        auto cmp = compareImage(*r.value(), tree.value(), CompareDepth::Full, token, nullptr);
        QVERIFY(cmp.ok());
        QVERIFY2(cmp->ok(), qPrintable(cmp->summary(CompareDepth::Full)));
        QCOMPARE(cmp->filesCompared, qint64(2414));
        // Paths resolve, Unicode included.
        QVERIFY(r.value()->resolve(QStringLiteral("/Ünïcödé/emoji 🎮.txt")).ok());
        QCOMPARE(r.value()->resolve(QStringLiteral("/nope")).error().code, ErrorCode::NotFound);
    }

    void emptyFolderAndOnlyEmptyFiles()
    {
        const QString src = m_tmp.filePath("emptyish");
        QVERIFY(QDir().mkpath(src + "/a"));
        QVERIFY(mktext(src + "/zero.bin", ""));
        const QString img = m_tmp.filePath("emptyish.ffpkg");
        QVERIFY(buildImage(src, img).ok());
        const auto c = check(img);
        QVERIFY2(c.ok() && c->ok(), qPrintable(problems(c)));
        QCOMPARE(snapshotImage(img), snapshotFolder(src));
    }

    void compareDetectsChangedMissingExtra()
    {
        const QString src = m_tmp.filePath("cmp");
        QVERIFY(makeGameTree(src));
        const QString img = m_tmp.filePath("cmp.ffpkg");
        QVERIFY(buildImage(src, img).ok());
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        auto compare = [&](CompareDepth d) {
            auto tree = scanLocalFolder(src, token);
            return compareImage(*r.value(), tree.value(), d, token, nullptr).value();
        };
        QVERIFY(compare(CompareDepth::Full).ok());
        // Same size, one byte different: only a full comparison sees it.
        {
            QFile f(src + "/data/big.pak");
            QVERIFY(f.open(QIODevice::ReadWrite) && f.seek(1500000));
            char c = 0;
            f.read(&c, 1);
            c = char(c ^ 0x5A);
            f.seek(1500000);
            f.write(&c, 1);
        }
        QVERIFY(compare(CompareDepth::Structure).ok());
        auto full = compare(CompareDepth::Full);
        QCOMPARE(full.changed.size(), 1);
        QVERIFY2(full.changed.first().contains(QStringLiteral("data/big.pak")) && full.changed.first().contains(QStringLiteral("1500000")),
                 qPrintable(full.changed.first()));
        // Size change, new file (missing from the image), deleted file (extra in the image).
        QVERIFY(mktext(src + "/sce_sys/param.json", "{\"changed\":true}"));
        QVERIFY(mktext(src + "/new.txt", "new"));
        QVERIFY(QFile::remove(src + "/eboot.bin"));
        auto st = compare(CompareDepth::Structure);
        QVERIFY(!st.ok());
        QVERIFY(st.missing.contains(QStringLiteral("new.txt")));
        QVERIFY(st.extra.contains(QStringLiteral("eboot.bin")));
        QCOMPARE(st.changed.size(), 1);
        QVERIFY(st.changed.first().startsWith(QStringLiteral("sce_sys/param.json")));
        // Folder replaced by a file of the same name.
        QVERIFY(QDir(src + "/emptydir").removeRecursively());
        QVERIFY(mktext(src + "/emptydir", "x"));
        st = compare(CompareDepth::Structure);
        QVERIFY(std::any_of(st.changed.begin(), st.changed.end(), [](const QString& s) { return s.startsWith(QStringLiteral("emptydir")); }));
        QVERIFY(!st.summary(CompareDepth::Structure).isEmpty());
    }

    void corruptionIsDetected()
    {
        const QString src = m_tmp.filePath("corrupt-src");
        QVERIFY(makeGameTree(src, 40 * 1024 * 1024)); // several cylinder groups
        const QString good = m_tmp.filePath("good.ffpkg");
        QVERIFY(buildImage(src, good).ok());
        auto r = Ufs2Reader::open(good);
        QVERIFY(r.ok());
        const SuperblockInfo sb = r.value()->sb();
        QVERIFY(sb.ncg > 1);
        r = Error(ErrorCode::Internal, QString());
        auto copy = [&](const QString& name) {
            const QString p = m_tmp.filePath(name);
            QFile::remove(p);
            return QFile::copy(good, p) ? p : QString();
        };
        // 1. Superblock magic.
        {
            const QString p = copy("bad-magic.ffpkg");
            QVERIFY(patch(p, kSuperblockOffset + 0x55C, le32(0x12345678)));
            auto o = Ufs2Reader::open(p);
            QVERIFY(!o.ok());
            QCOMPARE(o.error().code, ErrorCode::CorruptData);
        }
        // 2. Superblock geometry (block size).
        {
            const QString p = copy("bad-bsize.ffpkg");
            QVERIFY(patch(p, kSuperblockOffset + 0x30, le32(3000)));
            QCOMPARE(Ufs2Reader::open(p).error().code, ErrorCode::CorruptData);
        }
        // 3. Truncated image (incomplete copy).
        {
            const QString p = copy("truncated.ffpkg");
            QFile f(p);
            QVERIFY(f.resize(f.size() - 65536));
            auto o = Ufs2Reader::open(p);
            QCOMPARE(o.error().code, ErrorCode::CorruptData);
            QVERIFY(o.error().message.contains(QStringLiteral("incomplete")));
        }
        // 4. Not an image at all.
        {
            const QString p = m_tmp.filePath("random.bin");
            QVERIFY(mkfile(p, 300000, 77));
            QCOMPARE(Ufs2Reader::open(p).error().code, ErrorCode::CorruptData);
            QVERIFY(mkfile(p, 100, 77));
            QCOMPARE(Ufs2Reader::open(p).error().code, ErrorCode::CorruptData);
        }
        // 5. A cylinder-group header.
        {
            const QString p = copy("bad-cg.ffpkg");
            QVERIFY(patch(p, (qint64(sb.fpg) + sb.cblkno) * sb.fsize + 4, le32(0)));
            const auto c = check(p);
            QVERIFY(c.ok());
            QVERIFY(!c->ok());
            QVERIFY2(c->problems.join(u'\n').contains(QStringLiteral("Cylinder group 1")), qPrintable(problems(c)));
        }
        // 6. A free-fragment bit flipped (a used block marked free).
        {
            const QString p = copy("bad-map.ffpkg");
            auto rr = Ufs2Reader::open(p);
            QVERIFY(rr.ok());
            auto root = rr.value()->readInode(kRootIno);
            QVERIFY(root.ok());
            const qint64 rootBlock = root->db[0]; // in cylinder group 0
            rr = Error(ErrorCode::Internal, QString());
            const qint64 hdr = qint64(sb.cblkno) * sb.fsize;
            QFile f(p);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.seek(hdr + 0x60);
            QByteArray off = f.read(4);
            const qint64 byteOff = hdr + rd32s(off.constData()) + rootBlock / 8;
            f.seek(byteOff);
            char b = 0;
            f.read(&b, 1);
            b = char(b | (1 << (rootBlock % 8)));
            f.seek(byteOff);
            f.write(&b, 1);
            f.close();
            const auto c = check(p);
            QVERIFY(c.ok() && !c->ok());
            QVERIFY2(problems(c).contains(QStringLiteral("marked free")), qPrintable(problems(c)));
        }
        // 7. A directory entry with a zero record length.
        {
            const QString p = copy("bad-dir.ffpkg");
            auto rr = Ufs2Reader::open(p);
            const qint64 rootBlock = rr.value()->readInode(kRootIno)->db[0];
            rr = Error(ErrorCode::Internal, QString());
            QVERIFY(patch(p, rootBlock * sb.fsize + 12 + 4, QByteArray(2, '\0'))); // ".." d_reclen
            auto o = Ufs2Reader::open(p);
            QVERIFY(o.ok());
            QCOMPARE(o.value()->readDirectory(kRootIno).error().code, ErrorCode::CorruptData);
            CancellationToken token;
            QCOMPARE(o.value()->loadTree(token).error().code, ErrorCode::CorruptData);
            const auto c = check(p);
            QVERIFY(c.ok() && !c->ok());
        }
        // 8. A block pointer outside the file system.
        {
            const QString p = copy("bad-ptr.ffpkg");
            auto rr = Ufs2Reader::open(p);
            auto ino = rr.value()->resolve(QStringLiteral("/eboot.bin"));
            QVERIFY(ino.ok());
            const qint64 off = (qint64(ino.value() / quint32(sb.ipg)) * sb.fpg + sb.iblkno) * sb.fsize
                               + qint64(ino.value() % quint32(sb.ipg)) * kInodeSize + 0x70;
            rr = Error(ErrorCode::Internal, QString());
            QByteArray v(8, '\0');
            wr64(v.data(), sb.size + 100);
            QVERIFY(patch(p, off, v));
            auto o = Ufs2Reader::open(p);
            auto s = o.value()->openFile(ino.value());
            QCOMPARE(s.error().code, ErrorCode::CorruptData);
            const auto c = check(p);
            QVERIFY(c.ok() && !c->ok());
        }
        // 9. Link count.
        {
            const QString p = copy("bad-nlink.ffpkg");
            auto rr = Ufs2Reader::open(p);
            auto ino = rr.value()->resolve(QStringLiteral("/sce_sys"));
            rr = Error(ErrorCode::Internal, QString());
            const qint64 off = (qint64(ino.value() / quint32(sb.ipg)) * sb.fpg + sb.iblkno) * sb.fsize
                               + qint64(ino.value() % quint32(sb.ipg)) * kInodeSize + 2;
            QByteArray v(2, '\0');
            wr16(v.data(), 9);
            QVERIFY(patch(p, off, v));
            const auto c = check(p);
            QVERIFY(c.ok() && !c->ok());
            QVERIFY2(problems(c).contains(QStringLiteral("link count 9")), qPrintable(problems(c)));
        }
        // The untouched image is still clean.
        const auto c = check(good);
        QVERIFY2(c.ok() && c->ok(), qPrintable(problems(c)));
    }

    void cancellationStopsTheWriter()
    {
        const QString src = m_tmp.filePath("cancel-src");
        QVERIFY(mkfile(src + "/a.bin", 64 * 1024 * 1024, 5));
        CancellationSource cs;
        auto tree = scanLocalFolder(src, cs.token());
        QVERIFY(tree.ok());
        Ufs2Writer w(tree.value(), WriterOptions{});
        QVERIFY(w.plan().ok());
        ProgressReporter progress([&](const ProgressSnapshot& s) {
            if (s.done > 8 * 1024 * 1024)
                cs.cancel();
        }, 0);
        progress.begin(QStringLiteral("Writing"), tree->totalBytes);
        auto st = w.write(m_tmp.filePath("cancel.ffpkg.part"), cs.token(), &progress);
        QVERIFY(!st.ok());
        QVERIFY(st.error().isCancelled());
    }

    void sourceChangingDuringBuildFails()
    {
        const QString src = m_tmp.filePath("shrink-src");
        QVERIFY(mkfile(src + "/a.bin", 1000000, 5));
        CancellationToken token;
        auto tree = scanLocalFolder(src, token);
        QVERIFY(tree.ok());
        QVERIFY(mkfile(src + "/a.bin", 500000, 5)); // shrinks after the scan
        Ufs2Writer w(tree.value(), WriterOptions{});
        auto st = w.write(m_tmp.filePath("shrink.ffpkg"), token, nullptr);
        QVERIFY(!st.ok());
        QCOMPARE(st.error().code, ErrorCode::IoError);
        QVERIFY(st.error().message.contains(QStringLiteral("shorter")));
    }

    void extractRoundTrip()
    {
        const QString img = m_tmp.filePath("extract.ffpkg");
        QVERIFY(buildImage(m_src, img).ok());
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        const QString dest = m_tmp.filePath("extracted");
        QVERIFY(QDir().mkpath(dest));
        auto ex = extractTree(tree.value(), dest, token, nullptr);
        QVERIFY2(ex.ok(), ex.ok() ? "" : qPrintable(ex.error().toLogString()));
        QCOMPARE(ex->files, qint64(2414));
        QCOMPARE(snapshotFolder(dest), snapshotFolder(m_src));
        auto v = verifyExtracted(tree.value(), ex.value(), CompareDepth::Full, token, nullptr);
        QVERIFY(v.ok() && v->ok());
        // A damaged extracted file is caught.
        QVERIFY(patch(dest + "/media/movie.bin", 4096, "XXXX"));
        v = verifyExtracted(tree.value(), ex.value(), CompareDepth::Full, token, nullptr);
        QVERIFY(v.ok() && !v->ok());
        // Sub-folder extraction.
        auto sub = r.value()->loadTree(token, nullptr, QStringLiteral("/sce_sys"));
        QVERIFY(sub.ok());
        QCOMPARE(sub->fileCount, qint64(2));
    }

    void windowsFileNames()
    {
        QCOMPARE(hostFileName(QStringLiteral("a:b?.txt"), true), QStringLiteral("a_b_.txt"));
        QCOMPARE(hostFileName(QStringLiteral("CON"), true), QStringLiteral("_CON"));
        QCOMPARE(hostFileName(QStringLiteral("nul.txt"), true), QStringLiteral("_nul.txt"));
        QCOMPARE(hostFileName(QStringLiteral("trailing. "), true), QStringLiteral("trailing"));
        QCOMPARE(hostFileName(QStringLiteral("a:b?.txt"), false), QStringLiteral("a:b?.txt"));
        QCOMPARE(hostFileName(QStringLiteral("日本.bin"), true), QStringLiteral("日本.bin"));
    }

    void editsRebuildTheTree()
    {
        const QString src = m_tmp.filePath("edit-src");
        QVERIFY(makeGameTree(src));
        const QString img = m_tmp.filePath("edit.ffpkg");
        QVERIFY(buildImage(src, img).ok());
        const QString local = m_tmp.filePath("edit-local");
        QVERIFY(mktext(local + "/patch/param.json", "{\"patched\":1}"));
        QVERIFY(mktext(local + "/patch/eboot.bin", "NEWEBOOT"));
        QVERIFY(mkfile(local + "/addme/x/y.bin", 300000, 8));
        QVERIFY(mktext(local + "/single.txt", "single"));

        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        CancellationToken token;
        auto tree = r.value()->loadTree(token);
        QVERIFY(tree.ok());
        QJsonArray ops{
            QJsonObject{{"op", "add"}, {"path", "/added/dir"}, {"source", local + "/addme"}},
            QJsonObject{{"op", "add"}, {"path", "/single.txt"}, {"source", local + "/single.txt"}},
            QJsonObject{{"op", "replace"}, {"path", "/sce_sys/param.json"}, {"source", local + "/patch/param.json"}},
            QJsonObject{{"op", "replace"}, {"path", "/"}, {"source", local + "/patch"}}, // eboot.bin only
            QJsonObject{{"op", "delete"}, {"path", "/data/big.pak"}},
            QJsonObject{{"op", "rename"}, {"path", "/emptydir"}, {"newName", "renamed"}},
            QJsonObject{{"op", "mkdir"}, {"path", "/new/nested"}},
        };
        auto parsed = parseEditOps(ops);
        QVERIFY(parsed.ok());
        QStringList log;
        QVERIFY(applyEdits(tree.value(), parsed.value(), token, &log).ok());
        const QString out = m_tmp.filePath("edited.ffpkg");
        Ufs2Writer w(tree.value(), WriterOptions{});
        QVERIFY(w.write(out, token, nullptr).ok());
        const auto c = check(out);
        QVERIFY2(c.ok() && c->ok(), qPrintable(problems(c)));
        const auto snap = snapshotImage(out);
        QCOMPARE(snap.value(QStringLiteral("eboot.bin")), QByteArray("NEWEBOOT"));
        QCOMPARE(snap.value(QStringLiteral("sce_sys/param.json")), QByteArray("{\"patched\":1}"));
        QCOMPARE(snap.value(QStringLiteral("single.txt")), QByteArray("single"));
        QCOMPARE(snap.value(QStringLiteral("added/dir/x/y.bin")), readAll(local + "/addme/x/y.bin"));
        QVERIFY(!snap.contains(QStringLiteral("data/big.pak")));
        QVERIFY(snap.contains(QStringLiteral("renamed/")) && !snap.contains(QStringLiteral("emptydir/")));
        QVERIFY(snap.contains(QStringLiteral("new/nested/")));
        QCOMPARE(snap.value(QStringLiteral("data/level1/level2/level3/deep.bin")), readAll(src + "/data/level1/level2/level3/deep.bin"));
        // Invalid edits are refused.
        auto t2 = r.value()->loadTree(token);
        auto bad = parseEditOps(QJsonArray{QJsonObject{{"op", "add"}, {"path", "/eboot.bin"}, {"source", local + "/single.txt"}}});
        QCOMPARE(applyEdits(t2.value(), bad.value(), token).error().code, ErrorCode::AlreadyExists);
        bad = parseEditOps(QJsonArray{QJsonObject{{"op", "delete"}, {"path", "/missing"}}});
        QCOMPARE(applyEdits(t2.value(), bad.value(), token).error().code, ErrorCode::NotFound);
        bad = parseEditOps(QJsonArray{QJsonObject{{"op", "replace"}, {"path", "/sce_sys"}, {"source", local + "/single.txt"}}});
        QCOMPARE(applyEdits(t2.value(), bad.value(), token).error().code, ErrorCode::InvalidArgument);
        QVERIFY(!parseEditOps(QJsonArray{QJsonObject{{"op", "explode"}, {"path", "/x"}}}).ok());
        QVERIFY(!parseEditOps(QJsonArray{QJsonObject{{"op", "rename"}, {"path", "/x"}}}).ok());
    }

    void benchmark()
    {
        // ~200 MiB of random data in large and small files.
        const QString src = m_tmp.filePath("bench");
        for (int i = 0; i < 6; ++i)
            QVERIFY(mkfile(src + QStringLiteral("/big%1.bin").arg(i), 32 * 1024 * 1024 + i, quint32(900 + i)));
        for (int i = 0; i < 2000; ++i)
            QVERIFY(mkfile(src + QStringLiteral("/small/%1/f%2.bin").arg(i % 20).arg(i), 4000 + i, quint32(5000 + i)));
        CancellationToken token;
        auto tree = scanLocalFolder(src, token);
        QVERIFY(tree.ok());
        const QString img = m_tmp.filePath("bench.ffpkg");
        Ufs2Writer w(tree.value(), WriterOptions{});
        auto st = w.write(img, token, nullptr);
        QVERIFY(st.ok());
        auto r = Ufs2Reader::open(img);
        QVERIFY(r.ok());
        QElapsedTimer t;
        t.start();
        auto c = checkImage(*r.value(), token);
        const qint64 checkMs = t.elapsed();
        QVERIFY(c.ok() && c->ok());
        t.restart();
        auto cmp = compareImage(*r.value(), tree.value(), CompareDepth::Full, token, nullptr);
        const qint64 cmpMs = std::max<qint64>(1, t.elapsed());
        QVERIFY(cmp.ok() && cmp->ok());
        auto mbps = [](qint64 bytes, qint64 ms) { return double(bytes) / (1024.0 * 1024.0) / (double(std::max<qint64>(1, ms)) / 1000.0); };
        qInfo("benchmark: %lld files, %.1f MiB; write %lld ms = %.0f MiB/s; structure check %lld ms; full compare %lld ms = %.0f MiB/s",
              tree->fileCount, double(tree->totalBytes) / 1048576.0, st->elapsedMs, mbps(tree->totalBytes, st->elapsedMs), checkMs,
              cmpMs, mbps(tree->totalBytes, cmpMs));
    }
};

QTEST_GUILESS_MAIN(TestUfs2RoundTrip)
#include "tst_ufs2_roundtrip.moc"
