// Backport engine formats: SDK table, ELF parsing, SDK param patch, SELF
// writer (golden SHA-256 from docs/BACKPORT_PARITY.md Appendix A), SELF
// reader / unwrap, libc NID substitution.

#include "BackportFixtures.h"

#include "engines/backport/BackportEngine.h"
#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"
#include "engines/backport/NidPatch.h"
#include "engines/backport/SceParam.h"
#include "engines/backport/SdkTable.h"
#include "engines/backport/SelfReader.h"
#include "engines/backport/SelfWriter.h"

#include <QRandomGenerator>
#include <QTest>

#include <algorithm>
#include <cstring>

using namespace fw::backport;
using namespace fwtest::bp;

namespace {

QByteArray patched(QByteArray elf, int n)
{
    auto r = patchSdk(elf, *sdkPair(n));
    return r.ok() ? elf : QByteArray();
}

} // namespace

class TstBackportFormat : public QObject {
    Q_OBJECT

private slots:
    // ---- SdkTable ------------------------------------------------------------
    void sdkTableValues()
    {
        const quint32 ps5[] = {0x01000050, 0x02000009, 0x03000027, 0x04000031, 0x05000033,
                               0x06000038, 0x07000038, 0x08000041, 0x09000040, 0x10000040};
        const quint32 ps4[] = {0x07590001, 0x08050001, 0x08540001, 0x09040001, 0x09590001,
                               0x10090001, 0x10590001, 0x11090001, 0x11590001, 0x12090001};
        for (int n = 1; n <= 10; ++n) {
            const SdkPair* p = sdkPair(n);
            QVERIFY(p);
            QCOMPARE(p->ps5, ps5[n - 1]);
            QCOMPARE(p->ps4, ps4[n - 1]);
            QCOMPARE(bcdMajor(p->ps5).value_or(-1), n); // BCD: 0x10 is 10, not 16
        }
        QVERIFY(!sdkPair(0));
        QVERIFY(!sdkPair(11)); // S3 / blocked pairs 11-13
        QVERIFY(!sdkPair(13));
        QCOMPARE(bcdMajor(0x10000040).value_or(-1), 10);
        QVERIFY(!bcdMajor(0x1A000000).has_value());
        QCOMPARE(sdkVersionText(0x09590001), QStringLiteral("9.59"));
        QCOMPARE(firmwareLabel(4), QStringLiteral("SDK 4 · loads on FW 4.00+"));
        QCOMPARE(targetKey(10), QStringLiteral("10xx"));
        QCOMPARE(tableHash(), tableHash());
        QCOMPARE(tableHash().size(), 64);
    }

    // ---- ElfImage ------------------------------------------------------------
    void elfParsesFixtures()
    {
        auto a = ElfImage::parse(f1());
        QVERIFY(a.ok());
        QCOMPARE(int(a->programHeaders().size()), 5);
        QCOMPARE(a->programHeaders()[2].type, elf::PT_SCE_PROCPARAM);
        auto b = ElfImage::parse(f2());
        QVERIFY(b.ok());
        QCOMPARE(b->header().type, quint16(0xFE18));
    }

    void elfHeaderRulesRefuseSigning_data()
    {
        QTest::addColumn<int>("offset");
        QTest::addColumn<int>("width");
        QTest::addColumn<quint64>("value");
        QTest::newRow("e_machine 0x28") << 0x12 << 2 << quint64(0x28);
        QTest::newRow("EI_CLASS 1") << 0x04 << 1 << quint64(1);
        QTest::newRow("EI_DATA 2") << 0x05 << 1 << quint64(2);
        QTest::newRow("e_phentsize 0x40") << 0x36 << 2 << quint64(0x40);
        QTest::newRow("e_type 3") << 0x10 << 2 << quint64(3);
    }
    void elfHeaderRulesRefuseSigning() // F12
    {
        QFETCH(int, offset);
        QFETCH(int, width);
        QFETCH(quint64, value);
        QByteArray b = f1();
        if (width == 1)
            b[offset] = char(value);
        else
            put16(b, offset, quint16(value));
        auto r = makeFakeSelf(b);
        QVERIFY(!r.ok());
        const Code c = codeOf(r.error());
        QVERIFY2(c == Code::SignUnsupported || c == Code::ElfInvalid, qPrintable(codeName(c)));
    }

