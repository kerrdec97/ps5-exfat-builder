#include "connect/ps5/ConsoleBackup.h"

#include "connect/ps5/RemotePath.h"
#include "core/FileUtil.h"
#include "core/Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fw::ps5 {

namespace {

QString localPathFor(const QString& remotePath, const QString& backupDir)
{
    QString rel = normalizeRemotePath(remotePath).mid(1);
    QStringList parts = rel.split(u'/', Qt::SkipEmptyParts);
    for (QString& p : parts)
        p = sanitizeFileName(p);
    QString path = QDir(backupDir).filePath(parts.join(u'/'));
    QString candidate = path;
    for (int n = 2; QFileInfo::exists(candidate) || QFileInfo::exists(candidate + QStringLiteral(".part")); ++n)
        candidate = path + QStringLiteral(".%1").arg(n);
    return candidate;
}

} // namespace

QString newConsoleBackupDir()
{
    return QDir(Paths::dataDir())
        .filePath(QStringLiteral("ps5-backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
}

Result<QString> saveRemoteCopy(const QByteArray& bytes, const QString& remotePath, const QString& backupDir)
{
    const QString local = localPathFor(remotePath, backupDir);
    if (!QDir().mkpath(QFileInfo(local).absolutePath()))
        return Error(ErrorCode::IoError, QStringLiteral("The backup folder could not be created, so nothing on the "
                                                        "PS5 was changed."),
                     QDir::toNativeSeparators(backupDir));
    FW_TRY(writeFileAtomic(local, bytes));
    return local;
}

Result<QString> backupRemoteFile(FtpClient& c, const QString& remotePath, const QString& backupDir,
                                 const CancellationToken& token)
{
    const QString local = localPathFor(remotePath, backupDir);
    if (!QDir().mkpath(QFileInfo(local).absolutePath()))
        return Error(ErrorCode::IoError, QStringLiteral("The backup folder could not be created, so nothing on the "
                                                        "PS5 was changed."),
                     QDir::toNativeSeparators(backupDir));
    const QString part = local + QStringLiteral(".part");
    QFile out(part);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Error(ErrorCode::IoError, QStringLiteral("The backup could not be written, so nothing on the PS5 was "
                                                        "changed."),
                     out.errorString());
    const Status st = c.retrieve(remotePath, out, 0, {}, token);
    out.close();
    if (!st.ok()) {
        QFile::remove(part);
        if (st.error().isCancelled())
            return st.error();
        // 550: either missing (nothing to back up) or unreadable (refuse).
        if (st.error().code == ErrorCode::NotFound && c.isConnected() && !c.fileExists(remotePath))
            return QString();
        return Error(st.error().code,
                     QStringLiteral("%1 could not be backed up, so it was not changed. %2").arg(remotePath, st.error().message),
                     st.error().detail);
    }
    if (!QFile::rename(part, local)) {
        QFile::remove(part);
        return Error(ErrorCode::IoError, QStringLiteral("The backup could not be saved, so nothing on the PS5 was changed."));
    }
    return local;
}

} // namespace fw::ps5
