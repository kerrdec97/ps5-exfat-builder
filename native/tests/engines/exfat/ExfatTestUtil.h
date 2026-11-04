#pragma once

// Helpers shared by the exFAT module tests (header-only: the test CMake
// builds one executable per tst_*.cpp).

#include "core/Cancellation.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/exfat/ExfatWriter.h"
#include "engines/exfat/SourceScan.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTest>

#include <vector>

namespace exfattest {

inline bool writeRandom(const QString& path, qint64 size, quint32 seed)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QRandomGenerator rng(seed);
    std::vector<quint64> buf(size_t(1 << 17)); // 1 MiB
    while (size > 0) {
        rng.fillRange(buf.data(), qsizetype(buf.size()));
        const qint64 n = std::min<qint64>(size, qint64(buf.size() * 8));
        if (f.write(reinterpret_cast<const char*>(buf.data()), n) != n)
            return false;
        size -= n;
    }
    return true;
}

inline bool writeBytes(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// A synthetic game-like tree: nested folders, Unicode and 255-character
// names, empty files and folders, a few MiB of random data, many small files.
inline void makeSampleTree(const QString& root, int smallFiles = 300)
{
    QDir().mkpath(root);
    writeBytes(root + QStringLiteral("/eboot.bin"), QByteArray("\x7F" "ELF fake eboot", 15));
    writeRandom(root + QStringLiteral("/sce_sys/param.json.bin"), 3000, 1);
    writeBytes(root + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA01234","contentVersion":"01.000.000"})");
    writeRandom(root + QStringLiteral("/data/big.bin"), 3 * 1024 * 1024 + 123, 2);
    writeRandom(root + QStringLiteral("/data/medium.bin"), 700 * 1024, 3);
    writeBytes(root + QStringLiteral("/data/empty.dat"), {});
    QDir().mkpath(root + QStringLiteral("/empty folder"));
    QDir().mkpath(root + QStringLiteral("/a/b/c/d/e/f"));
    writeBytes(root + QStringLiteral("/a/b/c/d/e/f/deep.txt"), "deep");
    writeBytes(root + QString::fromUtf8("/Ünïcödé 日本語 файл 🎮.txt"), "unicode name");
    writeBytes(root + QString::fromUtf8("/Ελληνικά/ĞüŞİöç.bin"), "turkish and greek");
    const QString longName = QString(251, u'L') + QStringLiteral(".bin"); // 255 characters
    writeBytes(root + u'/' + longName, "255 characters");
    const QString longDir = QString(255, u'D');
    writeBytes(root + u'/' + longDir + QStringLiteral("/inside.txt"), "in a 255-character folder");
    QRandomGenerator rng(42);
    for (int i = 0; i < smallFiles; ++i) {
        const QString p = root + QStringLiteral("/many/sub%1/file_%2.dat").arg(i % 7).arg(i, 4, 10, QLatin1Char('0'));
        writeRandom(p, rng.bounded(0, 5000), quint32(100 + i));
    }
}

struct Fsck {
    bool available = false;
    int exitCode = -1;
    QString output;
};

inline QString fsckPath()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("fsck.exfat"));
    if (p.isEmpty())
        p = QStandardPaths::findExecutable(QStringLiteral("fsck.exfat"), {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin")});
    return p;
}

inline Fsck runFsck(const QString& image)
{
    Fsck r;
    const QString exe = fsckPath();
    if (exe.isEmpty())
        return r;
    r.available = true;
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, {QStringLiteral("-n"), image});
    if (!p.waitForFinished(120000))
        return r;
    r.exitCode = p.exitCode();
    r.output = QString::fromLocal8Bit(p.readAll());
    return r;
}

inline fw::Result<fw::exfat::ImagePlan> build(const QString& src, const QString& image,
                                             fw::exfat::WriterOptions o = {}, fw::exfat::ScanRules rules = {})
{
    fw::CancellationToken token;
    auto tree = fw::exfat::scanSource(src, rules, token);
    if (!tree.ok())
        return tree.takeError();
    if (o.label.isEmpty())
        o.label = QStringLiteral("OSFIMG");
    o.syncToDisk = false;
    auto plan = fw::exfat::planImage(tree.value(), o);
    if (!plan.ok())
        return plan.takeError();
    auto st = fw::exfat::writeImage(tree.value(), plan.value(), image, o, token);
    if (!st.ok())
        return st.takeError();
    return plan;
}

inline QByteArray readAllOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Every file/folder under `root` (relative path -> size, -1 for folders).
inline QMap<QString, qint64> listing(const QString& root)
{
    QMap<QString, qint64> out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        out.insert(base.relativeFilePath(fi.absoluteFilePath()), fi.isDir() ? -1 : fi.size());
    }
    return out;
}

inline void flipByte(const QString& path, qint64 offset, uchar mask = 0xFF)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.seek(offset));
    char c = 0;
    QCOMPARE(f.read(&c, 1), qint64(1));
    c = char(uchar(c) ^ mask);
    QVERIFY(f.seek(offset));
    QCOMPARE(f.write(&c, 1), qint64(1));
}

} // namespace exfattest