    void truncatedElfRefused() // F11 (sign)
    {
        auto r = makeFakeSelf(f1().left(0x5000));
        QVERIFY(!r.ok());
        QCOMPARE(codeOf(r.error()), Code::ElfInvalid);
        QCOMPARE(r.error().code, fw::ErrorCode::CorruptData);
        QVERIFY(!ElfImage::parse(f1().left(0x30)).ok());
        QByteArray noPh = f1();
        put16(noPh, 0x38, 0);
        QVERIFY(!ElfImage::parse(noPh).ok());
    }

    // ---- SceParam --------------------------------------------------------------
    void procParamPair4And10()
    {
        QByteArray e = f1();
        auto r = patchSdk(e, *sdkPair(4));
        QVERIFY(r.ok());
        QCOMPARE(int(r->patched.size()), 1);
        QCOMPARE(r->sdkBefore().value_or(-1), 7);
        QCOMPARE(e.mid(0x5800, 0x18).toHex(), QByteArray("60000000000000004f524249010000000100040931000004"));
        QCOMPARE(r->messages.front(),
                 QStringLiteral("PS5 SDK version 0x07000038 -> 0x04000031, PS4 version 0x09590001 -> 0x09040001"));
        QByteArray e10 = f1();
        QVERIFY(patchSdk(e10, *sdkPair(10)).ok());
        QVERIFY(e10.mid(0x5800, 0x18).toHex().endsWith("0100091240000010"));
    }

    void moduleWithoutParamIsSkipped() // F3
    {
        QByteArray e = f3();
        const QByteArray before = e;
        auto r = patchSdk(e, *sdkPair(4));
        QVERIFY(r.ok());
        QVERIFY(r->skippedNoParam);
        QVERIFY(r->patched.empty());
        QCOMPARE(e, before);
        QVERIFY(r->messages.contains(QStringLiteral("Module file has no param, skipping")));
    }

    void paramErrors() // F4, F5, F6
    {
        QByteArray a = f4();
        const QByteArray a0 = a;
        auto r4 = patchSdk(a, *sdkPair(4));
        QVERIFY(!r4.ok());
        QCOMPARE(codeOf(r4.error()), Code::ParamMagicInvalid);
        QVERIFY(r4.error().message.contains(QStringLiteral("0xDEADBEEF")));
        QCOMPARE(a, a0); // nothing written on failure

        QByteArray b = f5();
        auto r5 = patchSdk(b, *sdkPair(4));
        QVERIFY(!r5.ok());
        QCOMPARE(codeOf(r5.error()), Code::ParamSizeInvalid);
        QVERIFY(r5.error().message.contains(QStringLiteral("0x10")));

        QByteArray c = f6();
        auto r6 = patchSdk(c, *sdkPair(4));
        QVERIFY(!r6.ok());
        QCOMPARE(codeOf(r6.error()), Code::ParamMissing);
        QCOMPARE(r6.error().message, QStringLiteral("No process or module param segment found"));

        // A param whose struct runs past the end of the file.
        QByteArray d = f1().left(0x5810);
        auto rd = patchSdk(d, *sdkPair(4));
        QVERIFY(!rd.ok());
        QCOMPARE(codeOf(rd.error()), Code::ParamOutOfRange);

        // A signed file is not patched directly.
        QByteArray s = selfOf(f1());
        auto rs = patchSdk(s, *sdkPair(4));
        QVERIFY(!rs.ok());
        QVERIFY(rs.error().message.contains(QStringLiteral("signed")));
    }

    void everyParamSegmentIsPatched() // F13
    {
        QByteArray e = f13();
        auto r = patchSdk(e, *sdkPair(6));
        QVERIFY(r.ok());
        QCOMPARE(int(r->patched.size()), 2);
        QCOMPARE(get32(e, 0x5814), sdkPair(6)->ps5);
        QCOMPARE(get32(e, 0x1114), sdkPair(6)->ps5);
        QCOMPARE(get32(e, 0x5810), sdkPair(6)->ps4);
        QCOMPARE(get32(e, 0x1110), sdkPair(6)->ps4);
    }

