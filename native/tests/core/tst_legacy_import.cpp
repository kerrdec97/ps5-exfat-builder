// Every key of the legacy ~/.exfat_builder_settings.json (LEGACY_INVENTORY §4.2)
// is either imported into a native setting or listed here as deliberately not
// imported, with the reason. A new legacy key in the fixture without a decision
// fails the test.

#include "core/Settings.h"
#include "support/TestRegistry.h"
#include "support/TestSupport.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

// Legacy keys handled by whole-file importers (not per-key mappings).
const QStringList kImporterKeys = {QStringLiteral("bp_folders"), QStringLiteral("bp_scan_folder")};

// Deliberately not imported — key → reason.
const QMap<QString, QString>& notImported()
{
    static const QMap<QString, QString> m = {
        {QStringLiteral("osfmount_path"), QStringLiteral("OSFMount is no longer used (native image engines)")},
        {QStringLiteral("adv_robo_log"), QStringLiteral("robocopy is no longer used")},
        {QStringLiteral("adv_robo_log_dir"), QStringLiteral("robocopy is no longer used")},
        {QStringLiteral("adv_write_benchmark"), QStringLiteral("dead key in the legacy app")},
        {QStringLiteral("pipeline_to_pfs"), QStringLiteral("legacy pipeline mode replaced by image.convert / PFS jobs")},
        {QStringLiteral("pfs_backend"), QStringLiteral("one native PFS engine (no Lazy vs stock mkpfs choice)")},
        {QStringLiteral("experimental_mkpfs_folder"), QStringLiteral("no external mkpfs any more")},
        {QStringLiteral("experimental_mkpfs_enabled"), QStringLiteral("dead key written by a legacy migration")},
        {QStringLiteral("pfs_lazy_zlib_backend"), QStringLiteral("zlib only (ISA-L/zlib-ng not ported)")},
        {QStringLiteral("pfs_intermediate_via"), QStringLiteral("folder→.ffpfsc builds the nested image in memory, no intermediate")},
        {QStringLiteral("ffpkg_output_dir"), QStringLiteral("one output folder setting (storage.defaultOutputFolder)")},
        {QStringLiteral("ffpkg_extract_outdir"), QStringLiteral("extraction asks for its destination per job")},
        {QStringLiteral("lowercase_names"), QStringLiteral("orphan key in the legacy app")},
        {QStringLiteral("last_tab"), QStringLiteral("UI state (kept in QSettings by the new UI)")},
        {QStringLiteral("sidebar_collapsed"), QStringLiteral("UI state")},
        {QStringLiteral("window_geometry"), QStringLiteral("UI state")},
        {QStringLiteral("settings_active_rail"), QStringLiteral("UI state")},
        {QStringLiteral("adv_active_subtab"), QStringLiteral("UI state")},
        {QStringLiteral("ffpkg_active_subtab"), QStringLiteral("UI state")},
        {QStringLiteral("y2jb_active_subtab"), QStringLiteral("UI state")},
        {QStringLiteral("language"), QStringLiteral("translations not shipped yet (English only)")},
        {QStringLiteral("theme"), QStringLiteral("single Graphite theme")},
        {QStringLiteral("notify_sound"), QStringLiteral("covered by general.notifyOnFinish (notification + sound)")},
        {QStringLiteral("recent_folders"), QStringLiteral("UI history")},
        {QStringLiteral("release_last_seen_version"), QStringLiteral("legacy release-notes popup")},
        {QStringLiteral("support_op_count"), QStringLiteral("legacy support nudges")},
        {QStringLiteral("support_prompt_last_ts"), QStringLiteral("legacy support nudges")},
        {QStringLiteral("lib_auto_scan"), QStringLiteral("orphan in the legacy app; the Library scans on request")},
        {QStringLiteral("img_scan_dirs"), QStringLiteral("built-image library not ported")},
        {QStringLiteral("dumps_last_src"), QStringLiteral("UI state")},
        {QStringLiteral("adv_lang_mode"), QStringLiteral("orphan (never written by the legacy app)")},
        {QStringLiteral("adv_lang_folder"), QStringLiteral("UI state of the Language Stripper")},
        {QStringLiteral("shutdown_on_unibuild"), QStringLiteral("per-job-type triggers collapsed into one 'when the queue finishes' action")},
        {QStringLiteral("shutdown_on_exfat"), QStringLiteral("see shutdown_on_unibuild")},
        {QStringLiteral("shutdown_on_ffpkg"), QStringLiteral("see shutdown_on_unibuild")},
        {QStringLiteral("shutdown_on_pfs"), QStringLiteral("see shutdown_on_unibuild")},
        {QStringLiteral("shutdown_on_backport"), QStringLiteral("see shutdown_on_unibuild")},
        {QStringLiteral("shutdown_on_ftp"), QStringLiteral("see shutdown_on_unibuild")},
        {QStringLiteral("ftp_auto"), QStringLiteral("legacy FTP-tab auto-upload; post-build upload is adv_auto_upload")},
        {QStringLiteral("auto_ip"), QStringLiteral("dead key (read, never written); the sweep is PS5 connect › Find my PS5, run on request")},
        {QStringLiteral("ftp_local_path"), QStringLiteral("UI state of the FTP browser")},
        {QStringLiteral("klog_ip"), QStringLiteral("one console address (ps5.consoleIp)")},
        {QStringLiteral("pl_ip"), QStringLiteral("one console address (ps5.consoleIp)")},
        {QStringLiteral("payloads"), QStringLiteral("payload list lives in the UI's payloads.json store")},
        {QStringLiteral("shadowmount_config"), QStringLiteral("edited on the console (config.ini over FTP), not cached locally")},
        {QStringLiteral("micromount_config"), QStringLiteral("edited on the console (config.ini over FTP), not cached locally")},
        {QStringLiteral("smp_payload_path"), QStringLiteral("chosen per send")},
        {QStringLiteral("mm_payload_path"), QStringLiteral("chosen per send")},
        {QStringLiteral("y2jb_ftp_port"), QStringLiteral("one FTP port (ps5.ftpPort)")},
        {QStringLiteral("y2jb_pkg_usa"), QStringLiteral("chosen per install")},
        {QStringLiteral("y2jb_pkg_eu"), QStringLiteral("chosen per install")},
        {QStringLiteral("y2jb_pkg_jp"), QStringLiteral("chosen per install")},
        {QStringLiteral("abp_output"), QStringLiteral("orphan in the legacy app")},
        {QStringLiteral("bpr_log"), QStringLiteral("legacy results log; history is in the job history")},
    };
    return m;
}

} // namespace

