// ShadowMount+ fakelib manager. Rules from the legacy ui/tab_shadowmount.py
// (_fakelib_validate, _fakelib_list_dir, _fakelib_add/_replace/_remove/_toggle).

#include "connect/ps5/Fakelib.h"

#include "connect/ps5/RemotePath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace fw::ps5::fakelib {

namespace {

const QStringList& rejectedImageSuffixes()
{
    static const QStringList s = {QStringLiteral(".exfat"), QStringLiteral(".ffpkg"), QStringLiteral(".ffpfs"), QStringLiteral(".ffpfsc")};
    return s;
}

bool hasLibrarySuffix(const QString& lowName)
{
    return lowName.endsWith(QStringLiteral(".prx")) || lowName.endsWith(QStringLiteral(".sprx"));
}

// Lists one folder; a folder that is not there is empty.
Result<std::vector<FtpEntry>> listFolder(FtpClient& c, const QString& folder, const CancellationToken& token, bool* exists)
{
    auto r = c.list(folder, FtpListMode::Auto, token);
    if (!r.ok()) {
        if (r.error().code == ErrorCode::NotFound && c.isConnected()) {
            *exists = false;
            return std::vector<FtpEntry>{};
        }
        return r.takeError();
    }
    *exists = true;
    return r;
}

Result<QByteArray> readLocal(const QString& localPath, const QString& name)
{
    QFile f(localPath);
    if (f.size() > kMaxOverlayBytes)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("%1 is larger than %2 MiB; that is not a library overlay.")
                                                     .arg(name).arg(kMaxOverlayBytes / (1024 * 1024)));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(localPath)), f.errorString());
    return f.readAll();
}

Status checkBeforeUpload(const QString& localPath, bool confirmed)
{
    const LocalCheck check = checkLocalFile(localPath);
    if (check.verdict == Verdict::Reject)
        return Error(ErrorCode::InvalidArgument, check.message);
    if (check.verdict == Verdict::Confirm && !confirmed)
        return Error(ErrorCode::InvalidArgument, check.message);
    return success();
}

} // namespace

QString Overlay::path() const
{
    return joinRemote(folder(), name);
}

bool isListedName(const QString& name)
{
    const QString low = name.toLower();
    if (low.endsWith(QStringLiteral(".uploading")) || low.endsWith(QStringLiteral(".replacing.bak")))
        return false;
    return hasLibrarySuffix(low);
}

int Listing::enabledCount() const
{
    return int(std::count_if(overlays.begin(), overlays.end(), [](const Overlay& o) { return o.enabled; }));
}

Result<Listing> list(FtpClient& c, const CancellationToken& token)
{
    Listing out;
    for (const bool enabled : {true, false}) {
        bool exists = true;
        FW_TRY_ASSIGN(std::vector<FtpEntry> entries, listFolder(c, enabled ? kFolder : kDisabledFolder, token, &exists));
        if (enabled)
            out.folderExists = exists;
        for (const FtpEntry& e : entries) {
            if (e.isDir || !isListedName(e.name))
                continue;
            if (!remoteNameProblem(e.name).isEmpty()) {
                out.skipped << e.name;
                continue;
            }
            Overlay o;
            o.name = e.name;
            o.enabled = enabled;
            o.size = e.size;
            o.modified = e.modified;
            out.overlays.push_back(o);
        }
        if (!out.folderExists)
            break; // no fakelib folder: no disabled folder either
    }
    std::stable_sort(out.overlays.begin(), out.overlays.end(), [](const Overlay& a, const Overlay& b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        if (c != 0)
            return c < 0;
        return a.enabled && !b.enabled;
    });
    return out;
}

