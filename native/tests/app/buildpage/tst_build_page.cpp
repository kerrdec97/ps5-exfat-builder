// The Build page with its five outputs (exFAT, FFPKG, PFS, FPKG and the zstd
// Packed folder), driven in the real main window with a small fake game dump:
//
//  * each format shows only its own settings, with the 2-3 common ones in the
//    Format card and the rest folded in Advanced options;
//  * submit enqueues that format's job kind with only that format's overrides;
//  * real builds of the dump finish and verify in every format (FPKG with the
//    fake prospero-pub-cmd of the FPKG tests), also from a non-ASCII folder
//    and to a non-ASCII file name (#50, #22, #46);
//  * a wrapper folder resolves to the game below it, several games are offered
//    and a new selection replaces a running scan (#84, #53);
//  * the file name follows Settings › Build › Output name (presets, template,
//    firmware), keeps Unicode and can be edited per job (#60, #43);
//  * PFS offers uncompressed .ffpfs (#36, #19);
//  * Packed folder packs the dump (and a zipped dump) into a folder of zstd
//    packs with the bundled runtime, refuses the dump's own folder and an
//    existing folder, and shows the zstd settings only;
//  * the former "fpkg" page key opens the Build page with FPKG selected.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/BuildImagePage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Zip.h"
#include "engines/ampr/Module.h"
#include "engines/ampr/ZstdPack.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/gameinfo/Module.h"
#include "engines/gameinfo/Naming.h"
#include "engines/general/Module.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QtEndian>

using namespace fw;

namespace {

const QString kVerify = general::keys::kVerifyAfterBuild;
const QString kMode = general::keys::kVerifyMode;
const QString kThreads = general::keys::kWorkerThreads;
const QString kKeepTemp = general::keys::kKeepJobTemp;
const QString kUpload = integration::keys::kAutoUpload;

QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    qToBigEndian(v, b.data());
    return b;
}

// 1x1 RGB PNG (what sce_sys/icon0.png must be for an FPKG).
QByteArray makePng()
{
    QByteArray ihdr = be32(1) + be32(1);
    ihdr += char(8);
    ihdr += char(2);
    ihdr += QByteArray(3, '\0');
    QByteArray png("\x89PNG\r\n\x1A\n", 8);
    png += be32(13) + QByteArray("IHDR") + ihdr + be32(0);
    png += be32(0) + QByteArray("IEND") + be32(0);
    return png;
}

// A small PS5-style dump: param.json (title, content ID, version, required
// firmware 4.50, SDK 4.00), eboot.bin, the sce_sys files an FPKG needs and
// some data files.
bool makeDump(const QString& d, const QString& title, const QString& titleId = QStringLiteral("PPSA01342"),
              const QString& contentId = QStringLiteral("UP9000-PPSA01342_00-GHOSTOFYOTEI0000"))
{
    const QString param = QStringLiteral(
                              "{\"titleId\":\"%1\",\"contentId\":\"%2\",\"contentVersion\":\"01.000.000\","
                              "\"applicationCategoryType\":0,\"applicationDrmType\":\"free\","
                              "\"requiredSystemSoftwareVersion\":\"0x0450000000000000\",\"sdkVersion\":\"0x0400000000000000\","
                              "\"localizedParameters\":{\"defaultLanguage\":\"en-US\",\"en-US\":{\"titleName\":\"%3\"}}}")
                              .arg(titleId, contentId, title);
    bool ok = fwtest::writeTextFile(d + QStringLiteral("/sce_sys/param.json"), param.toUtf8());
    ok = ok && fwtest::writeTextFile(d + QStringLiteral("/eboot.bin"), QByteArray("\x7F" "ELF", 4) + QByteArray(70000, '\x02'));
    ok = ok && fwtest::writeTextFile(d + QStringLiteral("/sce_sys/keystone"), QByteArray(96, 'K'));
    ok = ok && fwtest::writeTextFile(d + QStringLiteral("/sce_sys/icon0.png"), makePng());
    ok = ok && fwtest::writeTextFile(d + QStringLiteral("/sce_sys/param.sfo"), "sfo");
    ok = ok && fwtest::writeRandomFile(d + QStringLiteral("/Data/a.pak"), 300 * 1024, 11);
    ok = ok && fwtest::writeRandomFile(d + QStringLiteral("/Data/sub/b.bin"), 50 * 1024, 12);
    ok = ok && fwtest::writeTextFile(d + QStringLiteral("/Data/readme.txt"), QByteArray(4000, 'r'));
    return ok;
}

bool waitForState(JobQueue& q, const QString& id, JobState st, int ms)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && r->state == st;
    }, ms);
}

QStringList ids(JobQueue& q)
{
    QStringList out;
    for (const auto& r : q.records())
        out << r.id;
    return out;
}

QString clean(const QString& p) { return QDir::cleanPath(QDir::fromNativeSeparators(p)); }

} // namespace

