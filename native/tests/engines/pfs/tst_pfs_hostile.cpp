// PFS engine against crafted images: entry names that try to leave the
// extraction folder (or that a drive cannot store), inodes whose size does not
// fit their blocks, and a PFSC header that declares a huge block table the
// image does not hold. Also the shared path-safety helpers the extraction
// relies on (core/PathSafety.h).

#include "engines/pfs/PfsBuilder.h"
#include "engines/pfs/PfsReader.h"
#include "engines/pfs/PfsSource.h"
#include "engines/pfs/PfsVerify.h"

#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "engines/pfs/PfsTestUtil.h"
#include "support/TestSupport.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;
using namespace fw::pfs;

namespace {

constexpr qint64 kTime = 1700000000;

BuildOptions options()
{
    BuildOptions o;
    o.layout.time = kTime;
    o.threads = 2;
    o.level = 6;
    return o;
}

SourcePlan plan(const QString& root)
{
    PlanOptions p;
    p.time = kTime;
    p.ampr = AmprMode::Off; // no generated index listing the names
    auto r = planFolder(root, p, CancellationToken());
    if (!r.ok())
        qFatal("plan failed: %s", qPrintable(r.error().toLogString()));
    return std::move(r).value();
}

QString buildFolderImage(const QString& src, const QString& out)
{
    BuildInput in;
    in.plan = plan(src);
    auto r = writeImage(in, OutputFormat::FolderImage, options(), out, CancellationToken());
    if (!r.ok())
        qFatal("build failed: %s", qPrintable(r.error().toLogString()));
    return out;
}

struct Opened {
    std::shared_ptr<FileByteSource> file;
    std::unique_ptr<PfsImage> img;
};

Result<Opened> openImage(const QString& path)
{
    Opened o;
    o.file = std::make_shared<FileByteSource>(path);
    FW_TRY(o.file->open());
    FW_TRY_ASSIGN(o.img, PfsImage::open(o.file));
    return o;
}

qint64 inodeOffset(qint64 bs, quint32 ino)
{
    const qint64 perBlock = bs / kInodeD32Size;
    return bs + qint64(ino / perBlock) * bs + qint64(ino % perBlock) * kInodeD32Size;
}

// Offset of the dirent naming `name` for inode `ino`, or -1.
qint64 findDirent(const QByteArray& bytes, quint32 ino, const QByteArray& name)
{
    for (qsizetype pos = bytes.indexOf(name); pos >= 16; pos = bytes.indexOf(name, pos + 1)) {
        const char* d = bytes.constData() + pos - 16;
        if (getU32(d) == ino && getI32(d + 8) == qint32(name.size()) && getI32(d + 12) >= 17)
            return pos - 16;
    }
    return -1;
}

// Renames the image entry `path` to `newName` (same length) in its dirent and,
// for it and everything below it, in the flat_path_table, so the image stays
// consistent and only the name itself is unusual.
bool renameEntry(const QString& image, const QByteArray& path, const QByteArray& newName)
{
    QByteArray bytes = pfstest::readAll(image);
    std::vector<std::pair<QByteArray, quint32>> affected; // old full path -> inode
    bool caseInsensitive = false;
    quint32 ino = 0;
    {
        auto o = openImage(image);
        if (!o.ok()) {
            qWarning("%s", qPrintable(o.error().toLogString()));
            return false;
        }
        const PfsImage& img = *o->img;
        caseInsensitive = img.header().caseInsensitive();
        bool found = false;
        for (const auto* list : {&img.files(), &img.dirs()})
            for (const PfsEntry& e : *list) {
                if (e.path == path) {
                    ino = e.inode;
                    found = true;
                }
                if (e.path == path || e.path.startsWith(path + '/'))
                    affected.emplace_back(e.path, e.inode);
            }
        if (!found)
            return false;
    }
    const qsizetype slash = path.lastIndexOf('/');
    const QByteArray oldName = slash < 0 ? path : path.mid(slash + 1);
    if (oldName.size() != newName.size())
        return false;
    const qint64 at = findDirent(bytes, ino, oldName);
    if (at < 0)
        return false;
    bytes.replace(at + 16, newName.size(), newName);
    const QByteArray newPath = (slash < 0 ? QByteArray() : path.left(slash + 1)) + newName;
    for (const auto& [oldFull, entryIno] : affected) {
        const QByteArray newFull = newPath + oldFull.mid(path.size());
        char pattern[8];
        putU32(pattern, fptHash('/' + oldFull, caseInsensitive));
        bool patched = false;
        for (qsizetype pos = bytes.indexOf(QByteArray(pattern, 4)); pos >= 0 && !patched;
             pos = bytes.indexOf(QByteArray(pattern, 4), pos + 1)) {
            if (pos % 8 != 0 || pos + 8 > bytes.size())
                continue;
            const quint32 value = getU32(bytes.constData() + pos + 4);
            if ((value & 0x80000000u) || (value & 0x1FFFFFFFu) != entryIno) // a plain entry for this inode
                continue;
            putU32(pattern, fptHash('/' + newFull, caseInsensitive));
            bytes.replace(pos, 4, QByteArray(pattern, 4));
            patched = true;
        }
        if (!patched)
            return false;
    }
    return writeFileAtomic(image, bytes).ok();
}

QStringList listTree(const QString& dir)
{
    QStringList out;
    QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        out << QDir(dir).relativeFilePath(it.next());
    out.sort();
    return out;
}

} // namespace