class TstLegacyImport : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase() { QVERIFY(m_root.isValid()); fwtest::isolateAppData(m_root); }

    void everyLegacyKeyHasADecision()
    {
        fwtest::Registry reg;
        QFile f(fwtest::dataDir() + QStringLiteral("/legacy/exfat_builder_settings.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject legacy = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(legacy.size() >= 100);
        QStringList undecided, both;
        for (const QString& k : legacy.keys()) {
            const bool mapped = reg.schema->legacyKeys().count(k) > 0 || kImporterKeys.contains(k);
            const bool skipped = notImported().contains(k);
            if (!mapped && !skipped)
                undecided << k;
            if (mapped && skipped)
                both << k;
        }
        QVERIFY2(undecided.isEmpty(), qPrintable(QStringLiteral("no decision for: ") + undecided.join(QStringLiteral(", "))));
        QVERIFY2(both.isEmpty(), qPrintable(QStringLiteral("mapped and listed as not imported: ") + both.join(QStringLiteral(", "))));
    }

    void importAppliesValues()
    {
        fwtest::Registry reg;
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("s/settings.json")));
        QVERIFY(store.load().ok());
        auto n = store.importLegacy(fwtest::dataDir() + QStringLiteral("/legacy/exfat_builder_settings.json"));
        QVERIFY2(n.ok(), n.ok() ? "" : qPrintable(n.error().toLogString()));
        QVERIFY2(n.value() >= 25, qPrintable(QString::number(n.value())));
        QCOMPARE(store.value(QStringLiteral("general.afterQueueAction")).toString(), QStringLiteral("sleep"));
        QCOMPARE(store.value(QStringLiteral("general.afterQueueDelaySeconds")).toInt(), 120);
        QCOMPARE(store.value(QStringLiteral("general.autoRetryCount")).toInt(), 2);
        QCOMPARE(store.value(QStringLiteral("general.checkForUpdatesOnStart")).toBool(), false);
        QCOMPARE(store.value(QStringLiteral("performance.workerThreads")).toInt(), 8);
        QCOMPARE(store.value(QStringLiteral("ps5.consoleIp")).toString(), QStringLiteral("192.168.1.50"));
        QCOMPARE(store.value(QStringLiteral("ps5.autoUploadAfterBuild")).toBool(), true);
        QCOMPARE(store.value(QStringLiteral("backport.notes")).toString(), QStringLiteral("my notes"));
        // Build tab naming (name_preset / name_custom_template): the Build page
        // proposes the name from these and the name stays editable per job.
        QCOMPARE(store.value(QStringLiteral("build.namePreset")).toString(), QStringLiteral("ppsa_title_ver"));
        QCOMPARE(store.value(QStringLiteral("build.nameTemplate")).toString(), QStringLiteral("%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)"));
    }

    void namingPresetsImport()
    {
        fwtest::Registry reg;
        SettingsStore store(reg.schema, m_root.filePath(QStringLiteral("n/settings.json")));
        QVERIFY(store.load().ok());
        const QString legacy = m_root.filePath(QStringLiteral("naming.json"));
        QVERIFY(fwtest::writeTextFile(legacy, R"({"name_preset": "ppsa_title_ver_fw", "name_custom_template": "%PPSA% - %TITLE% [%FIRMWARE%]"})"));
        auto n = store.importLegacy(legacy);
        QVERIFY2(n.ok(), n.ok() ? "" : qPrintable(n.error().toLogString()));
        QCOMPARE(n.value(), 2);
        QCOMPARE(store.value(QStringLiteral("build.namePreset")).toString(), QStringLiteral("ppsa_title_ver_fw"));
        QCOMPARE(store.value(QStringLiteral("build.nameTemplate")).toString(), QStringLiteral("%PPSA% - %TITLE% [%FIRMWARE%]"));
        // An unknown preset is not imported (the default stays).
        QVERIFY(fwtest::writeTextFile(legacy, R"({"name_preset": "something_else"})"));
        QVERIFY(store.importLegacy(legacy).ok());
        QCOMPARE(store.value(QStringLiteral("build.namePreset")).toString(), QStringLiteral("ppsa_title_ver_fw"));
    }
};

QTEST_GUILESS_MAIN(TstLegacyImport)
#include "tst_legacy_import.moc"
