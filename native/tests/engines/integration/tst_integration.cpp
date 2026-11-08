// Cross-engine workflows with the real engines: conversion round trip
// exFAT → FFPKG → FFPFSC → exFAT verified against the original dump, FPKG
// source stagers for every image format, and the post-build PS5 upload.

#include "connect/ps5/Module.h"
#include "core/Settings.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/fpkg/SourceStager.h"
#include "engines/general/Module.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/pfs/PfsVerify.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

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
        (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 120000);
        return queue->record(id.value());
    }
};

QString describe(const std::optional<JobRecord>& r)
{
    if (!r)
        return QStringLiteral("no record");
    return jobStateName(r->state) + QStringLiteral(": ") + (r->error ? r->error->toLogString() : (r->outcome ? r->outcome->summary : QString()));
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QMap<QString, QByteArray> snapshot(const QString& root)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFile f(it.filePath());
        if (f.open(QIODevice::ReadOnly))
            out.insert(QDir(root).relativeFilePath(it.filePath()), f.readAll());
    }
    return out;
}

} // namespace

class TstIntegration : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_game;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_game = m_root.filePath(QStringLiteral("dumps/PPSA01234"));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/eboot.bin"), QByteArray(70000, 'E')));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA01234","contentId":"UP0000-PPSA01234_00-0000000000000000","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeRandomFile(m_game + QStringLiteral("/data/level1.bin"), 3 * 1024 * 1024 + 7, 42));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/data/strings/ja.txt"), QByteArray(5000, 'j')));
        QVERIFY(fwtest::writeTextFile(m_game + QStringLiteral("/empty.bin"), QByteArray()));
    }

    void convertRoundTripThroughEveryFormat()
    {
        Env env(m_root.filePath(QStringLiteral("e1")));
        const QString out = m_root.filePath(QStringLiteral("out"));
        QDir().mkpath(out);
        auto built = env.run(QStringLiteral("image.exfat.build"),
                             {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/game.exfat")}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));

        auto toUfs = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + QStringLiteral("/game.exfat")},
                                                         {QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                         {QStringLiteral("output"), out + QStringLiteral("/game.ffpkg")}});
        QVERIFY2(toUfs && toUfs->state == JobState::Succeeded, qPrintable(describe(toUfs)));
        QCOMPARE(toUfs->outcome->verification, Verification::Passed);

        auto toPfs = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + QStringLiteral("/game.ffpkg")},
                                                         {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                         {QStringLiteral("output"), out + QStringLiteral("/game.ffpfsc")}});
        QVERIFY2(toPfs && toPfs->state == JobState::Succeeded, qPrintable(describe(toPfs)));

        auto back = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + QStringLiteral("/game.ffpfsc")},
                                                        {QStringLiteral("format"), QStringLiteral("exfat")},
                                                        {QStringLiteral("output"), out + QStringLiteral("/back.exfat")}});
        QVERIFY2(back && back->state == JobState::Succeeded, qPrintable(describe(back)));

        // The final image still matches the original dump byte for byte.
        auto verify = env.run(QStringLiteral("image.exfat.verify"), {{QStringLiteral("image"), out + QStringLiteral("/back.exfat")},
                                                                     {QStringLiteral("source"), m_game}},
                              {{general::keys::kVerifyMode, QStringLiteral("full")}});
        QVERIFY2(verify && verify->state == JobState::Succeeded, qPrintable(describe(verify)));
        QCOMPARE(verify->outcome->verification, Verification::Passed);

        // Standalone verify (no source) of every image.
        for (const QString& img : {QStringLiteral("game.exfat"), QStringLiteral("game.ffpkg"), QStringLiteral("game.ffpfsc"), QStringLiteral("back.exfat")}) {
            const QString kind = img.endsWith(QStringLiteral(".exfat")) ? QStringLiteral("image.exfat.verify")
                               : img.endsWith(QStringLiteral(".ffpkg")) ? QStringLiteral("image.ffpkg.verify")
                                                                         : QStringLiteral("image.pfs.verify");
            auto v = env.run(kind, {{QStringLiteral("image"), out + u'/' + img}});
            QVERIFY2(v && v->state == JobState::Succeeded, qPrintable(img + QStringLiteral(" ") + describe(v)));
            QVERIFY2(v->outcome->verification != Verification::Failed, qPrintable(img));
        }
    }

    // #27 / #33: .exfat/.ffpkg -> .ffpfsc packs the image as it is (no
    // extracted copy), under an ASCII inner name, whatever the image is called;
    // the container converts back by unpacking. #36 / #19: .ffpfs target.
    void directConvertKeepsUnicodeNamesAndConvertsBack()
    {
        Env env(m_root.filePath(QStringLiteral("e5")));
        const QString out = m_root.filePath(QStringLiteral("direct ü"));
        QDir().mkpath(out);
        const QString exfat = out + QStringLiteral("/PPSA01234 Ghost of Yōtei (01.000).exfat");
        auto built = env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), exfat}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));

        const QString pfsc = out + QStringLiteral("/PPSA01234 Ghost of Yōtei (01.000).ffpfsc");
        auto direct = env.run(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                          {QStringLiteral("output"), pfsc}});
        QVERIFY2(direct && direct->state == JobState::Succeeded, qPrintable(describe(direct)));
        QCOMPARE(direct->outcome->verification, Verification::Passed);
        QCOMPARE(direct->outcome->details.value(QStringLiteral("method")).toString(), QStringLiteral("direct"));
        const QString inner = direct->outcome->details.value(QStringLiteral("innerName")).toString();
        QCOMPARE(inner, QStringLiteral("PPSA01234 Ghost of Yotei (01.000).exfat"));
        QVERIFY(QFileInfo::exists(exfat)); // the source stays
        auto sum = pfs::inspectImage(pfsc);
        QVERIFY(sum.ok());
        QVERIFY(!sum->nested);
        QCOMPARE(QString::fromLatin1(sum->singleFile), inner);

        // Back to exFAT: the packed image is unpacked unchanged.
        const QString back = out + QStringLiteral("/back ü.exfat");
        auto unpacked = env.run(integration::kConvertJob, {{QStringLiteral("image"), pfsc}, {QStringLiteral("format"), QStringLiteral("exfat")},
                                                            {QStringLiteral("output"), back}});
        QVERIFY2(unpacked && unpacked->state == JobState::Succeeded, qPrintable(describe(unpacked)));
        QCOMPARE(fileBytes(back), fileBytes(exfat));

        // To FFPKG: unpacked, then repacked; it matches the dump.
        const QString ufs = out + QStringLiteral("/from container.ffpkg");
        auto toUfs = env.run(integration::kConvertJob, {{QStringLiteral("image"), pfsc}, {QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                         {QStringLiteral("output"), ufs}});
        QVERIFY2(toUfs && toUfs->state == JobState::Succeeded, qPrintable(describe(toUfs)));
        auto v = env.run(QStringLiteral("image.ffpkg.verify"), {{QStringLiteral("image"), ufs}, {QStringLiteral("source"), m_game}},
                         {{general::keys::kVerifyMode, QStringLiteral("full")}});
        QVERIFY2(v && v->state == JobState::Succeeded && v->outcome->verification == Verification::Passed, qPrintable(describe(v)));

        // Uncompressed .ffpfs: a folder image of the files (repack), and on to .ffpfsc.
        const QString ffpfs = out + QStringLiteral("/Yōtei.ffpfs");
        auto toFfpfs = env.run(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpfs")},
                                                           {QStringLiteral("output"), ffpfs}});
        QVERIFY2(toFfpfs && toFfpfs->state == JobState::Succeeded, qPrintable(describe(toFfpfs)));
        QCOMPARE(toFfpfs->outcome->details.value(QStringLiteral("method")).toString(), QStringLiteral("repack"));
        QCOMPARE(toFfpfs->outcome->details.value(QStringLiteral("format")).toString(), QStringLiteral("FFPFS"));
        auto pv = env.run(QStringLiteral("image.pfs.verify"), {{QStringLiteral("image"), ffpfs}, {QStringLiteral("source"), m_game}});
        QVERIFY2(pv && pv->state == JobState::Succeeded && pv->outcome->verification == Verification::Passed, qPrintable(describe(pv)));
        auto again = env.run(integration::kConvertJob, {{QStringLiteral("image"), ffpfs}, {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                         {QStringLiteral("output"), out + QStringLiteral("/repacked.ffpfsc")}});
        QVERIFY2(again && again->state == JobState::Succeeded, qPrintable(describe(again)));

        // An existing output is refused unless replacing is asked for; then the old one goes.
        QVERIFY(!env.queue->enqueue(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                               {QStringLiteral("output"), pfsc}}).ok());
        auto replaced = env.run(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpfsc")},
                                                            {QStringLiteral("output"), pfsc}, {QStringLiteral("method"), QStringLiteral("repack")},
                                                            {QStringLiteral("replaceExisting"), true}});
        QVERIFY2(replaced && replaced->state == JobState::Succeeded, qPrintable(describe(replaced)));
        QVERIFY(pfs::inspectImage(pfsc)->nested); // a folder container now
        QVERIFY(!QFileInfo::exists(pfsc + QStringLiteral(".old")));
        // "direct" only exists for .exfat/.ffpkg -> .ffpfsc.
        QVERIFY(!env.queue->enqueue(integration::kConvertJob, {{QStringLiteral("image"), exfat}, {QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                               {QStringLiteral("output"), out + QStringLiteral("/x.ffpkg")}, {QStringLiteral("method"), QStringLiteral("direct")}}).ok());
    }

    void convertRefusesNonsense()
    {
        Env env(m_root.filePath(QStringLiteral("e2")));
        QVERIFY(!env.queue->enqueue(integration::kConvertJob, {{QStringLiteral("image"), m_root.filePath(QStringLiteral("missing.exfat"))},
                                                               {QStringLiteral("format"), QStringLiteral("ffpkg")},
                                                               {QStringLiteral("output"), m_root.filePath(QStringLiteral("x.ffpkg"))}}).ok());
    }

    void fpkgStagersExtractEveryImageFormat()
    {
        Env env(m_root.filePath(QStringLiteral("e3")));
        const QString out = m_root.filePath(QStringLiteral("stagers"));
        QDir().mkpath(out);
        QVERIFY(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.exfat")}})->state == JobState::Succeeded);
        QVERIFY(env.run(QStringLiteral("image.ffpkg.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpkg")}})->state == JobState::Succeeded);
        QVERIFY(env.run(QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpfsc")},
                                                            {QStringLiteral("format"), QStringLiteral("ffpfsc")}})->state == JobState::Succeeded);
        const auto expected = snapshot(m_game);
        for (const QString& img : {QStringLiteral("g.exfat"), QStringLiteral("g.ffpkg"), QStringLiteral("g.ffpfsc")}) {
            auto stager = fpkg::sourceStagerFor(out + u'/' + img);
            QVERIFY2(stager, qPrintable(img));
            QVERIFY(stager->probe(out + u'/' + img).ok());
            const QString dest = m_root.filePath(QStringLiteral("staged-") + img);
            QDir().mkpath(dest);
            ProgressReporter pr;
            pr.begin(QStringLiteral("Prepare"), 0);
            auto st = stager->stage(out + u'/' + img, dest, CancellationToken(), &pr);
            QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
            // PFS containers may hold the game one level down after a deep unpack.
            QString root = dest;
            if (!QFile::exists(root + QStringLiteral("/eboot.bin")))
                for (const QFileInfo& fi : QDir(dest).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                    if (QFile::exists(fi.filePath() + QStringLiteral("/eboot.bin")))
                        root = fi.filePath();
            const auto got = snapshot(root);
            QCOMPARE(got.keys(), expected.keys());
            QVERIFY2(got == expected, qPrintable(img));
        }
    }

    void verifiedBuildsAreQueuedForPs5UploadWhenAsked()
    {
        Env env(m_root.filePath(QStringLiteral("e4")));
        QVERIFY(env.store->setValue(ps5::keys::kConsoleIp, QStringLiteral("127.0.0.1")).ok());
        integration::PostBuildActions actions(*env.queue, *env.store);
        QSignalSpy queued(&actions, &integration::PostBuildActions::uploadQueued);
        const QString out = m_root.filePath(QStringLiteral("upload"));
        QDir().mkpath(out);
        // Off by default: nothing queued.
        QVERIFY(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/a.exfat")}})->state == JobState::Succeeded);
        QTest::qWait(100);
        QCOMPARE(queued.count(), 0);
        // On for this job: the verified image is queued for upload.
        auto b = env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/b.exfat")}},
                         {{integration::keys::kAutoUpload, true}});
        QVERIFY(b && b->state == JobState::Succeeded);
        QVERIFY(QTest::qWaitFor([&] { return queued.count() == 1; }, 5000));
        const auto up = env.queue->record(queued.first().at(1).toString());
        QVERIFY(up);
        QCOMPARE(up->kind, ps5::jobkinds::kUpload);
        QCOMPARE(up->parameters.value(QStringLiteral("sources")).toArray().first().toString(), out + QStringLiteral("/b.exfat"));
        env.queue->cancel(up->id);
    }
};

QTEST_GUILESS_MAIN(TstIntegration)
#include "tst_integration.moc"
