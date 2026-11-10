#pragma once

// Paths on the console (always '/'-separated) and the legacy safety rules for
// writing to them.

#include <QString>
#include <QStringList>

#include <optional>

namespace fw::ps5 {

// For paths the user typed: trimmed, backslashes become slashes, then
// cleaned ("/data//x/" -> "/data/x"; "" -> "/"; ".." never climbs above "/").
QString normalizeRemotePath(const QString& path);
// For paths built by the app or received from the console: only '/'
// separators are collapsed and "."/".." resolved; characters are kept as they
// are (no trimming, no backslash conversion).
QString cleanRemotePath(const QString& path);
// `dir` + a trusted relative path ('/'-separated, e.g. from a local folder
// scan). Characters are kept exactly; "." and ".." segments are dropped.
QString joinRemote(const QString& dir, const QString& name);
QString remoteParent(const QString& path);   // "/a/b" -> "/a"; "/a" -> "/"
QString remoteFileName(const QString& path); // "/a/b" -> "b"

// Why a single name received from the console (LIST/MLSD/NLST) must not be
// used: empty, "." or "..", or containing '/', '\\', ':' or a control
// character (a path escape, or an NTFS alternate data stream on Windows).
// Empty when it is safe as one path segment.
QString remoteNameProblem(const QString& name);
// remoteNameProblem() plus, on Windows, the characters, trailing dot/space and
// device names (CON, NUL, COM1...) that would make a local file land elsewhere.
QString localNameProblem(const QString& name);
// `dir/name` for a name from a listing; nullopt when remoteNameProblem() is
// not empty. Use this, not joinRemote(), for names the console sent.
std::optional<QString> remoteChild(const QString& dir, const QString& name);

// The directories that need an extra confirmation before any write (upload,
// delete, rename, move, new folder): firmware and system state (/system,
// /system_ex, /system_data, /system_tmp, /preinst, /preinst2, /mini-syscore,
// /sandcastle, /update, /dev, /proc), installed apps, saves and trophies
// (/user/app, /user/home, /user/system) and the sandbox mounts (/mnt/sandbox).
// A path is protected when it is one of them, inside one, or a parent of one
// ("/user", "/mnt"; never "/" itself). Case-insensitive.
const QStringList& protectedRemoteRoots();
bool isProtectedRemotePath(const QString& path);
// What an upload of `localSources` (files and folders on this PC) into
// `remoteDir` would write to that is protected: the folder itself, or the
// top-level name a source gets there ("/" + a folder called "system_ex").
// Empty when nothing is. The upload job refuses such an upload unless
// allowProtectedPaths is set, so ask for the confirmation with this.
QString protectedUploadTarget(const QString& remoteDir, const QStringList& localSources);
// The legacy confirmation text for a write to a protected path.
QString protectedPathWarning(const QString& path, const QString& action);

// Legacy _ps5_dest_class(): a string heuristic on the path, no traffic.
enum class DestinationKind { External, Internal, Unknown };
struct DestinationInfo {
    DestinationKind kind = DestinationKind::Unknown;
    QString label; // "External USB / exFAT", "Internal PS5 storage", "Unknown location"
    QString note;
};
DestinationInfo classifyDestination(const QString& path);

} // namespace fw::ps5
