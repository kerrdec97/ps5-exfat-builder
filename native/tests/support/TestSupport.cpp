#include "support/TestSupport.h"

#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>

namespace fwtest {

void isolateAppData(QTemporaryDir& dir)
{
    fw::Paths::overrideRoot(dir.path());
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
