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
#include "app/pages/LibraryPage.h"
#include "app/services/JobSubmit.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/FileUtil.h"
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

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
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

    // A base package with its own param.json (version 01.000.000, like the test
    // game), written on first use. Empty when it could not be written.
    QString basePackage()
    {
        const QString base = path(QStringLiteral("Packages/UP0000-PPSA01234_00-GOLDENTEST000000-A0100-V0100.pkg"));
        if (QFileInfo(base).isFile())
            return base;
        const bool ok = fwtest::writeTextFile(base, QByteArray("\x7F" "FIH", 4) + QByteArray(4096, '\0')
                                                        + QByteArray("{\"contentId\":\"UP0000-PPSA01234_00-GOLDENTEST000000\","
                                                                     "\"contentVersion\":\"01.000.000\",\"titleId\":\"PPSA01234\"}")
                                                        + QByteArray(512, '\0'));
        return ok ? base : QString();
    }

    bool resetNaming()
    {
        auto& s = m_ctx->settings();
        return s.setValue(gameinfo::keys::kNamePreset, QStringLiteral("ppsa_title_ver")).ok()
               && s.setValue(gameinfo::keys::kNameTemplate, gameinfo::kDefaultImageNameTemplate).ok()
               && s.setValue(gameinfo::keys::kFirmwareOverride, QStringLiteral("auto")).ok();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        // The FPKG engine keeps a work folder under %ProgramData% and its ASCII
        // links in the system temporary folder: both are folders of this test,
        // and a toolkit the developer has configured for the app is not seen.
        qunsetenv("SCE_PROSPERO_PUBTOOLS");
        const QString sysTemp = path(QStringLiteral("sys-temp")), programData = path(QStringLiteral("program-data"));
        QVERIFY(QDir().mkpath(sysTemp) && QDir().mkpath(programData));
        for (const char* name : {"TMP", "TEMP"})
            qputenv(name, QDir::toNativeSeparators(sysTemp).toLocal8Bit());
        qputenv("ProgramData", QDir::toNativeSeparators(programData).toLocal8Bit());
        QCOMPARE(QFileInfo(QDir::tempPath()), QFileInfo(sysTemp));
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
        QVERIFY(resetNaming());

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
        QVERIFY(resetNaming());

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
        const QString base = basePackage();
        QVERIFY(!base.isEmpty());
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

    // A title from param.json that reads as markup is shown as it is, in the
    // strip and in its tooltip, never rendered.
    void titlesFromDumpsAreShownLiterally()
    {
        const QString title = QStringLiteral("<b>Bold</b> <img src=x.png> Game");
        const QString dump = path(QStringLiteral("Dumps/PPSA09999-markup"));
        QVERIFY(makeDump(dump, title, QStringLiteral("PPSA09999"), QStringLiteral("UP0000-PPSA09999_00-MARKUPTEST000000")));
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(dump));
        QCOMPARE(m_page->summary().title, title);
        ui::ElideLabel* strip = nullptr;
        for (auto* l : m_page->findChildren<ui::ElideLabel*>())
            if (l->fullText() == title)
                strip = l;
        QVERIFY(strip);
        QCOMPARE(strip->textFormat(), Qt::PlainText);
        // The page's own message rows (source, base package, free space) are
        // literal too; the ones inside setting rows show the app's own text.
        int checked = 0;
        for (auto* row : m_page->findChildren<ui::MessageRow*>()) {
            bool setting = false;
            for (QWidget* w = row->parentWidget(); w && !setting; w = w->parentWidget())
                setting = qobject_cast<ui::SettingRow*>(w) != nullptr;
            if (setting)
                continue;
            for (auto* l : row->findChildren<QLabel*>())
                if (l->pixmap().isNull()) // the icon is a picture, not text
                    QCOMPARE(l->textFormat(), Qt::PlainText);
            ++checked;
        }
        QVERIFY(checked >= 3);
        // What a tooltip shows for a text: Qt renders it when it looks like markup.
        const auto shown = [](const QString& tip) {
            if (!Qt::mightBeRichText(tip))
                return tip;
            QTextDocument doc;
            doc.setHtml(tip);
            return doc.toPlainText();
        };
        QVERIFY(Qt::mightBeRichText(title));
        QVERIFY(shown(title) != title); // handed over as it is, it would be rendered
        QCOMPARE(shown(ui::plainTip(title)), title);
        const QString folder = QStringLiteral("D:\\Games & more\\a.bin");
        QCOMPARE(shown(ui::plainTip(folder)), folder);
        // A tooltip set without the helper is shown literally all the same
        // (the application-wide tooltip rule).
        strip->setToolTip(title);
        QHelpEvent tip(QEvent::ToolTip, QPoint(2, 2), strip->mapToGlobal(QPoint(2, 2)));
        QVERIFY(QCoreApplication::sendEvent(strip, &tip));
        QCOMPARE(shown(QToolTip::text()), title);
        QToolTip::hideText();
        m_page->setSource({});
    }

    // Ctrl+B and drops act on the form the user sees: while the job monitor
    // is shown they must not start a second job or change the hidden source.
    void monitorViewIgnoresCtrlBAndDrops()
    {
        auto& q = m_ctx->queue();
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        m_page->setOutputFolder(path(QStringLiteral("monitor-out")));
        q.holdQueue(true);
        const QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        const QList<QUrl> drop{QUrl::fromLocalFile(m_asciiGame)};
        QVERIFY(m_page->acceptsDrop(drop));
        m_page->showMonitor(id);
        QVERIFY(m_page->isMonitorShown());
        const QStringList before = ids(q);
        m_page->primaryAction();
        QCOMPARE(ids(q), before);
        QVERIFY(!m_page->acceptsDrop(drop));
        m_page->dropUrls(drop);
        QCOMPARE(clean(m_page->source()), clean(m_game));
        m_page->showForm();
        QVERIFY(m_page->acceptsDrop(drop));
        QVERIFY(q.remove(id));
        q.holdQueue(false);
    }

    // The free-space answer is not cached: a refusal from earlier is checked
    // again when the button is pressed, and its text counts the reserve.
    void staleSpaceVerdictsAreCheckedAgain()
    {
        auto& q = m_ctx->queue();
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        m_page->setOutputFolder(path(QStringLiteral("space-out")));
        const qint64 needed = 48LL << 30, free = 47LL << 30, reserve = 2LL << 30;
        m_page->showSpaceError(needed, free, QStringLiteral("Q:"), reserve);
        QStringList shown;
        for (auto* bar : m_page->findChildren<ui::InfoBar*>())
            if (bar->isVisible())
                for (auto* l : bar->findChildren<QLabel*>())
                    shown << l->text();
        const QString text = shown.join(u'\n');
        QVERIFY2(text.contains(QStringLiteral("plus the %1 free-space reserve").arg(formatBytes(reserve))), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("Free %1 or").arg(formatBytes(needed + reserve - free))), qPrintable(text));
        // Without a reserve the sentence is the short one.
        m_page->showSpaceError(needed, free, QStringLiteral("Q:"));
        shown.clear();
        for (auto* bar : m_page->findChildren<ui::InfoBar*>())
            if (bar->isVisible())
                for (auto* l : bar->findChildren<QLabel*>())
                    shown << l->text();
        QVERIFY(!shown.join(u'\n').contains(QStringLiteral("reserve")));
        QVERIFY(shown.join(u'\n').contains(QStringLiteral("Free %1 or").arg(formatBytes(needed - free))));
        // The drive has room for this small dump: the old refusal must not stand.
        q.holdQueue(true);
        const QString id = addToQueue();
        QVERIFY2(!id.isEmpty(), "a stale \"not enough space\" answer refused the job");
        QVERIFY(q.remove(id));
        q.holdQueue(false);
    }

    // Packed folder: pack volumes stay below 4 GiB, so the page holds no
    // single-file size against the drive; and its hidden "Compress game files"
    // switch stays on whatever the global setting or the panel does.
    void packedFolderKeepsItsSwitchOnAndHasNoFileLimit()
    {
        auto& q = m_ctx->queue();
        auto& s = m_ctx->settings();
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        QVERIFY(m_page->largestOutputFileBytes() >= m_page->summary().bytes);
        m_page->setFormat(pages::kFormatZstdFolder);
        QCOMPARE(m_page->largestOutputFileBytes(), qint64(0));

        ui::JobOptionsPanel* panel = m_page->optionsPanel(pages::kFormatZstdFolder);
        QVERIFY(panel);
        const auto dependentsEnabled = [&] {
            for (const QString& key : {ampr::keys::kZstdLevel, ampr::keys::kPackStrategy, ampr::keys::kVerifyPacks}) {
                ui::SettingRow* row = panel->row(key);
                if (!row || !row->editor()->widget()->isEnabledTo(row))
                    return false;
            }
            return panel->effectiveValue(ampr::keys::kZstdPack).toBool();
        };
        const QVariant before = s.value(ampr::keys::kZstdPack);
        QVERIFY(dependentsEnabled());
        // The global setting on, then off again (the case that used to grey the rows out).
        QVERIFY(s.setValue(ampr::keys::kZstdPack, true).ok());
        QVERIFY(dependentsEnabled());
        m_page->setOutputFolder(path(QStringLiteral("zstd-switch")));
        q.holdQueue(true);
        QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->overrides.value(ampr::keys::kZstdPack).toBool(), true); // sent even when it equals the global value
        QVERIFY(q.remove(id));
        QVERIFY(s.setValue(ampr::keys::kZstdPack, false).ok());
        QVERIFY(dependentsEnabled());
        panel->clearOverrides();
        QVERIFY(dependentsEnabled());
        s.resetSection(QStringLiteral("Build"));
        QVERIFY(dependentsEnabled());
        id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->overrides.value(ampr::keys::kZstdPack).toBool(), true);
        QVERIFY(q.remove(id));
        q.holdQueue(false);
        QVERIFY(s.setValue(ampr::keys::kZstdPack, before).ok());
        QVERIFY(resetNaming());
        m_page->setOutputFolder({});
        m_page->setFormat(jobs::kFormatExfat);
    }

    // A name the user typed survives a visit to the FPKG tile (which borrows
    // the field for its package name) and reading the source again (F5).
    void typedNamesSurviveTheFpkgTileAndARefresh()
    {
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        m_page->setSource({});
        QVERIFY(selectSource(m_game));
        m_page->setFileStem(QStringLiteral("MyGame"));
        QCOMPARE(m_page->fileName(), QStringLiteral("MyGame.exfat"));
        m_page->setFormat(pages::kFormatFpkg);
        QVERIFY(m_page->fileName().endsWith(QStringLiteral(".pkg")));
        m_page->setFormat(jobs::kFormatFfpkg);
        QCOMPARE(m_page->fileName(), QStringLiteral("MyGame.ffpkg"));
        QVERIFY(m_page->fileNameEdited());
        m_page->setFormat(jobs::kFormatExfat);
        m_page->refresh();
        QVERIFY(selectSource(m_game)); // the summary is back
        QTRY_VERIFY(m_page->summary().exists);
        QCOMPARE(m_page->fileName(), QStringLiteral("MyGame.exfat"));
        // Another source gets its own name again.
        QVERIFY(selectSource(m_asciiGame));
        QVERIFY(!m_page->fileNameEdited());
        QVERIFY(m_page->fileName().startsWith(QStringLiteral("PPSA01234")));
    }

    // The exFAT size settings count in what the image needs; the speed note
    // under the Speed row stays after the row is edited.
    void sizeSettingsAndTheSpeedNote()
    {
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        ui::JobOptionsPanel* panel = m_page->optionsPanel(jobs::kFormatExfat);
        const qint64 automatic = m_page->largestOutputFileBytes();
        panel->setOverride(exfat::keys::kImageSizeGiB, 3.0);
        QCOMPARE(m_page->largestOutputFileBytes(), qint64(3) << 30);
        panel->clearOverrides();
        panel->setOverride(exfat::keys::kSpareSpace, QStringLiteral("fixed"));
        panel->setOverride(exfat::keys::kSpareSpaceMiB, 100);
        QCOMPARE(m_page->largestOutputFileBytes(), automatic + (qint64(100) << 20));
        panel->clearOverrides();
        QCOMPARE(m_page->largestOutputFileBytes(), automatic);

        const auto note = [&] {
            auto* row = panel->row(general::keys::kPerformanceMode);
            auto* message = row ? row->findChild<ui::MessageRow*>() : nullptr;
            return message ? message->text() : QString();
        };
        QVERIFY2(note().contains(QStringLiteral("logical cores")), qPrintable(note()));
        const QString mode = m_ctx->settings().value(general::keys::kPerformanceMode).toString();
        panel->setOverride(general::keys::kPerformanceMode, mode == QLatin1String("lowImpact") ? QStringLiteral("balanced") : QStringLiteral("lowImpact"));
        QVERIFY2(note().contains(QStringLiteral("logical cores")), qPrintable(note()));
        QVERIFY(m_ctx->settings().setValue(general::keys::kPerformanceMode, mode == QLatin1String("maximum") ? QStringLiteral("balanced") : QStringLiteral("maximum")).ok());
        QVERIFY2(note().contains(QStringLiteral("logical cores")), qPrintable(note()));
        QVERIFY(m_ctx->settings().setValue(general::keys::kPerformanceMode, mode).ok());
        panel->clearOverrides();
        QVERIFY(note().contains(QStringLiteral("logical cores")));
    }

    // A zipped dump keeps the requested format (only images and .gp5 projects
    // force FPKG); a ZIP that cannot be read shows its file name and reports
    // nothing to the status bar.
    void zipSourcesKeepTheFormat()
    {
        m_page->showForm();
        const QString zip = path(QStringLiteral("not-really.zip"));
        QVERIFY(fwtest::writeTextFile(zip, QByteArray(2048, 'z')));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatZstdFolder}, {QStringLiteral("source"), zip}});
        QCOMPARE(m_page->format(), pages::kFormatZstdFolder);
        QTRY_VERIFY_WITH_TIMEOUT(m_page->summary().exists, 15000);
        QVERIFY(!m_page->summary().isGame);
        QCOMPARE(m_page->statusContext(), QString());
        bool named = false;
        for (auto* l : m_page->findChildren<ui::ElideLabel*>())
            named = named || l->fullText() == QLatin1String("not-really.zip");
        QVERIFY(named);
        // An image still means FPKG.
        const QString image = path(QStringLiteral("PPSA05555 Other Game.exfat"));
        QVERIFY(fwtest::writeTextFile(image, QByteArray(4096, '\0')));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), jobs::kFormatExfat}, {QStringLiteral("source"), image}});
        QCOMPARE(m_page->format(), pages::kFormatFpkg);
        QVERIFY(m_page->statusContext().contains(QStringLiteral("PPSA05555 Other Game")));
        m_page->setSource({});
        m_page->setFormat(jobs::kFormatExfat);
    }

    // FPKG validation follows the engine: a content ID with another suffix
    // than _00 is accepted, an update needs a base package that exists, and a
    // base inspection that ends after the type left Backport changes nothing.
    void fpkgValidationFollowsTheEngine()
    {
        auto& q = m_ctx->queue();
        m_page->showForm();
        const QString dump = path(QStringLiteral("Dumps/PPSA07777-app"));
        QVERIFY(makeDump(dump, QStringLiteral("Suffix Game"), QStringLiteral("PPSA07777"), QStringLiteral("UP0000-PPSA07777_01-SUFFIXTEST000000")));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatFpkg},
                                                     {QStringLiteral("packageType"), QStringLiteral("app")},
                                                     {QStringLiteral("basePackage"), QString()},
                                                     {QStringLiteral("source"), dump}});
        QVERIFY(selectSource(dump));
        m_page->setOutputFolder(path(QStringLiteral("fpkg-suffix")));
        q.holdQueue(true);
        QString id = addToQueue();
        QVERIFY2(!id.isEmpty(), "a content ID the engine accepts was refused by the page");
        QCOMPARE(q.record(id)->parameters.value(QStringLiteral("contentId")).toString(), QStringLiteral("UP0000-PPSA07777_01-SUFFIXTEST000000"));
        QVERIFY(q.remove(id));

        // Update: the base must exist, and its path is sent without the spaces around it.
        const QString base = basePackage();
        QVERIFY(!base.isEmpty());
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("update")},
                                                     {QStringLiteral("basePackage"), path(QStringLiteral("Packages/missing.pkg"))}});
        QVERIFY(addToQueue().isEmpty());
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("basePackage"), QStringLiteral("  ") + base + QStringLiteral(" ")}});
        id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(clean(q.record(id)->parameters.value(QStringLiteral("basePackage")).toString()), clean(base));
        QVERIFY(q.remove(id));
        q.holdQueue(false);

        // Backport starts reading the base; App is chosen before the answer arrives.
        QLineEdit* version = nullptr;
        for (auto* e : m_page->packageCard()->findChildren<QLineEdit*>())
            if (e->accessibleName() == QLatin1String("Version"))
                version = e;
        QVERIFY(version);
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("app")}, {QStringLiteral("basePackage"), QString()}});
        QCOMPARE(version->text(), QStringLiteral("01.000.000"));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("backport")}, {QStringLiteral("basePackage"), base}});
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("app")}});
        QTest::qWait(1500);
        QCOMPARE(version->text(), QStringLiteral("01.000.000"));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("basePackage"), QString()}});
        m_page->setOutputFolder({});
        m_page->setSource({});
        m_page->setFormat(jobs::kFormatExfat);
    }

    // "Open the output folder when done" is this page's own switch: every
    // submit reports it once, with its value.
    void openFolderSwitchIsReportedOnce()
    {
        auto& q = m_ctx->queue();
        m_page->showForm();
        m_page->setFormat(jobs::kFormatExfat);
        QVERIFY(selectSource(m_game));
        m_page->setOutputFolder(path(QStringLiteral("open-when-done")));
        ui::Switch* open = nullptr;
        for (auto* s : m_page->findChildren<ui::Switch*>())
            if (s->accessibleName() == QLatin1String("Open the output folder when done"))
                open = s;
        QVERIFY(open);
        QSignalSpy spy(m_page, &pages::JobFormPage::jobSubmitted);
        q.holdQueue(true);
        open->setChecked(true);
        QVERIFY(m_page->openOutputWhenDone());
        QString id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), id);
        QCOMPARE(spy.at(0).at(1).toBool(), true);
        QVERIFY(q.remove(id));
        open->setChecked(false);
        spy.clear();
        id = addToQueue();
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.at(0).at(1).toBool(), false);
        QVERIFY(q.remove(id));
        q.holdQueue(false);
        m_page->setOutputFolder({});
    }

    // FPKG: a source without its own content ID and version (an image) must
    // not be packaged with the previous game's.
    void packageFieldsFollowTheSource()
    {
        m_page->showForm();
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("format"), pages::kFormatFpkg},
                                                     {QStringLiteral("packageType"), QStringLiteral("app")}});
        QVERIFY(selectSource(m_asciiGame));
        QLineEdit *contentId = nullptr, *version = nullptr, *title = nullptr;
        for (auto* e : m_page->packageCard()->findChildren<QLineEdit*>()) {
            if (e->accessibleName() == QLatin1String("Content ID"))
                contentId = e;
            else if (e->accessibleName() == QLatin1String("Version"))
                version = e;
            else if (e->accessibleName() == QLatin1String("Title"))
                title = e;
        }
        QVERIFY(contentId && version && title);
        QCOMPARE(contentId->text(), QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000"));
        QCOMPARE(version->text(), QStringLiteral("01.000.000"));
        QCOMPARE(title->text(), QStringLiteral("Test Game"));
        // The content ID of an app or update is the game's own: shown, not
        // edited. An add-on has an ID of its own to enter.
        QVERIFY(contentId->isReadOnly());
        QVERIFY2(contentId->toolTip().contains(QStringLiteral("param.json")), qPrintable(contentId->toolTip()));
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("update")}});
        QVERIFY(contentId->isReadOnly());
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("addon")}});
        QVERIFY(!contentId->isReadOnly());
        m_window->navigate(QStringLiteral("build"), {{QStringLiteral("packageType"), QStringLiteral("app")}});
        QVERIFY(contentId->isReadOnly());
        // An image has no param.json the page reads: nothing of the game before it may stay.
        const QString image = path(QStringLiteral("PPSA05555 Other Game.exfat"));
        QVERIFY(fwtest::writeTextFile(image, QByteArray(4096, '\0')));
        m_page->setSource(image);
        QTRY_COMPARE(clean(m_page->summary().folder), clean(image));
        QCOMPARE(contentId->text(), QString());
        QCOMPARE(version->text(), QString());
        QCOMPARE(title->text(), QString()); // the file's name is not a title
        QCOMPARE(m_page->fileName(), QString()); // no package name from the other game's ID
        // Back to a game: its details again; no source: none.
        QVERIFY(selectSource(m_asciiGame));
        QCOMPARE(contentId->text(), QStringLiteral("UP0000-PPSA01234_00-GOLDENTEST000000"));
        m_page->setSource({});
        QCOMPARE(contentId->text(), QString());
        QCOMPARE(version->text(), QString());
        QCOMPARE(title->text(), QString());
        m_page->setFormat(jobs::kFormatExfat);
    }

    // The five format tiles: one row where there is room, three and two below
    // that, then pairs, then a single column; and a tile is where a click on
    // its place lands. Pure geometry, so it holds on the offscreen platform.
    void formatTilesWrapByWidth()
    {
        QWidget* tiles = nullptr;
        for (auto* w : m_page->findChildren<QWidget*>())
            if (w->accessibleName() == QLatin1String("Output format"))
                tiles = w;
        QVERIFY(tiles);
        QVERIFY(tiles->hasHeightForWidth());
        const QStringList titles{QStringLiteral("exFAT"), QStringLiteral("FFPKG"), QStringLiteral("PFS"), QStringLiteral("FPKG"), QStringLiteral("Packed folder")};

        // Height and gap of a tile, from the widget's own answers for one and two rows.
        const int tileH = tiles->heightForWidth(4000);
        const int gap = tiles->heightForWidth(797) - 2 * tileH;
        QVERIFY2(tileH > 0 && gap > 0 && gap < tileH, qPrintable(QStringLiteral("%1 / %2").arg(tileH).arg(gap)));
        const auto rows = [&](int width) {
            const int h = tiles->heightForWidth(width);
            return (h + gap) % (tileH + gap) == 0 ? (h + gap) / (tileH + gap) : -1;
        };
        QCOMPARE(rows(1440), 1); // 5
        QCOMPARE(rows(798), 1);
        QCOMPARE(rows(797), 2); // 3 + 2
        QCOMPARE(rows(534), 2);
        QCOMPARE(rows(533), 3); // 2 + 2 + 1
        QCOMPARE(rows(392), 3);
        QCOMPARE(rows(391), 5); // one column
        QCOMPARE(rows(200), 5);

        // Each tile answers a click in the middle of its place, at every arrangement.
        const QSize laidOut = tiles->size();
        const auto restore = qScopeGuard([&] {
            tiles->resize(laidOut);
            m_page->setFormat(jobs::kFormatExfat);
        });
        const QList<QPair<int, int>> arrangements{{1000, 5}, {700, 3}, {450, 2}, {300, 1}}; // width, tiles per row
        for (const auto& [width, perRow] : arrangements) {
            QCOMPARE(rows(width), (5 + perRow - 1) / perRow);
            const int tileW = (width - (perRow - 1) * gap) / perRow;
            for (int i = 0; i < titles.size(); ++i) {
                tiles->resize(width, tiles->heightForWidth(width)); // the page's layout may have had its say meanwhile
                const QPoint middle((i % perRow) * (tileW + gap) + tileW / 2, (i / perRow) * (tileH + gap) + tileH / 2);
                QVERIFY2(tiles->rect().contains(middle), qPrintable(QStringLiteral("%1 px, tile %2").arg(width).arg(i)));
                QTest::mouseClick(tiles, Qt::LeftButton, Qt::NoModifier, middle);
                QVERIFY2(tiles->accessibleDescription() == QStringLiteral("Selected: ") + titles.at(i),
                         qPrintable(QStringLiteral("%1 px, tile %2: %3").arg(width).arg(i).arg(tiles->accessibleDescription())));
            }
        }
    }

    // Library › right-click a game › "Pack with zstd…": the Build page opens
    // with Packed folder chosen and that dump as its source, ready to queue
    // the pack job.
    void libraryPackWithZstdOpensTheBuildPage()
    {
        auto& q = m_ctx->queue();
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(lib);
        ui::LibraryGrid* grid = lib->gamesView();
        QVERIFY(grid);
        m_page->setFormat(jobs::kFormatExfat);
        m_page->setSource({});
        m_window->navigate(QStringLiteral("library"));
        QCOMPARE(m_window->currentPage(), QStringLiteral("library"));

        // The menu runs its own event loop: a timer inside it picks the entry.
        bool picked = false;
        QTimer pick;
        pick.setInterval(30);
        connect(&pick, &QTimer::timeout, this, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu)
                return;
            pick.stop();
            for (QAction* a : menu->actions())
                if (a->text() == QStringLiteral("Pack with zstd…")) {
                    picked = true;
                    a->trigger();
                }
            menu->close();
        });
        pick.start();
        emit grid->contextRequested(m_game, QPoint(40, 40));
        pick.stop();
        QVERIFY2(picked, "the game's menu has no \"Pack with zstd…\"");

        QCOMPARE(m_window->currentPage(), QStringLiteral("build"));
        QWidget* tiles = nullptr;
        for (auto* w : m_page->findChildren<QWidget*>())
            if (w->accessibleName() == QLatin1String("Output format"))
                tiles = w;
        QVERIFY(tiles);
        QCOMPARE(tiles->accessibleDescription(), QStringLiteral("Selected: Packed folder"));
        QVERIFY(QTest::qWaitFor([&] { return m_page->summary().isGame && clean(m_page->summary().folder) == clean(m_game); }, 15000));
        m_page->setOutputFolder(path(QStringLiteral("from-library-zstd")));
        q.holdQueue(true);
        const QString id = addToQueue();
        QVERIFY2(!id.isEmpty(), "the Build page did not queue the pack job");
        QCOMPARE(q.record(id)->kind, ampr::kPackJob);
        QCOMPARE(clean(q.record(id)->parameters.value(QStringLiteral("source")).toString()), clean(m_game));
        QCOMPARE(q.record(id)->overrides.value(ampr::keys::kZstdPack).toBool(), true);
        QVERIFY(q.remove(id));
        q.holdQueue(false);
        m_page->setOutputFolder({});
        m_page->setSource({});
        m_page->setFormat(jobs::kFormatExfat);
    }
};

QTEST_MAIN(TstBuildPage)
#include "tst_build_page.moc"
