// The readers the Backport tab's game list uses on every eboot.bin: the
// fake-signed SELF unwrapper and the SDK version in the process param.
// Every file is synthetic and built in memory.

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfReader.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QTest>
#include <QtEndian>

using namespace fw;
using namespace fw::backport;

namespace {

template <typename T>
void put(QByteArray& b, qsizetype at, T v)
{
    qToLittleEndian<T>(v, b.data() + at);
}

struct Segment {
    quint32 type;
    quint64 offset; // in the ELF
    quint64 filesz; // in the program header
};

constexpr quint64 kSegmentAt = 0x4000;
constexpr quint32 kPs5Sdk = 0x04500001; // SDK 4.50

QByteArray elfHeaderAndTable(const std::vector<Segment>& segments)
{
    QByteArray h(elf::kEhdrSize + qsizetype(segments.size()) * elf::kPhdrSize, '\0');
    memcpy(h.data(), "\x7F" "ELF", 4);
    h[4] = 2; // 64-bit
    h[5] = 1; // little endian
    h[6] = 1;
    put<quint16>(h, 0x10, elf::ET_SCE_EXEC);
    put<quint16>(h, 0x12, elf::EM_X86_64);
    put<quint32>(h, 0x14, 1);
    put<quint64>(h, 0x20, elf::kEhdrSize); // e_phoff
    put<quint16>(h, 0x34, elf::kEhdrSize);
    put<quint16>(h, 0x36, elf::kPhdrSize);
    put<quint16>(h, 0x38, quint16(segments.size()));
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const qsizetype p = elf::kEhdrSize + qsizetype(i) * elf::kPhdrSize;
        put<quint32>(h, p + 0x00, segments[i].type);
        put<quint64>(h, p + 0x08, segments[i].offset);
        put<quint64>(h, p + 0x20, segments[i].filesz);
        put<quint64>(h, p + 0x28, segments[i].filesz);
        put<quint64>(h, p + 0x30, 0x4000);
    }
    return h;
}

// The bytes of the one data segment: a process param at its start.
QByteArray segmentData()
{
    QByteArray d(0x200, '\0');
    put<quint64>(d, 0x00, 0x40);
    put<quint32>(d, 0x08, kProcParamMagic);
    put<quint32>(d, 0x10, 0x09008001);
    put<quint32>(d, 0x14, kPs5Sdk);
    return d;
}

// A fake-signed SELF holding `stored` as the data of program header 0, with
// whatever the program headers claim, plus `tail` after the SELF's own size.
QByteArray makeSelf(const std::vector<Segment>& segments, const QByteArray& stored, const QByteArray& tail = {})
{
    const QByteArray elfPart = elfHeaderAndTable(segments);
    const qsizetype elfAt = 0x40; // common header + one entry
    const qsizetype dataAt = qsizetype(self::alignUp(quint64(elfAt + elfPart.size() + self::kExInfoSize), 0x100));
    QByteArray s(dataAt, '\0');
    put<quint32>(s, 0x00, self::kMagicFake);
    s[4] = 0x00; // version, mode, endian, attributes as make_fself writes them
    s[5] = 0x01;
    s[6] = 0x01;
    s[7] = 0x12;
    put<quint32>(s, 0x08, 0x101);                           // key type
    put<quint64>(s, 0x10, quint64(dataAt + stored.size())); // file size
    put<quint16>(s, 0x18, 1);                               // one entry
    put<quint16>(s, 0x1A, 0x22);                            // flags
    put<quint64>(s, 0x20, self::props::kHasBlocks);         // a data entry for segment 0
    put<quint64>(s, 0x28, quint64(dataAt));
    put<quint64>(s, 0x30, quint64(stored.size()));
    put<quint64>(s, 0x38, quint64(stored.size()));
    memcpy(s.data() + elfAt, elfPart.constData(), size_t(elfPart.size()));
    return s + stored + tail;
}

} // namespace