    void eachPairIsWritten_data()
    {
        QTest::addColumn<int>("n");
        for (int n = 1; n <= 10; ++n)
            QTest::newRow(qPrintable(QStringLiteral("pair %1").arg(n))) << n;
    }
    void eachPairIsWritten()
    {
        QFETCH(int, n);
        QByteArray e = f2();
        auto r = patchSdk(e, *sdkPair(n));
        QVERIFY(r.ok());
        QCOMPARE(get32(e, 0x2014), sdkPair(n)->ps5);
        QCOMPARE(get32(e, 0x2010), sdkPair(n)->ps4);
        QCOMPARE(elfSdkMajor(e).value_or(-1), n);
        // And it survives signing + unwrap.
        auto s = makeFakeSelf(e);
        QVERIFY(s.ok());
        auto u = selfio::unwrap(s.value());
        QVERIFY(u.ok());
        QCOMPARE(elfSdkMajor(u.value()).value_or(-1), n);
    }

    // ---- SelfWriter (golden, Appendix A.1) ---------------------------------------
    void goldenHashes_data()
    {
        QTest::addColumn<int>("fixture");
        QTest::addColumn<int>("pair");
        QTest::addColumn<QByteArray>("elfSha");
        QTest::addColumn<QByteArray>("selfSha");
        QTest::addColumn<int>("selfSize");
        QTest::newRow("F1 pair 4") << 1 << 4
                                   << QByteArray("065462fac5402013fa5c15cb9777f5e6ff1eb7e1dc24c77d6e38aca2bda395f7")
                                   << QByteArray("10b80a5af227262378e246bcc723e2d92b34f1b48d6c839aaef5e43d109151f8")
                                   << 0x5080;
        QTest::newRow("F1 pair 10") << 1 << 10
                                    << QByteArray("0e839ea9a64af2d1bf5db04b760219c97acb498b50d3f0b9af06521e4909e04c")
                                    << QByteArray("00ddda8c3e02a4357a50a50430feb3bd82f77e6a2ffd2acb97bef3123bd876fb")
                                    << 0x5080;
        QTest::newRow("F2 pair 4") << 2 << 4
                                   << QByteArray("ea50e46a3831d8ae613aa67b7096b99002eb10c5e9f95d785e3c51db831177c8")
                                   << QByteArray("5d49441f2285515e94e80ece8f30383f1792ad722e33717933f88a0c70d236db")
                                   << 0x1880;
        QTest::newRow("F2 pair 10") << 2 << 10
                                    << QByteArray("b8cdf74f84056f601a4dccbd66c2dba6f86d2c3011b7e7b6fe97769c4f423ae1")
                                    << QByteArray("ba8002953a29b054610fc7663b21285d5e6dd01220897326355f15df04332c72")
                                    << 0x1880;
    }
    void goldenHashes()
    {
        QFETCH(int, fixture);
        QFETCH(int, pair);
        QFETCH(QByteArray, elfSha);
        QFETCH(QByteArray, selfSha);
        QFETCH(int, selfSize);
        const QByteArray base = fixture == 1 ? f1() : f2();
        QCOMPARE(sha(f1()), QByteArray("00af10a0e02eb93b2ec8d989a4103a32dd4cbb9b90e392a099a580b6aa65c08a"));
        QCOMPARE(sha(f2()), QByteArray("215eacb6dd2d12b7a281905466c6d3c902ba33806e2361dd95d72bd9ae4ef771"));
        const QByteArray e = patched(base, pair);
        QCOMPARE(sha(e), elfSha);
        auto s = makeFakeSelf(e);
        QVERIFY(s.ok());
        QCOMPARE(s->size(), selfSize);
        QCOMPARE(sha(s.value()), selfSha);
        // Round trip (A.1): unwrap gives the patched ELF back exactly.
        auto u = selfio::unwrap(s.value());
        QVERIFY(u.ok());
        QCOMPARE(u.value(), e);
    }

