#include "engines/backport/Common.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace fw::backport {

QString codeName(Code code)
{
    switch (code) {
    case Code::Ok: return QStringLiteral("Ok");
    case Code::NotAGame: return QStringLiteral("NotAGame");
    case Code::TargetSdkInvalid: return QStringLiteral("TargetSdkInvalid");
    case Code::TargetAboveGameSdk: return QStringLiteral("TargetAboveGameSdk");
    case Code::GameSdkUnknown: return QStringLiteral("GameSdkUnknown");
    case Code::AlreadyBackported: return QStringLiteral("AlreadyBackported");
    case Code::DiffersFromOriginals: return QStringLiteral("DiffersFromOriginals");
    case Code::FakelibInvalid: return QStringLiteral("FakelibInvalid");
    case Code::ZipFolderInvalid: return QStringLiteral("ZipFolderInvalid");
    case Code::PathTooLong: return QStringLiteral("PathTooLong");
    case Code::InsufficientSpace: return QStringLiteral("InsufficientSpace");
    case Code::NothingToDo: return QStringLiteral("NothingToDo");
    case Code::ElfInvalid: return QStringLiteral("ElfInvalid");
    case Code::ParamMissing: return QStringLiteral("ParamMissing");
    case Code::ParamSizeInvalid: return QStringLiteral("ParamSizeInvalid");
    case Code::ParamMagicInvalid: return QStringLiteral("ParamMagicInvalid");
    case Code::ParamOutOfRange: return QStringLiteral("ParamOutOfRange");
    case Code::SelfEncrypted: return QStringLiteral("SelfEncrypted");
    case Code::SelfCompressed: return QStringLiteral("SelfCompressed");
    case Code::SelfMalformed: return QStringLiteral("SelfMalformed");
    case Code::SignUnsupported: return QStringLiteral("SignUnsupported");
    case Code::FileTooLarge: return QStringLiteral("FileTooLarge");
    case Code::Unreadable: return QStringLiteral("Unreadable");
    case Code::VerifyFailed: return QStringLiteral("VerifyFailed");
    case Code::ZipWriteFailed: return QStringLiteral("ZipWriteFailed");
    case Code::ZipVerifyFailed: return QStringLiteral("ZipVerifyFailed");
    case Code::GameChanged: return QStringLiteral("GameChanged");
    case Code::ApplyFailed: return QStringLiteral("ApplyFailed");
    case Code::InterruptedApply: return QStringLiteral("InterruptedApply");
    case Code::RollbackFailed: return QStringLiteral("RollbackFailed");
    case Code::RestoreFailed: return QStringLiteral("RestoreFailed");
    case Code::ManifestMismatch: return QStringLiteral("ManifestMismatch");
    case Code::ModuleNameCase: return QStringLiteral("ModuleNameCase");
    case Code::ProviderChanged: return QStringLiteral("ProviderChanged");
    case Code::Cancelled: return QStringLiteral("Cancelled");
    case Code::Internal: return QStringLiteral("Internal");
    }
    return QStringLiteral("Internal");
}

ErrorCode toErrorCode(Code code)
{
    switch (code) {
    case Code::Ok:
    case Code::Internal: return ErrorCode::Internal;
    case Code::NotAGame: return ErrorCode::NotFound;
    case Code::AlreadyBackported: return ErrorCode::AlreadyExists;
    case Code::TargetSdkInvalid:
    case Code::TargetAboveGameSdk:
    case Code::GameSdkUnknown:
    case Code::DiffersFromOriginals:
    case Code::FakelibInvalid:
    case Code::ZipFolderInvalid:
    case Code::PathTooLong:
    case Code::NothingToDo:
    case Code::ModuleNameCase:
    case Code::InterruptedApply: return ErrorCode::InvalidArgument;
    case Code::InsufficientSpace: return ErrorCode::InsufficientSpace;
    case Code::ElfInvalid:
    case Code::ParamMissing:
    case Code::ParamSizeInvalid:
    case Code::ParamMagicInvalid:
    case Code::ParamOutOfRange:
    case Code::SelfMalformed: return ErrorCode::CorruptData;
    case Code::SelfEncrypted:
    case Code::SelfCompressed:
    case Code::SignUnsupported:
    case Code::FileTooLarge: return ErrorCode::Unsupported;
    case Code::Unreadable:
    case Code::ZipWriteFailed:
    case Code::ApplyFailed:
    case Code::RollbackFailed:
    case Code::RestoreFailed: return ErrorCode::IoError;
    case Code::VerifyFailed:
    case Code::ZipVerifyFailed:
    case Code::GameChanged:
    case Code::ProviderChanged:
    case Code::ManifestMismatch: return ErrorCode::VerificationFailed;
    case Code::Cancelled: return ErrorCode::Cancelled;
    }
    return ErrorCode::Internal;
}

Error makeError(Code code, const QString& message, const QString& detail)
{
    QString d = QStringLiteral("[%1]").arg(codeName(code));
    if (!detail.isEmpty())
        d += u' ' + detail;
    return Error(toErrorCode(code), message, d);
}

