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

// False also when the job was refused before it could be queued.
bool succeeded(const std::optional<JobRecord>& r)
{
    return r && r->state == JobState::Succeeded;
}

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

    // A conversion carries the image's tree over as it is: the build settings
    // that leave files out, look for a game folder or rebuild an index are for
    // builds from a dump, not for this.
    void convertKeepsFilesTheBuildSettingsWouldLeaveOut()
    {
        Env env(m_root.filePath(QStringLiteral("e-neutral")));
        const QString game = m_root.filePath(QStringLiteral("neutral/PPSA05678"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray(50000, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA05678","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/keep.bin"), QByteArray(9000, 'k')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/notes.bak"), QByteArray(700, 'n')));
        // A second game folder below the first: a game-root search would build from here only.
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/data/inner/eboot.bin"), QByteArray(300, 'i')));
        // An AMPR game whose index is not the one a build would make.
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray(400, 'a')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/ampr_emu.index"), QByteArray("the image's own index")));
        const QString out = m_root.filePath(QStringLiteral("neutral-out"));
        QDir().mkpath(out);
        // Built before the user's exclusions exist: the image holds every file.
        auto built = env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), game}, {QStringLiteral("output"), out + QStringLiteral("/game.exfat")}},
                             {{exfat::keys::kAmprIndex, QStringLiteral("off")}});
        QVERIFY2(built && built->state == JobState::Succeeded, qPrintable(describe(built)));

        QVERIFY(env.store->setValue(pfs::keys::kExcludes, QStringList{QStringLiteral("*.bak")}).ok());
        QVERIFY(env.store->setValue(pfs::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        QVERIFY(env.store->setValue(exfat::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        QVERIFY(env.store->setValue(ufs2::keys::kAmprIndex, QStringLiteral("refresh")).ok());
        QVERIFY(env.store->setValue(exfat::keys::kExcludePatterns, QStringList{QStringLiteral("*.bak"), QStringLiteral("keep.bin")}).ok());
        QVERIFY(env.store->setValue(exfat::keys::kExcludeHidden, true).ok());

        const QMap<QString, QByteArray> source = snapshot(game);
        const auto treeOf = [&](const QString& image, const QString& name) {
            const QString dir = m_root.filePath(QStringLiteral("neutral-x/") + name);
            const QString kind = image.endsWith(QLatin1String(".exfat")) ? QStringLiteral("image.exfat.extract")
                               : image.endsWith(QLatin1String(".ffpkg")) ? QStringLiteral("image.ffpkg.extract")
                                                                         : QStringLiteral("image.pfs.extract");
            auto x = env.run(kind, {{QStringLiteral("image"), image}, {QStringLiteral("output"), dir}});
            if (!x || x->state != JobState::Succeeded)
                qWarning() << "extract failed:" << describe(x);
            QMap<QString, QByteArray> files = snapshot(dir);
            files.remove(QStringLiteral(".pfskeep"));
            return files;
        };
        const QStringList chain{QStringLiteral("game.exfat"), QStringLiteral("game.ffpfs"), QStringLiteral("game.ffpkg"), QStringLiteral("back.exfat")};
        for (qsizetype i = 1; i < chain.size(); ++i) {
            const QString format = QFileInfo(chain[i]).suffix();
            auto conv = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + u'/' + chain[i - 1]},
                                                           {QStringLiteral("format"), format},
                                                           {QStringLiteral("output"), out + u'/' + chain[i]},
                                                           {QStringLiteral("method"), QStringLiteral("repack")}});
            QVERIFY2(conv && conv->state == JobState::Succeeded, qPrintable(chain[i] + QStringLiteral(": ") + describe(conv)));
            const QMap<QString, QByteArray> got = treeOf(out + u'/' + chain[i], chain[i]);
            QVERIFY2(got.contains(QStringLiteral("data/notes.bak")), qPrintable(chain[i] + QStringLiteral(": ") + QStringList(got.keys()).join(u' ')));
            QVERIFY2(got.contains(QStringLiteral("data/keep.bin")), qPrintable(chain[i]));
            QVERIFY2(got.contains(QStringLiteral("eboot.bin")) && got.contains(QStringLiteral("data/inner/eboot.bin")), qPrintable(chain[i]));
            QCOMPARE(got, source);
        }
        // The settings were not changed by the job.
        QCOMPARE(env.store->value(pfs::keys::kExcludes).toStringList(), QStringList{QStringLiteral("*.bak")});
    }

    // An exFAT image does not tell letter case apart, so its AMPR index may be
    // AMPR_EMU.INDEX, which the runtime finds there. An image that tells case
    // apart (FFPKG, a case-sensitive PFS image) would hide it from the runtime,
    // which opens /app0/ampr_emu.index: a conversion gives it that name and
    // keeps its content as it is.
    void convertGivesTheIndexTheNameTheRuntimeOpens()
    {
        Env env(m_root.filePath(QStringLiteral("e-upper")));
        const QString game = m_root.filePath(QStringLiteral("upper/PPSA05679"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray(50000, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA05679","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/fakelib/libSceAmpr.sprx"), QByteArray(400, 'a')));
        const QByteArray own("the image's own index");
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/AMPR_EMU.INDEX"), own));
        const QString out = m_root.filePath(QStringLiteral("upper-out"));
        QDir().mkpath(out);
        auto built = env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), game}, {QStringLiteral("output"), out + QStringLiteral("/game.exfat")}},
                             {{exfat::keys::kAmprIndex, QStringLiteral("off")}});
        QVERIFY2(succeeded(built), qPrintable(describe(built)));
        QVERIFY(env.store->setValue(pfs::keys::kCaseInsensitive, false).ok());

        const auto filesOf = [&](const QString& image, const QString& kind) {
            const QString dir = m_root.filePath(QStringLiteral("upper-x/") + QFileInfo(image).fileName());
            auto x = env.run(kind, {{QStringLiteral("image"), image}, {QStringLiteral("output"), dir}});
            if (!succeeded(x))
                qWarning() << "extract failed:" << describe(x);
            return snapshot(dir);
        };
        // The exFAT image keeps the dump's spelling.
        QCOMPARE(filesOf(out + QStringLiteral("/game.exfat"), QStringLiteral("image.exfat.extract")).value(QStringLiteral("AMPR_EMU.INDEX")), own);

        const QList<std::pair<QString, QString>> targets{{QStringLiteral("game.ffpkg"), QStringLiteral("image.ffpkg.extract")},
                                                         {QStringLiteral("game.ffpfs"), QStringLiteral("image.pfs.extract")}};
        for (const auto& [target, extract] : targets) {
            auto conv = env.run(integration::kConvertJob, {{QStringLiteral("image"), out + QStringLiteral("/game.exfat")},
                                                           {QStringLiteral("format"), QFileInfo(target).suffix()},
                                                           {QStringLiteral("output"), out + u'/' + target},
                                                           {QStringLiteral("method"), QStringLiteral("repack")}});
            QVERIFY2(succeeded(conv), qPrintable(target + QStringLiteral(": ") + describe(conv)));
            QCOMPARE(conv->outcome->verification, Verification::Passed);
            const QMap<QString, QByteArray> got = filesOf(out + u'/' + target, extract);
            QVERIFY2(!got.contains(QStringLiteral("AMPR_EMU.INDEX")), qPrintable(target + QStringLiteral(": ") + QStringList(got.keys()).join(u' ')));
            QCOMPARE(got.value(QStringLiteral("ampr_emu.index")), own);
            QCOMPARE(got.value(QStringLiteral("eboot.bin")), QByteArray(50000, 'E'));
        }
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
        QVERIFY(succeeded(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.exfat")}})));
        QVERIFY(succeeded(env.run(QStringLiteral("image.ffpkg.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpkg")}})));
        QVERIFY(succeeded(env.run(QStringLiteral("image.pfs.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/g.ffpfsc")},
                                                            {QStringLiteral("format"), QStringLiteral("ffpfsc")}})));
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

    void fpkgStagingKeepsDdsAndDropsAuthInfo()
    {
        // A package from an image: the .dds copies in sce_sys are what a missing
        // pic1.png/pic2.png is rebuilt from, so the staged copy keeps them; a
        // dumper's eboot.bin.auth_info is refused by img_verify, so it goes.
        Env env(m_root.filePath(QStringLiteral("e3b")));
        const QString game = m_root.filePath(QStringLiteral("dumps/PPSA05678"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin"), QByteArray(70000, 'E')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/eboot.bin.auth_info"), QByteArray(64, 'A')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_module/libc.prx.auth_info"), QByteArray(64, 'A')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_module/libc.prx"), QByteArray(4096, 'L')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA05678","contentId":"UP0000-PPSA05678_00-0000000000000000","contentVersion":"01.000.000"})"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/pic1.dds"), QByteArray(2048, 'D')));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/pic2.dds"), QByteArray(2048, 'D')));
        const QString out = m_root.filePath(QStringLiteral("stagers-dds"));
        QDir().mkpath(out);
        QVERIFY(succeeded(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), game}, {QStringLiteral("output"), out + QStringLiteral("/g.exfat")}})));
        auto stager = fpkg::sourceStagerFor(out + QStringLiteral("/g.exfat"));
        QVERIFY(stager);
        const QString dest = m_root.filePath(QStringLiteral("staged-dds"));
        QDir().mkpath(dest);
        ProgressReporter pr;
        pr.begin(QStringLiteral("Prepare"), 0);
        auto st = stager->stage(out + QStringLiteral("/g.exfat"), dest, CancellationToken(), &pr);
        QVERIFY2(st.ok(), st.ok() ? "" : qPrintable(st.error().toLogString()));
        QString root = dest; // the game goes into a folder of its own
        if (!QFile::exists(root + QStringLiteral("/eboot.bin")))
            for (const QFileInfo& fi : QDir(dest).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (QFile::exists(fi.filePath() + QStringLiteral("/eboot.bin")))
                    root = fi.filePath();
        const auto got = snapshot(root);
        QVERIFY2(got.contains(QStringLiteral("eboot.bin")), qPrintable(QStringList(got.keys()).join(u',')));
        QVERIFY(got.contains(QStringLiteral("sce_sys/pic1.dds")));
        QVERIFY(got.contains(QStringLiteral("sce_sys/pic2.dds")));
        QVERIFY(got.contains(QStringLiteral("sce_module/libc.prx")));
        QVERIFY(!got.contains(QStringLiteral("eboot.bin.auth_info")));
        QVERIFY(!got.contains(QStringLiteral("sce_module/libc.prx.auth_info")));
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
        QVERIFY(succeeded(env.run(QStringLiteral("image.exfat.build"), {{QStringLiteral("source"), m_game}, {QStringLiteral("output"), out + QStringLiteral("/a.exfat")}})));
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