    void workedExampleHeader() // §2.8 worked example
    {
        auto s = makeFakeSelf(patched(f1(), 4));
        QVERIFY(s.ok());
        QCOMPARE(s->left(0x40).toHex(),
                 QByteArray("4f153d1d0001011201010000b00230036050000000000000060022000000000004001100000000"
                            "00e00500000000000040000000000000004000000000000000"));
        auto info = selfio::inspect(s.value());
        QVERIFY(info.ok());
        QCOMPARE(info->kind, SelfKind::FakeSigned);
        QCOMPARE(info->headerSize, quint16(0x2B0));
        QCOMPARE(info->metaSize, quint16(0x330));
        QCOMPARE(info->fileSize, quint64(0x5060));
        const quint64 offsets[] = {0x5E0, 0x620, 0x4E20, 0x4E40, 0x4F40, 0x4F60};
        const quint64 props[] = {0x110004, 0x2804, 0x310004, 0x102804, 0x510004, 0x302804};
        QCOMPARE(int(info->entries.size()), 6);
        for (int i = 0; i < 6; ++i) {
            QCOMPARE(info->entries[size_t(i)].offset, offsets[i]);
            QCOMPARE(info->entries[size_t(i)].props, props[i]);
        }
        QCOMPARE(info->paid.value_or(0), quint64(0x3100000000000002ULL));
        QCOMPARE(info->ptype.value_or(0), quint64(1));
        // Version tail appended after file_size.
        QCOMPARE(s->right(0x20), QByteArray("VERSIONDATA-0123456789abcdefXYZ!"));
    }

    void sectionHeadersAreDropped()
    {
        QByteArray e = f1();
        put16(e, 0x3C, 3);      // e_shnum 3 ...
        put64(e, 0x28, 0x5A20); // ... pointing past the data (ignored by the signer)
        auto s = makeFakeSelf(e);
        QVERIFY(s.ok());
        auto info = selfio::inspect(s.value());
        QVERIFY(info.ok());
        QCOMPARE(info->elf.shnum, quint16(0));
        QCOMPARE(info->elf.shoff, quint64(0x5A20)); // kept
    }

    // ---- SelfReader ----------------------------------------------------------------
    void encryptedAndCompressedDetected() // F7, F8
    {
        auto i7 = selfio::inspect(f7());
        QVERIFY(i7.ok());
        QCOMPARE(i7->kind, SelfKind::Encrypted);
        auto u7 = selfio::unwrap(f7());
        QVERIFY(!u7.ok());
        QCOMPARE(codeOf(u7.error()), Code::SelfEncrypted);
        QVERIFY(u7.error().message.contains(QStringLiteral("encrypted")));
        auto i8 = selfio::inspect(f8());
        QVERIFY(i8.ok());
        QCOMPARE(i8->kind, SelfKind::Compressed);
        QCOMPARE(codeOf(selfio::unwrap(f8()).error()), Code::SelfCompressed);
    }

    void ps5MagicUnwrapsLikeFake() // F9
    {
        auto u = selfio::unwrap(f9());
        QVERIFY(u.ok());
        QCOMPARE(u.value(), f1());
    }

    void paddedHeaderRoundTrips() // F10 (Python would misplace the phdrs)
    {
        const QByteArray e = f1(0x80);
        QVERIFY(ElfImage::parse(e).ok());
        auto s = makeFakeSelf(e);
        QVERIFY(s.ok());
        auto u = selfio::unwrap(s.value());
        QVERIFY(u.ok());
        QCOMPARE(u.value(), e);
    }

    void truncatedSelfIsMalformed() // F11 (SELF)
    {
        auto i = selfio::inspect(selfOf(f1()).left(0x1000));
        QVERIFY(i.ok());
        QCOMPARE(i->kind, SelfKind::Malformed);
        QCOMPARE(codeOf(selfio::unwrap(selfOf(f1()).left(0x1000)).error()), Code::SelfMalformed);
        QVERIFY(!selfio::inspect(QByteArray("\x4f\x15\x3d\x1d", 4)).ok());
        QVERIFY(!selfio::inspect(f1()).ok()); // an ELF is not a SELF
    }

