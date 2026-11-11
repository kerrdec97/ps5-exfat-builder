#include "app/services/PortableMode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

namespace fw::portable {

namespace {

QString native(const QString& path) { return QDir::toNativeSeparators(path); }

bool sameFolder(const QString& a, const QString& b)
{
    return QString::compare(QDir::cleanPath(a), QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

} // namespace

Plan planFor(const QString& launcherExe, const QString& currentDataDir)
{
    Plan p;
    const QFileInfo exe(launcherExe);
    p.exePath = QDir::cleanPath(exe.absoluteFilePath());
    p.exeDir = QDir::cleanPath(exe.absolutePath());
    const QDir dir(p.exeDir);
    p.markerFile = dir.filePath(QStringLiteral("portable.txt"));
    // The launcher's portable root is "<product>.data" (src/onefile/Runtime.cpp, runtimeRoots()).
    p.programDir = dir.filePath(QStringLiteral("exFAT Image Builder.data"));
    p.dataDir = dir.filePath(QStringLiteral("data")); // core/Paths, portable mode
    p.currentDataDir = QDir::cleanPath(currentDataDir);
    return p;
}

Plan planForFolderEdition(const QString& appExe, const QString& currentDataDir)
{
    Plan p = planFor(appExe, currentDataDir);
    p.programDir = p.exeDir;
    p.unpacks = false;
    return p;
}

bool isFolderEdition(const QString& appDir)
{
    if (appDir.isEmpty())
        return false;
    const QDir dir(appDir);
    return QFileInfo(dir.filePath(QStringLiteral("LICENCE.txt"))).isFile() && QFileInfo(dir.filePath(QStringLiteral("Qt6Core.dll"))).isFile();
}

QStringList filesToCopy(const Plan& plan)
{
    QStringList out;
    if (plan.currentDataDir.isEmpty())
        return out;
    const QDir dir(plan.currentDataDir);
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
        if (fi.isSymbolicLink() || fi.isJunction())
            continue;
        // The running app holds its lock file open; the portable copy makes its own.
        if (fi.suffix().compare(QLatin1String("lock"), Qt::CaseInsensitive) == 0)
            continue;
        out << fi.fileName();
    }
    return out;
}

Result<Outcome> makePortable(const Plan& plan, bool copyData)
{
    if (plan.exeDir.isEmpty() || !QFileInfo(plan.exeDir).isDir())
        return Error(ErrorCode::NotFound, QStringLiteral("The folder %1 does not exist.").arg(native(plan.exeDir)));
    if (QFileInfo::exists(plan.markerFile))
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 is already there: this copy is portable from its next start.").arg(native(plan.markerFile)));

    // The launcher unpacks the program into this folder and the app writes its
    // settings there: it must take new files. Tried, not asked: on Windows the
    // read-only flag of a folder says nothing, its permissions do.
    {
        QTemporaryFile probe(QDir(plan.exeDir).filePath(QStringLiteral(".fw-portable-XXXXXX")));
        if (!probe.open())
            return Error(ErrorCode::PermissionDenied,
                         QStringLiteral("%1 cannot be written to, so the app cannot keep its files there. Move the exe to a folder "
                                        "you can write to (not a read-only drive or a protected folder) and try again.")
                             .arg(native(plan.exeDir)),
                         probe.errorString());
    }

    Outcome out;
    if (copyData && !sameFolder(plan.dataDir, plan.currentDataDir)) {
        const QStringList names = filesToCopy(plan);
        if (!names.isEmpty() && !QDir().mkpath(plan.dataDir))
            return Error(ErrorCode::IoError, QStringLiteral("The folder %1 could not be created. Nothing was changed.").arg(native(plan.dataDir)));
        for (const QString& name : names) {
            const QString from = QDir(plan.currentDataDir).filePath(name);
            const QString to = QDir(plan.dataDir).filePath(name);
            // A file from an earlier portable use of this folder is the user's: it stays.
            if (QFileInfo::exists(to) || QFileInfo(to).isSymbolicLink()) {
                out.kept << name;
                continue;
            }
            QFile source(from);
            if (!source.copy(to)) // never over an existing file, and whole or not at all
                return Error(ErrorCode::IoError,
                             QStringLiteral("%1 could not be copied to %2. This copy was not made portable; what was copied so far "
                                            "can stay or be deleted.")
                                 .arg(name, native(plan.dataDir)),
                             source.errorString());
            out.copied << name;
        }
    }

    // Last: from here on the next start is portable.
    QFile marker(plan.markerFile);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be created. This copy was not made portable.").arg(native(plan.markerFile)),
                     marker.errorString());
    marker.write("exFAT Image Builder: portable mode.\r\n"
                 "While this file is next to the exe, the app keeps its unpacked program, settings, queue and logs in folders "
                 "next to the exe.\r\n"
                 "Delete this file to turn portable mode off.\r\n");
    marker.close();
    if (marker.error() != QFileDevice::NoError) {
        marker.remove(); // half a marker would still switch the next start
        return Error(ErrorCode::IoError, QStringLiteral("%1 could not be written. This copy was not made portable.").arg(native(plan.markerFile)),
                     marker.errorString());
    }
    return out;
}

} // namespace fw::portable
