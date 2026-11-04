// dlc_emu.ini editor backend against goldens produced by the legacy Python
// module (ui/dlc_emu_ini.py): render, validation messages, offline auto-find,
// plus save with backup and refusal of invalid files.

#include "engines/tools/DlcEmu.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace fw::tools;

namespace {
QByteArray readFile(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QStringList toList(const QJsonValue& v)
{
    QStringList out;
    for (const auto& x : v.toArray())
        out << x.toString();
    return out;
}
} // namespace

class TstDlcEmu : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_data;
    QJsonObject m_expected;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_data = fwtest::dataDir() + QStringLiteral("/dlcemu");
        m_expected = QJsonDocument::fromJson(readFile(m_data + QStringLiteral("/expected.json"))).object();
        QVERIFY(!m_expected.isEmpty());
    }

    void renderMatchesLegacyByteForByte()
    {
        const auto entries = dlcemu::parse(QString::fromUtf8(readFile(m_data + QStringLiteral("/sample.ini"))));
        QCOMPARE(int(entries.size()), 3);
        QCOMPARE(entries[0].section, QStringLiteral("PSAC")); // upper-cased
        QCOMPARE(entries[0].get(QStringLiteral("custom_key")), QStringLiteral("keep me")); // unknown key kept
        QCOMPARE(dlcemu::render(entries).toUtf8(), readFile(m_data + QStringLiteral("/sample.expected.ini")));
        // Lossless round trip.
        QCOMPARE(dlcemu::render(dlcemu::parse(dlcemu::render(entries))), dlcemu::render(entries));
    }

    void validationMatchesLegacyMessages()
    {
        const auto entries = dlcemu::parse(QString::fromUtf8(readFile(m_data + QStringLiteral("/sample.ini"))));
        const auto v = dlcemu::validate(entries, m_data + QStringLiteral("/app0"));
        QCOMPARE(v.errors, toList(m_expected.value(QStringLiteral("errors"))));
        QCOMPARE(v.warnings, toList(m_expected.value(QStringLiteral("warnings"))));
        QVERIFY(!v.ok());
    }

    void limitsAreEnforced()
    {
        std::vector<dlcemu::Entry> many;
        for (int i = 0; i < dlcemu::kMaxEntries + 1; ++i) {
            dlcemu::Entry e;
            e.set(QStringLiteral("content_id"), QStringLiteral("UP9000-PPSA01234_00-%1").arg(i, 16, 10, QLatin1Char('0')));
            many.push_back(e);
        }
        const auto v = dlcemu::validate(many);
        QVERIFY(v.errors.first().startsWith(QStringLiteral("Too many entries: 1025")));
        QVERIFY(std::any_of(v.errors.begin(), v.errors.end(), [](const QString& e) { return e.contains(QStringLiteral("32 KiB")); }));
    }

    void autoFindMatchesLegacySuggestions()
    {
        // Copy the fixture so nothing is written next to the test data.
        const QString app0 = m_root.filePath(QStringLiteral("app0"));
        for (const QString& rel : {QStringLiteral("eboot.bin"), QStringLiteral("sce_sys/param.json")})
            QVERIFY(fwtest::writeTextFile(app0 + u'/' + rel, readFile(m_data + QStringLiteral("/app0/") + rel)));
        for (const QString& d : {QStringLiteral("addcont0"), QStringLiteral("addcont1"), QStringLiteral("addcont2")})
            QDir().mkpath(app0 + u'/' + d);
        const auto s = dlcemu::autoFind(app0, true);
        QCOMPARE(s.prefix, m_expected.value(QStringLiteral("prefix")).toString());
        QCOMPARE(s.prefixIsPlaceholder, m_expected.value(QStringLiteral("prefix_placeholder")).toBool());
        QCOMPARE(s.addcont, toList(m_expected.value(QStringLiteral("addcont"))));
        QCOMPARE(s.baseIds, toList(m_expected.value(QStringLiteral("base_ids"))));
        const QJsonArray exp = m_expected.value(QStringLiteral("suggestions")).toArray();
        QCOMPARE(int(s.entries.size()), exp.size());
        for (int i = 0; i < exp.size(); ++i) {
            const QJsonObject e = exp[i].toObject();
            const dlcemu::Entry& got = s.entries[size_t(i)];
            QCOMPARE(got.confidence, e.value(QStringLiteral("confidence")).toString());
            QCOMPARE(got.source, e.value(QStringLiteral("source")).toString());
            QCOMPARE(got.placeholder, e.value(QStringLiteral("placeholder")).toBool());
            const QJsonObject params = e.value(QStringLiteral("params")).toObject();
            QCOMPARE(int(got.params.size()), params.size());
            for (const QString& k : params.keys())
                QCOMPARE(got.get(k), params.value(k).toString());
        }
    }

    void saveBacksUpAndRefusesInvalidFiles()
    {
        const QString app0 = m_root.filePath(QStringLiteral("save"));
        QDir().mkpath(app0);
        dlcemu::Entry good;
        good.set(QStringLiteral("content_id"), QStringLiteral("UP9000-PPSA01234_00-SEASONPASS000001"));
        QVERIFY(dlcemu::save(app0, {good}).ok());
        const QByteArray first = readFile(app0 + QStringLiteral("/dlc_emu.ini"));
        QVERIFY(first.startsWith("[PSAC]\r\ncontent_id="));
        good.set(QStringLiteral("download_status"), QStringLiteral("INSTALLED"));
        QVERIFY(dlcemu::save(app0, {good}).ok());
        QCOMPARE(readFile(app0 + QStringLiteral("/dlc_emu.ini.bak")), first); // previous version kept
        dlcemu::Entry bad;
        bad.set(QStringLiteral("content_id"), QStringLiteral("SHORT"));
        const QByteArray before = readFile(app0 + QStringLiteral("/dlc_emu.ini"));
        QVERIFY(!dlcemu::save(app0, {bad}).ok());
        QCOMPARE(readFile(app0 + QStringLiteral("/dlc_emu.ini")), before); // untouched
        auto loaded = dlcemu::load(app0);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded->front().get(QStringLiteral("download_status")), QStringLiteral("INSTALLED"));
    }
};

QTEST_GUILESS_MAIN(TstDlcEmu)
#include "tst_dlcemu.moc"
