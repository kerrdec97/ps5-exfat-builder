// Compatibility with the real tools.
//
//  * UFS2Tool 4.0.0 (the UFS2Tool.dll the legacy app embeds, run with dotnet;
//    FW_UFS2TOOL_DLL=<path>): for the same folder, the native writer must
//    produce byte-identical images. UFS2Tool stamps the build time and a
//    process-id based fs_id; those are read back from its image and given to
//    the native writer, nothing else is normalised.
//  * FreeBSD fsck_ffs / dumpfs (ufsutils: fsck.ufs, dumpfs.ufs; FW_FSCK_UFS /
//    FW_DUMPFS_UFS or PATH) must accept the native images.
//  * UFS2Tool's own fsck_ufs and ls must accept them too.
//  * NetBSD makefs (PATH) images must be readable by the native reader.
// Each check is skipped when its tool is not installed.

#include "Ufs2TestUtil.h"

#include <QTemporaryDir>

using namespace ufs2test;

class TestUfs2Compat : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tmp;

    struct RefMeta {
        qint64 t1 = 0, t2 = 0;
        quint32 fsId1 = 0;
    };

    static RefMeta refMeta(const QString& image)
    {
        RefMeta m;
        auto r = Ufs2Reader::open(image);
        if (!r.ok())
            return m;
        m.t1 = r.value()->sb().time;
        m.fsId1 = r.value()->sb().id[1];
        auto root = r.value()->readInode(kRootIno);
        if (root.ok())
            m.t2 = root->mtime;
        return m;
    }

    static QString describeOffset(const QString& image, qint64 off)
    {
        auto r = Ufs2Reader::open(image);
        if (!r.ok())
            return QStringLiteral("offset %1").arg(off);
        const auto& sb = r.value()->sb();
        const qint64 frag = off / sb.fsize;
        const qint64 cg = frag / sb.fpg;
        return QStringLiteral("offset %1 (fragment %2, cg %3, frag-in-cg %4, byte-in-frag %5)")
            .arg(off)
            .arg(frag)
            .arg(cg)
            .arg(frag % sb.fpg)
            .arg(off % sb.fsize);
    }

    void compareWithUfs2Tool(const QString& name, const QString& src, const FormatParams& p)
    {
        if (ufs2toolDll().isEmpty() || dotnet().isEmpty())
            QSKIP("UFS2Tool.dll (FW_UFS2TOOL_DLL) or dotnet not available");
        const QString ref = m_tmp.filePath(name + "-ufs2tool.ffpkg");
        const QString mine = m_tmp.filePath(name + "-native.ffpkg");
        QFile::remove(ref);
        const ToolRun run = runUfs2Tool({QStringLiteral("newfs"), QStringLiteral("-O"), QStringLiteral("2"),
                                         QStringLiteral("-b"), QString::number(p.blockSize), QStringLiteral("-f"),
                                         QString::number(p.fragmentSize), QStringLiteral("-m"),
                                         QString::number(p.minFreePercent), QStringLiteral("-S"), QStringLiteral("512"),
                                         QStringLiteral("-i"), QString::number(p.bytesPerInode), QStringLiteral("-D"), src,
                                         ref});
        QVERIFY2(run.exitCode == 0 && QFileInfo::exists(ref), qPrintable(run.output.right(2000)));
        const RefMeta meta = refMeta(ref);
        WriterOptions opt;
        opt.params = p;
        opt.formatTime = meta.t1;
        opt.populateTime = meta.t2;
        opt.fsId1 = meta.fsId1;
        PlanSummary summary;
        auto st = buildImage(src, mine, opt, &summary);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QVERIFY(!summary.enlarged);
        const QByteArray a = readAll(ref);
        const QByteArray b = readAll(mine);
        QCOMPARE(b.size(), a.size());
        const qint64 diff = firstDifference(a, b);
        QVERIFY2(diff < 0, qPrintable(QStringLiteral("%1: images differ at %2").arg(name, describeOffset(ref, diff))));
        qInfo("%s: %lld bytes, byte-identical to UFS2Tool (%d cylinder groups)", qPrintable(name),
              qint64(a.size()), summary.geometry.ncg);
        // UFS2Tool's own checker accepts it (it accepts the identical reference anyway).
        const ToolRun fsck = runUfs2Tool({QStringLiteral("fsck_ufs"), QStringLiteral("-n"), mine});
        QVERIFY2(fsck.exitCode == 0, qPrintable(fsck.output.right(2000)));
    }