    // ---- hostile headers (review bug 1) ------------------------------------------------
    void hostileProgramHeadersAreRefused_data()
    {
        QTest::addColumn<int>("phdr");
        QTest::addColumn<quint64>("offset");
        QTest::addColumn<quint64>("filesz");
        QTest::newRow("offset wraps") << 0 << quint64(0xFFFFFFFFFFFFFFC0ULL) << quint64(0x4800);
        QTest::newRow("32 GiB") << 0 << quint64(0x800000000ULL) << quint64(0x4800);
        QTest::newRow("crosses the cap") << 0 << quint64(kMaxExecBytes - 0x20) << quint64(0x4800);
        QTest::newRow("size wraps") << 1 << quint64(0x5800) << quint64(0xFFFFFFFFFFFFFF00ULL);
        QTest::newRow("version segment far away") << 4 << quint64(0x7FFFFFFFFFFFFFFFULL) << quint64(0x20);
        QTest::newRow("unplaced phdr far away") << 2 << quint64(0xFFFFFFFFFFFFFFFFULL) << quint64(0x60);
    }
    void hostileProgramHeadersAreRefused()
    {
        QFETCH(int, phdr);
        QFETCH(quint64, offset);
        QFETCH(quint64, filesz);
        QByteArray s = selfOf(f1());
        const qsizetype ph = 0x20 + 6 * 0x20 + 0x40 + qsizetype(phdr) * 0x38; // phdr in the SELF's ELF copy
        put64(s, ph + 0x08, offset);
        put64(s, ph + 0x20, filesz);
        auto info = selfio::inspect(s);
        QVERIFY(info.ok());
        QCOMPARE(info->kind, SelfKind::Malformed);
        auto u = selfio::unwrap(s);
        QVERIFY(!u.ok());
        QCOMPARE(codeOf(u.error()), Code::SelfMalformed);
    }

    void reviewerReproducerIsRefused()
    {
        // The reviewer's hand-made SELF: one data entry, one PT_LOAD at p_offset.
        for (quint64 off : {0xFFFFFFFFFFFFFFC0ULL, 0x800000000ULL}) {
            QByteArray d(0x140, '\0');
            put32(d, 0x00, 0x1D3D154F);
            put16(d, 0x18, 1);
            put64(d, 0x20, 0x2804);
            put64(d, 0x28, 0x100);
            put64(d, 0x30, 0x40);
            put64(d, 0x38, 0x40);
            const char ident[7] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
            std::memcpy(d.data() + 0x40, ident, 7);
            put16(d, 0x50, 0xFE00);
            put16(d, 0x52, 0x3E);
            put32(d, 0x54, 1);
            put64(d, 0x60, 0x40);
            put16(d, 0x74, 0x40);
            put16(d, 0x76, 0x38);
            put16(d, 0x78, 1);
            put32(d, 0x80, 1);
            put64(d, 0x88, off);
            put64(d, 0xA0, 0x40);
            put64(d, 0xA8, 0x40);
            std::memset(d.data() + 0x100, 'A', 0x40);
            QCOMPARE(selfio::inspect(d)->kind, SelfKind::Malformed);
            QCOMPARE(codeOf(selfio::unwrap(d).error()), Code::SelfMalformed);
            // A data entry of size 0 at that offset is refused as well (no huge zero fill).
            put64(d, 0x30, 0);
            QCOMPARE(codeOf(selfio::unwrap(d).error()), Code::SelfMalformed);
        }
    }

