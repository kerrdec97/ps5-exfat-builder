// image.compare and the format-neutral file access behind it (ImageFiles.h),
// with images built by the real engines: identical exFAT / FFPKG / FFPFSC
// images of one dump compare clean against each other and against the dump;
// an edited copy shows every difference kind (only in A, only in B, folders,
// size, content with the first differing offset); structure depth skips the
// contents; lists are capped while counts stay exact; cancellation; job
// validation; and byte slices read from every format match the source file.

#include "core/Settings.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/general/Module.h"
#include "engines/integration/ImageCompare.h"
#include "engines/integration/ImageFiles.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"

#include <cstring>
#include "engines/ufs2/Module.h"
#include "connect/ps5/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::integration;

namespace {

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    std::unique_ptr<SettingsStore> store;
    std::unique_ptr<JobQueue> queue;
    explicit Env(const QString& dir)
    {
        ModuleContext ctx{*schema, deps, jobs};
        general::registerModule(ctx);
        exfat::registerModule(ctx);
        ufs2::registerModule(ctx);
        pfs::registerModule(ctx);
        fpkg::registerModule(ctx);
        ps5::registerModule(ctx);
        integration::registerModule(ctx);
        store = std::make_unique<SettingsStore>(schema, dir + QStringLiteral("/settings.json"));
        (void)store->load();
        queue = std::make_unique<JobQueue>(*store, jobs);
    }
    std::optional<JobRecord> run(const QString& kind, const QJsonObject& params, const QVariantMap& overrides = {})
    {
        auto id = queue->enqueue(kind, params, overrides);
        if (!id.ok()) {
            qWarning() << "enqueue failed:" << id.error().toLogString();
            return std::nullopt;
        }
        (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 180000);
        return queue->record(id.value());
    }
};

QString describe(const std::optional<JobRecord>& r)
{
    if (!r)
        return QStringLiteral("no record");
    return jobStateName(r->state) + QStringLiteral(": ") + (r->error ? r->error->toLogString() : (r->outcome ? r->outcome->summary : QString()));
}

bool copyTree(const QString& from, const QString& to)
{
    QDir().mkpath(to);
    for (const QFileInfo& fi : QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString dst = to + u'/' + fi.fileName();
        if (fi.isDir() ? !copyTree(fi.filePath(), dst) : !QFile::copy(fi.filePath(), dst))
            return false;
    }
    return true;
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// An in-memory tree, to drive compareTrees through cases no builder produces.
class MemoryTree final : public FileTree {
public:
    MemoryTree() : FileTree(TreeFormat::Pfs, QStringLiteral("/memory")) {}
    void add(const QString& path, const QByteArray& bytes, qint64 padBlock = 0)
    {
        TreeItem it;
        it.path = path;
        it.size = bytes.size();
        it.padBlock = padBlock;
        it.handle = qint64(m_data.size());
        m_data.push_back(bytes);
        m_items.push_back(it);
        finish();
    }
    Result<qint64> read(int index, qint64 offset, char* buf, qint64 len) override
    {
        const QByteArray& d = m_data[size_t(m_items[size_t(index)].handle)];
        const qint64 n = std::max<qint64>(0, std::min<qint64>(len, d.size() - offset));
        if (n > 0)
            std::memcpy(buf, d.constData() + offset, size_t(n));
        return n;
    }

private:
    std::vector<QByteArray> m_data;
};

QStringList strings(const QJsonValue& v)
{
    QStringList out;
    for (const auto& x : v.toArray())
        out << x.toString();
    return out;
}

} // namespace