Code codeOf(const Error& error)
{
    if (error.isCancelled())
        return Code::Cancelled;
    const QString& d = error.detail;
    if (!d.startsWith(u'['))
        return Code::Internal;
    const qsizetype end = d.indexOf(u']');
    if (end < 0)
        return Code::Internal;
    const QString name = d.mid(1, end - 1);
    for (int c = int(Code::Ok); c <= int(Code::Internal); ++c)
        if (codeName(Code(c)) == name)
            return Code(c);
    return Code::Internal;
}

bool isOsMetadataFile(const QString& fileName)
{
    const QString low = fileName.toLower();
    if (low.startsWith(QStringLiteral("._")))
        return true;
    static const QStringList names = {QStringLiteral(".ds_store"), QStringLiteral("thumbs.db"),
                                      QStringLiteral("desktop.ini"), QStringLiteral("ehthumbs.db"),
                                      QStringLiteral(".localized")};
    return names.contains(low);
}

bool isOsMetadataDir(const QString& dirName)
{
    static const QStringList names = {QStringLiteral("__macosx"),
                                      QStringLiteral(".trashes"),
                                      QStringLiteral(".fseventsd"),
                                      QStringLiteral(".spotlight-v100"),
                                      QStringLiteral(".temporaryitems"),
                                      QStringLiteral("system volume information"),
                                      QStringLiteral("$recycle.bin")};
    return names.contains(dirName.toLower());
}

QString normalizeRel(const QString& rel)
{
    QString r = rel;
    r.replace(u'\\', u'/');
    while (r.startsWith(QStringLiteral("./")))
        r.remove(0, 2);
    while (r.startsWith(u'/'))
        r.remove(0, 1);
    return r;
}

QString joinPath(const QString& root, const QString& rel)
{
    const QString r = normalizeRel(rel);
    if (r.isEmpty())
        return root;
    if (root.endsWith(u'/') || root.endsWith(u'\\'))
        return root + r;
    return root + u'/' + r;
}

Result<QByteArray> readFileBytes(const QString& path, qint64 maxBytes)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return makeError(Code::Unreadable,
                         QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
    const qint64 size = f.size();
    if (size > maxBytes)
        return makeError(Code::FileTooLarge,
                         QStringLiteral("%1 is larger than %2, which this tool does not process.")
                             .arg(QFileInfo(path).fileName(), formatBytes(maxBytes)),
                         QStringLiteral("size %1").arg(size));
    QByteArray data = f.readAll();
    if (data.size() != size)
        return makeError(Code::Unreadable,
                         QStringLiteral("Could not read all of %1.").arg(QDir::toNativeSeparators(path)),
                         f.errorString());
    return data;
}

Result<QByteArray> readHead(const QString& path, qint64 n)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return makeError(Code::Unreadable,
                         QStringLiteral("Could not open %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
    return f.read(n);
}

Status writeFileBytes(const QString& path, const QByteArray& data, bool replaceExisting)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return makeError(Code::ZipWriteFailed,
                         QStringLiteral("Could not create the folder for %1.").arg(QDir::toNativeSeparators(path)));
    PartFile part(path);
    {
        QFile f(toLongPath(part.partPath()));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
        if (f.write(data) != data.size() || !f.flush())
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
        f.close();
    }
    return part.commit(replaceExisting);
}

void clearReadOnly(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring p = toLongPath(path).toStdWString();
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        SetFileAttributesW(p.c_str(), attrs & ~DWORD(FILE_ATTRIBUTE_READONLY));
#else
    const QFileInfo fi(path);
    if (fi.isFile() && !fi.isSymLink() && !(fi.permissions() & QFileDevice::WriteOwner))
        QFile::setPermissions(path, fi.permissions() | QFileDevice::WriteOwner);
#endif
}

bool removeFileForce(const QString& path)
{
    if (QFile::remove(path))
        return true;
    clearReadOnly(path);
    return QFile::remove(path);
}

Status replaceFileAtomic(const QString& src, const QString& dst)
{
#ifdef Q_OS_WIN
    const std::wstring s = toLongPath(src).toStdWString();
    const std::wstring d = toLongPath(dst).toStdWString();
    // A read-only target (a dump copied from a disc or a read-only share)
    // cannot be replaced; the backup of its content is already verified.
    clearReadOnly(dst);
    if (!MoveFileExW(s.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not replace %1. Close any program using it and try again.")
                         .arg(QDir::toNativeSeparators(dst)),
                     QStringLiteral("MoveFileExW error %1").arg(qulonglong(err)));
    }
    return success();
#else
    if (std::rename(QFile::encodeName(src).constData(), QFile::encodeName(dst).constData()) != 0)
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not replace %1.").arg(QDir::toNativeSeparators(dst)),
                     QString::fromLocal8Bit(std::strerror(errno)));
    return success();
#endif
}

} // namespace fw::backport
