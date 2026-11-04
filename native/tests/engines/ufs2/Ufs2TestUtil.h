#pragma once

// Helpers shared by the UFS2 tests (header-only: every tst_*.cpp is its own
// executable).

#include "core/FileUtil.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "engines/ufs2/Ufs2Tree.h"
#include "engines/ufs2/Ufs2Writer.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

namespace ufs2test {

using namespace fw;
using namespace fw::ufs2;

inline bool mkfile(const QString& path, qint64 size, quint32 seed)
{
    return fwtest::writeRandomFile(path, size, seed);
}

inline bool mktext(const QString& path, const QByteArray& text) { return fwtest::writeTextFile(path, text); }

// A small PS5-dump-like tree: eboot, sce_sys, nested folders, empty file and
// folder, a file that needs an indirect block (> 12 blocks).
inline bool makeGameTree(const QString& root, qint64 bigFile = 2 * 1024 * 1024)
{
    bool ok = true;
    ok &= mkfile(root + "/eboot.bin", 200000, 1);
    ok &= mktext(root + "/sce_sys/param.json", "{\"titleId\":\"PPSA00000\",\"contentVersion\":\"01.000.000\"}");
    ok &= mkfile(root + "/sce_sys/icon0.png", 70000, 2);
    ok &= mkfile(root + "/data/level1/level2/level3/deep.bin", 5000, 3);
    ok &= mkfile(root + "/data/big.pak", bigFile, 4);
    ok &= mktext(root + "/data/empty.txt", "");
    ok &= QDir().mkpath(root + "/emptydir");
    ok &= mkfile(root + "/.hidden", 10, 5);
    return ok;
}

// Many entries in one directory (several directory blocks at 4 KiB... here
// 64 KiB blocks need ~2000+ long names to span blocks).
inline bool makeWideDir(const QString& dir, int count, int nameLen, quint32 seed)
{
    bool ok = QDir().mkpath(dir);
    for (int i = 0; i < count && ok; ++i) {
        QString name = QStringLiteral("f%1_").arg(i, 5, 10, QLatin1Char('0'));
        while (name.size() < nameLen)
            name += QLatin1Char('a' + (i + name.size()) % 26);
        ok &= mkfile(dir + u'/' + name, (i * 37) % 3000, seed + quint32(i));
    }
    return ok;
}

inline QString findTool(const QString& envName, const QStringList& names)
{
    const QString env = qEnvironmentVariable(envName.toLatin1().constData());
    if (!env.isEmpty())
        return QFileInfo(env).isFile() ? env : QString();
    for (const QString& n : names) {
        const QString p = QStandardPaths::findExecutable(n);
        if (!p.isEmpty())
            return p;
    }
    return {};
}

struct ToolRun {
    int exitCode = -1;
    QString output;
};

inline ToolRun runTool(const QString& program, const QStringList& args, int timeoutMs = 600000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    ToolRun r;
    if (!p.waitForStarted(30000))
        return r;
    p.waitForFinished(timeoutMs);
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.output = QString::fromUtf8(p.readAll());
    return r;
}

// UFS2Tool 4.0.0 (the managed UFS2Tool.dll the legacy app embeds), run with
// `dotnet`. FW_UFS2TOOL_DLL points at it; empty when unavailable.
inline QString ufs2toolDll() { return findTool(QStringLiteral("FW_UFS2TOOL_DLL"), {}); }
inline QString dotnet() { return findTool(QStringLiteral("FW_DOTNET"), {QStringLiteral("dotnet")}); }

inline ToolRun runUfs2Tool(const QStringList& args)
{
    return runTool(dotnet(), QStringList{ufs2toolDll()} + args);
}

// FreeBSD fsck_ffs / dumpfs ports (Debian/Ubuntu "ufsutils": fsck.ufs, dumpfs.ufs).
inline QString fsckUfs() { return findTool(QStringLiteral("FW_FSCK_UFS"), {QStringLiteral("fsck.ufs"), QStringLiteral("fsck_ufs"), QStringLiteral("fsck_ffs")}); }
inline QString dumpfsUfs() { return findTool(QStringLiteral("FW_DUMPFS_UFS"), {QStringLiteral("dumpfs.ufs"), QStringLiteral("dumpfs")}); }

inline QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// First differing byte offset, or -1.
inline qint64 firstDifference(const QByteArray& a, const QByteArray& b)
{
    const qint64 n = std::min(a.size(), b.size());
    for (qint64 i = 0; i < n; ++i)
        if (a[i] != b[i])
            return i;
    return a.size() == b.size() ? -1 : n;
}

// Every file under `root` with its content, relative paths, sorted.
inline QMap<QString, QByteArray> snapshotFolder(const QString& root)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QString rel = base.relativeFilePath(fi.absoluteFilePath());
        out.insert(rel + (fi.isDir() ? QStringLiteral("/") : QString()), fi.isDir() ? QByteArray() : readAll(fi.absoluteFilePath()));
    }
    return out;
}

inline Result<WriteStats> buildImage(const QString& src, const QString& out, const WriterOptions& opt = {},
                                     PlanSummary* summary = nullptr)
{
    CancellationToken token;
    FW_TRY_ASSIGN(SourceTree tree, scanLocalFolder(src, token));
    Ufs2Writer w(tree, opt);
    FW_TRY_ASSIGN(PlanSummary s, w.plan());
    if (summary)
        *summary = s;
    return w.write(out, token, nullptr);
}

} // namespace ufs2test