    void mutatedHeadersNeverCrash()
    {
        // Seeded byte and word mutations of real headers through every parser
        // and the whole per-file pipeline. Run under ASan in CI to catch
        // out-of-bounds access; here it checks results stay bounded.
        QRandomGenerator rng(0xB4C7);
        const QByteArray bases[] = {selfOf(patched(f1(), 4)), selfOf(f2()), f1(), f2(), f9()};
        int unwrapped = 0;
        for (int iter = 0; iter < 3000; ++iter) {
            QByteArray d = bases[iter % 5];
            const int region = std::min<int>(int(d.size()), 0x300);
            const int edits = 1 + int(rng.bounded(6));
            // Mutations avoid producing offsets between 16 MiB and 1 GiB:
            // those are accepted by design (up to kMaxExecBytes) and would only
            // make the loop allocate hundreds of MiB.
            for (int k = 0; k < edits; ++k) {
                const int at = int(rng.bounded(region));
                switch (rng.bounded(3)) {
                case 0:
                    if ((at & 7) != 3)
                        d[at] = char(rng.generate() & 0xFF);
                    break;
                case 1:
                    if (at + 8 <= d.size())
                        put64(d, at & ~7, rng.generate64());
                    break;
                default:
                    if (at + 8 <= d.size()) {
                        int bit = int(rng.bounded(54));
                        if (bit >= 20)
                            bit += 10; // skip bits 20-29
                        put64(d, at & ~7, get64(d, at & ~7) ^ (quint64(1) << bit));
                    }
                    break;
                }
            }
            if (selfio::hasSelfMagic(d)) {
                auto info = selfio::inspect(d);
                auto u = selfio::unwrap(d);
                if (u.ok()) {
                    ++unwrapped;
                    QVERIFY(u->size() <= kMaxExecBytes);
                    if (u->size() > 64 * 1024 * 1024)
                        continue; // bounded, but too large to push through the pipeline quickly
                }
            } else {
                (void)ElfImage::parse(d, Strictness::Signable);
                (void)ElfImage::parse(d, Strictness::Lenient);
                (void)findParams(d);
                QByteArray copy = d;
                (void)patchSdk(copy, *sdkPair(4));
                (void)makeFakeSelf(d);
            }
            (void)processExecutable(d, *sdkPair(4), NidMode::Auto, true);
        }
        QVERIFY(unwrapped > 0); // the fuzz also reached the successful path
    }

    void versionTailMismatchIsSkipped()
    {
        QByteArray s = selfOf(f1());
        s.append("extra");
        QStringList warnings;
        auto u = selfio::unwrap(s, &warnings);
        QVERIFY(u.ok());
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(u->size(), 0x5A00); // version segment left out
    }

    // ---- NidPatch (A.2) -------------------------------------------------------------
    void nidGolden()
    {
        QByteArray s = selfOf(patched(f2(), 4));
        const QByteArray original = s;
        NidResult r = applyNid(s, 4, NidMode::Auto);
        QCOMPARE(r.action, NidAction::Applied);
        QCOMPARE(r.count, 1);
        QCOMPARE(sha(s), QByteArray("6f93078810d17f6862d9ca29aa1c7f75a73232f11098213cb94e7f00f6a1bc68"));
        // Idempotent: a second run changes nothing.
        const QByteArray once = s;
        NidResult again = applyNid(s, 4, NidMode::Auto);
        QCOMPARE(again.count, 0);
        QCOMPARE(s, once);
        // reverseNid restores the pre-substitution bytes exactly.
        QByteArray back = s;
        reverseNid(back, r);
        QCOMPARE(back, original);
        // Target above 6 reverts.
        NidResult rv = applyNid(s, 7, NidMode::Auto);
        QCOMPARE(rv.action, NidAction::Reverted);
        QCOMPARE(s, original);
    }

    void nidBoundaryAndModes_data()
    {
        QTest::addColumn<int>("target");
        QTest::addColumn<bool>("applies");
        for (int n = 1; n <= 10; ++n)
            QTest::newRow(qPrintable(QStringLiteral("SDK %1").arg(n))) << n << (n <= 6);
    }
    void nidBoundaryAndModes()
    {
        QFETCH(int, target);
        QFETCH(bool, applies);
        QByteArray d = f2();
        const QByteArray d0 = d;
        NidResult r = applyNid(d, target, NidMode::Auto);
        QCOMPARE(r.action == NidAction::Applied, applies);
        QCOMPARE(d != d0, applies);
        QByteArray off = f2();
        QCOMPARE(applyNid(off, target, NidMode::Off).action, NidAction::Disabled);
        QCOMPARE(off, d0);
    }

    void nidAlreadyPatchedAndAbsent()
    {
        QByteArray both = QByteArray("xx4h6F1LLbTiw#A#Byy IWIBBdTHit4#A#B");
        const QByteArray both0 = both;
        QCOMPARE(applyNid(both, 4, NidMode::Auto).action, NidAction::AlreadyPatched);
        QCOMPARE(both, both0);
        QByteArray none = f1();
        QCOMPARE(applyNid(none, 4, NidMode::Auto).action, NidAction::None);
        QCOMPARE(none, f1());
    }
};

QTEST_GUILESS_MAIN(TstBackportFormat)
#include "tst_backport_format.moc"
