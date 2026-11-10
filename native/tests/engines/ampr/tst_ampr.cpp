// zstd asset packs: the index and volume format, the packer (volumes, thread
// independence, read-back), the reader, packing a whole game (rules, runtime,
// links, folders, ampr_emu.index) and unpacking it again, and the LZ4
// (AMPRPAK4) reader on a hand-built pack.

#include "core/FileUtil.h"
#include "core/PathSafety.h"
#include "engines/ampr/GameFiles.h"
#include "engines/ampr/GamePack.h"
#include "engines/ampr/HostPaths.h"
#include "engines/ampr/Lz4Pack.h"
#include "engines/ampr/PackRules.h"
#include "engines/ampr/Runtime.h"
#include "engines/ampr/ZstdPack.h"
#include "engines/pfs/AmprIndex.h"
#include "support/TestSupport.h"

#include <lz4.h>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <cstring>

using namespace fw;
using namespace fw::ampr;

namespace {

QByteArray bytesOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray compressible(qint64 n, char seed)
{
    QByteArray b(qsizetype(n), '\0');
    for (qint64 i = 0; i < n; ++i)
        b[qsizetype(i)] = char('a' + ((i / 97 + seed) % 7));
    return b;
}

QByteArray randomBytes(qint64 n, quint32 seed)
{
    QByteArray b(qsizetype(n), '\0');
    QRandomGenerator g(seed);
    for (qint64 i = 0; i + 4 <= n; i += 4) {
        const quint32 v = g.generate();
        std::memcpy(b.data() + i, &v, 4);
    }
    return b;
}

// Every file under `root` -> its bytes (relative '/' paths).
QMap<QString, QByteArray> snapshot(const QString& root, const QStringList& skip = {})
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    const QDir r(root);
    while (it.hasNext()) {
        it.next();
        const QString rel = r.relativeFilePath(it.filePath());
        if (!skip.contains(rel))
            out.insert(rel, bytesOf(it.filePath()));
    }
    return out;
}

class BytesReader final : public SequentialReader {
public:
    explicit BytesReader(QByteArray b) : m_b(std::move(b)) {}
    Result<qint64> read(char* dst, qint64 max) override
    {
        const qint64 n = std::min<qint64>(max, m_b.size() - m_pos);
        std::memcpy(dst, m_b.constData() + m_pos, size_t(n));
        m_pos += n;
        return n;
    }

private:
    QByteArray m_b;
    qint64 m_pos = 0;
};

class FileAccess final : public RandomAccess {
public:
    explicit FileAccess(const QString& p) : m_f(p) { (void)m_f.open(QIODevice::ReadOnly); }
    qint64 size() const override { return m_f.size(); }
    Status readAt(qint64 off, char* dst, qint64 len) override
    {
        if (!m_f.seek(off) || m_f.read(dst, len) != len)
            return Error(ErrorCode::IoError, QStringLiteral("read"));
        return success();
    }

private:
    QFile m_f;
};

std::vector<PackInput> inputsOf(const QMap<QByteArray, QByteArray>& files)
{
    std::vector<PackInput> in;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        PackInput p;
        p.path = it.key();
        p.size = it.value().size();
        const QByteArray data = it.value();
        p.open = [data]() -> Result<std::unique_ptr<SequentialReader>> { return std::unique_ptr<SequentialReader>(new BytesReader(data)); };
        in.push_back(std::move(p));
    }
    return in;
}

void put32(QByteArray& b, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b[off + i] = char((v >> (8 * i)) & 0xFF);
}
void put64(QByteArray& b, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        b[off + i] = char((v >> (8 * i)) & 0xFF);
}

} // namespace