class TstBuildPage : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    pages::BuildImagePage* m_page = nullptr;
    QString m_game;      // non-ASCII folder and title
    QString m_asciiGame; // for the fake Publishing Tools
    QString m_tools;

    QString path(const QString& rel) const { return m_root.filePath(rel); }

    bool selectSource(const QString& folder)
    {
        m_page->setSource(folder);
        return QTest::qWaitFor([&] {
            return m_page->summary().exists && m_page->summary().isGame && clean(m_page->summary().folder) == clean(folder);
        }, 15000);
    }

    // Clicks "Add to queue"; returns the new job's id (empty if nothing was added).
    QString addToQueue()
    {
        const QStringList before = ids(m_ctx->queue());
        m_page->queueButton()->click();
        for (const QString& id : ids(m_ctx->queue()))
            if (!before.contains(id))
                return id;
        return {};
    }

    void resetNaming()
    {
        auto& s = m_ctx->settings();
        QVERIFY(s.setValue(gameinfo::keys::kNamePreset, QStringLiteral("ppsa_title_ver")).ok());
        QVERIFY(s.setValue(gameinfo::keys::kNameTemplate, gameinfo::kDefaultImageNameTemplate).ok());
        QVERIFY(s.setValue(gameinfo::keys::kFirmwareOverride, QStringLiteral("auto")).ok());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        // The user's own Publishing Tools, played by the FPKG tests' fake.
        m_tools = path(QStringLiteral("tools"));
        QVERIFY(QDir().mkpath(m_tools + QStringLiteral("/toolchain/ext")));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), m_tools + QStringLiteral("/toolchain/prospero-pub-cmd.exe")));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), m_tools + QStringLiteral("/prospero-dds2png.exe")));
        QVERIFY(fwtest::writeTextFile(m_tools + QStringLiteral("/toolchain/libScePubTools.dll"), ""));
        qputenv("FAKE_PUBCMD_LOG", path(QStringLiteral("fake-pubcmd.log")).toUtf8());

        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        auto& s = m_ctx->settings();
        QVERIFY(s.setValue(fpkg::keys::kPubToolsPath, m_tools).ok());
        QVERIFY(s.setValue(fpkg::keys::kPlayGoChunkCount, 4).ok());
        QVERIFY(s.setValue(general::keys::kFreeSpaceMarginMiB, 0).ok());

        m_game = path(QStringLiteral("Dumps/Ghost of Yōtei"));
        QVERIFY(makeDump(m_game, QStringLiteral("Ghost of Yōtei")));
        m_asciiGame = path(QStringLiteral("Dumps/PPSA01234-app"));
        QVERIFY(makeDump(m_asciiGame, QStringLiteral("Test Game"), QStringLiteral("PPSA01234"),
                         QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000")));

        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        m_page = qobject_cast<pages::BuildImagePage*>(m_window->page(QStringLiteral("build")));
        QVERIFY(m_page);
        m_window->navigate(QStringLiteral("build"));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void formatsShowOnlyTheirOptions()
    {
        struct Case {
            QString format, prefix, kind, ext;
            QStringList shared, mustHave, mustNot, essentials;
        };
        // Every format: the speed row (performance mode) up front; the image
        // formats also the zstd packs switch, with its details under Advanced.
        const QString kSpeed = general::keys::kPerformanceMode;
        const QStringList zstd = {ampr::keys::kZstdPack, ampr::keys::kZstdLevel, ampr::keys::kPackStrategy, ampr::keys::kVerifyPacks};
        const QList<Case> cases = {
            {jobs::kFormatExfat, QStringLiteral("exfat."), QStringLiteral("image.exfat.build"), QStringLiteral(".exfat"),
             QStringList{kVerify, kMode, kThreads, kSpeed, kKeepTemp, kUpload} + zstd,
             {exfat::keys::kClusterSize, exfat::keys::kImageSizeGiB, exfat::keys::kLabelPolicy, exfat::keys::kExcludePatterns,
              exfat::keys::kDeleteSourceAfterVerify, kVerify, kMode, kThreads, kKeepTemp, kUpload, kSpeed, ampr::keys::kZstdPack,
              ampr::keys::kZstdLevel},
             {exfat::keys::kEditStrategy, exfat::keys::kExtractParallel, exfat::keys::kExtractThreads},
             {kSpeed, exfat::keys::kClusterSize, exfat::keys::kImageSizeGiB, ampr::keys::kZstdPack}},
            {jobs::kFormatFfpkg, QStringLiteral("ufs2."), QStringLiteral("image.ffpkg.build"), QStringLiteral(".ffpkg"),
             QStringList{kVerify, kMode, kThreads, kSpeed, kKeepTemp, kUpload} + zstd, // threads: the zstd packer
             {ufs2::keys::kBlockSize, ufs2::keys::kBytesPerInode, ufs2::keys::kDeleteSourceAfterBuild, kVerify, kMode, kSpeed,
              ampr::keys::kZstdPack},
             {},
             {kSpeed, ampr::keys::kZstdPack}},
            {jobs::kFormatFfpfsc, QStringLiteral("pfs."), QStringLiteral("image.pfs.build"), QStringLiteral(".ffpfsc"),
             QStringList{kVerify, kMode, kThreads, kSpeed, kKeepTemp, kUpload} + zstd,
             {pfs::keys::kCompressionLevel, pfs::keys::kContainerMinCompressMiB, pfs::keys::kExcludes, pfs::keys::kThreads, kThreads,
              ampr::keys::kZstdPack},
             {pfs::keys::kDeleteSourceAfterConvert, pfs::keys::kCompressFiles, pfs::keys::kFileMinCompressMiB},
             {kSpeed, pfs::keys::kCompressionLevel, pfs::keys::kCompressFiles, ampr::keys::kZstdPack}},
            {jobs::kFormatFfpfs, QStringLiteral("pfs."), QStringLiteral("image.pfs.build"), QStringLiteral(".ffpfs"),
             QStringList{kVerify, kMode, kThreads, kSpeed, kKeepTemp, kUpload} + zstd,
             {pfs::keys::kCompressionLevel, pfs::keys::kCompressFiles, pfs::keys::kFileMinCompressMiB, pfs::keys::kSkipExecutables},
             {pfs::keys::kDeleteSourceAfterConvert, pfs::keys::kContainerMinCompressMiB, pfs::keys::kContainerBlockGain},
             {kSpeed, pfs::keys::kCompressionLevel, pfs::keys::kCompressFiles, ampr::keys::kZstdPack}},
            {pages::kFormatFpkg, QStringLiteral("fpkg."), QStringLiteral("fpkg.build"), {},
             {kVerify, kThreads, kSpeed, kKeepTemp},
             {fpkg::keys::kPlayGoChunkCount, fpkg::keys::kPostVerify, fpkg::keys::kCompressionLevel, fpkg::keys::kClearVersionFileUri,
              fpkg::keys::kPlayGoFallback, fpkg::keys::kComputeSha256, kVerify, kSpeed},
             {kMode, kUpload, fpkg::keys::kPubToolsPath, ampr::keys::kZstdPack},
             {kSpeed, fpkg::keys::kPlayGoChunkCount, fpkg::keys::kPostVerify}},
            // Packed folder: the zstd settings (the switch itself is implied and hidden), speed,
            // threads and temporary files; no image verification, no PS5 upload.
            {pages::kFormatZstdFolder, QStringLiteral("ampr."), ampr::kPackJob, {},
             {kThreads, kSpeed, kKeepTemp},
             {ampr::keys::kZstdLevel, ampr::keys::kPackStrategy, ampr::keys::kVerifyPacks, kSpeed, kThreads, kKeepTemp},
             {ampr::keys::kZstdPack, kVerify, kMode, kUpload, fpkg::keys::kPubToolsPath, exfat::keys::kClusterSize, pfs::keys::kCompressionLevel},
             {kSpeed, ampr::keys::kZstdLevel, ampr::keys::kPackStrategy}},
        };
        m_page->advancedSection()->setExpanded(true);
        for (const Case& c : cases) {
            m_page->setFormat(c.format);
            QCOMPARE(m_page->format(), c.format);
            const QStringList keys = m_page->visibleOptionKeys();
            QVERIFY2(!keys.isEmpty(), qPrintable(c.format));
            QStringList folded;
            for (const QString& key : keys)
                if (!m_page->essentialOptionKeys().contains(key))
                    folded << key;
            qInfo("%s: card [%s] advanced [%s]", qPrintable(c.format), qPrintable(m_page->essentialOptionKeys().join(QStringLiteral(", "))),
                  qPrintable(folded.join(QStringLiteral(", "))));
            for (const QString& key : keys)
                QVERIFY2(key.startsWith(c.prefix) || c.shared.contains(key), qPrintable(c.format + u' ' + key));
            for (const QString& key : c.mustHave)
                QVERIFY2(keys.contains(key), qPrintable(c.format + QStringLiteral(" lacks ") + key));
            for (const QString& key : c.mustNot)
                QVERIFY2(!keys.contains(key), qPrintable(c.format + QStringLiteral(" shows ") + key));
            QCOMPARE(m_page->essentialOptionKeys(), c.essentials);
            // What is on screen: no row of another format, every row of this one.
            QStringList onScreen;
            for (ui::SettingRow* row : m_page->findChildren<ui::SettingRow*>())
                if (row->isVisibleTo(m_page))
                    onScreen << row->def().key;
            onScreen.sort();
            QStringList expected = keys;
            expected.sort();
            QCOMPARE(onScreen, expected);
            QCOMPARE(m_page->packageCard()->isVisibleTo(m_page), c.format == pages::kFormatFpkg);
            QCOMPARE(m_page->primaryButton()->text(),
                     c.format == pages::kFormatFpkg ? QStringLiteral("Build package")
                     : c.format == pages::kFormatZstdFolder ? QStringLiteral("Pack game") : QStringLiteral("Build image"));
            if (c.format == pages::kFormatZstdFolder) {
                // Its level and strategy are usable although the global "Compress game files" switch is off.
                ui::JobOptionsPanel* panel = m_page->optionsPanel(c.format);
                QVERIFY(!m_ctx->settings().value(ampr::keys::kZstdPack).toBool());
                QVERIFY(panel->row(ampr::keys::kZstdLevel)->editor()->widget()->isEnabled());
                QVERIFY(panel->row(ampr::keys::kPackStrategy)->editor()->widget()->isEnabled());
                QVERIFY(panel->row(ampr::keys::kZstdPack)->isHidden());
            }
            if (!c.ext.isEmpty())
                QCOMPARE(jobs::outputExtension(c.format), c.ext);
        }
        m_page->advancedSection()->setExpanded(false);
        m_page->setFormat(jobs::kFormatExfat);
    }

    void submitSendsTheFormatsJobAndOverrides()
    {
        QVERIFY(selectSource(m_game));
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        struct Case {
            QString format, kind, ext, key;
            QVariant value;
        };
        const QList<Case> cases = {
            {jobs::kFormatExfat, QStringLiteral("image.exfat.build"), QStringLiteral(".exfat"), exfat::keys::kLabelPolicy, QStringLiteral("titleId")},
            {jobs::kFormatFfpkg, QStringLiteral("image.ffpkg.build"), QStringLiteral(".ffpkg"), ufs2::keys::kMinFreePercent, 3},
            {jobs::kFormatFfpfsc, QStringLiteral("image.pfs.build"), QStringLiteral(".ffpfsc"), pfs::keys::kCompressionLevel, 6},
            {jobs::kFormatFfpfs, QStringLiteral("image.pfs.build"), QStringLiteral(".ffpfs"), pfs::keys::kCompressFiles, true},
            {pages::kFormatFpkg, QStringLiteral("fpkg.build"), QStringLiteral(".pkg"), fpkg::keys::kPlayGoChunkCount, 7},
            {pages::kFormatZstdFolder, ampr::kPackJob, {}, ampr::keys::kZstdLevel, QStringLiteral("9")},
        };
        // An override set on another format's panel never travels with this job.
        m_page->optionsPanel(jobs::kFormatExfat)->setOverride(exfat::keys::kOverwriteExisting, false);
        for (const Case& c : cases) {
            m_page->setFormat(c.format);
            m_page->setOutputFolder(path(QStringLiteral("queued/") + c.format));
            ui::JobOptionsPanel* panel = m_page->optionsPanel(c.format);
            QVERIFY(panel);
            panel->setOverride(c.key, c.value);
            QVERIFY2(panel->overrides().contains(c.key), qPrintable(c.key));
            const QString id = addToQueue();
            QVERIFY2(!id.isEmpty(), qPrintable(c.format));
            auto rec = q.record(id);
            QVERIFY(rec);
            QCOMPARE(rec->kind, c.kind);
            QCOMPARE(rec->state, JobState::Queued);
            QCOMPARE(rec->overrides.value(c.key), c.value);
            for (auto it = rec->overrides.cbegin(); it != rec->overrides.cend(); ++it)
                QVERIFY2(panel->keys().contains(it.key()), qPrintable(c.format + QStringLiteral(" sent ") + it.key()));
            if (c.format != pages::kFormatFpkg) {
                QCOMPARE(clean(rec->parameters.value(QStringLiteral("source")).toString()), clean(m_game));
                const QString out = rec->parameters.value(QStringLiteral("output")).toString();
                QCOMPARE(QFileInfo(out).fileName(), QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000)") + c.ext);
                QCOMPARE(clean(QFileInfo(out).absolutePath()), clean(path(QStringLiteral("queued/") + c.format)));
                if (c.kind == QLatin1String("image.pfs.build"))
                    QCOMPARE(rec->parameters.value(QStringLiteral("format")).toString(), c.format);
            } else {
                QCOMPARE(clean(rec->parameters.value(QStringLiteral("source")).toString()), clean(m_game));
                QCOMPARE(rec->parameters.value(QStringLiteral("contentId")).toString(), QStringLiteral("UP9000-PPSA01342_00-GHOSTOFYOTEI0000"));
                QCOMPARE(m_page->fileName(), QStringLiteral("UP9000-PPSA01342_00-GHOSTOFYOTEI0000-A0100-V0100.pkg"));
            }
            QVERIFY(q.remove(id));
            panel->clearOverrides();
        }
        m_page->optionsPanel(jobs::kFormatExfat)->clearOverrides();
        m_page->setFormat(jobs::kFormatExfat);
    }

    void realBuildsOfADummyDumpVerify()
    {
        // exFAT, FFPKG, FFPFSC and FFPFS from a folder named "Ghost of Yōtei",
        // saved as "PPSA01342 Ghost of Yōtei (01.000.000).<ext>".
        QVERIFY(selectSource(m_game));
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        const QString outDir = path(QStringLiteral("e2e"));
        QList<QPair<QString, QString>> queued; // id, format
        for (const QString& f : {jobs::kFormatExfat, jobs::kFormatFfpkg, jobs::kFormatFfpfsc, jobs::kFormatFfpfs}) {
            m_page->setFormat(f);
            m_page->setOutputFolder(outDir);
            const QString id = addToQueue();
            QVERIFY2(!id.isEmpty(), qPrintable(f));
            queued << qMakePair(id, f);
        }
        q.holdQueue(false);
        for (const auto& [id, f] : queued) {
            const bool done = QTest::qWaitFor([&] {
                auto r = q.record(id);
                return r && (r->state == JobState::Succeeded || r->state == JobState::Failed || r->state == JobState::Cancelled);
            }, 240000);
            QVERIFY2(done, qPrintable(f));
            auto rec = q.record(id);
            QVERIFY2(rec->state == JobState::Succeeded,
                     qPrintable(f + QStringLiteral(": ") + (rec->error ? rec->error->toLogString() : QString())));
            QVERIFY(rec->outcome);
            QCOMPARE(rec->outcome->verification, Verification::Passed);
            QCOMPARE(rec->outcome->outputs.size(), 1);
            const QFileInfo out(rec->outcome->outputs.first());
            QCOMPARE(out.fileName(), QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000)") + jobs::outputExtension(f));
            QVERIFY(out.isFile());
            QVERIFY(out.size() > 0);
            qInfo("%s: %s, %lld bytes, %s", qPrintable(f), qPrintable(out.fileName()), out.size(), qPrintable(rec->outcome->verificationDetail));
        }
        QVERIFY(!QFile::exists(QDir(outDir).filePath(QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000).exfat.part"))));
        q.clearFinished();
    }

    void realFpkgBuildWithTheFakePublishingTools()
    {
        auto& q = m_ctx->queue();
        // From the dump folder.
        m_page->setFormat(pages::kFormatFpkg);
        QVERIFY(selectSource(m_asciiGame));
        m_page->setOutputFolder(path(QStringLiteral("e2e-pkg")));
        QCOMPARE(m_page->fileName(), QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000-A0100-V0100.pkg"));
        q.holdQueue(true);
        const QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->kind, QStringLiteral("fpkg.build"));
        q.holdQueue(false);
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(id);
            return r && (r->state == JobState::Succeeded || r->state == JobState::Failed);
        }, 120000));
        auto rec = q.record(id);
        QVERIFY2(rec->state == JobState::Succeeded, qPrintable(rec->error ? rec->error->toLogString() : QString()));
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        const QString pkg = path(QStringLiteral("e2e-pkg/UP0000-PPSA01234_00-GOLDENTEST000000-A0100-V0100.pkg"));
        QCOMPARE(rec->outcome->outputs, QStringList{pkg});
        QVERIFY(QFileInfo(pkg).size() > 0);
        qInfo("fpkg (folder): %s, %s", qPrintable(QFileInfo(pkg).fileName()), qPrintable(rec->outcome->verificationDetail));

        // From an .exfat image of the same dump (the FPKG source can be an image).
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_asciiGame));
        m_page->setOutputFolder(path(QStringLiteral("e2e-img")));
        q.holdQueue(true);
        const QString imgJob = addToQueue();
        QVERIFY(!imgJob.isEmpty());
        q.holdQueue(false);
        QVERIFY(waitForState(q, imgJob, JobState::Succeeded, 120000));
        const QString image = q.record(imgJob)->outcome->outputs.first();
        QVERIFY(QFileInfo(image).isFile());
        m_page->navigateWith({{QStringLiteral("source"), image}});
        QCOMPARE(m_page->format(), pages::kFormatFpkg); // only FPKG takes an image
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().exists && clean(m_page->summary().folder) == clean(image); }, 5000));
        m_page->setOutputFolder(path(QStringLiteral("e2e-pkg-from-image")));
        q.holdQueue(true);
        const QString fromImage = addToQueue();
        QVERIFY(!fromImage.isEmpty());
        QCOMPARE(clean(q.record(fromImage)->parameters.value(QStringLiteral("source")).toString()), clean(image));
        q.holdQueue(false);
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(fromImage);
            return r && (r->state == JobState::Succeeded || r->state == JobState::Failed);
        }, 120000));
        rec = q.record(fromImage);
        QVERIFY2(rec->state == JobState::Succeeded, qPrintable(rec->error ? rec->error->toLogString() : QString()));
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        qInfo("fpkg (from .exfat): %s, %s", qPrintable(QFileInfo(rec->outcome->outputs.value(0)).fileName()),
              qPrintable(rec->outcome->verificationDetail));
        q.clearFinished();
        m_page->setFormat(jobs::kFormatExfat);
    }

    void packedFolderOfADummyDump()
    {
        // Packed folder: the dump's files in zstd packs next to the loose system
        // files and the bundled runtime, from the folder and from a zipped dump.
        auto& q = m_ctx->queue();
        m_page->setFormat(pages::kFormatZstdFolder);
        QVERIFY(selectSource(m_game));
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000)"));
        const QString outDir = path(QStringLiteral("e2e-zstd"));
        m_page->setOutputFolder(outDir);
        q.holdQueue(true);
        const QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        auto rec = q.record(id);
        QCOMPARE(rec->kind, ampr::kPackJob);
        QCOMPARE(rec->overrides.value(ampr::keys::kZstdPack).toBool(), true);
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("output")).toString()),
                 clean(QDir(outDir).filePath(QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000)"))));
        q.holdQueue(false);
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(id);
            return r && isTerminal(r->state);
        }, 120000));
        rec = q.record(id);
        QVERIFY2(rec->state == JobState::Succeeded, qPrintable(rec->error ? rec->error->toLogString() : QString()));
        QCOMPARE(rec->outcome->verification, Verification::Passed);
        const QDir out(QDir(outDir).filePath(QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000)")));
        QCOMPARE(clean(rec->outcome->outputs.value(0)), clean(out.path()));
        QVERIFY(QFileInfo(out.filePath(ampr::kZstdIndexName)).isFile());
        QVERIFY(QFileInfo(out.filePath(QStringLiteral("ampr_zstd-000.pak"))).isFile());
        QVERIFY(QFileInfo(out.filePath(QStringLiteral("eboot.bin"))).isFile());
        QVERIFY(QFileInfo(out.filePath(QStringLiteral("sce_sys/param.json"))).isFile());
        QVERIFY(QFileInfo(out.filePath(QStringLiteral("fakelib/libSceAmpr.sprx"))).isFile());
        QVERIFY(rec->outcome->details.value(QStringLiteral("packedFiles")).toInt() >= 1);
        qInfo("zstd folder: %s", qPrintable(rec->outcome->summary));

        // The same folder again is refused before anything is queued, and so is
        // the dump folder itself (next to the dump under the dump's own name the
        // page proposes " (zstd)").
        q.holdQueue(true);
        QVERIFY(addToQueue().isEmpty());
        QVERIFY(m_ctx->settings().setValue(gameinfo::keys::kNamePreset, QStringLiteral("folder")).ok());
        m_page->setOutputFolder(QFileInfo(m_game).absolutePath());
        QCOMPARE(m_page->fileName(), QStringLiteral("Ghost of Yōtei (zstd)"));
        m_page->setFileStem(QStringLiteral("Ghost of Yōtei"));
        QVERIFY(addToQueue().isEmpty());
        resetNaming();

        // A zipped dump packs the same way (the ZIP is read for its title and version).
        const QString zipPath = path(QStringLiteral("zips/PPSA01234-app.zip"));
        {
            QDir().mkpath(QFileInfo(zipPath).absolutePath());
            zip::Writer w;
            QVERIFY(w.open(zipPath, true).ok());
            QDirIterator it(m_asciiGame, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString p = it.next();
                QVERIFY(w.addFile(QStringLiteral("PPSA01234-app/") + QDir(m_asciiGame).relativeFilePath(p), p, zip::Method::Deflate, {}).ok());
            }
            QVERIFY(w.finish(true).ok());
        }
        m_page->setSource(zipPath);
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && clean(m_page->summary().folder) == clean(zipPath); }, 15000));
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01234 Test Game (01.000.000)"));
        m_page->setOutputFolder(path(QStringLiteral("e2e-zstd-zip")));
        const QString zipJob = addToQueue();
        QVERIFY(!zipJob.isEmpty());
        q.holdQueue(false);
        QVERIFY(QTest::qWaitFor([&] {
            auto r = q.record(zipJob);
            return r && isTerminal(r->state);
        }, 120000));
        rec = q.record(zipJob);
        QVERIFY2(rec->state == JobState::Succeeded, qPrintable(rec->error ? rec->error->toLogString() : QString()));
        const QDir zout(path(QStringLiteral("e2e-zstd-zip/PPSA01234 Test Game (01.000.000)")));
        QVERIFY(QFileInfo(zout.filePath(ampr::kZstdIndexName)).isFile());
        QVERIFY(QFileInfo(zout.filePath(QStringLiteral("eboot.bin"))).isFile());
        q.clearFinished();
        m_page->setFormat(jobs::kFormatExfat);
        m_page->setSource({});
    }

    void wrapperFoldersFindTheGame()
    {
        auto& q = m_ctx->queue();
        m_page->setFormat(jobs::kFormatExfat);
        // One game below the chosen folder: it is used (#84, #53).
        const QString wrapper = path(QStringLiteral("wrap1"));
        const QString inner = wrapper + QStringLiteral("/Game Sub/PPSA01342-app");
        QVERIFY(makeDump(inner, QStringLiteral("Wrapped")));
        m_page->setSource(wrapper);
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && clean(m_page->summary().folder) == clean(inner); }, 15000));
        QCOMPARE(clean(m_page->source()), clean(inner));
        QVERIFY2(m_page->sourceMessage().contains(QStringLiteral("Found the game")), qPrintable(m_page->sourceMessage()));
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 Wrapped (01.000.000).exfat"));

        // Two games: both are offered and nothing can be queued until one is chosen.
        const QString two = path(QStringLiteral("wrap2"));
        QVERIFY(makeDump(two + QStringLiteral("/A/PPSA00001-app"), QStringLiteral("Game A"), QStringLiteral("PPSA00001")));
        QVERIFY(makeDump(two + QStringLiteral("/B/PPSA00002-app"), QStringLiteral("Game B"), QStringLiteral("PPSA00002")));
        m_page->setSource(two);
        QVERIFY(QTest::qWaitFor([&] { return m_page->gameRootChoices().size() == 2; }, 15000));
        QVERIFY(!m_page->summary().isGame);
        QVERIFY2(m_page->sourceMessage().contains(QStringLiteral("2 games")), qPrintable(m_page->sourceMessage()));
        q.holdQueue(true);
        QVERIFY(addToQueue().isEmpty());
        // Choosing one of them.
        bool clicked = false;
        for (QPushButton* b : m_page->findChildren<QPushButton*>())
            if (b->accessibleName() == QStringLiteral("Use PPSA00002-app") && b->isVisibleTo(m_page)) {
                b->click();
                clicked = true;
                break;
            }
        QVERIFY(clicked);
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && m_page->summary().titleId == QStringLiteral("PPSA00002"); }, 15000));

        // No game at all: a clear message, no walk over the whole tree.
        const QString none = path(QStringLiteral("nothing-here/deeper"));
        QVERIFY(QDir().mkpath(none));
        m_page->setSource(path(QStringLiteral("nothing-here")));
        QVERIFY(QTest::qWaitFor([&] { return m_page->sourceMessage().contains(QStringLiteral("No game here")); }, 15000));
        QVERIFY(addToQueue().isEmpty());

        // A new selection replaces a scan that is still running.
        m_page->setSource(two);
        m_page->setSource(m_game);
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && clean(m_page->summary().folder) == clean(m_game); }, 15000));
        QTest::qWait(300);
        QCOMPARE(clean(m_page->summary().folder), clean(m_game));
        QVERIFY(m_page->gameRootChoices().isEmpty());
    }

    void fileNamesFollowTheTemplateAndCanBeEdited()
    {
        auto& s = m_ctx->settings();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        // Default preset: Title ID, title and version; Unicode kept (#60: no more "Y_tei").
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000).exfat"));
        QVERIFY(s.setValue(gameinfo::keys::kNamePreset, QStringLiteral("ppsa_title_ver_fw")).ok());
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 Ghost of Yōtei (01.000.000) (FW4).exfat"));
        QVERIFY(s.setValue(gameinfo::keys::kNameTemplate, QStringLiteral("%PPSA% - %TITLE% [%FIRMWARE%] %TYPE% %sdk%")).ok());
        QVERIFY(s.setValue(gameinfo::keys::kNamePreset, QStringLiteral("custom")).ok());
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 - Ghost of Yōtei [FW4] exFAT 4.00.exfat"));
        m_page->setFormat(jobs::kFormatFfpkg);
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 - Ghost of Yōtei [FW4] ffpkg 4.00.ffpkg"));
        QVERIFY(s.setValue(gameinfo::keys::kFirmwareOverride, QStringLiteral("7")).ok());
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01342 - Ghost of Yōtei [FW7] ffpkg 4.00.ffpkg"));
        QVERIFY(s.setValue(gameinfo::keys::kNamePreset, QStringLiteral("folder")).ok()); // #43
        QCOMPARE(m_page->fileName(), QStringLiteral("Ghost of Yōtei.ffpkg"));
        resetNaming();

        // Typed names stay for this job (also across formats), with the format's extension.
        m_page->setFileStem(QStringLiteral("Mon jeu ☆ (copie)"));
        QVERIFY(m_page->fileNameEdited());
        m_page->setFormat(jobs::kFormatFfpfs);
        QCOMPARE(m_page->fileName(), QStringLiteral("Mon jeu ☆ (copie).ffpfs"));
        auto& q = m_ctx->queue();
        q.holdQueue(true);
        m_page->setOutputFolder(path(QStringLiteral("named")));
        const QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(QFileInfo(q.record(id)->parameters.value(QStringLiteral("output")).toString()).fileName(),
                 QStringLiteral("Mon jeu ☆ (copie).ffpfs"));
        QVERIFY(q.remove(id));
        // A name Windows cannot use is refused before anything is queued.
        m_page->setFileStem(QStringLiteral("bad: name?"));
        QVERIFY(addToQueue().isEmpty());
        // A new source gets its own name again (#16).
        QVERIFY(selectSource(m_asciiGame));
        QVERIFY(!m_page->fileNameEdited());
        QCOMPARE(m_page->fileName(), QStringLiteral("PPSA01234 Test Game (01.000.000).ffpfs"));
        m_page->setFormat(jobs::kFormatExfat);
    }

    void formatDetailsAndNotes()
    {
        // exFAT: ShadowMount+ 1.7 needs 64 KiB clusters; Automatic is 64 KiB.
        m_page->setFormat(jobs::kFormatExfat);
        ui::JobOptionsPanel* exfatPanel = m_page->optionsPanel(jobs::kFormatExfat);
        QCOMPARE(m_ctx->settings().value(exfat::keys::kClusterSize).toString(), QStringLiteral("auto"));
        exfatPanel->setOverride(exfat::keys::kClusterSize, QStringLiteral("32768"));
        QVERIFY(exfatPanel->row(exfat::keys::kClusterSize)->messageText().contains(QStringLiteral("ShadowMount+")));
        exfatPanel->setOverride(exfat::keys::kClusterSize, QStringLiteral("65536"));
        QVERIFY(exfatPanel->row(exfat::keys::kClusterSize)->messageText().isEmpty());
        // "N changed" counts the folded settings of this format only.
        auto summaryText = [this] {
            for (ui::Badge* b : m_page->advancedSection()->findChildren<ui::Badge*>())
                if (b->text().contains(QStringLiteral("changed")) || b->text() == QStringLiteral("Defaults"))
                    return b->text();
            return QString();
        };
        exfatPanel->clearOverrides();
        QCOMPARE(summaryText(), QStringLiteral("Defaults"));
        exfatPanel->setOverride(exfat::keys::kLabelPolicy, QStringLiteral("none"));
        QCOMPARE(summaryText(), QStringLiteral("1 changed"));
        m_page->setFormat(jobs::kFormatFfpkg);
        QCOMPARE(summaryText(), QStringLiteral("Defaults"));
        m_page->setFormat(jobs::kFormatExfat);
        QCOMPARE(summaryText(), QStringLiteral("1 changed"));
        exfatPanel->clearOverrides();
        // PFS: uncompressed .ffpfs is offered (#36, #19); its level only matters when files are compressed.
        m_page->setFormat(jobs::kFormatFfpfs);
        QCOMPARE(m_page->format(), jobs::kFormatFfpfs);
        QCOMPARE(jobs::imageBuildKind(m_page->format()), QStringLiteral("image.pfs.build"));
        ui::JobOptionsPanel* pfsPanel = m_page->optionsPanel(jobs::kFormatFfpfs);
        QVERIFY(!pfsPanel->row(pfs::keys::kCompressionLevel)->editor()->widget()->isEnabled());
        pfsPanel->setOverride(pfs::keys::kCompressFiles, true);
        QVERIFY(pfsPanel->row(pfs::keys::kCompressionLevel)->editor()->widget()->isEnabled());
        pfsPanel->clearOverrides();
        m_page->setFormat(jobs::kFormatFfpfsc);
        QVERIFY(pfsPanel->row(pfs::keys::kCompressionLevel)->editor()->widget()->isEnabled());
        m_page->setFormat(jobs::kFormatExfat);
    }

    void fpkgPageKeyOpensTheBuildPage()
    {
        // No separate FPKG page any more; every caller of the old key lands here.
        QVERIFY(!m_window->pageKeys().contains(QStringLiteral("fpkg")));
        m_window->navigate(QStringLiteral("home"));
        m_window->navigate(QStringLiteral("fpkg"), {{QStringLiteral("source"), m_asciiGame}});
        QCOMPARE(m_window->currentPage(), QStringLiteral("build"));
        QCOMPARE(m_page->format(), pages::kFormatFpkg);
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && clean(m_page->summary().folder) == clean(m_asciiGame); }, 15000));
        m_window->navigate(QStringLiteral("fpkg"));
        QCOMPARE(m_window->currentPage(), QStringLiteral("build"));
        // Drops: a folder for every format; images and .gp5 projects only for FPKG.
        const QString image = path(QStringLiteral("drop/PPSA01234 Test.exfat"));
        const QString project = path(QStringLiteral("drop/game.gp5"));
        QVERIFY(fwtest::writeTextFile(image, "img"));
        QVERIFY(fwtest::writeTextFile(project, "<psproject/>"));
        QVERIFY(m_page->acceptsDrop({QUrl::fromLocalFile(image)}));
        QVERIFY(m_page->acceptsDrop({QUrl::fromLocalFile(project)}));
        QVERIFY(m_page->acceptsDrop({QUrl::fromLocalFile(m_game)}));
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(!m_page->acceptsDrop({QUrl::fromLocalFile(image)}));
        QVERIFY(m_page->acceptsDrop({QUrl::fromLocalFile(m_game)}));
        // An image source shows its kind; switching away from FPKG explains why it cannot be used.
        m_page->setFormat(pages::kFormatFpkg);
        m_page->dropUrls({QUrl::fromLocalFile(image)});
        QCOMPARE(m_page->summary().titleId, QStringLiteral("PPSA01234"));
        QVERIFY(m_page->packageCard()->isVisibleTo(m_page));
        m_page->setFormat(jobs::kFormatFfpfsc);
        QVERIFY2(m_page->sourceMessage().contains(QStringLiteral("Only FPKG")), qPrintable(m_page->sourceMessage()));
        m_ctx->queue().holdQueue(true);
        QVERIFY(addToQueue().isEmpty());
        m_page->setFormat(jobs::kFormatExfat);
        m_page->setSource({});
    }

    void demoStatesRender()
    {
        // Every demo state at the full and the narrow window width. With
        // FW_BUILD_PAGE_SHOTS=<folder>, the whole page (not only the visible
        // part) is saved there for review.
        const QString shots = qEnvironmentVariable("FW_BUILD_PAGE_SHOTS");
        for (const int width : {1440, 1100}) {
            m_window->resize(width, width == 1440 ? 920 : 760);
            m_window->navigate(QStringLiteral("build"));
            QTest::qWait(50);
            for (const QString& state : m_page->demoStates()) {
                m_page->applyDemoState(state);
                QTest::qWait(60);
                QWidget* content = m_page->scrollArea()->widget();
                const int viewport = m_page->scrollArea()->viewport()->width();
                if (content->width() > viewport) {
                    QStringList wide;
                    for (QWidget* w : content->findChildren<QWidget*>())
                        if (w->isVisibleTo(content) && w->minimumSizeHint().width() > viewport / 2)
                            wide << QStringLiteral("%1(%2) min %3").arg(QString::fromLatin1(w->metaObject()->className()),
                                                                        qobject_cast<QLabel*>(w) ? qobject_cast<QLabel*>(w)->text() : w->objectName())
                                        .arg(w->minimumSizeHint().width());
                    qWarning("%s: content %d > viewport %d: %s", qPrintable(state), content->width(), viewport, qPrintable(wide.join(u' ')));
                }
                // No sideways scrolling. The offscreen platform has no fonts (text
                // metrics are far wider than on Windows), so only a real platform asserts it.
                if (QGuiApplication::platformName() != QLatin1String("offscreen"))
                    QVERIFY2(content->width() <= viewport, qPrintable(state));
                if (!shots.isEmpty()) {
                    QDir().mkpath(shots);
                    QVERIFY(content->grab().save(QDir(shots).filePath(QStringLiteral("%1-%2.png").arg(width).arg(state))));
                }
                m_page->clearDemoState();
            }
        }
        m_window->resize(1440, 920);
        QCOMPARE(m_page->format(), jobs::kFormatExfat);
        QVERIFY(m_page->source().isEmpty());
    }

    // FPKG › Backport (the ppr-pkg Companion's Backport tab) and FPKG › App
    // with backport files (its dual backports) enqueue fpkg.backport jobs.
    void backportUpdatesSubmitBackportJobs()
    {
        auto& q = m_ctx->queue();
        const QString base = path(QStringLiteral("Packages/UP0000-PPSA01234_00-GOLDENTEST000000-A0100-V0100.pkg"));
        // A base package with its own param.json (version 01.000.000, like the test game).
        QVERIFY(fwtest::writeTextFile(base, QByteArray("\x7F" "FIH", 4) + QByteArray(4096, '\0')
                                                + QByteArray("{\"contentId\":\"UP0000-PPSA01234_00-GOLDENTEST000000\","
                                                             "\"contentVersion\":\"01.000.000\",\"titleId\":\"PPSA01234\"}")
                                                + QByteArray(512, '\0')));
        const QString b4 = path(QStringLiteral("Backports/FW 4xx PPSA01234 Test (01.000.000) backport files"));
        QVERIFY(fwtest::writeTextFile(b4 + QStringLiteral("/eboot.bin"), "backported"));
        q.holdQueue(true);

        // Backport type: base package + 4.xx folder, no game folder.
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatFpkg},
                                                     {QStringLiteral("packageType"), QStringLiteral("backport")},
                                                     {QStringLiteral("basePackage"), base},
                                                     {QStringLiteral("backport4xx"), b4}});
        QVERIFY(m_page->packageCard()->isVisible());
        QString id = addToQueue();
        QVERIFY2(!id.isEmpty(), "no backport job was added");
        auto rec = q.record(id);
        QVERIFY(rec);
        QCOMPARE(rec->kind, QStringLiteral("fpkg.backport"));
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("package")).toString()), clean(base));
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("backport4xx")).toString()), clean(b4));
        QCOMPARE(rec->parameters.value(QStringLiteral("buildBase")).toBool(), false);
        QVERIFY(q.remove(id));

        // With the game folder too: the game's version (= the base's) becomes base + 1.
        m_page->setSource(m_asciiGame);
        QVERIFY(selectSource(m_asciiGame));
        QLineEdit* version = nullptr;
        for (auto* e : m_page->findChildren<QLineEdit*>())
            if (e->accessibleName() == QStringLiteral("Version"))
                version = e;
        QVERIFY(version);
        QVERIFY(QTest::qWaitFor([&] { return version->text() == QStringLiteral("01.000.001"); }, 15000));
        id = addToQueue();
        QVERIFY(!id.isEmpty());
        rec = q.record(id);
        QCOMPARE(rec->parameters.value(QStringLiteral("version")).toString(), QStringLiteral("01.000.001"));
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("game")).toString()), clean(m_asciiGame));
        QVERIFY(q.remove(id));

        // App type with a backport: the package and its update in one job.
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatFpkg},
                                                     {QStringLiteral("packageType"), QStringLiteral("app")},
                                                     {QStringLiteral("source"), m_asciiGame}});
        QVERIFY(selectSource(m_asciiGame));
        QCOMPARE(version->text(), QStringLiteral("01.000.000")); // leaving Backport puts the game's version back
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("backport4xx"), QString()}, {QStringLiteral("backport5xx"), b4}});
        id = addToQueue();
        if (id.isEmpty()) {
            QStringList shown;
            for (auto* bar : m_page->findChildren<ui::InfoBar*>())
                if (bar->isVisible())
                    for (auto* l : bar->findChildren<QLabel*>())
                        shown << l->text();
            qWarning() << "form error:" << shown;
        }
        QVERIFY2(!id.isEmpty(), "no dual backport job was added");
        rec = q.record(id);
        QCOMPARE(rec->kind, QStringLiteral("fpkg.backport"));
        QCOMPARE(rec->parameters.value(QStringLiteral("buildBase")).toBool(), true);
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("game")).toString()), clean(m_asciiGame));
        QCOMPARE(clean(rec->parameters.value(QStringLiteral("backport5xx")).toString()), clean(b4));
        QVERIFY(q.remove(id));
        q.holdQueue(false);
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("backport5xx"), QString()}});
        m_page->setFormat(jobs::kFormatExfat);
    }
};

QTEST_MAIN(TstBuildPage)
#include "tst_build_page.moc"