LocalCheck checkLocalFile(const QString& localPath)
{
    const QFileInfo fi(localPath);
    const QString base = fi.fileName();
    const QString low = base.toLower();
    QString norm = localPath;
    norm.replace(u'\\', u'/');
    if (fi.isDir())
        return {Verdict::Reject, QStringLiteral("Folders cannot be deployed as overlays. Select a single .prx or .sprx library file.")};
    if (low == u"eboot.bin")
        return {Verdict::Reject, QStringLiteral("eboot.bin cannot be replaced via fakelib overlays — it lives inside the image. Use the normal rebuild workflow.")};
    for (const QString& part : norm.split(u'/', Qt::SkipEmptyParts))
        if (part.compare(QLatin1String("sce_sys"), Qt::CaseInsensitive) == 0)
            return {Verdict::Reject, QStringLiteral("sce_sys files cannot be deployed as overlays.")};
    for (const QString& s : rejectedImageSuffixes())
        if (low.endsWith(s))
            return {Verdict::Reject, QStringLiteral("Image files (.exfat / .ffpkg / .ffpfs / .ffpfsc) cannot be deployed as overlays. Fakelib Manager only handles library files.")};
    if (!hasLibrarySuffix(low))
        return {Verdict::Reject, QStringLiteral("Only .prx / .sprx library overlays are accepted. \"%1\" is not a supported file.").arg(base)};
    if (!remoteNameProblem(base).isEmpty())
        return {Verdict::Reject, QStringLiteral("\"%1\" cannot be used as a file name on the console.").arg(base)};
    if (!fi.exists())
        return {Verdict::Reject, QStringLiteral("The file %1 does not exist.").arg(QDir::toNativeSeparators(localPath))};
    QFile f(localPath);
    QByteArray magic;
    const bool readable = f.open(QIODevice::ReadOnly);
    if (readable)
        magic = f.read(4);
    if (!readable)
        return {Verdict::Confirm, QStringLiteral("Could not read \"%1\" to confirm it is an ELF/SELF library. Upload it as an overlay anyway?").arg(base)};
    const bool elfOrSelf = magic == QByteArray("\x7f" "ELF", 4) || magic == QByteArray("\x4f\x15\x3d\x1d", 4)
                        || magic == QByteArray("\x54\x14\xf5\xee", 4);
    if (!elfOrSelf)
        return {Verdict::Confirm, QStringLiteral("\"%1\" does not have an ELF/SELF header, so it may not be a valid library. Upload it as an overlay anyway?").arg(base)};
    return {};
}

Status add(FtpClient& c, const QString& localPath, bool confirmed, const CancellationToken& token)
{
    FW_TRY(checkBeforeUpload(localPath, confirmed));
    const QString name = QFileInfo(localPath).fileName();
    FW_TRY_ASSIGN(QByteArray data, readLocal(localPath, name));
    FW_TRY(c.mkdirs(kFolder));
    return c.uploadReplace(joinRemote(kFolder, name), data, QStringLiteral(".uploading"), token);
}

Status replace(FtpClient& c, const Overlay& target, const QString& localPath, bool confirmed, const CancellationToken& token)
{
    if (!remoteNameProblem(target.name).isEmpty())
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" cannot be used safely as a name on the console.").arg(target.name));
    FW_TRY(checkBeforeUpload(localPath, confirmed));
    FW_TRY_ASSIGN(QByteArray data, readLocal(localPath, QFileInfo(localPath).fileName()));
    return c.uploadReplace(target.path(), data, QStringLiteral(".uploading"), token);
}

Status remove(FtpClient& c, const Overlay& target)
{
    if (!remoteNameProblem(target.name).isEmpty() || !isListedName(target.name))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" is not an overlay the manager can remove.").arg(target.name));
    return c.dele(target.path());
}

Status setEnabled(FtpClient& c, const Overlay& target, bool enabled)
{
    if (!remoteNameProblem(target.name).isEmpty() || !isListedName(target.name))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("\"%1\" is not an overlay the manager can move.").arg(target.name));
    if (target.enabled == enabled)
        return success();
    Overlay moved = target;
    moved.enabled = enabled;
    if (!enabled)
        FW_TRY(c.mkdirs(kDisabledFolder));
    if (c.fileExists(moved.path()))
        return Error(ErrorCode::AlreadyExists,
                     QStringLiteral("%1 is already %2. Remove one of the two copies first.")
                         .arg(target.name, enabled ? QStringLiteral("enabled") : QStringLiteral("in the disabled folder")));
    if (auto st = c.rename(target.path(), moved.path()); !st.ok())
        return Error(st.error().code, QStringLiteral("Could not move %1: %2").arg(target.name, st.error().message), st.error().detail);
    return success();
}

} // namespace fw::ps5::fakelib