class TestAmpr : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        fwtest::isolateAppData(m_tmp);
    }

    void chunkRecordsRoundTrip()
    {
        ZstdChunk c;
        c.offset = 0xFFFFFFF0u;
        c.volume = 4095;
        c.stored = 262144;
        c.codec = 1;
        const ZstdChunk d = ZstdChunk::decode(c.encode());
        QCOMPARE(d.offset, c.offset);
        QCOMPARE(d.volume, c.volume);
        QCOMPARE(d.stored, c.stored);
        QCOMPARE(d.codec, c.codec);
        QCOMPARE(zstdVolumeName(0), QStringLiteral("ampr_zstd-000.pak"));
        QCOMPARE(zstdVolumeName(1234), QStringLiteral("ampr_zstd-1234.pak"));
        QVERIFY(isZstdPackFileName(QStringLiteral("ampr_zstd-007.pak")));
        QVERIFY(!isZstdPackFileName(QStringLiteral("AMPR_ZSTD-007.pak")));
        // FNV-1a 64 test vectors.
        QCOMPARE(fnv1a64(QByteArray()), 14695981039346656037ULL);
        QCOMPARE(fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
    }

    void packsReadBackAcrossVolumesAndThreads()
    {
        QMap<QByteArray, QByteArray> files;
        files.insert("data/level1.bin", compressible(900 * 1024 + 13, 1));
        files.insert("data/noise.bin", randomBytes(1600 * 1024 + 5, 7)); // stored raw, several volumes
        files.insert("Data/Case.bin", compressible(262144, 3));          // exactly one chunk, case kept
        files.insert("a/b/c/deep.txt", QByteArray("hello zstd"));
        files.insert("tiny", QByteArray("x"));

        ZstdPackOptions o;
        o.level = 5;
        o.volumeLimit = kZstdVolumeHeaderBytes + 3 * kZstdChunkBytes; // forces several volumes
        o.archiveId.fill('A');
        QMap<QString, QByteArray> first;
        for (int threads : {1, 3, 8}) {
            const QString dir = m_tmp.filePath(QStringLiteral("packs-%1").arg(threads));
            QVERIFY(QDir().mkpath(dir));
            o.threads = threads;
            auto r = writeZstdPack(inputsOf(files), o, dir, {}, nullptr);
            QVERIFY2(r.ok(), r.ok() ? "" : qPrintable(r.error().toLogString()));
            QVERIFY(r->volumes.size() >= 3);
            QVERIFY(r->rawChunks >= 2 && r->zstdChunks >= 4);
            QVERIFY(verifyWrittenZstdPack(dir, *r, {}, nullptr).ok());
            const auto snap = snapshot(dir);
            if (first.isEmpty())
                first = snap;
            else
                QVERIFY2(snap == first, "the output must not depend on the thread count");

            // Read back through the reader, whole and at awkward offsets.
            auto reader = ZstdPackReader::open(bytesOf(dir + QStringLiteral("/ampr_zstd.index")),
                                               [&](quint32 v) -> Result<std::shared_ptr<RandomAccess>> {
                                                   return std::shared_ptr<RandomAccess>(std::make_shared<FileAccess>(dir + u'/' + zstdVolumeName(int(v))));
                                               });
            QVERIFY2(reader.ok(), reader.ok() ? "" : qPrintable(reader.error().toLogString()));
            for (auto it = files.cbegin(); it != files.cend(); ++it) {
                const int fi = (*reader)->index().find(it.key());
                QVERIFY2(fi >= 0, it.key().constData());
                QByteArray back(it.value().size(), '\0');
                auto n = (*reader)->read(fi, 0, back.data(), back.size());
                QVERIFY(n.ok());
                QCOMPARE(*n, qint64(back.size()));
                QCOMPARE(back, it.value());
                if (it.value().size() > 400000) {
                    QByteArray mid(70000, '\0');
                    auto m = (*reader)->read(fi, 262144 - 1000, mid.data(), mid.size());
                    QVERIFY(m.ok());
                    QCOMPARE(mid, it.value().mid(262144 - 1000, 70000));
                }
            }
        }
    }

    void damagedPacksAreRefused()
    {
        const QString dir = m_tmp.filePath(QStringLiteral("packs-1"));
        QByteArray idx = bytesOf(dir + QStringLiteral("/ampr_zstd.index"));
        QVERIFY(decodeZstdIndex(idx).ok());
        QByteArray bad = idx;
        bad[bad.size() - 1] = char(bad[bad.size() - 1] ^ 0x20); // a path byte
        QVERIFY(!decodeZstdIndex(bad).ok());
        bad = idx;
        bad[16] = 16; // 64 KiB chunks: refused
        QCOMPARE(decodeZstdIndex(bad).error().code, ErrorCode::Unsupported);
        // A volume of another pack.
        auto ix = decodeZstdIndex(idx);
        QByteArray header = bytesOf(dir + QStringLiteral("/ampr_zstd-000.pak")).left(64);
        QVERIFY(checkZstdVolumeHeader(header, 0, *ix).ok());
        QVERIFY(!checkZstdVolumeHeader(header, 1, *ix).ok());
        header[30] = char(header[30] ^ 1);
        QVERIFY(!checkZstdVolumeHeader(header, 0, *ix).ok());
    }

    void packRules()
    {
        QVERIFY(!alwaysLooseReason(QStringLiteral("eboot.bin"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("bin/EBOOT.BIN"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("Media/Modules/x.PRX"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("sce_sys/param.json"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("SCE_MODULE/libc.prx"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("fakelib_old/a.bin"), 10).isEmpty());
        QVERIFY(!alwaysLooseReason(QStringLiteral("data/x.bin"), 0).isEmpty()); // empty
        QVERIFY(!alwaysLooseReason(QStringLiteral("ampr_emu.index"), 10).isEmpty());
        QVERIFY(alwaysLooseReason(QStringLiteral("data/sce_sys/x.bin"), 10).isEmpty()); // not the root sce_sys
        QVERIFY(alwaysLooseReason(QStringLiteral("game.ini"), 10).isEmpty());          // root files are packed
        QVERIFY(!compatibleLooseReason(QStringLiteral("game.ini")).isEmpty());
        QVERIFY(!compatibleLooseReason(QStringLiteral("Content/Paks/a.PAK")).isEmpty());
        QVERIFY(!compatibleLooseReason(QStringLiteral("data/Movies/intro.bin")).isEmpty());
        QVERIFY(!compatibleLooseReason(QStringLiteral("build/bootload_main")).isEmpty());
        QVERIFY(compatibleLooseReason(QStringLiteral("data/level1.bin")).isEmpty());
        QVERIFY(packJobDrops(QStringLiteral("ampr_commands.bin")));
        QVERIFY(!packJobDrops(QStringLiteral("data/ampr_commands.bin")));
        QCOMPARE(executableKind(QByteArray("\x54\x14\xF5\xEE....", 8)), ExecutableKind::Self);
        QCOMPARE(executableKind(QByteArray("\x7F" "ELF....", 8)), ExecutableKind::PlainElf);
    }

    void bundledRuntimeIsPinnedAndIdentified()
    {
        auto rt = loadBundledRuntime();
        QVERIFY2(rt.ok(), rt.ok() ? "" : qPrintable(rt.error().toLogString()));
        const RuntimeInfo info = identifyRuntime(*rt);
        QCOMPARE(info.kind, RuntimeKind::AmprEmuNg);
        QVERIFY(info.bundled);
        QCOMPARE(info.version, QStringLiteral("0.3.3"));
        QCOMPARE(identifyRuntime(QByteArray("....ampr-emu-ng 0.3.2....")).version, QStringLiteral("0.3.2"));
        const RuntimeInfo up = identifyRuntime(QByteArray("[AMPR_EMU] name=libSceAmpr version=%s ... 0.4.2.1 (c) Drakmor"));
        QCOMPARE(up.kind, RuntimeKind::AmprEmu);
        QCOMPARE(up.version, QStringLiteral("0.4.2.1"));
        QCOMPARE(identifyRuntime(QByteArray("other")).kind, RuntimeKind::Unknown);
    }

    void packAGameAndUnpackItAgain()
    {
        const QString game = m_tmp.filePath(QStringLiteral("games/PPSA01234"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4) + QByteArray(60000, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA01234"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray("old ampr-emu-ng 0.3.1 build")));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/fakelib/libSceNpAuth.sprx"), QByteArray("backport lib")));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/level1.bin"), compressible(1200 * 1024, 2)));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/sub/noise.bin"), randomBytes(300 * 1024, 9)));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/Media/Modules/game.prx"), QByteArray(5000, 'P')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/settings.ini"), QByteArray("[a]\nb=1\n")));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/empty.bin"), QByteArray()));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_commands.bin"), QByteArray("journal")));
        QVERIFY(QDir().mkpath(game + QStringLiteral("/saves/empty")));

        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        const QString out = m_tmp.filePath(QStringLiteral("packed/PPSA01234"));
        PackGameOptions o;
        o.threads = 4;
        o.linkFromFolder = game;
        o.freeSpaceMarginBytes = 0;
        auto rep = packGame(*files, out, o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QCOMPARE(rep->packedFiles, qint64(3)); // level1.bin, noise.bin, settings.ini
        QCOMPARE(rep->droppedFiles, QStringList{QStringLiteral("ampr_commands.bin")});
        QCOMPARE(rep->previousRuntime.kind, RuntimeKind::AmprEmuNg);
        QVERIFY(rep->amprIndexWritten);
        QVERIFY(rep->linkedFiles >= 4);

        // On disk: loose files, the packs, the bundled runtime; no folder that
        // holds only packed files (data/ would hide the packed listing).
        QVERIFY(QFile::exists(out + QStringLiteral("/ampr_zstd.index")));
        QVERIFY(QFile::exists(out + QStringLiteral("/ampr_zstd-000.pak")));
        QVERIFY(!QFileInfo::exists(out + QStringLiteral("/data")));
        QVERIFY(!QFile::exists(out + QStringLiteral("/settings.ini")));
        QVERIFY(!QFile::exists(out + QStringLiteral("/ampr_commands.bin")));
        QVERIFY(QFileInfo(out + QStringLiteral("/saves/empty")).isDir()); // a real empty folder stays
        QCOMPARE(bytesOf(out + QStringLiteral("/fakelib/libSceNpAuth.sprx")), QByteArray("backport lib"));
        QCOMPARE(bytesOf(out + QStringLiteral("/fakelib/libSceAmpr.sprx")), *loadBundledRuntime());
        QCOMPARE(bytesOf(out + QStringLiteral("/Media/Modules/game.prx")), bytesOf(game + QStringLiteral("/Media/Modules/game.prx")));

        // ampr_emu.index lists the packed files (as the game sees them) and the new runtime.
        auto scan = pfs::scanAmprFolder(game, {});
        const QByteArray amprIdx = bytesOf(out + QStringLiteral("/ampr_emu.index"));
        QVERIFY(amprIdx.startsWith("AMPRIDX3"));
        QVERIFY(amprIdx.contains("/app0/data/level1.bin"));
        QVERIFY(amprIdx.contains("/app0/fakelib/libSceAmpr.sprx"));
        QVERIFY(!amprIdx.contains("ampr_zstd-000.pak"));

        // Seen through its packs, the packed game is the original (minus the
        // journal, with the new runtime and index).
        auto packed = openFolderGame(out, {});
        QVERIFY(packed.ok());
        auto logical = logicalGame(*packed, {});
        QVERIFY(logical.ok());
        const QStringList ignore = {QStringLiteral("ampr_commands.bin"), QStringLiteral("fakelib/libSceAmpr.sprx"), QStringLiteral("ampr_emu.index")};
        const auto original = snapshot(game, ignore);
        for (auto it = original.cbegin(); it != original.cend(); ++it) {
            const int i = (*logical)->find(it.key());
            QVERIFY2(i >= 0, qPrintable(it.key()));
            QByteArray back(it.value().size(), '\0');
            auto n = (*logical)->read(i, 0, back.data(), back.size());
            QVERIFY(n.ok());
            QCOMPARE(back, it.value());
        }

        // Unpack: plain files again, no pack files, a fresh ampr_emu.index.
        const QString un = m_tmp.filePath(QStringLiteral("unpacked/PPSA01234"));
        auto u = unpackGame(*packed, un, {}, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(u.ok(), u.ok() ? "" : qPrintable(u.error().toLogString()));
        QCOMPARE(u->kind, PackKind::Zstd);
        QCOMPARE(u->unpackedFiles, qint64(3));
        QCOMPARE(snapshot(un, ignore), original);
        QVERIFY(!QFile::exists(un + QStringLiteral("/ampr_zstd.index")));
        QVERIFY(QFile::exists(un + QStringLiteral("/ampr_emu.index")));

        // Packing a packed game again works from its packs.
        auto again = packGame(*packed, m_tmp.filePath(QStringLiteral("repacked/PPSA01234")), o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(again.ok(), again.ok() ? "" : qPrintable(again.error().toLogString()));
        QCOMPARE(again->sourcePacks, PackKind::Zstd);
        QCOMPARE(again->packedFiles, qint64(3));
    }

    void readOnlyOriginalsStayReadOnly()
    {
        const QString game = m_tmp.filePath(QStringLiteral("rogame/PPSA05555"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4)));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), "{}"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/a.bin"), compressible(300000, 1)));
        const QString ro = game + QStringLiteral("/sce_sys/param.json");
        QVERIFY(QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::ReadUser));
        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        PackGameOptions o;
        o.linkFromFolder = game;
        o.freeSpaceMarginBytes = 0;
        const QString out = m_tmp.filePath(QStringLiteral("rogame-out/PPSA05555"));
        auto rep = packGame(*files, out, o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QVERIFY(removeTreeSafely(m_tmp.filePath(QStringLiteral("rogame-out"))).ok());
        QVERIFY2(!QFileInfo(ro).isWritable(), "removing the packed folder must not change the original");
        QVERIFY(QFile::setPermissions(ro, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    void compatibleStrategyKeepsConfigsLoose()
    {
        const QString game = m_tmp.filePath(QStringLiteral("games/PPSA01234"));
        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        PackGameOptions o;
        o.strategy = PackStrategy::Compatible;
        o.freeSpaceMarginBytes = 0;
        const QString out = m_tmp.filePath(QStringLiteral("compat/PPSA01234"));
        auto rep = packGame(*files, out, o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QVERIFY(QFile::exists(out + QStringLiteral("/settings.ini")));
        QCOMPARE(rep->packedFiles, qint64(2));
    }

private:
    struct Lz4Game {
        QByteArray data, vol, c1;
        quint64 off1 = 0;
    };
    // A hand-built AMPRPAK4 game in `game`: one packed file listed as `p0`
    // (two LZ4 chunks of 64 KiB blocks, the second raw), one loose record, a
    // CRC sidecar.
    void buildLz4Game(const QString& game, const QByteArray& p0, Lz4Game& built)
    {
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray("\x54\x14\xF5\xEE", 4)));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), "{}"));
        const QByteArray data = compressible(65536, 4) + randomBytes(30000, 5);
        const QByteArray c0 = data.left(65536);
        QByteArray z0(LZ4_compressBound(65536), '\0');
        const int z0n = LZ4_compress_default(c0.constData(), z0.data(), 65536, z0.size());
        QVERIFY(z0n > 0 && z0n < 65536);
        z0.truncate(z0n);
        const QByteArray c1 = data.mid(65536);
        const quint32 page = 4096;
        const quint64 payloadOff = page;
        // chunk 0 at payloadOff (contained in pages), chunk 1 next, 64-byte aligned.
        const quint64 off0 = payloadOff;
        const quint64 off1 = ((off0 + quint64(z0n) + 63) / 64) * 64;
        quint64 fileSize = off1 + quint64(c1.size());
        fileSize = ((fileSize + page - 1) / page) * page;
        const quint64 payload = fileSize - payloadOff;
        QByteArray buildId(16, 'B');

        QByteArray vol(qsizetype(fileSize), '\0');
        std::memcpy(vol.data(), "AMPRDAT3", 8);
        put32(vol, 8, 3);
        put32(vol, 12, 64);
        put32(vol, 16, 0);
        put32(vol, 20, 2);
        std::memcpy(vol.data() + 24, buildId.constData(), 16);
        put64(vol, 40, payloadOff);
        put64(vol, 48, payload);
        put32(vol, 56, crc32(vol.constData(), 64));
        std::memcpy(vol.data() + off0, z0.constData(), size_t(z0n));
        std::memcpy(vol.data() + off1, c1.constData(), size_t(c1.size()));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_assets-000.pak"), vol));

        const QByteArray p1 = "/app0/eboot.bin";
        const QByteArray v0 = "ampr_assets-000.pak";
        QByteArray strings = p0 + '\0' + p1 + '\0' + v0 + '\0';
        const quint64 fc = 2, cc = 2, pc = 1;
        const quint64 filesOff = 128, chunksOff = filesOff + 48 * fc, packsOff = chunksOff + 12 * cc, stringsOff = packsOff + 32 * pc;
        QByteArray m(qsizetype(stringsOff + quint64(strings.size())), '\0');
        std::memcpy(m.data(), "AMPRPAK4", 8);
        put32(m, 8, 4);
        put32(m, 12, 128);
        put32(m, 20, 0x01020304);
        std::memcpy(m.data() + 24, buildId.constData(), 16);
        put64(m, 40, fc);
        put64(m, 48, cc);
        put32(m, 56, quint32(pc));
        put32(m, 60, 48);
        put32(m, 64, 12);
        put32(m, 68, 32);
        put64(m, 72, filesOff);
        put64(m, 80, chunksOff);
        put64(m, 88, packsOff);
        put64(m, 96, stringsOff);
        put64(m, 104, quint64(strings.size()));
        // file 0: packed
        int f = int(filesOff);
        put64(m, f + 8, quint64(data.size()));
        put32(m, f + 24, 0);
        put32(m, f + 28, 2);
        put32(m, f + 32, 0);
        put32(m, f + 36, quint32(p0.size()));
        put32(m, f + 40, 1);
        m[f + 44] = 16;
        // file 1: loose record
        f += 48;
        put32(m, f + 32, quint32(p0.size() + 1));
        put32(m, f + 36, quint32(p1.size()));
        // chunks
        put64(m, int(chunksOff), off0);
        put32(m, int(chunksOff) + 8, quint32(z0n - 1) | (1u << 20) | (4u << 22));
        put64(m, int(chunksOff) + 12, off1);
        put32(m, int(chunksOff) + 20, quint32(c1.size() - 1));
        // pack
        put64(m, int(packsOff), payload);
        put64(m, int(packsOff) + 8, fileSize);
        put32(m, int(packsOff) + 16, quint32(p0.size() + 1 + p1.size() + 1));
        put32(m, int(packsOff) + 20, quint32(v0.size()));
        put32(m, int(packsOff) + 24, 2);
        put32(m, int(packsOff) + 28, page);
        std::memcpy(m.data() + stringsOff, strings.constData(), size_t(strings.size()));
        put32(m, 112, crc32(m.constData() + 128, m.size() - 128));
        {
            QByteArray head = m.left(128);
            std::memset(head.data() + 116, 0, 4);
            put32(m, 116, crc32(head.constData(), 128));
        }
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_assets.index"), m));
        // CRC sidecar of the decoded chunks.
        QByteArray crc(qsizetype(48 + 4 * cc), '\0');
        std::memcpy(crc.data(), "AMPRCRC1", 8);
        put32(crc, 8, 1);
        put32(crc, 12, 48);
        std::memcpy(crc.data() + 16, buildId.constData(), 16);
        put64(crc, 32, cc);
        put32(crc, 48, crc32(c0.constData(), c0.size()));
        put32(crc, 52, crc32(c1.constData(), c1.size()));
        put32(crc, 40, crc32(crc.constData() + 48, crc.size() - 48));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_assets.index.crc"), crc));
        built = {data, vol, c1, off1};
    }

