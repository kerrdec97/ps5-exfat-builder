// Zipped dumps as a build source (GitHub #67, #37): a ZIP of the game's
// contents and a ZIP of the game folder both build verified exFAT, FFPKG and
// FFPFSC images through the real queue; the images hold the same files as
// the dump; archives without a game or with two games are refused before
// anything is written; inspectZip reads param.json without unpacking.

#include "core/ArchiveSource.h"
#include "core/Settings.h"
#include "core/Zip.h"
#include "connect/ps5/Module.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/Module.h"
#include "engines/general/Module.h"
#include "engines/integration/ImageCompare.h"
#include "engines/integration/ImageFiles.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
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
    std::optional<JobRecord> run(const QString& kind, const QJsonObject& params)
    {
        auto id = queue->enqueue(kind, params, {});
        if (!id.ok())
            return std::nullopt;
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

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// Zips `folder` with every entry under `prefix` ("" or "Name/").
bool zipFolder(const QString& folder, const QString& zipPath, const QString& prefix)
{
    zip::Writer w;
    if (!w.open(zipPath, true).ok())
        return false;
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        if (!w.addFile(prefix + QDir(folder).relativeFilePath(p), p, zip::Method::Deflate, {}).ok())
            return false;
    }
    return w.finish(true).ok();
}

} // namespace

class TstZipSource : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<Env> m_env;
    QString m_game;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_env = std::make_unique<Env>(m_root.path());
        m_game = m_root.filePath(QStringLiteral("PPSA09999 Zip Test"));
        QVERIFY(writeFile(m_game + QStringLiteral("/eboot.bin"), QByteArray("\x7f" "ELF", 4) + QByteArray(4000, 'e')));
        QVERIFY(writeFile(m_game + QStringLiteral("/sce_sys/param.json"),
                  R"({"titleId":"PPSA09999","contentVersion":"01.000.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Zip Test"}}})"));
        QVERIFY(writeFile(m_game + QStringLiteral("/data/a.bin"), QByteArray(70000, 'a')));
        QVERIFY(writeFile(m_game + QStringLiteral("/data/sub/b.txt"), "hello zip"));
        QVERIFY(writeFile(m_game + QStringLiteral("/sce_module/libc.prx"), QByteArray(3000, 'c')));
    }

    void inspectReadsParamWithoutUnpacking()
    {
        const QString z = m_root.filePath(QStringLiteral("inspect.zip"));
        QVERIFY(zipFolder(m_game, z, QStringLiteral("PPSA09999-app0/")));
        QVERIFY(archive::isZipSource(z));
        auto g = archive::inspectZip(z);
        QVERIFY2(g.ok(), g.ok() ? "" : qPrintable(g.error().message));
        QCOMPARE(g->rootPrefix, QStringLiteral("PPSA09999-app0/"));
        QVERIFY(g->hasEboot && g->hasParam);
        QCOMPARE(g->files, qint64(5));
        QVERIFY(g->paramJson.contains("Zip Test"));
        QVERIFY(g->otherGames.isEmpty());
    }

    void buildsEveryImageFormatFromAZip_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("ext");
        QTest::addColumn<QString>("prefix");
        QTest::newRow("exfat, contents") << QStringLiteral("image.exfat.build") << QStringLiteral(".exfat") << QString();
        QTest::newRow("exfat, folder") << QStringLiteral("image.exfat.build") << QStringLiteral(".exfat") << QStringLiteral("Game/");
        QTest::newRow("ffpkg, folder") << QStringLiteral("image.ffpkg.build") << QStringLiteral(".ffpkg") << QStringLiteral("PPSA09999-app0/");
        QTest::newRow("ffpfsc, contents") << QStringLiteral("image.pfs.build") << QStringLiteral(".ffpfsc") << QString();
    }

    void buildsEveryImageFormatFromAZip()
    {
        QFETCH(QString, kind);
        QFETCH(QString, ext);
        QFETCH(QString, prefix);
        const QString tag = QString::fromLatin1(QTest::currentDataTag()).replace(u',', u'_').replace(u' ', u'_');
        const QString z = m_root.filePath(QStringLiteral("dump-%1.zip").arg(tag));
        QVERIFY(zipFolder(m_game, z, prefix));
        const QString out = m_root.filePath(QStringLiteral("out/%1%2").arg(tag, ext));
        QDir().mkpath(QFileInfo(out).absolutePath());
        QJsonObject p{{QStringLiteral("source"), z}, {QStringLiteral("output"), out}};
        if (ext == QLatin1String(".ffpfsc"))
            p.insert(QStringLiteral("format"), QStringLiteral("ffpfsc"));
        const auto r = m_env->run(kind, p);
        QVERIFY2(r && r->state == JobState::Succeeded, qPrintable(describe(r)));
        QVERIFY(QFileInfo::exists(out));
        QCOMPARE(r->outcome->verification, Verification::Passed);
        // The image holds exactly the dump's files.
        auto ta = integration::openFileTree(out, {});
        auto tb = integration::openFileTree(m_game, {});
        QVERIFY(ta.ok() && tb.ok());
        integration::CompareOptions o;
        o.depth = integration::CompareDepth::Full;
        auto cmp = integration::compareTrees(*ta.value(), *tb.value(), o, {});
        QVERIFY2(cmp.ok(), cmp.ok() ? "" : qPrintable(cmp.error().message));
        QVERIFY2(cmp->identical(), qPrintable(cmp->summary()));
    }

    void refusesZipsWithoutOneGame()
    {
        // No game.
        const QString empty = m_root.filePath(QStringLiteral("nogame.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(empty, true).ok());
            QVERIFY(w.addData(QStringLiteral("readme.txt"), "not a game").ok());
            QVERIFY(w.finish().ok());
        }
        QVERIFY(!archive::checkZipSource(empty).ok());
        auto r = m_env->run(QStringLiteral("image.exfat.build"),
                            {{QStringLiteral("source"), empty}, {QStringLiteral("output"), m_root.filePath(QStringLiteral("out/nogame.exfat"))}});
        QVERIFY(!r || r->state != JobState::Succeeded);
        QVERIFY(!QFileInfo::exists(m_root.filePath(QStringLiteral("out/nogame.exfat"))));
        // Two games.
        const QString two = m_root.filePath(QStringLiteral("two.zip"));
        {
            zip::Writer w;
            QVERIFY(w.open(two, true).ok());
            QVERIFY(w.addData(QStringLiteral("A/eboot.bin"), "x").ok());
            QVERIFY(w.addData(QStringLiteral("B/eboot.bin"), "y").ok());
            QVERIFY(w.finish().ok());
        }
        const Status st = archive::checkZipSource(two);
        QVERIFY(!st.ok());
        QVERIFY2(st.error().message.contains(QStringLiteral("more than one game")), qPrintable(st.error().message));
    }
};

QTEST_GUILESS_MAIN(TstZipSource)
#include "tst_zip_source.moc"