private slots:
    void initTestCase() { QVERIFY(m_tmp.isValid()); }

    void identical_gameTree_legacyParams()
    {
        const QString src = m_tmp.filePath("game");
        QVERIFY(makeGameTree(src));
        compareWithUfs2Tool(QStringLiteral("game"), src, FormatParams{});
    }

    void identical_32k_blocks()
    {
        const QString src = m_tmp.filePath("game32");
        QVERIFY(makeGameTree(src, 5 * 1024 * 1024));
        compareWithUfs2Tool(QStringLiteral("game32"), src, FormatParams{32768, 32768, 0, 262144});
    }

    void identical_fragments_and_minfree()
    {
        // 64 KiB blocks with 32 KiB fragments: short files end in a fragment
        // and their unused tail is freed; -m 8 and -i 131072.
        const QString src = m_tmp.filePath("frag");
        QVERIFY(makeGameTree(src));
        QVERIFY(mkfile(src + "/odd/a.bin", 33000, 11));
        QVERIFY(mkfile(src + "/odd/b.bin", 65537, 12));
        QVERIFY(mkfile(src + "/odd/c.bin", 1, 13));
        compareWithUfs2Tool(QStringLiteral("frag"), src, FormatParams{65536, 32768, 8, 131072});
    }

    void identical_bytesPerInode_512k()
    {
        const QString src = m_tmp.filePath("inode512");
        QVERIFY(makeGameTree(src));
        compareWithUfs2Tool(QStringLiteral("inode512"), src, FormatParams{65536, 65536, 0, 524288});
    }

    void identical_wideDirectory_withIndirectBlock()
    {
        // 1700 names of 200 bytes at 32 KiB blocks: the directory needs more
        // than 12 blocks and therefore an indirect block.
        const QString src = m_tmp.filePath("wide");
        QVERIFY(makeWideDir(src + "/many", 1700, 200, 100));
        QVERIFY(mkfile(src + "/eboot.bin", 1000, 7));
        compareWithUfs2Tool(QStringLiteral("wide"), src, FormatParams{32768, 32768, 0, 262144});
    }

    void identical_doubleIndirect_manyGroups()
    {
        // 130 MiB file at 32 KiB blocks: past the single-indirect range
        // (12 + 4096 blocks), spread over many cylinder groups.
        const QString src = m_tmp.filePath("huge");
        QVERIFY(QDir().mkpath(src));
        {
            QFile f(src + "/huge.bin");
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(130LL * 1024 * 1024 + 12345));
            f.seek(1000);
            f.write("start");
            f.seek(129LL * 1024 * 1024);
            f.write("end");
        }
        QVERIFY(mkfile(src + "/after.bin", 100000, 9));
        compareWithUfs2Tool(QStringLiteral("huge"), src, FormatParams{32768, 32768, 0, 262144});
    }

    void identical_lastGroupJustFits()
    {
        // A 143,196,160-byte file: UFS2Tool's size leaves a last cylinder group
        // of exactly dblkno fragments (metadata only). Still identical.
        const QString src = m_tmp.filePath("lastfits");
        QVERIFY(QDir().mkpath(src));
        QFile f(src + "/big.bin");
        QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(143196160));
        f.close();
        compareWithUfs2Tool(QStringLiteral("lastfits"), src, FormatParams{});
    }

    void lastGroupTooShortIsDropped()
    {
        // A 142,868,480-byte file leaves a 1-fragment last group. UFS2Tool
        // writes its header and inode table past fs_size and has no superblock
        // copy there; the native writer drops the group like FreeBSD newfs.
        const QString src = m_tmp.filePath("lasttiny");
        QVERIFY(QDir().mkpath(src));
        QFile f(src + "/big.bin");
        QVERIFY(f.open(QIODevice::WriteOnly) && f.resize(142868480));
        f.close();
        const QString mine = m_tmp.filePath("lasttiny-native.ffpkg");
        PlanSummary s;
        QVERIFY(buildImage(src, mine, {}, &s).ok());
        QCOMPARE(s.autoSizeBytes, qint64(167837696));         // UFS2Tool's size: 2561 fragments
        QCOMPARE(s.geometry.ncg, 5);                           // instead of 6
        QCOMPARE(QFileInfo(mine).size(), qint64(2560) * 65536); // fs_size * fsize
        auto r = Ufs2Reader::open(mine);
        QVERIFY(r.ok());
        QCOMPARE(r.value()->sb().size, qint64(2560));
        const QString fsck = fsckUfs();
        if (!fsck.isEmpty()) {
            const ToolRun run = runTool(fsck, {QStringLiteral("-n"), mine});
            QVERIFY2(run.exitCode == 0 && run.output.contains(QStringLiteral("Phase 5")) && !run.output.contains(QStringLiteral("BAD")),
                     qPrintable(run.output));
        }
        if (!ufs2toolDll().isEmpty() && !dotnet().isEmpty()) {
            const QString ref = m_tmp.filePath("lasttiny-ufs2tool.ffpkg");
            const ToolRun run = runUfs2Tool({QStringLiteral("newfs"), QStringLiteral("-O"), QStringLiteral("2"), QStringLiteral("-b"),
                                             QStringLiteral("65536"), QStringLiteral("-f"), QStringLiteral("65536"), QStringLiteral("-m"),
                                             QStringLiteral("0"), QStringLiteral("-S"), QStringLiteral("512"), QStringLiteral("-i"),
                                             QStringLiteral("262144"), QStringLiteral("-D"), src, ref});
            QCOMPARE(run.exitCode, 0);
            auto rr = Ufs2Reader::open(ref);
            QVERIFY(rr.ok());
            QCOMPARE(rr.value()->sb().ncg, 6);
            QVERIFY(QFileInfo(ref).size() > rr.value()->sb().bytes()); // metadata past the end
            if (!fsck.isEmpty()) {
                const ToolRun bad = runTool(fsck, {QStringLiteral("-n"), ref});
                QVERIFY2(bad.output.contains(QStringLiteral("FIRST ALTERNATE")), qPrintable(bad.output));
            }
        }
    }

    void freebsdFsckAcceptsNativeImages()
    {
        const QString fsck = fsckUfs();
        if (fsck.isEmpty())
            QSKIP("fsck.ufs (FreeBSD fsck_ffs, Debian ufsutils) not available (set FW_FSCK_UFS)");
        const QString src = m_tmp.filePath("fsck-src");
        QVERIFY(makeGameTree(src, 40 * 1024 * 1024)); // several cylinder groups
        QVERIFY(makeWideDir(src + "/many", 300, 120, 500));
        QVERIFY(mkfile(src + QStringLiteral("/unicodé ü 日本.bin"), 3000, 3));
        for (const FormatParams& p : {FormatParams{}, FormatParams{65536, 32768, 8, 131072}, FormatParams{32768, 32768, 0, 524288}}) {
            const QString img = m_tmp.filePath(QStringLiteral("fsck-%1-%2.ffpkg").arg(p.blockSize).arg(p.fragmentSize));
            WriterOptions opt;
            opt.params = p;
            auto st = buildImage(src, img, opt);
            QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
            auto reader = Ufs2Reader::open(img);
            QVERIFY(reader.ok());
            // fsck_ffs 8.2 compares the primary superblock with the copy in the
            // last group; a one-group image has none (UFS2Tool writes copies only
            // for groups > 0), so then -b selects the primary at sector 128.
            QStringList args{QStringLiteral("-n")};
            if (reader.value()->sb().ncg == 1)
                args << QStringLiteral("-b") << QStringLiteral("128");
            args << img;
            const ToolRun r = runTool(fsck, args);
            QVERIFY2(r.exitCode == 0, qPrintable(r.output));
            QVERIFY2(r.output.contains(QStringLiteral("Phase 5")), qPrintable(r.output));
            for (const QString& bad : {QStringLiteral("UNREF"), QStringLiteral("BAD"), QStringLiteral("DUP"),
                                       QStringLiteral("INCORRECT"), QStringLiteral("SALVAGE"), QStringLiteral("FIX"),
                                       QStringLiteral("CORRUPT"), QStringLiteral("MISSING"), QStringLiteral("ALLOCATED")})
                QVERIFY2(!r.output.contains(bad), qPrintable(r.output));
            qInfo("fsck_ffs: %s", qPrintable(r.output.trimmed().split(u'\n').last()));
        }
    }

    void freebsdDumpfsReadsNativeImage()
    {
        const QString dumpfs = dumpfsUfs();
        if (dumpfs.isEmpty())
            QSKIP("dumpfs.ufs (Debian ufsutils) not available (set FW_DUMPFS_UFS)");
        const QString src = m_tmp.filePath("dumpfs-src");
        QVERIFY(makeGameTree(src));
        const QString img = m_tmp.filePath("dumpfs.ffpkg");
        PlanSummary s;
        auto st = buildImage(src, img, {}, &s);
        QVERIFY(st.ok());
        const ToolRun r = runTool(dumpfs, {img});
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(r.output.contains(QStringLiteral("magic\t19540119 (UFS2)")), qPrintable(r.output.left(500)));
        QVERIFY(r.output.contains(QStringLiteral("bsize\t65536")));
        QVERIFY(r.output.contains(QStringLiteral("fsize\t65536")));
        QVERIFY(r.output.contains(QStringLiteral("ncg\t%1").arg(s.geometry.ncg)));
        QVERIFY(r.output.contains(QStringLiteral("cg 0:")));
    }

    void ufs2toolReadsNativeImage()
    {
        if (ufs2toolDll().isEmpty() || dotnet().isEmpty())
            QSKIP("UFS2Tool.dll (FW_UFS2TOOL_DLL) or dotnet not available");
        const QString src = m_tmp.filePath("ls-src");
        QVERIFY(makeGameTree(src));
        const QString img = m_tmp.filePath("ls.ffpkg");
        QVERIFY(buildImage(src, img).ok());
        const ToolRun ls = runUfs2Tool({QStringLiteral("ls"), img, QStringLiteral("/sce_sys")});
        QCOMPARE(ls.exitCode, 0);
        QVERIFY2(ls.output.contains(QStringLiteral("param.json")) && ls.output.contains(QStringLiteral("icon0.png")),
                 qPrintable(ls.output));
        const QString out = m_tmp.filePath("ls-extract");
        const ToolRun ex = runUfs2Tool({QStringLiteral("extract"), img, out});
        QVERIFY2(ex.exitCode == 0, qPrintable(ex.output.right(1000)));
        QCOMPARE(snapshotFolder(out), snapshotFolder(src));
    }

    void readsNetBsdMakefsImage()
    {
        const QString makefs = findTool(QStringLiteral("FW_MAKEFS"), {QStringLiteral("makefs")});
        if (makefs.isEmpty())
            QSKIP("makefs (NetBSD makefs, an independent FFS writer) not available");
        const QString src = m_tmp.filePath("makefs-src");
        QVERIFY(makeGameTree(src, 3 * 1024 * 1024 + 777));
        QVERIFY(mkfile(src + "/frag/tail.bin", 9000, 21)); // ends in a fragment
        const QString img = m_tmp.filePath("makefs.img");
        const ToolRun r = runTool(makefs, {QStringLiteral("-t"), QStringLiteral("ffs"), QStringLiteral("-o"),
                                           QStringLiteral("version=2,bsize=32768,fsize=4096"), QStringLiteral("-B"),
                                           QStringLiteral("le"), QStringLiteral("-s"), QStringLiteral("24m"), img, src});
        QVERIFY2(r.exitCode == 0, qPrintable(r.output));
        auto reader = Ufs2Reader::open(img);
        QVERIFY2(reader.ok(), reader.ok() ? "" : qPrintable(reader.error().toLogString()));
        CancellationToken token;
        auto tree = reader.value()->loadTree(token);
        QVERIFY2(tree.ok(), tree.ok() ? "" : qPrintable(tree.error().toLogString()));
        QCOMPARE(tree->fileCount, qint64(8));
        // Every file reads back identically.
        const auto expected = snapshotFolder(src);
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (it.key().endsWith(u'/'))
                continue;
            auto ino = reader.value()->resolve(it.key());
            QVERIFY2(ino.ok(), qPrintable(it.key()));
            auto s = reader.value()->openFile(ino.value());
            QVERIFY(s.ok());
            QByteArray got(it.value().size() + 16, '\0');
            auto n = s.value()->read(got.data(), got.size());
            QVERIFY(n.ok());
            got.resize(int(n.value()));
            QVERIFY2(got == it.value(), qPrintable(it.key()));
        }
    }
};

QTEST_GUILESS_MAIN(TestUfs2Compat)
#include "tst_ufs2_compat.moc"
