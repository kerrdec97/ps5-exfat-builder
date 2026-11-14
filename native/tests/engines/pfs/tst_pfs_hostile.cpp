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
#include <QTemporaryDir>
#include <QTest>

#include <utility>

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

Result<SourcePlan> plan(const QString& root)
{
    PlanOptions p;
    p.time = kTime;
    p.ampr = AmprMode::Off; // no generated index listing the names
    return planFolder(root, p, CancellationToken());
}

// The image's path, or an empty string (with the reason in the log) when it
// could not be built: the caller asserts on that.
QString buildFolderImage(const QString& src, const QString& out)
{
    auto p = plan(src);
    if (!p.ok()) {
        qWarning("plan failed: %s", qPrintable(p.error().toLogString()));
        return {};
    }
    BuildInput in;
    in.plan = std::move(p).value();
    auto r = writeImage(in, OutputFormat::FolderImage, options(), out, CancellationToken());
    if (!r.ok()) {
        qWarning("build failed: %s", qPrintable(r.error().toLogString()));
        return {};
    }
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
    void initTestCase()
    {
        QVERIFY(m_appData.isValid());
        fwtest::isolateAppData(m_appData);
    }

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
            FWTEST_COMPARE_ERROR(r, ErrorCode::InvalidArgument);
        }

        // The Windows mapping (shared by the PFS, UFS2 and exFAT extraction).
        QCOMPARE(hostFileName(QStringLiteral("a:b?.txt"), true), QStringLiteral("a_b_.txt"));
        QCOMPARE(hostFileName(QStringLiteral("日本.bin"), true), QStringLiteral("日本.bin"));
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
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/abcdefghij"), "payload one", -1));
        const QString good = buildFolderImage(src, tmp.filePath(QStringLiteral("good.ffpfs")));
        QVERIFY(!good.isEmpty());
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
            FWTEST_COMPARE_ERROR(o, ErrorCode::CorruptData);
            QVERIFY2(o.error().message.contains(QStringLiteral("invalid name")), qPrintable(o.error().toLogString()));
            const QString out = tmp.filePath(QStringLiteral("x"));
            auto ex = extractImage(bad, out, ExtractOptions{true, false, 0}, CancellationToken());
            QVERIFY(!ex.ok());
            FWTEST_COMPARE_ERROR(ex, ErrorCode::CorruptData);
            QVERIFY(!QFileInfo::exists(out));
            QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("x.dl"))));
        }

        // ".." (the name shortened in place) is a second parent entry.
        QByteArray bytes = goodBytes;
        quint32 ino = 0;
        {
            auto o = openImage(good);
            QVERIFY(o.ok());
            const PfsEntry* entry = o->img->findFile("abcdefghij");
            QVERIFY(entry);
            ino = entry->inode;
        }
        const qint64 at = findDirent(bytes, ino, "abcdefghij");
        QVERIFY(at > 0);
        putI32(bytes.data() + at + 8, 2);
        bytes.replace(at + 16, 10, QByteArray("..\0\0\0\0\0\0\0\0", 10));
        QVERIFY(writeFileAtomic(bad, bytes).ok());
        auto o = openImage(bad);
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::CorruptData);
    }

    // Names an image may hold but a drive cannot store as they are are renamed
    // on extraction (and reported); nothing lands outside the output folder.
    void unusualNamesAreMappedOnExtraction()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
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
        QVERIFY(!image.isEmpty());
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
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data.bin"), pfstest::genContent(QStringLiteral("random"), 100000, 3), -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QByteArray good = pfstest::readAll(image);
        quint32 ino = 0;
        qint64 bs = 0;
        Inode original;
        {
            auto o = openImage(image);
            QVERIFY(o.ok());
            const PfsEntry* entry = o->img->findFile("data.bin");
            QVERIFY(entry);
            ino = entry->inode;
            bs = o->img->header().blockSize;
            original = o->img->inodes()[ino];

            // openFile / readFile check the inode they are given.
            Inode tooBig = original;
            tooBig.size = qint64(tooBig.blocks) * bs + 1;
            tooBig.sizeCompressed = tooBig.size;
            auto f = o->img->openFile(tooBig);
            QVERIFY(!f.ok());
            FWTEST_COMPARE_ERROR(f, ErrorCode::CorruptData);
            Inode noBlocks = original;
            noBlocks.blocks = 0;
            QVERIFY(!o->img->openFile(noBlocks).ok());
            const Status rs = o->img->readFile(noBlocks, [](const char*, qint64) { return success(); }, CancellationToken());
            QVERIFY(!rs.ok());
            FWTEST_COMPARE_ERROR(rs, ErrorCode::CorruptData);
        }

        // blocks = 0 on disk, the data wiped (Reviewer's craft_blocks0.py).
        QByteArray bytes = good;
        putU32(bytes.data() + inodeOffset(bs, ino) + 0x60, 0);
        std::memset(bytes.data() + original.db[0] * bs, 0, size_t(qint64(original.blocks) * bs));
        QVERIFY(writeFileAtomic(image, bytes).ok());
        auto o = openImage(image);
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::CorruptData);
        QVERIFY2(o.error().message.contains(QStringLiteral("no blocks")), qPrintable(o.error().toLogString()));

        auto p = plan(src);
        QVERIFY2(p.ok(), p.ok() ? "" : qPrintable(p.error().toLogString()));
        VerifyExpectation ex;
        ex.kind = VerifyExpectation::Kind::Folder;
        ex.format = OutputFormat::FolderImage;
        ex.plan = &p.value();
        auto vr = verifyImage(image, ex, VerifyDepth::Full, CancellationToken());
        QVERIFY(!vr.ok());
        FWTEST_COMPARE_ERROR(vr, ErrorCode::CorruptData);
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
        FWTEST_COMPARE_ERROR(big, ErrorCode::CorruptData);
    }

    // A flat_path_table whose entries all point into the same collision
    // group, each one entry further in, would have the reader parse the
    // group once per entry. The work is bounded by the resolver's size and
    // the image is reported as damaged.
    void overlappingCollisionGroupsAreBounded()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
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
        QVERIFY(!image.isEmpty());

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

        // "groups overlap" is the bounded check speaking: the reader stopped
        // at the resolver's size instead of parsing the group once per entry.
        auto o = openImage(image);
        QVERIFY(!o.ok());
        FWTEST_COMPARE_ERROR(o, ErrorCode::CorruptData);
        QVERIFY2(o.error().toLogString().contains(QStringLiteral("groups overlap")), qPrintable(o.error().toLogString()));
    }

    // The marker of a staged extraction lives next to the staged folder, so
    // an image file may have any name, the old marker's included.
    void anImageFileMayBeNamedLikeTheOldMarker()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/.fw-pfs-extract"), "game data", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
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
        FWTEST_COMPARE_ERROR(refused, ErrorCode::AlreadyExists);
        QVERIFY(pfstest::writeFile(again + QStringLiteral(".part.extracting"), "", -1));
        auto replaced = extractImage(image, again, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(replaced.ok(), replaced.ok() ? "" : qPrintable(replaced.error().toLogString()));
        QVERIFY(!QFileInfo::exists(again + QStringLiteral("/half.bin")));
        QCOMPARE(listTree(tmp.path()).filter(QStringLiteral(".part")), QStringList());
    }

    // Extracting into a folder the user allowed to be filled: a file of theirs
    // called like the temporary name of an extracted file ("<name>.part") is
    // left as it is.
    void aUsersPartFileIsNeverTaken()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/level.bin"), "level data", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QString out = tmp.filePath(QStringLiteral("filled"));
        QVERIFY(pfstest::writeFile(out + QStringLiteral("/data/level.bin.part"), "the user's download", -1));
        auto er = extractImage(image, out, ExtractOptions{true, true, 0}, CancellationToken());
        QVERIFY2(er.ok(), er.ok() ? "" : qPrintable(er.error().toLogString()));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/level.bin")), QByteArray("level data"));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/level.bin.part")), QByteArray("the user's download"));
        QCOMPARE(listTree(out), (QStringList{QStringLiteral("data"), QStringLiteral("data/level.bin"), QStringLiteral("data/level.bin.part"),
                                             QStringLiteral("eboot.bin")}));
    }

    // A file of the image called like the working name of another one
    // ("<name>.fw.part") is never taken for a leftover and removed. Windows
    // ignores letter case and the image's sorted order puts "A.BIN.FW.PART"
    // before "a.bin": that one then gets a free name.
    void aFileCalledLikeAWorkingNameIsKept()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/A.BIN.FW.PART"), "named like a working file", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/a.bin"), "level data", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/b.bin"), "more data", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/b.bin.fw.part"), "named like one too", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QString out = tmp.filePath(QStringLiteral("x"));
        auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(er.ok(), er.ok() ? "" : qPrintable(er.error().toLogString()));
        const bool windows = hostUsesWindowsNames();
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/A.BIN.FW.PART")), QByteArray("named like a working file"));
        QCOMPARE(pfstest::readAll(out + (windows ? QStringLiteral("/data/a.bin (2)") : QStringLiteral("/data/a.bin"))),
                 QByteArray("level data"));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/b.bin")), QByteArray("more data"));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/b.bin.fw.part")), QByteArray("named like one too"));
        QCOMPARE(er->files, qint64(5));
        QCOMPARE(listTree(out).size(), 6); // data and the five files
        QCOMPARE(er->renamed.join(u'\n').contains(QStringLiteral("data/a.bin → a.bin (2)")), windows);
    }

    // A folder of the image called like a file's working name is never taken
    // for that file's leftover (nor written into): the file gets a free name.
    void aFolderCalledLikeAWorkingNameIsKept()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/c.bin.fw.part/inside.bin"), "in a folder", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/c.bin"), "beside the folder", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QString out = tmp.filePath(QStringLiteral("x"));
        auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(er.ok(), er.ok() ? "" : qPrintable(er.error().toLogString()));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/c.bin.fw.part/inside.bin")), QByteArray("in a folder"));
        QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/c.bin (2)")), QByteArray("beside the folder"));
        QCOMPARE(listTree(out).size(), 5); // data, its folder, three files
        const QString renamed = er->renamed.join(u'\n');
        QVERIFY2(renamed.contains(QStringLiteral("data/c.bin → c.bin (2)")), qPrintable(renamed));
    }

    // A name of 248 to 255 characters has no room for the working name of an
    // extracted file ("<name>.fw.part"): it is written under a shortened
    // working name of its own and renamed.
    void longNamesAreExtracted()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        const QStringList names{QString(246, u'n') + QStringLiteral(".bin"), QString(251, u'm') + QStringLiteral(".bin")}; // 250, 255
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        for (const QString& n : names)
            QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/") + n, n.left(1).toLatin1(), -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QString out = tmp.filePath(QStringLiteral("x"));
        auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY2(er.ok(), er.ok() ? "" : qPrintable(er.error().toLogString()));
        for (const QString& n : names)
            QCOMPARE(pfstest::readAll(out + QStringLiteral("/data/") + n), n.left(1).toLatin1());
        QCOMPARE(listTree(out).size(), 4); // data, its two files, eboot.bin: nothing else
    }

    // The empty folder chosen for the extraction is in use when the finished
    // extraction is to take its name: the files are kept next to it, not
    // deleted, the error says where, and the user's folder stays as it was.
    void finishedExtractionIsKeptWhenTheFolderCannotBeUsed()
    {
#ifndef Q_OS_WIN
        QSKIP("A folder in use can be removed on this platform.");
#endif
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/level.bin"), "level data", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        const QString out = tmp.filePath(QStringLiteral("held"));
        QVERIFY(QDir().mkpath(out));
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(out)); // the current folder of a process cannot be removed
        auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken());
        QVERIFY(QDir::setCurrent(before));
        QVERIFY(!er.ok());
        // The work is saved: not an error the queue retries by itself, which
        // would extract another full copy.
        QVERIFY2(er.error().code != ErrorCode::IoError, qPrintable(er.error().toLogString()));
        const QString kept = out + QStringLiteral(".new");
        QVERIFY2(er.error().message.contains(QStringLiteral("were extracted")), qPrintable(er.error().toLogString()));
        QVERIFY2(er.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(er.error().toLogString()));
        QCOMPARE(pfstest::readAll(kept + QStringLiteral("/data/level.bin")), QByteArray("level data"));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part.extracting")));
        QVERIFY(QFileInfo(out).isDir());
        QVERIFY(QDir(out).isEmpty());
    }

    // What is at the folder's name when the extraction is done decides what
    // the error says: files were put into it meanwhile, or a file of that
    // name was made. The extracted files are kept next to it either way, and
    // what the user put there is not touched.
    void finishedExtractionSaysWhyTheFolderCouldNotBeUsed()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/eboot.bin"), "boot", -1));
        QVERIFY(pfstest::writeFile(src + QStringLiteral("/data/level.bin"), "level data", -1));
        const QString image = buildFolderImage(src, tmp.filePath(QStringLiteral("game.ffpfs")));
        QVERIFY(!image.isEmpty());
        for (const bool fileInTheWay : {false, true}) {
            const QString out = tmp.filePath(fileInTheWay ? QStringLiteral("file") : QStringLiteral("filled"));
            QVERIFY(QDir().mkpath(out));
            bool done = false;
            ProgressHooks hooks;
            hooks.item = [&](const QString&) {
                if (std::exchange(done, true))
                    return;
                if (fileInTheWay)
                    done = QDir().rmdir(out) && pfstest::writeFile(out, "theirs", -1);
                else
                    done = pfstest::writeFile(out + QStringLiteral("/theirs.txt"), "theirs", -1);
            };
            auto er = extractImage(image, out, ExtractOptions{true, false, 0}, CancellationToken(), hooks);
            QVERIFY(done);
            QVERIFY(!er.ok());
            QCOMPARE(er.error().code, ErrorCode::AlreadyExists);
            const QString kept = out + QStringLiteral(".new");
            const QString why = fileInTheWay ? QStringLiteral("exists and is not a folder") : QStringLiteral("is no longer empty");
            QVERIFY2(er.error().message.contains(why), qPrintable(er.error().toLogString()));
            QVERIFY2(er.error().message.contains(QDir::toNativeSeparators(kept)), qPrintable(er.error().toLogString()));
            QCOMPARE(pfstest::readAll(kept + QStringLiteral("/data/level.bin")), QByteArray("level data"));
            QCOMPARE(pfstest::readAll(fileInTheWay ? out : out + QStringLiteral("/theirs.txt")), QByteArray("theirs"));
            QVERIFY(!QFileInfo::exists(out + QStringLiteral(".part")));
        }
    }

    // A tiny image whose PFSC header declares a 16 GiB block offset table
    // (the inode and header stretched to a 1 TiB image the file does not
    // hold; Reviewer's craft_pfsc.py) is rejected without allocating it.
    void pfscTableBeyondImageIsDamage()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
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

        // Refused with "damaged" each time: the 16 GiB table is neither
        // allocated nor read (a read past the 8 MiB file could not succeed).
        auto o = openImage(crafted);
        QVERIFY2(o.ok(), o.ok() ? "" : qPrintable(o.error().toLogString())); // the metadata itself is intact
        QVERIFY(o->img->missingTailBytes() > (qint64(1) << 39));
        const Inode& ino = o->img->inodes()[target.number];
        const Status rs = o->img->readFile(ino, [](const char*, qint64) { return success(); }, CancellationToken());
        QVERIFY(!rs.ok());
        FWTEST_COMPARE_ERROR(rs, ErrorCode::CorruptData);
        QVERIFY2(rs.error().message.contains(QStringLiteral("past the end of the image")), qPrintable(rs.error().toLogString()));
        auto f = o->img->openFile(ino);
        QVERIFY(!f.ok());
        FWTEST_COMPARE_ERROR(f, ErrorCode::CorruptData);
        o = Error(); // release the image before verifying the file again
        auto vr = verifyImage(crafted, VerifyExpectation{}, VerifyDepth::Structure, CancellationToken());
        QVERIFY(!vr.ok());
        FWTEST_COMPARE_ERROR(vr, ErrorCode::CorruptData);
    }

private:
    QTemporaryDir m_appData;
};

QTEST_GUILESS_MAIN(TestPfsHostile)
#include "tst_pfs_hostile.moc"
