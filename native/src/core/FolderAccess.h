#pragma once

// A folder that cannot be listed must not pass for an empty one.
//
// QDir::entryInfoList() returns an empty list both for an empty folder and
// for one the user may not open, and on Windows QFileInfo::isReadable() does
// not look at NTFS permissions. Source scans call this when a listing comes
// back empty, so a folder with a foreign owner fails the scan instead of
// silently losing its files. Used by every engine that walks a source folder.

#include "core/Result.h"

#include <QString>

namespace fw {

// Success when the operating system can list `dir` (empty or not);
// PermissionDenied with a message naming the folder otherwise.
Status ensureFolderListable(const QString& dir);

// A name for the folder `dir` that is the same whichever symbolic link or
// junction it is reached through (its volume and file id; QFileInfo's
// canonical path does not resolve NTFS junctions). "" when it cannot be had.
// Scans keep the identities of the folders they are inside of, to notice a
// link that leads back into them.
QString folderIdentity(const QString& dir);

} // namespace fw
