#include "support/TestSupport.h"

#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>

#include <algorithm>

namespace fwtest {

void isolateAppData(QTemporaryDir& dir)
{
    fw::Paths::overrideRoot(dir.path());
    if (!qEnvironmentVariableIsSet("FW_OFFLINE"))
        qputenv("FW_OFFLINE", "1");
    const auto own = [&dir](const char* variable, const char* folder) {
        qputenv(variable, QDir::toNativeSeparators(dir.filePath(QLatin1String(folder))).toUtf8());
    };
    own("D97BETA_CONFIG_DIR", "bp-config");
    own("D97BETA_DATA_DIR", "bp-data");
    own("D97BETA_ENGINE_HOME", "bp-engine-home");
}

bool denyListing(const QString& dir, bool deny)
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("icacls"),
            {QDir::toNativeSeparators(dir), deny ? QStringLiteral("/deny") : QStringLiteral("/remove:d"),
             deny ? QStringLiteral("*S-1-1-0:(RD)") : QStringLiteral("*S-1-1-0")});
    return p.waitForFinished(20000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
#else
    return QFile::setPermissions(dir, deny ? QFileDevice::Permissions() : QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
}

bool writeRandomFile(const QString& path, qint64 size, quint32 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QRandomGenerator rng(seed);
    QByteArray buf;
    const qint64 chunk = 1 << 20;
    while (size > 0) {
        const qint64 n = std::min(size, chunk);
        buf.resize(int(n));
        for (qint64 i = 0; i < n; ++i)
            buf[int(i)] = char(rng.generate() & 0xFF);
        if (f.write(buf) != n)
            return false;
        size -= n;
    }
    return true;
}

bool writeTextFile(const QString& path, const QByteArray& text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text) == text.size();
}

QString helperPath() { return QStringLiteral(FW_TEST_HELPER); }
QString dataDir() { return QStringLiteral(FW_TEST_DATA_DIR); }

} // namespace fwtest