class TstBackportSelf : public QObject {
    Q_OBJECT

private slots:
    void fakeSelfUnwrapsToItsElfAndSdk()
    {
        const QByteArray stored = segmentData();
        const QByteArray selfFile = makeSelf({{elf::PT_LOAD, kSegmentAt, quint64(stored.size())},
                                              {elf::PT_SCE_PROCPARAM, kSegmentAt, 0x40}},
                                             stored);
        QVERIFY(selfio::hasSelfMagic(selfFile));
        auto info = selfio::inspect(selfFile);
        QVERIFY(info.ok());
        QCOMPARE(info->kind, SelfKind::FakeSigned);
        auto elfFile = selfio::unwrap(selfFile);
        QVERIFY2(elfFile.ok(), elfFile.ok() ? "" : qPrintable(elfFile.error().toLogString()));
        QCOMPARE(elfFile->size(), qsizetype(kSegmentAt + quint64(stored.size())));
        QCOMPARE(elfFile->mid(qsizetype(kSegmentAt)), stored);
        QVERIFY(elf::hasElfMagic(*elfFile));
        quint32 ps5 = 0;
        const auto major = elfSdkMajor(*elfFile, &ps5);
        QVERIFY(major.has_value());
        QCOMPARE(*major, 4);
        QCOMPARE(ps5, kPs5Sdk);
        QCOMPARE(sdkVersionText(ps5), QStringLiteral("4.50"));
    }

    // A few hundred bytes may not make the unwrapper allocate a gigabyte.
    void programHeadersCannotInflateTheOutput()
    {
        QElapsedTimer timer;
        timer.start();
        // An empty data entry whose program header claims 1 GiB of file data.
        const QByteArray empty = makeSelf({{elf::PT_LOAD, 0, 0x40000000}}, QByteArray());
        QVERIFY(empty.size() < 1024);
        auto info = selfio::inspect(empty);
        QVERIFY(info.ok());
        auto a = selfio::unwrap(empty);
        QVERIFY(!a.ok());
        QCOMPARE(codeOf(a.error()), Code::SelfMalformed);

        // Real data, but placed just under the 1 GiB limit.
        const QByteArray stored = segmentData();
        auto b = selfio::unwrap(makeSelf({{elf::PT_LOAD, 0x3FFF0000, quint64(stored.size())}}, stored));
        QVERIFY(!b.ok());
        QCOMPARE(codeOf(b.error()), Code::SelfMalformed);

        // The version segment (taken from the bytes after the SELF) far away.
        const QByteArray tail(16, 'v');
        auto c = selfio::unwrap(makeSelf({{elf::PT_LOAD, kSegmentAt, quint64(stored.size())},
                                          {elf::PT_SCE_VERSION, 0x3FFF0000, quint64(tail.size())}},
                                         stored, tail));
        QVERIFY(!c.ok());
        QCOMPARE(codeOf(c.error()), Code::SelfMalformed);
        QVERIFY2(timer.elapsed() < 5000, "refusing must not involve gigabyte-sized buffers");

        // Gaps of an ordinary size between segments still unpack.
        auto d = selfio::unwrap(makeSelf({{elf::PT_LOAD, 0x200000, quint64(stored.size())}}, stored));
        QVERIFY2(d.ok(), d.ok() ? "" : qPrintable(d.error().toLogString()));
        QCOMPARE(d->size(), qsizetype(0x200000 + stored.size()));
        // The version segment in its usual place is kept.
        auto e = selfio::unwrap(makeSelf({{elf::PT_LOAD, kSegmentAt, quint64(stored.size())},
                                          {elf::PT_SCE_VERSION, kSegmentAt + 0x1000, quint64(tail.size())}},
                                         stored, tail));
        QVERIFY2(e.ok(), e.ok() ? "" : qPrintable(e.error().toLogString()));
        QCOMPARE(e->mid(qsizetype(kSegmentAt + 0x1000), tail.size()), tail);
    }

    void brokenInputIsRefusedNotCrashedOn()
    {
        QVERIFY(!selfio::unwrap(QByteArray()).ok());
        QVERIFY(!selfio::unwrap(QByteArray(64, 'x')).ok());
        const QByteArray stored = segmentData();
        const QByteArray good = makeSelf({{elf::PT_LOAD, kSegmentAt, quint64(stored.size())}}, stored);
        // Every truncation of a good file is either refused or unpacks; none may crash.
        for (qsizetype n = 0; n < good.size(); n += 7)
            (void)selfio::unwrap(good.left(n));
        // An encrypted segment is reported as such.
        QByteArray encrypted = good;
        put<quint64>(encrypted, 0x20, self::props::kHasBlocks | self::props::kEncrypted);
        auto r = selfio::unwrap(encrypted);
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::SelfEncrypted);
        // No param in a plain ELF: no SDK, no failure.
        QVERIFY(!elfSdkMajor(elfHeaderAndTable({{elf::PT_LOAD, 0, 0}})).has_value());
        QVERIFY(!elfSdkMajor(QByteArray(10, '\0')).has_value());
    }
};

QTEST_GUILESS_MAIN(TstBackportSelf)
#include "tst_backport_self.moc"