class TestPfsHostile : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { fwtest::isolateAppData(m_appData); }

    void pathSafetyHelpers()
    {
        for (const char* ok : {"eboot.bin", "a b", ".hidden", "x..y", "CON", "end."})
            QVERIFY2(isSafeEntryName(QString::fromLatin1(ok)), ok);
        for (const char* bad : {"", ".", "..", "...", " ", ". .", ".. ", "a/b", "a\\b", "..\\..\\x", "C:x", "a:b", "\x01",
                                "tab\there"})
            QVERIFY2(!isSafeEntryName(QString::fromLatin1(bad)), bad);
        QVERIFY(!isSafeEntryName(QString::fromLatin1("a\0b", 3)));

        QCOMPARE(childPathInside(QStringLiteral("/out"), QStringLiteral("a/b.bin")).value(), QStringLiteral("/out/a/b.bin"));
        QCOMPARE(childPathInside(QStringLiteral("/out/"), QStringLiteral("x")).value(), QStringLiteral("/out/x"));
        for (const char* bad : {"../x", "a/../../x", "a/./b", "/etc/passwd", "a//b", "", "..\\..\\Windows\\evil.dll",
                                "C:/Windows/x", "sub/C:x", "a/.. /b"}) {
            const auto r = childPathInside(QStringLiteral("/out"), QString::fromLatin1(bad));
            QVERIFY2(!r.ok(), bad);
            QCOMPARE(r.error().code, ErrorCode::InvalidArgument);
        }

        // The Windows mapping (moved here from the UFS2 engine, unchanged).
        QCOMPARE(hostFileName(QStringLiteral("a:b?.txt"), true), QStringLiteral("a_b_.txt"));
        QCOMPARE(hostFileName(QStringLiteral("CON"), true), QStringLiteral("_CON"));
        QCOMPARE(hostFileName(QStringLiteral("nul.txt"), true), QStringLiteral("_nul.txt"));
        QCOMPARE(hostFileName(QStringLiteral("LPT9"), true), QStringLiteral("_LPT9"));
        QCOMPARE(hostFileName(QStringLiteral("trailing. "), true), QStringLiteral("trailing"));
        QCOMPARE(hostFileName(QStringLiteral("a:b?.txt"), false), QStringLiteral("a:b?.txt"));
        // On this host: always one safe component.
        const bool win = hostUsesWindowsNames();
        QCOMPARE(hostEntryName(QStringLiteral("..")), QStringLiteral("_"));
        QCOMPARE(hostEntryName(QStringLiteral("...")), QStringLiteral("_"));
        QCOMPARE(hostEntryName(QStringLiteral("..\\..\\evil.dll")), QStringLiteral(".._.._evil.dll"));
        QCOMPARE(hostEntryName(QStringLiteral("a:b")), QStringLiteral("a_b"));
        QCOMPARE(hostEntryName(QStringLiteral("CON")), win ? QStringLiteral("_CON") : QStringLiteral("CON"));
        QCOMPARE(hostEntryName(QStringLiteral("end.")), win ? QStringLiteral("end") : QStringLiteral("end."));
        QCOMPARE(hostNameKey(QStringLiteral("Ab")), win ? QStringLiteral("AB") : QStringLiteral("Ab"));
        for (const char* n : {"", ".", "..", " . ", "a/b", "a\\b", "x:y", "\x1f", "CON", "com1.txt", "end. "})
            QVERIFY2(isSafeEntryName(hostEntryName(QString::fromLatin1(n))), n);
    }

    // A directory entry whose name holds a separator or NUL (or is a dot
    // entry in disguise) could point outside an extraction folder: the image
    // is damaged and nothing is extracted.
    void separatorAndDotNamesAreDamage()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/abcdefghij"), "payload one", -1));
        const QString good = buildFolderImage(src, tmp.filePath(QStringLiteral("good.ffpfs")));
        QVERIFY(openImage(good).ok());
        const QByteArray goodBytes = pfstest::readAll(good);
        const QString bad = tmp.filePath(QStringLiteral("bad.ffpfs"));

        const QByteArray names[] = {QByteArray("..\\..\\x.dl"), QByteArray("../../x.dl"), QByteArray("abc\0efghij", 10),
                                    QByteArray("x\\..\\..\\..")};
        for (const QByteArray& name : names) {
            QVERIFY(writeFileAtomic(bad, goodBytes).ok());
            QVERIFY2(renameEntry(bad, "abcdefghij", name), name.constData());
            auto o = openImage(bad);
            QVERIFY2(!o.ok(), name.constData());
            QCOMPARE(o.error().code, ErrorCode::CorruptData);
            QVERIFY2(o.error().message.contains(QStringLiteral("invalid name")), qPrintable(o.error().toLogString()));
            const QString out = tmp.filePath(QStringLiteral("x"));
            auto ex = extractImage(bad, out, ExtractOptions{true, false, 0}, CancellationToken());
            QVERIFY(!ex.ok());
            QCOMPARE(ex.error().code, ErrorCode::CorruptData);
            QVERIFY(!QFileInfo::exists(out));
            QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x.dl"))));
        }

        // ".." (the name shortened in place) is a second parent entry.
        QByteArray bytes = goodBytes;
        quint32 ino = 0;
        {
            auto o = openImage(good);
            QVERIFY(o.ok());
            ino = o->img->findFile("abcdefghij")->inode;
        }
        const qint64 at = findDirent(bytes, ino, "abcdefghij");
        QVERIFY(at > 0);
        putI32(bytes.data() + at + 8, 2);
        bytes.replace(at + 16, 10, QByteArray("..\0\0\0\0\0\0\0\0", 10));
        QVERIFY(writeFileAtomic(bad, bytes).ok());
        auto o = openImage(bad);
        QVERIFY(!o.ok());
        QCOMPARE(o.error().code, ErrorCode::CorruptData);
    }

    // Names an image may hold but a drive cannot store as they are are renamed
    // on extraction (and reported); nothing lands outside the output folder.
    void unusualNamesAreMappedOnExtraction()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("src"));
        struct Item {
            const char* placeholder; // created on disk (any OS can)
            QByteArray imageName;    // patched into the image
        };
        const Item items[] = {
            {"zzzzz", "a:b?c"}, {"ccc", QByteArray("x\x01y")}, {"qqq", "CON"},  {"eeeee", "end. "},
            {"dupp", "dup."},   {"ddd", "..."},                {"dup", "dup"},
        };
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        for (const Item& it : items)
            QVERIFY(pfstest::writeFile(src + u'/' + QLatin1String(it.placeholder),
                                       QByteArray("content of ") + it.imageName, -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/dirxx/f.bin"), "inside", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("names.ffpfs")));
        for (const Item& it : items)
            if (it.imageName != it.placeholder)
                QVERIFY2(renameEntry(image, it.placeholder, it.imageName), it.placeholder);
        QVERIFY(renameEntry(image, "dirxx", "dir:x"));
        {
            auto o = openImage(image);
            QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString()));
            QVERIFY(o->img->findFile("dir:x/f.bin"));
            QVERIFY(o->img->findFile("..."));
        }

        const QString out = tmp.filePath(QStringLiteral("out"));
        auto r = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
        const bool win = hostUsesWindowsNames();
        const QList<QPair<QByteArray, QString>> expected = {
            {"a:b?c", QStringLiteral("a_b_c")},
            {QByteArray("x\x01y"), QStringLiteral("x_y")},
            {"CON", win ? QStringLiteral("_CON") : QStringLiteral("CON")},
            {"end. ", win ? QStringLiteral("end") : QStringLiteral("end. ")},
            {"dup", QStringLiteral("dup")},
            {"dup.", win ? QStringLiteral("dup (2)") : QStringLiteral("dup.")},
            {"...", QStringLiteral("_")},
        };
        QStringList want{QStringLiteral("eboot.bin"), QStringLiteral("dir_x"), QStringLiteral("dir_x/f.bin")};
        for (const auto& [imageName, hostName] : expected) {
            QCOMPARE(pfstest::readAll(out + u'/' + hostName), QByteArray("content of ") + imageName);
            want << hostName;
        }
        want.sort();
        QCOMPARE(listTree(out), want);
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/dir_x/f.bin")), QByteArray("inside"));
        QCOMPARE(r->renamed.size(), win ? 7 : 4);
        QVERIFY(r->renamed.contains(QStringLiteral("dir:x → dir_x")));
        QVERIFY(r->renamed.contains(QStringLiteral("... → _")));
        // Nothing next to the output folder.
        QCOMPARE(QDir(tmp.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
                 (QStringList{QStringLiteral("names.ffpfs"), QStringLiteral("out"), QStringLiteral("src")}));
    }

    // An inode that claims bytes but has no blocks (or more bytes than its
    // blocks hold) is damage: verification fails instead of passing, and
    // extraction does not write an empty file.
    void dataWithoutBlocksIsDamage()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data.bin"), pfstest::genContent(QStringLiteral("random"), 100000, 3), -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        const QByteArray good = pfstest::readAll(image);
        quint32 ino = 0;
        qint64 bs = 0;
        Inode original;
        {
            auto o = openImage(image);
            QVERIFY(o.ok());
            ino = o->img->findFile("data.bin")->inode;
            bs = o->img->header().blockSize;
            original = o->img->inodes()[ino];

            // openFile / readFile check the inode they are given.
            Inode tooBig = original;
            tooBig.size = qint64(tooBig.blocks) * bs + 1;
            tooBig.sizeCompressed = tooBig.size;
            auto f = o->img->openFile(tooBig);
            QVERIFY(!f.ok());
            QCOMPARE(f.error().code, ErrorCode::CorruptData);
            Inode noBlocks = original;
            noBlocks.blocks = 0;
            QVERIFY(!o->img->openFile(noBlocks).ok());
            const Status rs = o->img->readFile(noBlocks, [](const char*, qint64) { return success(); }, CancellationToken());
            QVERIFY(!rs.ok());
            QCOMPARE(rs.error().code, ErrorCode::CorruptData);
        }

        // blocks = 0 on disk, the data wiped (Reviewer's craft_blocks0.py).
        QByteArray bytes = good;
        putU32(bytes.data() + inodeOffset(bs, ino) + 0x60, 0);
        std::memset(bytes.data() + original.db[0] * bs, 0, size_t(qint64(original.blocks) * bs));
        QVERIFY(writeFileAtomic(image, bytes).ok());
        auto o = openImage(image);
        QVERIFY(!o.ok());
        QCOMPARE(o.error().code, ErrorCode::CorruptData);
        QVERIFY2(o.error().message.contains(QStringLiteral("no blocks")), qPrintable(o.error().toLogString()));

        SourcePlan p = plan(src);
        VerifyExpectation ex;
        ex.kind = VerifyExpectation::Kind::Folder;
        ex.format = OutputFormat::FolderImage;
        ex.plan = &p;
        auto vr = verifyImage(image, ex, VerifyDepth::Full, CancellationToken());
        QVERIFY(!vr.ok());
        QCOMPARE(vr.error().code, ErrorCode::CorruptData);
        auto er = extractImage(image, tmp.filePath(QStringLiteral("x")), ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY(!er.ok());
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x"))));

        // More bytes than the blocks hold.
        bytes = good;
        putI64(bytes.data() + inodeOffset(bs, ino) + 0x08, qint64(original.blocks) * bs + 1);
        putI64(bytes.data() + inodeOffset(bs, ino) + 0x10, qint64(original.blocks) * bs + 1);
        QVERIFY(writeFileAtomic(image, bytes).ok());
        auto big = openImage(image);
        QVERIFY(!big.ok());
        QCOMPARE(big.error().code, ErrorCode::CorruptData);
    }

    // A flat_path_table whose entries all point into the same collision
    // group, each one entry further in, would have the reader parse the
    // group once per entry. The work is bounded by the resolver's size and
    // the image is reported as damaged.
    void overlappingCollisionGroupsAreBounded()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        // "BB" and "C#" have the same path hash, so every name made of seven
        // such pairs lands in one collision group.
        QStringList colliding;
        for (int i = 0; i < 128; ++i) {
            QString name;
            for (int bit = 0; bit < 7; ++bit)
                name += (i >> bit) & 1 ? QStringLiteral("C#") : QStringLiteral("BB");
            colliding << name;
            QVERIFY(pfstest::writeFile(src + u'/' + name, QByteArray::number(i), -1));
        }
        for (int i = 0; i < 200; ++i)
            QVERIFY(pfstest::writeFile(src + QStringLiteral("/plain/%1.bin").arg(i), QByteArray::number(i), -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));

        // The honest image reads, collisions included.
        qint64 bs = 0;
        Inode fptIno, colIno;
        {
            auto o = openImage(image);
            QVERIFY2(o.ok(), qPrintable(o.ok() ? QString() : o.error().toLogString()));
            QCOMPARE(qint64(o->img->files().size()), qint64(1 + 128 + 200));
            for (const QString& name : colliding)
                QVERIFY(o->img->findFile(name.toLatin1()));
            bs = o->img->header().blockSize;
            fptIno = o->img->inodes()[1];
            colIno = o->img->inodes()[2];
        }
        QByteArray bytes = pfstest::readAll(image);
        const qint64 fptAt = fptIno.db[0] * bs, colAt = colIno.db[0] * bs;
        const qint32 entSize = getI32(bytes.constData() + colAt + 12);
        QVERIFY(entSize >= 24);
        QVERIFY(qint64(entSize) * 128 <= colIno.size);
        int patched = 0;
        for (qint64 i = 0; i + 8 <= fptIno.size; i += 8) {
            char* value = bytes.data() + fptAt + i + 4;
            if (getU32(value) & 0x80000000u)
                continue; // the real group, at offset 0
            putU32(value, 0x80000000u | quint32(entSize * (1 + patched % 127)));
            ++patched;
        }
        QVERIFY(patched >= 200);
        QVERIFY(writeFileAtomic(image, bytes).ok());

        QElapsedTimer timer;
        timer.start();
        auto o = openImage(image);
        QVERIFY(!o.ok());
        QCOMPARE(o.error().code, ErrorCode::CorruptData);
        QVERIFY2(o.error().toLogString().contains(QStringLiteral("groups overlap")), qPrintable(o.error().toLogString()));
        QVERIFY(timer.elapsed() < 5000);
    }

    // The marker of a staged extraction lives next to the staged folder, so
    // an image file may have any name, the old marker's included.
    void anImageFileMayBeNamedLikeTheOldMarker()
    {
        QTemporaryDir tmp;
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/.fw-pfs-extract"), "game data", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        const QString out = tmp.filePath(QStringLiteral("x"));
        auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(er.ok(), er.ok() ? "" : qPrintable(er.error().toLogString()));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/.fw-pfs-extract")), QByteArray("game data"));
        QCOMPARE(listTree(tmp.path()).filter(QStringLiteral("x.part")), QStringList());

        // A leftover of an interrupted extraction is replaced; a folder of
        // that name that is not ours is refused.
        const QString again = tmp.filePath(QStringLiteral("y"));
        QVERIFY(pfstest::writeFile(again + QStringLiteral(".part/half.bin"), "half", -1));
        auto refused = extractImage(image, again, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY(!refused.ok());
        QCOMPARE(refused.error().code, ErrorCode::AlreadyExists);
        QVERIFY(pfstest::writeFile(again + QStringLiteral(".part.extracting"), "", -1));
        auto replaced = extractImage(image, again, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(replaced.ok(), replaced.ok() ? "" : qPrintable(replaced.error().toLogString()));
        QVERIFY(!QFileInfo::exists(again + QStringLiteral("/half.bin")));
        QCOMPARE(listTree(tmp.path()).filter(QStringLiteral(".part")), QStringList());
    }

    // A tiny image whose PFSC header declares a 16 GiB block offset table
    // (the inode and header stretched to a 1 TiB image the file does not
    // hold; Reviewer's craft_pfsc.py) is rejected without allocating it.
    void pfscTableBeyondImageIsDamage()
    {
        QTemporaryDir tmp;
        const QString file = tmp.filePath(QStringLiteral("game.exfat"));
        QVERIFY(pfstest::writeFile(file, pfstest::genContent(QStringLiteral("text"), 3 * 1024 * 1024, 5), -1));
        BuildInput in;
        in.isFolder = false;
        in.file = file;
        const QString image = tmp.filePath(QStringLiteral("game.ffpfsc"));
        QVERIFY(writeImage(in, OutputFormat::Container, options(), image, CancellationToken()).ok());
        QByteArray bytes = pfstest::readAll(image);
        Inode target;
        qint64 bs = 0;
        {
            auto o = openImage(image);
            QVERIFY(o.ok());
            bs = o->img->header().blockSize;
            for (const Inode& i : o->img->inodes())
                if (i.blocks > 0 && i.db[0] > target.db[0])
                    target = i;
            QVERIFY(target.isCompressed()); // the last payload is the compressed file
        }
        const qint64 newNd = (qint64(1) << 40) / bs;
        const qint64 newBlocks = newNd - target.db[0];
        const qint64 n = qint64(1) << 31;
        const qint64 base = target.db[0] * bs;
        putI64(bytes.data() + 0x38, newNd);
        putU32(bytes.data() + inodeOffset(bs, target.number) + 0x60, quint32(newBlocks));
        putI64(bytes.data() + inodeOffset(bs, target.number) + 0x08, newBlocks * bs);
        putU64(bytes.data() + base + 0x20, quint64(0x400 + (n + 1) * 8));
        putI64(bytes.data() + base + 0x28, n * kPfscBlock);
        const QString crafted = tmp.filePath(QStringLiteral("crafted.ffpfsc"));
        QVERIFY(writeFileAtomic(crafted, bytes).ok());
        QVERIFY(QFileInfo(crafted).size() < 8 * 1024 * 1024);

        QElapsedTimer timer;
        timer.start();
        auto o = openImage(crafted);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString())); // the metadata itself is intact
        QVERIFY(o->img->missingTailBytes() > (qint64(1) << 39));
        const Inode& ino = o->img->inodes()[target.number];
        const Status rs = o->img->readFile(ino, [](const char*, qint64) { return success(); }, CancellationToken());
        QVERIFY(!rs.ok());
        QCOMPARE(rs.error().code, ErrorCode::CorruptData);
        QVERIFY2(rs.error().message.contains(QStringLiteral("past the end of the image")), qPrintable(rs.error().toLogString()));
        auto f = o->img->openFile(ino);
        QVERIFY(!f.ok());
        QCOMPARE(f.error().code, ErrorCode::CorruptData);
        o = Error(); // release the image before verifying the file again
        auto vr = verifyImage(crafted, VerifyExpectation{}, VerifyDepth::Structure, CancellationToken());
        QVERIFY(!vr.ok());
        QCOMPARE(vr.error().code, ErrorCode::CorruptData);
        QVERIFY2(timer.elapsed() < 30000, "the crafted table was read instead of refused");
    }

private:
    QTemporaryDir m_appData;
};

QTEST_GUILESS_MAIN(TestPfsHostile)
#include "tst_pfs_hostile.moc"
