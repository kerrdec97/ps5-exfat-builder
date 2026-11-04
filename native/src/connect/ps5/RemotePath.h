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

// The system directories the legacy app warned about before any write
// (upload, delete, rename, move, new folder): /system, /system_ex, /preinst,
// /preinst2, /mini-syscore, /sandcastle, /update, /dev, /proc. A path is
// protected when it is one of them or inside one (case-insensitive).
const QStringList& protectedRemoteRoots();
bool isProtectedRemotePath(const QString& path);
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
