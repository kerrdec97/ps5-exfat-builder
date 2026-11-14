// The app's backend object and its small stores (no window): a read-only
// start next to the running app, where the window state is kept, the queue
// model's row changes, and store files that cannot be read.

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/AppStores.h"
#include "app/services/LauncherIdentity.h"
#include "connect/ps5/Module.h"
#include "core/Paths.h"
#include "engines/gameinfo/Module.h"
#include "engines/general/Module.h"
#include "support/ClosedPort.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

// A job that validates and stays queued while the queue is held. Its port is
// one that refuses, should the job ever get to run.
Result<QString> queueDiagnosis(AppContext& ctx)
{
    return ctx.queue().enqueue(ps5::jobkinds::kFtpDiagnose,
                               QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")}, {QStringLiteral("port"), int(fwtest::refusedPort())},
                                           {QStringLiteral("speedTest"), false}});
}

} // namespace

class TstAppShellContext : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
    }

    // --self-test next to the open app: the queue file is neither restored nor rewritten.
    void readOnlyStartLeavesTheQueueAlone()
    {
        QByteArray saved;
        {
            AppContext ctx;
            QVERIFY(ctx.initialize().ok());
            (void)ctx.settings().setValue(general::keys::kRestoreQueue, true);
            ctx.queue().holdQueue(true);
            const auto id = queueDiagnosis(ctx);
            QVERIFY2(id.ok(), qPrintable(id.ok() ? QString() : id.error().message));
            QVERIFY(ctx.queue().saveQueue().ok());
            saved = readAll(Paths::queueFile());
            QVERIFY(saved.contains("ps5.ftp.diagnose"));
            ctx.queue().remove(id.value());
        }
        QVERIFY(writeBytes(Paths::queueFile(), saved)); // as the running app has it on disk
        {
            AppContext probe;
            QVERIFY(probe.initialize(AppContext::Mode::ReadOnly).ok());
            QVERIFY(probe.queue().records().empty());
            QVERIFY(!probe.jobFactory().kinds().isEmpty()); // what the self-test reports is there
        }
        QCOMPARE(readAll(Paths::queueFile()), saved);
        {
            AppContext ctx; // a normal start does restore it
            QVERIFY(ctx.initialize().ok());
            QCOMPARE(int(ctx.queue().records().size()), 1);
            QVERIFY(ctx.queue().isHeld());
            ctx.queue().remove(ctx.queue().records().front().id);
            (void)ctx.settings().setValue(general::keys::kRestoreQueue, false);
        }
    }

    // A run with its own data folder keeps the window state there, not in the user's registry.
    void windowStateFollowsTheDataFolder()
    {
        const QByteArray before = qgetenv("FW_DATA_DIR");
        qunsetenv("FW_DATA_DIR");
        if (!Paths::isPortable())
            QVERIFY(!AppContext::usePortableWindowState()); // an installed copy keeps its place
        // Whatever happens below, the rest of the program is back on the
        // test's own INI files and the variable is as it was.
        const auto restore = qScopeGuard([this, before] {
            if (before.isEmpty())
                qunsetenv("FW_DATA_DIR");
            else
                qputenv("FW_DATA_DIR", before);
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        });
        qputenv("FW_DATA_DIR", m_root.path().toUtf8());
        // The switch itself must choose the INI file: the test's own choice
        // from initTestCase is taken away first.
        QSettings::setDefaultFormat(QSettings::NativeFormat);
        QVERIFY(AppContext::usePortableWindowState());
        QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
        // Asked with the INI format spelled out: with the app's real names a
        // native store would be the user's registry.
        const QString file = QDir::fromNativeSeparators(
            QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kerrdec97"), QStringLiteral("exFAT Image Builder Farewell")).fileName());
        QVERIFY2(file.startsWith(QDir::fromNativeSeparators(Paths::dataDir()), Qt::CaseInsensitive), qPrintable(file));
    }

    // Jobs come and go as rows; only a changed order resets the model.
    void queueModelChangesRowByRow()
    {
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        ctx.queue().holdQueue(true);
        JobQueueModel model(ctx.queue());
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removes(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy counts(&model, &JobQueueModel::countsChanged);
        QStringList ids;
        for (int i = 0; i < 3; ++i) {
            const auto id = queueDiagnosis(ctx);
            QVERIFY(id.ok());
            ids << id.value();
        }
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(inserts.size(), 3);
        QCOMPARE(resets.size(), 0);
        QVERIFY(counts.size() >= 3);
        QCOMPARE(model.idAt(0), ids[0]);
        QCOMPARE(model.idAt(2), ids[2]);
        QVERIFY(ctx.queue().move(ids[2], 0));
        QCOMPARE(resets.size(), 1);
        QCOMPARE(model.idAt(0), ids[2]);
        (void)ctx.queue().move(ids[2], 0); // the same place: nothing to show
        QCOMPARE(resets.size(), 1);
        QVERIFY(ctx.queue().remove(ids[0]));
        QCOMPARE(removes.size(), 1);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.rowOf(ids[0]), -1);
        QCOMPARE(resets.size(), 1);
        for (const QString& id : {ids[1], ids[2]})
            ctx.queue().remove(id);
        QCOMPARE(model.rowCount(), 0);
    }

    // A store file that cannot be parsed is kept as .bad instead of being overwritten by the next save.
    void unreadableStoresAreKept()
    {
        const QString payloads = QDir(Paths::dataDir()).filePath(QStringLiteral("payloads.json"));
        const QByteArray broken = "{\"payloads\": [ {\"name\": \"elfldr\", ";
        QVERIFY(writeBytes(payloads, broken));
        ui::PayloadStore store;
        QVERIFY(store.entries().empty());
        QCOMPARE(readAll(payloads + QStringLiteral(".bad")), broken);
        store.add({QStringLiteral("kstuff"), QStringLiteral("C:/x/kstuff.elf"), {}, {}});
        QVERIFY(readAll(payloads).contains("kstuff"));
        QCOMPARE(readAll(payloads + QStringLiteral(".bad")), broken);

        // markSent for a payload that is not in the list changes nothing.
        QSignalSpy changed(&store, &ui::PayloadStore::changed);
        store.markSent(QStringLiteral("C:/x/other.elf"));
        QCOMPARE(changed.size(), 0);
        store.markSent(QStringLiteral("C:/x/kstuff.elf"));
        QCOMPARE(changed.size(), 1);
        QVERIFY(store.entries().front().lastSent.isValid());
        ui::PayloadStore again; // what was saved loads again
        QCOMPARE(int(again.entries().size()), 1);
        QVERIFY(again.entries().front().lastSent.isValid());

        // Damaged again, differently: the first copy stays, the second gets a
        // name of its own, and the same damage read twice is kept once.
        const QByteArray brokenAgain = "{\"payloads\": [ {\"name\": \"kstuff\" ";
        QVERIFY(writeBytes(payloads, brokenAgain));
        for (int i = 0; i < 2; ++i) {
            ui::PayloadStore damaged;
            QVERIFY(damaged.entries().empty());
        }
        const QString second = QDir(Paths::dataDir()).filePath(QStringLiteral("payloads.json (2).bad"));
        QCOMPARE(readAll(payloads + QStringLiteral(".bad")), broken);
        QCOMPARE(readAll(second), brokenAgain);
        QVERIFY(!QFileInfo::exists(QDir(Paths::dataDir()).filePath(QStringLiteral("payloads.json (3).bad"))));
    }

    // The window icon is the brand mark in colour, in the sizes the shell asks for.
    void appIconIsTheBrandMark()
    {
        const QIcon icon = launcher::appIcon();
        QVERIFY(!icon.isNull());
        const QList<QSize> sizes = icon.availableSizes();
        for (const int px : {16, 24, 32, 48, 64, 256})
            QVERIFY2(sizes.contains(QSize(px, px)), qPrintable(QString::number(px)));
        const QImage big = icon.pixmap(256, 256).toImage();
        QCOMPARE(big.size(), QSize(256, 256));
        QCOMPARE(qAlpha(big.pixel(0, 0)), 0); // rounded corners
        const QColor tile = big.pixelColor(40, 128);
        QVERIFY2(tile.alpha() == 255 && tile.blue() > 200 && tile.blue() > tile.red(), qPrintable(tile.name()));
    }

    // One scan folder is one entry, however it is written.
    void scanFoldersAreComparedAsPaths()
    {
        QFile::remove(QDir(Paths::dataDir()).filePath(QStringLiteral("library.json")));
        ui::LibraryStore store;
        store.addScanFolder(QStringLiteral("D:/Games"));
        store.addScanFolder(QStringLiteral("d:\\games\\"));
        store.addScanFolder(QStringLiteral("D:/Games/sub/.."));
        QCOMPARE(store.scanFolders(), QStringList{QStringLiteral("D:/Games")});
        store.setScanFolders({QStringLiteral("E:\\Dumps"), QStringLiteral("e:/dumps"), QStringLiteral("D:/Games")});
        QCOMPARE(store.scanFolders().size(), 2);
        store.removeScanFolder(QStringLiteral("e:/DUMPS/"));
        QCOMPARE(store.scanFolders(), QStringList{QStringLiteral("D:/Games")});
    }

    // The Library page's folders are the "Library folders" setting: folders
    // imported from version 4 or typed in Settings are scanned, and a folder
    // added on the page is in Settings.
    void libraryFoldersAreTheSetting()
    {
        const QString libraryJson = QDir(Paths::dataDir()).filePath(QStringLiteral("library.json"));
        QFile::remove(libraryJson);
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        SettingsStore& s = ctx.settings();
        const QString key = gameinfo::keys::kLibraryFolders;
        s.reset(key);
        const auto restore = qScopeGuard([&s, key] { s.reset(key); });

        // An import of version 4.0.2's folders while the page's store exists.
        ui::LibraryStore store; // bound to the running app's settings
        QVERIFY(store.scanFolders().isEmpty());
        QSignalSpy changed(&store, &ui::LibraryStore::changed);
        const QString legacy = m_root.filePath(QStringLiteral("v4-folders.json"));
        QVERIFY(writeBytes(legacy, R"({"lib_folders": ["D:\\PS5\\Games", "E:\\Jeux vid\u00e9o"], "language": "en"})"));
        QVERIFY(s.importLegacy(legacy).ok());
        const QString accented = QString::fromUtf8("E:/Jeux vid\xc3\xa9o");
        QCOMPARE(store.scanFolders(), (QStringList{QStringLiteral("D:/PS5/Games"), accented}));
        QVERIFY(changed.count() >= 1);
        QVERIFY(readAll(libraryJson).contains("D:/PS5/Games"));

        // A folder added on the page is in Settings (as the Settings page writes folders).
        store.addScanFolder(QStringLiteral("F:/More"));
        QCOMPARE(s.value(key).toStringList(),
                 (QStringList{QStringLiteral("D:\\PS5\\Games"), QDir::toNativeSeparators(accented), QStringLiteral("F:\\More")}));
        // One removed in Settings leaves the page.
        QVERIFY(s.setValue(key, QStringList{QStringLiteral("F:\\More")}).ok());
        QCOMPARE(store.scanFolders(), QStringList{QStringLiteral("F:/More")});
        store.removeScanFolder(QStringLiteral("f:\\more\\"));
        QVERIFY(s.value(key).toStringList().isEmpty());

        // 5.0.2 kept them apart: at the next start the two lists are joined,
        // the page's folders first, nothing twice. (The setting first: the
        // store above follows it and saves library.json.)
        QVERIFY(s.setValue(key, QStringList{QStringLiteral("d:\\games"), QStringLiteral("H:\\Typed")}).ok());
        QVERIFY(writeBytes(libraryJson, R"({"folders": ["G:/Old", "D:/Games"], "games": []})"));
        ui::LibraryStore later(&s);
        QCOMPARE(later.scanFolders(), (QStringList{QStringLiteral("G:/Old"), QStringLiteral("D:/Games"), QStringLiteral("H:/Typed")}));
        QCOMPARE(s.value(key).toStringList(),
                 (QStringList{QStringLiteral("G:\\Old"), QStringLiteral("D:\\Games"), QStringLiteral("H:\\Typed")}));
        QCOMPARE(store.scanFolders(), later.scanFolders()); // the other store follows the setting too

        // Without settings the store is what it was: library.json only.
        ui::LibraryStore alone(static_cast<SettingsStore*>(nullptr));
        alone.addScanFolder(QStringLiteral("J:/Solo"));
        QCOMPARE(s.value(key).toStringList().size(), 3);
    }

    // A version 4 import while the page already has folders adds its folders
    // to them, as the next start does; one it shares is not listed twice.
    void legacyImportJoinsTheLibraryFolders()
    {
        const QString libraryJson = QDir(Paths::dataDir()).filePath(QStringLiteral("library.json"));
        QFile::remove(libraryJson);
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        SettingsStore& s = ctx.settings();
        const QString key = gameinfo::keys::kLibraryFolders;
        s.reset(key);
        const auto restore = qScopeGuard([&s, key] { s.reset(key); });
        ui::LibraryStore store(&s);
        store.addScanFolder(QStringLiteral("G:/Games"));
        const QString legacy = m_root.filePath(QStringLiteral("v4-join.json"));
        QVERIFY(writeBytes(legacy, R"({"lib_folders": ["D:\\PS5\\Games", "g:\\games\\"]})"));
        QVERIFY(s.importLegacy(legacy).ok());
        QCOMPARE(store.scanFolders(), (QStringList{QStringLiteral("G:/Games"), QStringLiteral("D:/PS5/Games")}));
        QCOMPARE(s.value(key).toStringList(), (QStringList{QStringLiteral("G:\\Games"), QStringLiteral("D:\\PS5\\Games")}));
        QVERIFY(readAll(libraryJson).contains("G:/Games"));
        QVERIFY(s.importLegacy(legacy).ok()); // the same file again changes nothing
        QCOMPARE(store.scanFolders().size(), 2);
    }

    // The Library page's folders are the user's own list, not a preference:
    // Reset section on General and Reset all leave them (and library.json),
    // while the other settings there go back to their defaults.
    void libraryFoldersSurviveAReset()
    {
        const QString libraryJson = QDir(Paths::dataDir()).filePath(QStringLiteral("library.json"));
        QFile::remove(libraryJson);
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        SettingsStore& s = ctx.settings();
        const QString key = gameinfo::keys::kLibraryFolders;
        const QString preset = gameinfo::keys::kRenamePreset;
        s.reset(key);
        const auto restore = qScopeGuard([&s, key] { s.reset(key); });
        ui::LibraryStore store(&s);
        store.addScanFolder(QStringLiteral("D:/PS5/Games"));
        store.addScanFolder(QStringLiteral("E:/Dumps"));
        const QStringList folders{QStringLiteral("D:/PS5/Games"), QStringLiteral("E:/Dumps")};
        QSignalSpy changed(&store, &ui::LibraryStore::changed);

        QVERIFY(s.setValue(preset, QStringLiteral("ppsa")).ok());
        s.resetSection(QStringLiteral("General"));
        QVERIFY(s.isDefault(preset));
        QCOMPARE(store.scanFolders(), folders);
        QVERIFY(!s.isDefault(key));
        QVERIFY(readAll(libraryJson).contains("E:/Dumps"));

        QVERIFY(s.setValue(preset, QStringLiteral("ppsa")).ok());
        s.resetAll();
        QVERIFY(s.isDefault(preset));
        QCOMPARE(store.scanFolders(), folders);
        QCOMPARE(s.value(key).toStringList(), (QStringList{QStringLiteral("D:\\PS5\\Games"), QStringLiteral("E:\\Dumps")}));
        QVERIFY(readAll(libraryJson).contains("E:/Dumps"));
        QCOMPARE(changed.count(), 0);

        // The setting asked for by name still goes back, and the page follows.
        s.reset(key);
        QVERIFY(store.scanFolders().isEmpty());
    }
};

QTEST_MAIN(TstAppShellContext)
#include "tst_app_shell_context.moc"