class TstImageCompare : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_game, m_edited, m_out;
    std::unique_ptr<Env> m_env;

    QString image(const QString& name) const { return m_out + u'/' + name; }

    Result<CompareReport> compare(const QString& a, const QString& b, CompareDepth depth, int limit = 500)
    {
        FW_TRY_ASSIGN(auto ta, openFileTree(a, {}));
        FW_TRY_ASSIGN(auto tb, openFileTree(b, {}));
        CompareOptions o;
        o.depth = depth;
        o.listLimit = limit;
        o.chunkBytes = 64 * 1024; // several chunks per file
        return compareTrees(*ta, *tb, o, {});
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_game = m_root.filePath(QStringLiteral("dumps/PPSA04321"));
        QVERIFY(fwtest::writeRandomFile(m_game + QStringLiteral("/eboot.bin"), 300000, 1));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA04321","contentId":"UP0000-PPSA04321_00-0000000000000000","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeRandomFile(m_game + QStringLiteral("/data/level1.bin"), 2 * 1024 * 1024 + 13, 2));
        QVERIFY(fwtest::writeRandomFile(m_game + QStringLiteral("/data/level2.bin"), 700001, 3));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/strings/en.txt"), QByteArray(5000, 'e')));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/remove_me.txt"), QByteArray("bye")));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/empty.bin"), QByteArray()));

        // The edited copy: one file removed, one added, one folder added, one
        // file resized, one file changed in place (same size) far from its start.
        m_edited = m_root.filePath(QStringLiteral("dumps/PPSA04321-edited"));
        QVERIFY(copyTree(m_game, m_edited));
        QVERIFY(QFile::remove(m_edited + QStringLiteral("/data/remove_me.txt")));
        QVERIFY(fwtest::writeTextFile(m_edited + QStringLiteral("/data/added.txt"), QByteArray("new file")));
        QVERIFY(fwtest::writeTextFile(m_edited + QStringLiteral("/extra_dir/inside.txt"), QByteArray("x")));
        QVERIFY(fwtest::writeTextFile(m_edited + QStringLiteral("/data/strings/en.txt"), QByteArray(5001, 'e')));
        {
            QFile f(m_edited + QStringLiteral("/data/level1.bin"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(1500000));
            char c = 0;
            QVERIFY(f.getChar(&c));
            QVERIFY(f.seek(1500000));
            QVERIFY(f.putChar(char(c ^ 0x5A)));
        }

        m_out = m_root.filePath(QStringLiteral("images"));
        QDir().mkpath(m_out);
        m_env = std::make_unique<Env>(m_root.filePath(QStringLiteral("env")));
        const auto build = [&](const QString& kind, const QString& source, const QString& out, const QJsonObject& extra = {}) {
            QJsonObject p = extra;
            p.insert(QStringLiteral("source"), source);
            p.insert(QStringLiteral("output"), out);
            auto r = m_env->run(kind, p);
            return r && r->state == JobState::Succeeded ? QString() : describe(r);
        };
        QString e;
        e = build(QStringLiteral("image.exfat.build"), m_game, image(QStringLiteral("game.exfat")));
        QVERIFY2(e.isEmpty(), qPrintable(e));
        e = build(QStringLiteral("image.ffpkg.build"), m_game, image(QStringLiteral("game.ffpkg")));
        QVERIFY2(e.isEmpty(), qPrintable(e));
        e = build(QStringLiteral("image.pfs.build"), m_game, image(QStringLiteral("game.ffpfsc")), {{QStringLiteral("format"), QStringLiteral("ffpfsc")}});
        QVERIFY2(e.isEmpty(), qPrintable(e));
        e = build(QStringLiteral("image.ffpkg.build"), m_edited, image(QStringLiteral("edited.ffpkg")));
        QVERIFY2(e.isEmpty(), qPrintable(e));
    }

    void cleanupTestCase() { m_env.reset(); }

    void treesListEveryFormatTheSame()
    {
        auto folder = openFileTree(m_game, {});
        QVERIFY2(folder.ok(), folder.ok() ? "" : qPrintable(folder.error().toLogString()));
        QCOMPARE(folder.value()->format(), TreeFormat::Folder);
        QCOMPARE(folder.value()->fileCount(), 7);
        for (const QString& name : {QStringLiteral("game.exfat"), QStringLiteral("game.ffpkg"), QStringLiteral("game.ffpfsc")}) {
            auto t = openFileTree(image(name), {});
            QVERIFY2(t.ok(), t.ok() ? "" : qPrintable(name + u' ' + t.error().toLogString()));
            QCOMPARE(t.value()->fileCount(), folder.value()->fileCount());
            QCOMPARE(t.value()->dirCount(), folder.value()->dirCount());
            QVERIFY(t.value()->find(QStringLiteral("data/strings/en.txt")) >= 0);
            QVERIFY(t.value()->find(QStringLiteral("/eboot.bin")) >= 0);
            QCOMPARE(t.value()->find(QStringLiteral("nope.bin")), -1);
        }
        QCOMPARE(openFileTree(image(QStringLiteral("game.ffpfsc")), {}).value()->describe(), QStringLiteral("FFPFSC image"));
        QCOMPARE(openFileTree(m_root.filePath(QStringLiteral("missing.exfat")), {}).error().code, ErrorCode::NotFound);
        QVERIFY(fwtest::writeTextFile(m_root.filePath(QStringLiteral("notes.txt")), "x"));
        QCOMPARE(openFileTree(m_root.filePath(QStringLiteral("notes.txt")), {}).error().code, ErrorCode::Unsupported);
    }

    void identicalImagesOfOneDumpHaveNoDifferences()
    {
        // exFAT vs FFPKG built from the same folder, through the queue job.
        for (const QString& depth : {QStringLiteral("full"), QStringLiteral("structure")}) {
            auto r = m_env->run(kCompareJob, {{QStringLiteral("a"), image(QStringLiteral("game.exfat"))},
                                              {QStringLiteral("b"), image(QStringLiteral("game.ffpkg"))},
                                              {QStringLiteral("depth"), depth}});
            QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
            QCOMPARE(r->outcome->verification, Verification::NotRun);
            const QJsonObject d = r->outcome->details;
            QVERIFY2(d.value(QStringLiteral("identical")).toBool(), qPrintable(r->outcome->summary));
            QCOMPARE(d.value(QStringLiteral("depth")).toString(), depth);
            const QJsonObject c = d.value(QStringLiteral("counts")).toObject();
            QCOMPARE(c.value(QStringLiteral("differences")).toInt(), 0);
            QCOMPARE(c.value(QStringLiteral("filesInBoth")).toInt(), 7);
            QCOMPARE(c.value(QStringLiteral("contentCompared")).toInt(), depth == u"full" ? 7 : 0);
            QCOMPARE(d.value(QStringLiteral("a")).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("exFAT image"));
            QCOMPARE(d.value(QStringLiteral("b")).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("FFPKG (UFS2) image"));
            QVERIFY(r->outcome->summary.startsWith(QStringLiteral("No differences")));
        }
        // Every pair of formats, and each image against the dump folder.
        const QStringList all{image(QStringLiteral("game.exfat")), image(QStringLiteral("game.ffpkg")), image(QStringLiteral("game.ffpfsc")), m_game};
        for (int i = 0; i < all.size(); ++i)
            for (int j = 0; j < all.size(); ++j) {
                if (i == j)
                    continue;
                auto rep = compare(all[i], all[j], CompareDepth::Full);
                QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
                QVERIFY2(rep.value().identical(), qPrintable(all[i] + QStringLiteral(" vs ") + all[j] + QStringLiteral(": ") + rep.value().summary()));
                QCOMPARE(rep.value().contentCompared, 7);
            }
    }

    void editedCopyShowsEveryDifferenceKind()
    {
        auto r = m_env->run(kCompareJob, {{QStringLiteral("a"), image(QStringLiteral("game.exfat"))},
                                          {QStringLiteral("b"), image(QStringLiteral("edited.ffpkg"))}});
        QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
        const QJsonObject d = r->outcome->details;
        QCOMPARE(d.value(QStringLiteral("depth")).toString(), QStringLiteral("full")); // the default
        QVERIFY(!d.value(QStringLiteral("identical")).toBool());
        const QJsonObject c = d.value(QStringLiteral("counts")).toObject();
        QCOMPARE(strings(d.value(QStringLiteral("missing"))), QStringList{QStringLiteral("data/remove_me.txt")});
        QCOMPARE(strings(d.value(QStringLiteral("extra"))), (QStringList{QStringLiteral("data/added.txt"), QStringLiteral("extra_dir/inside.txt")}));
        QCOMPARE(strings(d.value(QStringLiteral("extraDirs"))), QStringList{QStringLiteral("extra_dir")});
        QVERIFY(strings(d.value(QStringLiteral("missingDirs"))).isEmpty());
        const QJsonArray sizes = d.value(QStringLiteral("sizeDifferent")).toArray();
        QCOMPARE(sizes.size(), 1);
        QCOMPARE(sizes[0].toObject().value(QStringLiteral("path")).toString(), QStringLiteral("data/strings/en.txt"));
        QCOMPARE(sizes[0].toObject().value(QStringLiteral("a")).toInteger(), 5000);
        QCOMPARE(sizes[0].toObject().value(QStringLiteral("b")).toInteger(), 5001);
        const QJsonArray contents = d.value(QStringLiteral("contentDifferent")).toArray();
        QCOMPARE(contents.size(), 1);
        QCOMPARE(contents[0].toObject().value(QStringLiteral("path")).toString(), QStringLiteral("data/level1.bin"));
        QCOMPARE(contents[0].toObject().value(QStringLiteral("offset")).toInteger(), 1500000);
        QCOMPARE(c.value(QStringLiteral("missing")).toInt(), 1);
        QCOMPARE(c.value(QStringLiteral("extra")).toInt(), 2);
        QCOMPARE(c.value(QStringLiteral("extraDirs")).toInt(), 1);
        QCOMPARE(c.value(QStringLiteral("sizeDifferent")).toInt(), 1);
        QCOMPARE(c.value(QStringLiteral("contentDifferent")).toInt(), 1);
        QCOMPARE(c.value(QStringLiteral("unreadable")).toInt(), 0);
        QCOMPARE(c.value(QStringLiteral("differences")).toInt(), 6);
        QVERIFY(!d.value(QStringLiteral("truncated")).toBool());
        QVERIFY2(r->outcome->summary.startsWith(QStringLiteral("6 differences")), qPrintable(r->outcome->summary));

        // Reversed: A's extras are B's missing files. Against the edited folder too.
        auto rev = compare(m_edited, image(QStringLiteral("game.ffpfsc")), CompareDepth::Full);
        QVERIFY2(rev.ok(), rev.ok() ? "" : qPrintable(rev.error().toLogString()));
        QCOMPARE(rev.value().missing, (QStringList{QStringLiteral("data/added.txt"), QStringLiteral("extra_dir/inside.txt")}));
        QCOMPARE(rev.value().missingDirs, QStringList{QStringLiteral("extra_dir")});
        QCOMPARE(rev.value().extra, QStringList{QStringLiteral("data/remove_me.txt")});
        QCOMPARE(rev.value().sizeCount, 1);
        QCOMPARE(rev.value().contentCount, 1);
        QCOMPARE(rev.value().contentDifferent.front().offset, 1500000);

        // Structure depth: names and sizes only, so the same-size change is not seen.
        auto st = compare(image(QStringLiteral("game.exfat")), image(QStringLiteral("edited.ffpkg")), CompareDepth::Structure);
        QVERIFY(st.ok());
        QCOMPARE(st.value().missingCount, 1);
        QCOMPARE(st.value().extraCount, 2);
        QCOMPARE(st.value().sizeCount, 1);
        QCOMPARE(st.value().contentCount, 0);
        QCOMPARE(st.value().contentCompared, 0);
        QCOMPARE(st.value().bytesCompared, 0);
    }

    void fileAgainstFolderAndContentAtTheFirstByte()
    {
        // A path that is a file on one side and a folder on the other is both
        // "only in A" (the file) and "only in B" (the folder).
        const QString a = m_root.filePath(QStringLiteral("kinds/a"));
        const QString b = m_root.filePath(QStringLiteral("kinds/b"));
        QVERIFY(fwtest::writeTextFile(a + QStringLiteral("/thing"), "file"));
        QVERIFY(fwtest::writeTextFile(b + QStringLiteral("/thing/inner.txt"), "x"));
        QVERIFY(fwtest::writeTextFile(a + QStringLiteral("/first.bin"), "Abcd"));
        QVERIFY(fwtest::writeTextFile(b + QStringLiteral("/first.bin"), "abcd"));
        auto rep = compare(a, b, CompareDepth::Full);
        QVERIFY(rep.ok());
        QCOMPARE(rep.value().missing, QStringList{QStringLiteral("thing")});
        QCOMPARE(rep.value().extraDirs, QStringList{QStringLiteral("thing")});
        QCOMPARE(rep.value().extra, QStringList{QStringLiteral("thing/inner.txt")});
        QCOMPARE(rep.value().contentDifferent.size(), size_t(1));
        QCOMPARE(rep.value().contentDifferent.front().offset, 0);
    }

    // A build gives an AMPR game its ampr_emu.index in the image only: against
    // the dump folder that is one difference, and the report says what it is.
    void builtAmprIndexIsExplained()
    {
        const QString game = m_root.filePath(QStringLiteral("dumps/PPSA09393"));
        QVERIFY(copyTree(m_game, game));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray(500, 'a')));
        for (const QString& fmt : {QStringLiteral("exfat"), QStringLiteral("ffpkg")}) {
            const QString img = image(QStringLiteral("ampr.") + fmt);
            auto built = m_env->run(QStringLiteral("image.%1.build").arg(fmt), {{QStringLiteral("source"), game}, {QStringLiteral("output"), img}});
            QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));
            auto rep = compare(game, img, CompareDepth::Full);
            QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
            QCOMPARE(rep.value().extra, QStringList{QStringLiteral("ampr_emu.index")});
            QCOMPARE(rep.value().differenceCount(), qint64(1));
            QCOMPARE(rep.value().warnings.size(), 1);
            QVERIFY2(rep.value().warnings.first().contains(QStringLiteral("AMPR file index")), qPrintable(rep.value().warnings.first()));
            // Two images of the same build have nothing to explain.
            auto same = compare(img, img, CompareDepth::Structure);
            QVERIFY(same.ok());
            QVERIFY(same.value().identical() && same.value().warnings.isEmpty());
        }
        // The folder's own, older index against the rebuilt one in the image.
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_emu.index"), QByteArray("an old index")));
        auto rep = compare(image(QStringLiteral("ampr.exfat")), game, CompareDepth::Full);
        QVERIFY(rep.ok());
        QCOMPARE(rep.value().sizeCount, qint64(1));
        QCOMPARE(rep.value().warnings.size(), 1);
        QVERIFY(rep.value().warnings.first().contains(QStringLiteral("AMPR file index")));
    }

    void pfsBlockPaddingIsTolerated()
    {
        // A real .ffpfs with per-file compression: the compressible file is
        // stored with its size rounded up to the block size (.txt files are
        // never compressed, hence .dat).
        const QString pad = m_root.filePath(QStringLiteral("pad"));
        QByteArray text;
        while (text.size() < 300001)
            text += "The quick brown fox jumps over the lazy dog. ";
        text.truncate(300001);
        QVERIFY(fwtest::writeTextFile(pad + QStringLiteral("/big.dat"), text));
        QVERIFY(fwtest::writeTextFile(pad + QStringLiteral("/small.txt"), "small"));
        auto built = m_env->run(QStringLiteral("image.pfs.build"),
                                {{QStringLiteral("source"), pad}, {QStringLiteral("output"), image(QStringLiteral("pad.ffpfs"))},
                                 {QStringLiteral("format"), QStringLiteral("ffpfs")}},
                                {{pfs::keys::kCompressFiles, true}, {pfs::keys::kFileMinCompressMiB, 0}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));
        auto tree = openFileTree(image(QStringLiteral("pad.ffpfs")), {});
        QVERIFY(tree.ok());
        const TreeItem& big = tree.value()->items()[size_t(tree.value()->find(QStringLiteral("big.dat")))];
        QVERIFY2(big.padBlock > 0, "big.dat was not stored compressed");
        QVERIFY(big.size > text.size());
        auto rep = compare(image(QStringLiteral("pad.ffpfs")), pad, CompareDepth::Full);
        QVERIFY(rep.ok());
        QVERIFY2(rep.value().identical(), qPrintable(rep.value().summary()));
        QCOMPARE(rep.value().paddedFiles, qint64(1));
        QVERIFY(!rep.value().warnings.isEmpty());
        auto slice = readFileSlice(image(QStringLiteral("pad.ffpfs")), QStringLiteral("big.dat"), 299990, 64);
        QVERIFY(slice.ok());
        QCOMPARE(slice.value().data.left(11), text.mid(299990, 11));

        // Non-zero padding is a content difference at the first non-zero byte;
        // a size that is not the rounded-up one is a size difference.
        MemoryTree a, b;
        a.add(QStringLiteral("ok.bin"), QByteArray("abc") + QByteArray(13, '\0'), 16);
        a.add(QStringLiteral("dirty.bin"), QByteArray("abc") + QByteArray(5, '\0') + "x" + QByteArray(7, '\0'), 16);
        a.add(QStringLiteral("wrong.bin"), QByteArray(32, 'w'), 16);
        b.add(QStringLiteral("ok.bin"), "abc");
        b.add(QStringLiteral("dirty.bin"), "abc");
        b.add(QStringLiteral("wrong.bin"), QByteArray(3, 'w'));
        CompareOptions o;
        o.chunkBytes = 4096;
        auto mem = compareTrees(a, b, o, {});
        QVERIFY(mem.ok());
        QCOMPARE(mem.value().paddedFiles, qint64(2));
        QCOMPARE(mem.value().contentCount, qint64(1));
        QCOMPARE(mem.value().contentDifferent.front().path, QStringLiteral("dirty.bin"));
        QCOMPARE(mem.value().contentDifferent.front().offset, qint64(8));
        QCOMPARE(mem.value().sizeCount, qint64(1));
        QCOMPARE(mem.value().sizeDifferent.front().path, QStringLiteral("wrong.bin"));
    }

    void listsAreCappedCountsAreExact()
    {
        const QString a = m_root.filePath(QStringLiteral("many/a"));
        const QString b = m_root.filePath(QStringLiteral("many/b"));
        QDir().mkpath(b);
        for (int i = 0; i < 30; ++i)
            QVERIFY(fwtest::writeTextFile(a + QStringLiteral("/f%1.txt").arg(i, 2, 10, QLatin1Char('0')), "x"));
        auto rep = compare(a, b, CompareDepth::Full, 10);
        QVERIFY(rep.ok());
        QCOMPARE(rep.value().missingCount, 30);
        QCOMPARE(rep.value().missing.size(), 10);
        QCOMPARE(rep.value().missing.first(), QStringLiteral("f00.txt"));
        QVERIFY(rep.value().truncated());
        const QJsonObject j = rep.value().toJson();
        QCOMPARE(j.value(QStringLiteral("counts")).toObject().value(QStringLiteral("missing")).toInt(), 30);
        QCOMPARE(j.value(QStringLiteral("missing")).toArray().size(), 10);
        QCOMPARE(j.value(QStringLiteral("listLimit")).toInt(), 10);
        QVERIFY(j.value(QStringLiteral("truncated")).toBool());
    }

    void cancellationStopsTheComparison()
    {
        auto ta = openFileTree(image(QStringLiteral("game.exfat")), {});
        auto tb = openFileTree(image(QStringLiteral("game.ffpkg")), {});
        QVERIFY(ta.ok() && tb.ok());
        // Cancelled while comparing contents (from the first progress report).
        CancellationSource source;
        ProgressReporter progress([&](const ProgressSnapshot& s) {
            if (s.done > 0)
                source.cancel();
        }, 0);
        progress.setPhases({QStringLiteral("Comparing")});
        CompareOptions o;
        o.chunkBytes = 4096;
        auto rep = compareTrees(*ta.value(), *tb.value(), o, source.token(), &progress, 0);
        QVERIFY(!rep.ok());
        QVERIFY(rep.error().isCancelled());
        // Through the queue: a cancelled job ends Cancelled.
        auto id = m_env->queue->enqueue(kCompareJob, {{QStringLiteral("a"), image(QStringLiteral("game.exfat"))},
                                                      {QStringLiteral("b"), image(QStringLiteral("game.ffpfsc"))}});
        QVERIFY(id.ok());
        m_env->queue->cancel(id.value());
        QVERIFY(QTest::qWaitFor([&] { auto r = m_env->queue->record(id.value()); return r && isTerminal(r->state); }, 60000));
        QCOMPARE(m_env->queue->record(id.value())->state, JobState::Cancelled);
    }

    void jobValidation()
    {
        const auto refused = [&](const QJsonObject& p) { return !m_env->queue->enqueue(kCompareJob, p).ok(); };
        const QString img = image(QStringLiteral("game.exfat"));
        QVERIFY(refused({{QStringLiteral("a"), img}}));
        QVERIFY(refused({{QStringLiteral("b"), img}}));
        QVERIFY(refused({{QStringLiteral("a"), img}, {QStringLiteral("b"), img}}));
        QVERIFY(refused({{QStringLiteral("a"), img}, {QStringLiteral("b"), m_root.filePath(QStringLiteral("missing.ffpkg"))}}));
        QVERIFY(refused({{QStringLiteral("a"), img}, {QStringLiteral("b"), m_root.filePath(QStringLiteral("notes.txt"))}}));
        QVERIFY(refused({{QStringLiteral("a"), img}, {QStringLiteral("b"), m_game}, {QStringLiteral("depth"), QStringLiteral("deep")}}));
        auto job = m_env->jobs.create(kCompareJob, {{QStringLiteral("a"), img}, {QStringLiteral("b"), m_game}});
        QVERIFY(job.ok());
        QCOMPARE(job.value()->busyPaths().size(), 2);
        QCOMPARE(job.value()->phases().size(), 3);
    }

    void slicesReadTheSameBytesFromEveryFormat()
    {
        const QByteArray level1 = fileBytes(m_game + QStringLiteral("/data/level1.bin"));
        const QByteArray eboot = fileBytes(m_game + QStringLiteral("/eboot.bin"));
        QVERIFY(level1.size() > 2 * 1024 * 1024);
        for (const QString& src : {image(QStringLiteral("game.exfat")), image(QStringLiteral("game.ffpkg")), image(QStringLiteral("game.ffpfsc")), m_game}) {
            auto tree = openFileTree(src, {});
            QVERIFY2(tree.ok(), qPrintable(src));
            FileTree& t = *tree.value();
            // First 64 KiB, the next 64 KiB, a slice across the end, past the end.
            for (const qint64 off : {qint64(0), qint64(65536), qint64(level1.size() - 1000), qint64(level1.size()), qint64(level1.size() + 5)}) {
                auto s = readSlice(t, QStringLiteral("/data/level1.bin"), off, 65536);
                QVERIFY2(s.ok(), s.ok() ? "" : qPrintable(src + u' ' + s.error().toLogString()));
                QCOMPARE(s.value().fileSize, qint64(level1.size()));
                QCOMPARE(s.value().offset, std::min<qint64>(off, level1.size()));
                QCOMPARE(s.value().data, level1.mid(qsizetype(std::min<qint64>(off, level1.size())), 65536));
            }
            auto e = readSlice(t, QStringLiteral("eboot.bin"), 0, 64);
            QVERIFY(e.ok());
            QCOMPARE(e.value().data, eboot.left(64));
            auto empty = readSlice(t, QStringLiteral("empty.bin"), 0, 65536);
            QVERIFY(empty.ok());
            QVERIFY(empty.value().data.isEmpty());
            QCOMPARE(empty.value().fileSize, 0);
            QCOMPARE(readSlice(t, QStringLiteral("data"), 0, 16).error().code, ErrorCode::InvalidArgument);
            QCOMPARE(readSlice(t, QStringLiteral("data/none.bin"), 0, 16).error().code, ErrorCode::NotFound);
        }
        // The one-shot helper opens and closes the image.
        auto one = readFileSlice(image(QStringLiteral("game.ffpkg")), QStringLiteral("eboot.bin"), 100, 50);
        QVERIFY(one.ok());
        QCOMPARE(one.value().data, eboot.mid(100, 50));
    }
};

QTEST_GUILESS_MAIN(TstImageCompare)
#include "tst_image_compare.moc"
