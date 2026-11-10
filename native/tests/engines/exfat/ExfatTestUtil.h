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

#include <algorithm>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// QCOMPARE on the error code of a result that must have failed. A result that
// succeeded fails the test here; asking it for its error would end the whole
// test program instead.
#ifndef FWTEST_COMPARE_ERROR
#define FWTEST_COMPARE_ERROR(result, expectedCode)                                \
    do {                                                                          \
        const auto& fwtestResult_ = (result);                                     \
        QVERIFY2(!fwtestResult_.ok(), "this succeeded; expected " #expectedCode); \
        QCOMPARE(fwtestResult_.error().code, expectedCode);                       \
    } while (false)
#endif

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

// Gives an existing file the hidden attribute. Windows only: elsewhere a file
// is hidden by a leading dot in its name and there is no attribute to set
// (false).
inline bool setHiddenAttribute(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(native.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(native.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
#else
    Q_UNUSED(path);
    return false;
#endif
}

// A synthetic game-like tree: nested folders, Unicode and 255-character
// names, empty files and folders, a few MiB of random data, many small files.
// False when any of it could not be created (a file system that refuses the
// long or the Unicode names would otherwise quietly test without them).
[[nodiscard]] inline bool makeSampleTree(const QString& root, int smallFiles = 300)
{
    bool ok = QDir().mkpath(root);
    ok &= writeBytes(root + QStringLiteral("/eboot.bin"), QByteArray("\x7F" "ELF fake eboot", 15));
    ok &= writeRandom(root + QStringLiteral("/sce_sys/param.json.bin"), 3000, 1);
    ok &= writeBytes(root + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA01234","contentVersion":"01.000.000"})");
    ok &= writeRandom(root + QStringLiteral("/data/big.bin"), 3 * 1024 * 1024 + 123, 2);
    ok &= writeRandom(root + QStringLiteral("/data/medium.bin"), 700 * 1024, 3);
    ok &= writeBytes(root + QStringLiteral("/data/empty.dat"), {});
    ok &= QDir().mkpath(root + QStringLiteral("/empty folder"));
    ok &= QDir().mkpath(root + QStringLiteral("/a/b/c/d/e/f"));
    ok &= writeBytes(root + QStringLiteral("/a/b/c/d/e/f/deep.txt"), "deep");
    ok &= writeBytes(root + QString::fromUtf8("/Ünïcödé 日本語 файл 🎮.txt"), "unicode name");
    ok &= writeBytes(root + QString::fromUtf8("/Ελληνικά/ĞüŞİöç.bin"), "turkish and greek");
    const QString longName = QString(251, u'L') + QStringLiteral(".bin"); // 255 characters
    ok &= writeBytes(root + u'/' + longName, "255 characters");
    const QString longDir = QString(255, u'D');
    ok &= writeBytes(root + u'/' + longDir + QStringLiteral("/inside.txt"), "in a 255-character folder");
    QRandomGenerator rng(42);
    for (int i = 0; i < smallFiles; ++i) {
        const QString p = root + QStringLiteral("/many/sub%1/file_%2.dat").arg(i % 7).arg(i, 4, 10, QLatin1Char('0'));
        ok &= writeRandom(p, rng.bounded(0, 5000), quint32(100 + i));
    }
    return ok;
}

// What listing() counts for makeSampleTree(root, smallFiles): 11 files and
// 11 folders of its own, then "many", its (at most 7) folders and the small
// files in them.
inline qsizetype sampleTreeEntries(int smallFiles)
{
    return 11 + 11 + (smallFiles > 0 ? 1 : 0) + std::min(smallFiles, 7) + smallFiles;
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

// Copies the folder `from` (files and sub-folders) to the new folder `to`.
inline bool copyTree(const QString& from, const QString& to)
{
    if (!QDir().mkpath(to))
        return false;
    QDirIterator it(from, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    const QDir base(from);
    while (it.hasNext()) {
        it.next();
        const QString dst = to + u'/' + base.relativeFilePath(it.filePath());
        if (it.fileInfo().isDir()) {
            if (!QDir().mkpath(dst))
                return false;
        } else if (!QDir().mkpath(QFileInfo(dst).absolutePath()) || !QFile::copy(it.filePath(), dst)) {
            return false;
        }
    }
    return true;
}

// Manual cross-checks with other exFAT drivers. When the environment variable
// names a folder, the image and the tree it must hold are copied into the new
// sub-folder `name` of it. The folder is somebody's own: nothing in it is
// removed or replaced, and a sub-folder that is already there (and not empty)
// is an error for whoever asked for the copy to clear. Returns an empty
// string, or what went wrong.
inline QString keepForCrossCheck(const char* variable, const QString& name, const QString& image,
                                 const QString& imageName, const QString& tree, const QString& treeName)
{
    const QString base = qEnvironmentVariable(variable);
    if (base.isEmpty())
        return {};
    const QString dir = QDir(base).filePath(name);
    if (QFileInfo::exists(dir) && !(QFileInfo(dir).isDir() && QDir(dir).isEmpty()))
        return QStringLiteral("%1: %2 already exists; remove it or name another folder").arg(QLatin1String(variable), dir);
    if (!QDir().mkpath(dir) || !QFile::copy(image, dir + u'/' + imageName) || !copyTree(tree, dir + u'/' + treeName))
        return QStringLiteral("%1: could not copy the sample to %2").arg(QLatin1String(variable), dir);
    return {};
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