private slots:
    void lz4PacksUnpack()
    {
        const QString game = m_tmp.filePath(QStringLiteral("lz4game"));
        Lz4Game built;
        buildLz4Game(game, "/app0/data/big.bin", built);
        if (QTest::currentTestFailed())
            return;
        const QByteArray& data = built.data;
        const QByteArray& vol = built.vol;
        const QByteArray& c1 = built.c1;
        const quint64 off1 = built.off1;

        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        QCOMPARE(detectPackKind(**files), PackKind::Lz4);
        auto logical = logicalGame(*files, {});
        QVERIFY2(logical.ok(), logical.ok() ? "" : qPrintable(logical.error().toLogString()));
        const int i = (*logical)->find(QStringLiteral("data/big.bin"));
        QVERIFY(i >= 0);
        QByteArray back(data.size(), '\0');
        auto n = (*logical)->read(i, 0, back.data(), back.size());
        QVERIFY2(n.ok(), n.ok() ? "" : qPrintable(n.error().toLogString()));
        QCOMPARE(back, data);
        QCOMPARE((*logical)->find(QStringLiteral("ampr_assets.index")), -1);
        QCOMPARE((*logical)->find(QStringLiteral("ampr_assets-000.pak")), -1);
        QCOMPARE((*logical)->find(QStringLiteral("ampr_assets.index.crc")), -1);

        // A damaged chunk fails its CRC.
        QByteArray volBad = vol;
        volBad[qsizetype(off1 + 10)] = char(volBad[qsizetype(off1 + 10)] ^ 0xFF);
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_assets-000.pak"), volBad));
        auto files2 = openFolderGame(game, {});
        auto logical2 = logicalGame(*files2, {});
        QVERIFY(logical2.ok());
        QByteArray tail(c1.size(), '\0');
        QVERIFY(!(*logical2)->read((*logical2)->find(QStringLiteral("data/big.bin")), 65536, tail.data(), tail.size()).ok());
    }

    // A name cannot be a file and a folder at once, whatever sorts between
    // the two ("a-1" comes after "a" and before "a/b").
    void fileAndFolderOfOneNameAreRefused()
    {
        const QString dir = m_tmp.filePath(QStringLiteral("both"));
        QVERIFY(QDir().mkpath(dir));
        auto w = writeZstdPack(inputsOf({{"a", "file"}, {"a-1", "other"}, {"a/b", "inside"}}), {}, dir, {});
        QVERIFY(!w.ok());
        QVERIFY2(w.error().message.contains(QLatin1String("both a file and a folder")), qPrintable(w.error().toLogString()));
        QVERIFY(QDir(dir).isEmpty());

        // A loose file next to packed files below the same name.
        const QString game = m_tmp.filePath(QStringLiteral("both-game"));
        QVERIFY(QDir().mkpath(game));
        QVERIFY(writeZstdPack(inputsOf({{"a/b", QByteArray(1000, 'x')}}), {}, game, {}).ok());
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/a"), "loose"));
        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        auto logical = logicalGame(*files, {});
        QVERIFY(!logical.ok());
        QCOMPARE(logical.error().code, ErrorCode::CorruptData);
    }

    // Stored paths are untrusted: nothing a pack index or an image lists may
    // be written outside the output folder.
    void hostPathsStayInsideTheirRoot()
    {
        const QString root = m_tmp.filePath(QStringLiteral("hostpaths/out"));
        for (const char* bad : {"../x", "a/../../x", "..", "a/./b", "a//b", "/abs/x", "a/", "", "C:/Windows/x", "c:x/y", "a/D:/x",
                                "a\\..\\x"}) {
            QVERIFY2(!storedPathProblem(QString::fromLatin1(bad)).isEmpty(), bad);
            HostPaths paths(root);
            auto r = paths.add(QString::fromLatin1(bad));
            QVERIFY2(!r.ok(), bad);
            QCOMPARE(r.error().code, ErrorCode::CorruptData);
            QVERIFY2(r.error().message.contains(QLatin1String("unsafe path")), qPrintable(r.error().message));
        }
        HostPaths paths(root);
        // Names this drive cannot store are mapped, never refused, and stay inside.
        for (const char* odd : {"data/ab:c.bin", "data/CON", "data/trailing.", "data/q?.bin", "...", "data/ x"}) {
            auto r = paths.add(QString::fromLatin1(odd));
            QVERIFY2(r.ok(), odd);
            QVERIFY2(isSameOrInside(*r, root) && *r != QDir::cleanPath(root), qPrintable(*r));
            QCOMPARE(*r, QDir::cleanPath(root) + u'/' + paths.relative(QString::fromLatin1(odd)));
        }
        // The same stored path always goes to the same place.
        QCOMPARE(*paths.add(QStringLiteral("data/ab:c.bin")), *paths.add(QStringLiteral("data/ab:c.bin")));
        QCOMPARE(*paths.add(QStringLiteral("Media/x.bin")), QDir::cleanPath(root) + QStringLiteral("/Media/x.bin"));
        if (hostUsesWindowsNames()) {
            QVERIFY(!paths.renamed().isEmpty());
            // Two files that are one file on this drive: the second is renamed, not overwritten.
            const QString first = *paths.add(QStringLiteral("Media/Clip.bik"));
            const QString second = *paths.add(QStringLiteral("Media/clip.BIK"));
            QVERIFY(first.compare(second, Qt::CaseInsensitive) != 0);
            QVERIFY(second.endsWith(QLatin1String(" (2)")));
            // Folders that differ only in case stay one folder.
            QCOMPARE(*paths.add(QStringLiteral("media/y.bin")), QDir::cleanPath(root) + QStringLiteral("/Media/y.bin"));
            QVERIFY(paths.renamed().join(u'\n').contains(QLatin1String("Media/clip.BIK")));
            QVERIFY(!paths.renamed().join(u'\n').contains(QLatin1String("media/y.bin")));
        }
        QVERIFY(!QFileInfo::exists(m_tmp.filePath(QStringLiteral("hostpaths")))); // deciding a path creates nothing
    }

    // An LZ4 manifest that lists a path with a drive letter: unpacking and
    // packing again both refuse it before anything is written.
    void hostileLz4ManifestCannotLeaveTheOutput()
    {
        const QString escaped = m_tmp.filePath(QStringLiteral("lz4-escaped"));
        if (escaped.startsWith(u'/'))
            QSKIP("An absolute path in a manifest needs a drive letter (it starts with '/' here and the manifest refuses it).");
        const QString game = m_tmp.filePath(QStringLiteral("lz4hostile"));
        Lz4Game built;
        buildLz4Game(game, "/app0/" + (escaped + QStringLiteral("/evil.sprx")).toUtf8(), built);
        if (QTest::currentTestFailed())
            return;
        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        auto logical = logicalGame(*files, {});
        QVERIFY2(logical.ok(), logical.ok() ? "" : qPrintable(logical.error().toLogString()));

        const QString out = m_tmp.filePath(QStringLiteral("lz4hostile-out/unpacked"));
        auto u = unpackGame(*files, out, {}, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY(!u.ok());
        QVERIFY2(u.error().message.contains(QLatin1String("unsafe path")), qPrintable(u.error().toLogString()));
        PackGameOptions o;
        o.freeSpaceMarginBytes = 0;
        auto p = packGame(*files, m_tmp.filePath(QStringLiteral("lz4hostile-out/packed")), o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY(!p.ok());
        QVERIFY(!QFileInfo::exists(escaped));
        QVERIFY(!QFileInfo::exists(m_tmp.filePath(QStringLiteral("lz4hostile-out"))));
    }

    // A kept output never shares file data with the source; only a work
    // folder (linkFromFolder) may.
    void looseFilesAreCopiedUnlessLinkingIsAskedFor()
    {
        const QString game = m_tmp.filePath(QStringLiteral("games/PPSA01234"));
        auto files = openFolderGame(game, {});
        QVERIFY(files.ok());
        PackGameOptions o;
        o.freeSpaceMarginBytes = 0;
        const QString out = m_tmp.filePath(QStringLiteral("copied/PPSA01234"));
        auto rep = packGame(*files, out, o, {}, nullptr, Log(QStringLiteral("test")));
        QVERIFY2(rep.ok(), rep.ok() ? "" : qPrintable(rep.error().toLogString()));
        QCOMPARE(rep->linkedFiles, qint64(0));
        const QByteArray original = bytesOf(game + QStringLiteral("/eboot.bin"));
        {
            QFile f(out + QStringLiteral("/eboot.bin"));
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.write("XXXX") == 4);
        }
        QCOMPARE(bytesOf(game + QStringLiteral("/eboot.bin")), original);
    }

    // The staged folder: nothing under the final name until commit(), and a
    // final folder that is not ours to replace is refused both times.
    void stagedFolderOnlyReplacesAnEmptyOrMissingFolder()
    {
        const QString base = m_tmp.filePath(QStringLiteral("staged"));
        const QString out = base + QStringLiteral("/out");
        QVERIFY(fwtest::writeTextFile(out, "a file"));
        QCOMPARE(checkNewFolderOutput(out).error().code, ErrorCode::AlreadyExists);
        QVERIFY(!StagedFolder::begin(out).ok());
        QCOMPARE(bytesOf(out), QByteArray("a file"));
        QVERIFY(QFile::remove(out));
        {
            auto s = StagedFolder::begin(out);
            QVERIFY(s.ok());
            QVERIFY(QFileInfo((*s)->path()).isDir());
            QVERIFY(fwtest::writeTextFile((*s)->path() + QStringLiteral("/a.bin"), "a"));
            QVERIFY(!QFileInfo::exists(out));
        } // abandoned
        QVERIFY(QDir(base).isEmpty());
        {
            auto s = StagedFolder::begin(out);
            QVERIFY(s.ok());
            QVERIFY(fwtest::writeTextFile((*s)->path() + QStringLiteral("/a.bin"), "a"));
            QVERIFY(fwtest::writeTextFile(out + QStringLiteral("/theirs.txt"), "theirs")); // appears meanwhile
            QCOMPARE((*s)->commit().error().code, ErrorCode::AlreadyExists);
        }
        QCOMPARE(QDir(base).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("out")});
        QCOMPARE(bytesOf(out + QStringLiteral("/theirs.txt")), QByteArray("theirs"));
        QVERIFY(QFile::remove(out + QStringLiteral("/theirs.txt")));
        {
            auto s = StagedFolder::begin(out); // an empty folder may be replaced
            QVERIFY(s.ok());
            QVERIFY(fwtest::writeTextFile((*s)->path() + QStringLiteral("/a.bin"), "a"));
            QVERIFY((*s)->commit().ok());
        }
        QCOMPARE(bytesOf(out + QStringLiteral("/a.bin")), QByteArray("a"));
        QCOMPARE(QDir(base).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("out")});
    }

private:
    QTemporaryDir m_tmp;
};

QTEST_GUILESS_MAIN(TestAmpr)
#include "tst_ampr.moc"
