#pragma once

// Paths on the console (always '/'-separated) and the legacy safety rules for
// writing to them.

#include <QString>
#include <QStringList>

namespace fw::ps5 {

// "/data//x/" -> "/data/x"; "" -> "/"; backslashes become slashes.
QString normalizeRemotePath(const QString& path);
QString joinRemote(const QString& dir, const QString& name);
QString remoteParent(const QString& path);   // "/a/b" -> "/a"; "/a" -> "/"
QString remoteFileName(const QString& path); // "/a/b" -> "b"

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
