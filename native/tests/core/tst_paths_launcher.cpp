// Paths and the single-file launcher (src/onefile): the launcher runs the app
// from an unpacked copy and sets FW_LAUNCHER_DIR to its own folder, which then
// counts as the app folder (portable.txt, the portable data folder).

#include "core/Paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

class TstPathsLauncher : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QByteArray m_savedDataDir;
    bool m_hadDataDir = false;

    static void setEnv(const char* name, const QString& value) { qputenv(name, QDir::toNativeSeparators(value).toLocal8Bit()); }

    QString folder(const QString& name, bool portable)
    {
        const QString dir = m_root.filePath(name);
        QDir().mkpath(dir);
        if (portable) {
            QFile f(dir + QStringLiteral("/portable.txt"));
            if (!f.open(QIODevice::WriteOnly))
                qWarning("cannot create %s", qPrintable(f.fileName()));
        }
        return QDir::cleanPath(dir);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        m_hadDataDir = qEnvironmentVariableIsSet("FW_DATA_DIR");
        m_savedDataDir = qgetenv("FW_DATA_DIR");
    }

    void cleanup()
    {
        qunsetenv("FW_LAUNCHER_DIR");
        if (m_hadDataDir)
            qputenv("FW_DATA_DIR", m_savedDataDir);
        else
            qunsetenv("FW_DATA_DIR");
        Paths::resetForTests();
    }

    void appDirIsTheExecutableFolderWithoutTheLauncher()
    {
        qunsetenv("FW_LAUNCHER_DIR");
        QCOMPARE(Paths::appDir(), QCoreApplication::applicationDirPath());
    }

    void appDirIsTheLauncherFolder()
    {
        const QString dir = folder(QStringLiteral("launcher folder"), false);
        setEnv("FW_LAUNCHER_DIR", dir);
        QCOMPARE(Paths::appDir(), dir);
        setEnv("FW_LAUNCHER_DIR", dir + QStringLiteral("/")); // trailing separator, as for "D:\"
        QCOMPARE(Paths::appDir(), dir);
    }

    void aMissingLauncherFolderIsIgnored()
    {
        setEnv("FW_LAUNCHER_DIR", m_root.filePath(QStringLiteral("does not exist")));
        QCOMPARE(Paths::appDir(), QCoreApplication::applicationDirPath());
    }

    void portableTxtNextToTheLauncherMeansPortable()
    {
        const QString dir = folder(QStringLiteral("portable launcher"), true);
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/portable.txt"))); // without it the resolver would turn to the PC's own folders
        qunsetenv("FW_DATA_DIR");
        setEnv("FW_LAUNCHER_DIR", dir);
        Paths::resetForTests();
        QVERIFY(Paths::isPortable());
        QCOMPARE(QDir::cleanPath(Paths::dataDir()), dir + QStringLiteral("/data"));
        QCOMPARE(Paths::localDir(), Paths::dataDir());
        QVERIFY(QFileInfo(dir + QStringLiteral("/data")).isDir());
        QCOMPARE(QDir::cleanPath(Paths::settingsFile()), dir + QStringLiteral("/data/settings.json"));
    }

    void theDataDirOverrideStillWins()
    {
        const QString dir = folder(QStringLiteral("portable launcher 2"), true);
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/portable.txt")));
        const QString override = m_root.filePath(QStringLiteral("override"));
        setEnv("FW_DATA_DIR", override);
        setEnv("FW_LAUNCHER_DIR", dir);
        Paths::resetForTests();
        QVERIFY(!Paths::isPortable());
        QCOMPARE(QDir::cleanPath(Paths::dataDir()), QDir::cleanPath(override + QStringLiteral("/data")));
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/data")));
    }
};

QTEST_GUILESS_MAIN(TstPathsLauncher)
#include "tst_paths_launcher.moc"
