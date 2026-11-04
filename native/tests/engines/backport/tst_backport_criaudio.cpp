// CRI Unity audio fix (docs/BACKPORT_PARITY.md matrix #40, CriAudio.h): the
// 12-channel speaker rack becomes 8 channels. Vectors from the owner's engine
// test (test_cri_audio_rack.py) plus the refusal rules of
// families_v2.cri_audio_rack: "CRI Atom" required, exactly one 12-channel
// rack in an executable segment, an 8-channel rack 1..0x40 bytes before it.

#include "BackportTestEnv.h"

#include "engines/backport/BackportEngine.h"
#include "engines/backport/CriAudio.h"
#include "engines/backport/SelfReader.h"
#include "support/TestSupport.h"

#include <QJsonArray>
#include <QTest>

using namespace fw;
using namespace fw::backport;
using namespace fwtest::bp;

namespace {

const QByteArray kRack12 = QByteArray::fromHex("c74594000c0000c7030c0000006a0c58");
const QByteArray kRack8 = QByteArray::fromHex("c7459400080000c703080000006a0858");

void put(QByteArray& b, qsizetype off, const QByteArray& bytes)
{
    std::memcpy(b.data() + off, bytes.constData(), size_t(bytes.size()));
}

// F2 (code PT_LOAD R+X at file 0x1000 = VA 0) with the plugin's two racks:
// the 8-channel one at 0x1100 and the 12-channel one 0x12 bytes later.
QByteArray criModule(bool withName = true)
{
    QByteArray b = f2(3);
    put(b, 0x1100, kRack8);
    put(b, 0x1112, kRack12);
    if (withName)
        put(b, 0x3100, QByteArray("CRI Atom Ver.2.28"));
    return b;
}

QByteArray unwrapFile(const QString& path)
{
    auto u = selfio::unwrap(readBytes(path));
    return u.ok() ? u.value() : QByteArray();
}

} // namespace

class TstBackportCriAudio : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
    }

    void rackPatterns()
    {
        QVERIFY(isCriRack(kRack12, 12));
        QVERIFY(!isCriRack(kRack12, 8));
        QVERIFY(isCriRack(kRack8, 8));
        QByteArray other = kRack12;
        other[2] = char(0x88); // a wildcard byte
        other[8] = char(0x07);
        QVERIFY(isCriRack(other, 12));
        other[13] = char(0x6B);
        QVERIFY(!isCriRack(other, 12));
        QVERIFY(!isCriRack(kRack12.left(15), 12));
    }

    void patchesTheOneConfirmedLayout()
    {
        QByteArray m = criModule();
        auto a = analyzeCriRack(m);
        QVERIFY(a.ok());
        QCOMPARE(a->state, CriRackState::Patch);
        QCOMPARE(a->va, quint64(0x112));
        QCOMPARE(a->fileOffset, quint64(0x1112));
        const QByteArray before = m;
        auto r = applyCriRackFix(m);
        QVERIFY(r.ok());
        QCOMPARE(m.mid(0x1112, 16), kRack8); // 0xC00 -> 0x800, 12 -> 8, push 12 -> push 8
        int changed = 0;
        for (qsizetype i = 0; i < m.size(); ++i)
            changed += m[i] != before[i] ? 1 : 0;
        QCOMPARE(changed, 3);
        // Applied once: the next look finds it fixed and changes nothing.
        QCOMPARE(analyzeCriRack(m)->state, CriRackState::AlreadyFixed);
        const QByteArray fixed = m;
        QCOMPARE(applyCriRackFix(m)->state, CriRackState::AlreadyFixed);
        QCOMPARE(m, fixed);
    }

    void leavesEverythingElseAlone()
    {
        // Not a CRI Atom module.
        QCOMPARE(analyzeCriRack(criModule(false))->state, CriRackState::NotApplicable);
        QCOMPARE(analyzeCriRack(QByteArray::fromHex("7f454c46") + QByteArray(64, '\0'))->state,
                 CriRackState::NotApplicable);
        // Two 12-channel racks.
        QByteArray two = criModule();
        put(two, 0x1400, kRack8);
        put(two, 0x1412, kRack12);
        auto a = analyzeCriRack(two);
        QCOMPARE(a->state, CriRackState::Unknown);
        QVERIFY(a->why.contains(QStringLiteral("2 12-channel")));
        QByteArray untouched = two;
        QCOMPARE(applyCriRackFix(untouched)->state, CriRackState::Unknown);
        QCOMPARE(untouched, two);
        // The 8-channel rack 0x41 bytes before: too far.
        QByteArray far = f2(3);
        put(far, 0x1100, kRack8);
        put(far, 0x1141, kRack12);
        put(far, 0x3100, QByteArray("CRI Atom"));
        QCOMPARE(analyzeCriRack(far)->state, CriRackState::Unknown);
        // A rack in a non-executable segment (the R/W PT_LOAD at 0x2000) is not code.
        QByteArray data = f2(3);
        put(data, 0x2040, kRack12); // would be "unknown" (no 8-channel rack) if data were scanned
        put(data, 0x3100, QByteArray("CRI Atom"));
        QCOMPARE(analyzeCriRack(data)->state, CriRackState::NotApplicable);
    }

    void pipelineAppliesItOnlyWhenAsked()
    {
        const QByteArray self = selfOf(criModule());
        auto plain = processExecutable(self, *sdkPair(4), NidMode::Auto, true);
        QVERIFY(plain.ok());
        QCOMPARE(plain->criRack.state, CriRackState::NotApplicable);
        QCOMPARE(plain->elf.mid(0x1112, 16), kRack12);
        ExecutableFixes fixes;
        fixes.criAudioRack = true;
        auto fixed = processExecutable(self, *sdkPair(4), NidMode::Auto, true, nullptr, fixes);
        QVERIFY2(fixed.ok(), qPrintable(fixed.ok() ? QString() : fixed.error().message));
        QCOMPARE(fixed->criRack.state, CriRackState::Patch);
        QCOMPARE(selfio::unwrap(fixed->output).value().mid(0x1112, 16), kRack8);
    }

    void runFixesThePluginByDefault()
    {
        Env e;
        const QString plugin = e.game + "/Media/Plugins/cri_ware_unity.prx";
        QVERIFY(writeBytes(plugin, selfOf(criModule())));
        auto r = runBackport(e.options(false), e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(unwrapFile(plugin).mid(0x1112, 16), kRack8);
        QCOMPARE(r->details.value(QStringLiteral("criAudioFix")).toArray().size(), 1);
        QCOMPARE(r->details.value(QStringLiteral("criAudioFix")).toArray().at(0).toString(),
                 QStringLiteral("Media/Plugins/cri_ware_unity.prx"));
    }

    void runLeavesThePluginWhenTurnedOff()
    {
        Env e;
        const QString plugin = e.game + "/Media/Plugins/cri_ware_unity.prx";
        QVERIFY(writeBytes(plugin, selfOf(criModule())));
        auto o = e.options(false);
        o.criAudioFix = false;
        auto r = runBackport(o, e.ctx);
        QVERIFY2(r.ok(), qPrintable(r.ok() ? QString() : r.error().toLogString()));
        QCOMPARE(unwrapFile(plugin).mid(0x1112, 16), kRack12);
        QVERIFY(!r->details.contains(QStringLiteral("criAudioFix")));
    }
};

QTEST_GUILESS_MAIN(TstBackportCriAudio)
#include "tst_backport_criaudio.moc"
