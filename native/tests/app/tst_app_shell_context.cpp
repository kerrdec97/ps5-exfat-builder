// The app's backend object and its small stores (no window): a read-only
// start next to the running app, where the window state is kept, the queue
// model's row changes, and store files that cannot be read.

#include "app/AppContext.h"
#include "app/models/JobQueueModel.h"
#include "app/services/AppStores.h"
#include "app/services/LauncherIdentity.h"
#include "connect/ps5/Module.h"
#include "core/Paths.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QJsonObject>
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

// A job that validates and stays queued while the queue is held.
Result<QString> queueDiagnosis(AppContext& ctx)
{
    return ctx.queue().enqueue(ps5::jobkinds::kFtpDiagnose,
                               QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")}, {QStringLiteral("port"), 1}, {QStringLiteral("speedTest"), false}});
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
        qputenv("FW_DATA_DIR", m_root.path().toUtf8());
        QVERIFY(AppContext::usePortableWindowState());
        QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
        const QString file = QDir::fromNativeSeparators(
            QSettings(QSettings::defaultFormat(), QSettings::UserScope, QStringLiteral("kerrdec97"), QStringLiteral("exFAT Image Builder Farewell")).fileName());
        QVERIFY2(file.startsWith(QDir::fromNativeSeparators(Paths::dataDir()), Qt::CaseInsensitive), qPrintable(file));
        if (before.isEmpty())
            qunsetenv("FW_DATA_DIR");
        else
            qputenv("FW_DATA_DIR", before);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
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
};

QTEST_MAIN(TstAppShellContext)
#include "tst_app_shell_context.moc"
